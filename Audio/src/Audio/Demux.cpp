#include <Audio/Demux.hpp>

#include <algorithm>
#include <cstring>

namespace SFT::Audio {

    namespace {
        class IndexedDemuxer final : public Demuxer {
          public:
            IndexedDemuxer(EncodedBytes bytes, ContainerIndex index) : bytes_(std::move(bytes)), index_(std::move(index)) {
                // Playback order is timestamp order; formats may interleave tracks or (MP4 fragments) list them per track.
                std::ranges::stable_sort(index_.packets, [this](const PacketRef &a, const PacketRef &b) { return seconds(a) < seconds(b); });
                for (DemuxTrack &track : index_.tracks) {
                    track.packet_count = static_cast<u64>(std::ranges::count(index_.packets, track.id, &PacketRef::track));
                }
            }

            [[nodiscard]] ContainerKind kind() const noexcept override { return index_.kind; }
            [[nodiscard]] std::span<const DemuxTrack> tracks() const noexcept override { return index_.tracks; }
            [[nodiscard]] f64 duration_seconds() const noexcept override { return index_.duration_seconds; }
            [[nodiscard]] u64 packet_count() const noexcept override { return index_.packets.size(); }

            std::optional<DemuxPacket> next() override {
                if (position_ >= index_.packets.size()) return std::nullopt;
                return make(index_.packets[position_++]);
            }

            std::optional<DemuxPacket> next(u32 track) override {
                while (position_ < index_.packets.size()) {
                    const PacketRef &ref = index_.packets[position_++];
                    if (ref.track == track) return make(ref);
                }
                return std::nullopt;
            }

            void seek(f64 seconds_target) override {
                const auto it = std::ranges::upper_bound(index_.packets, seconds_target, {}, [this](const PacketRef &p) { return seconds(p); });
                position_ = it == index_.packets.begin() ? 0 : static_cast<usize>(it - index_.packets.begin()) - 1;
            }

          private:
            [[nodiscard]] f64 tick_seconds(u32 track) const {
                for (usize i = 0; i < index_.tracks.size(); ++i) {
                    if (index_.tracks[i].id == track) return index_.tick_seconds[i];
                }
                return 0.0;
            }
            [[nodiscard]] f64 seconds(const PacketRef &ref) const { return static_cast<f64>(ref.pts_ticks) * tick_seconds(ref.track); }

            [[nodiscard]] DemuxPacket make(const PacketRef &ref) const {
                const f64 scale = tick_seconds(ref.track);
                return DemuxPacket{ref.track, static_cast<f64>(ref.pts_ticks) * scale, static_cast<f64>(ref.duration_ticks) * scale, ref.keyframe,
                                   std::span<const std::byte>(bytes_->data() + ref.offset, ref.size)};
            }

            EncodedBytes bytes_;
            ContainerIndex index_;
            usize position_ = 0;
        };

        bool starts_with(std::span<const std::byte> data, usize offset, const char *text) {
            const usize n = std::strlen(text);
            return data.size() >= offset + n && std::memcmp(data.data() + offset, text, n) == 0;
        }
    } // namespace

    std::unique_ptr<Demuxer> make_indexed_demuxer(EncodedBytes bytes, ContainerIndex index) {
        return std::make_unique<IndexedDemuxer>(std::move(bytes), std::move(index));
    }

    std::optional<ContainerKind> Demuxer::sniff(std::span<const std::byte> header) noexcept {
        if (header.size() >= 4 && static_cast<u8>(header[0]) == 0x1A && static_cast<u8>(header[1]) == 0x45 && static_cast<u8>(header[2]) == 0xDF && static_cast<u8>(header[3]) == 0xA3) {
            return ContainerKind::Matroska;
        }
        // ISO base media starts with a box whose type is a known leading box.
        if (starts_with(header, 4, "ftyp") || starts_with(header, 4, "moov") || starts_with(header, 4, "styp") || starts_with(header, 4, "mdat") ||
            starts_with(header, 4, "free") || starts_with(header, 4, "wide")) {
            return ContainerKind::Mp4;
        }
        return std::nullopt;
    }

    std::expected<std::unique_ptr<Demuxer>, UString> Demuxer::open(EncodedBytes bytes) {
        if (!bytes || bytes->empty()) {
            return std::unexpected("audio: the file is empty");
        }
        const std::span<const std::byte> file(bytes->data(), bytes->size());
        const auto kind = sniff(file.first(std::min<usize>(file.size(), 16)));
        if (!kind) {
            return std::unexpected("audio: not a Matroska/WebM or MP4 container");
        }
        auto index = *kind == ContainerKind::Matroska ? parse_matroska(file) : parse_mp4(file);
        if (!index) {
            return std::unexpected(std::move(index.error()));
        }
        if (index->tracks.empty()) {
            return std::unexpected("audio: the container has no audio track");
        }
        return make_indexed_demuxer(std::move(bytes), std::move(*index));
    }

    std::expected<std::unique_ptr<Demuxer>, UString> Demuxer::open_file(const std::filesystem::path &path) {
        auto bytes = read_file_bytes(path, Foundation::Io::AccessHint::Sequential);
        if (!bytes) {
            return std::unexpected(std::move(bytes.error()));
        }
        return open(std::move(*bytes));
    }

} // namespace SFT::Audio
