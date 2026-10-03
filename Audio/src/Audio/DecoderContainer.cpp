// Decoding the audio of Matroska/WebM and MP4 files: the demuxer's packets are re-wrapped as the codec's own file format (Ogg
// for Opus and Vorbis, ADTS for AAC, a bare stream for FLAC and MP3, WAV for PCM) and handed to the decoder registry, so every
// codec the engine can read from a plain file works inside a container too, with its seeking and metadata. The re-wrapped
// stream lives in memory; code that must stream a movie larger than RAM drives `Demuxer` itself.

#include <Audio/Demux.hpp>
#include <Audio/Text.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace SFT::Audio {

    namespace {
        using Bytes = std::vector<std::byte>;

        void put(Bytes &out, std::span<const std::byte> data) { out.insert(out.end(), data.begin(), data.end()); }
        void put(Bytes &out, const std::vector<u8> &data) {
            for (const u8 b : data) out.push_back(static_cast<std::byte>(b));
        }
        void put_le(Bytes &out, u64 value, int bytes) {
            for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
        }
        void put_text(Bytes &out, const char *text) {
            for (; *text != 0; ++text) out.push_back(static_cast<std::byte>(*text));
        }

        // ---- Ogg ----------------------------------------------------------------------------------------------------------

        u32 ogg_crc(std::span<const u8> page) {
            static const std::array<u32, 256> table = [] {
                std::array<u32, 256> t{};
                for (u32 i = 0; i < 256; ++i) {
                    u32 r = i << 24;
                    for (int k = 0; k < 8; ++k) r = (r & 0x80000000u) != 0 ? (r << 1) ^ 0x04C11DB7u : r << 1;
                    t[i] = r;
                }
                return t;
            }();
            u32 crc = 0;
            for (const u8 b : page) crc = (crc << 8) ^ table[((crc >> 24) & 0xFF) ^ b];
            return crc;
        }

        /// A minimal Ogg muxer: one logical stream, every packet is flushed on its own page (or pages, for big packets).
        class OggWriter {
          public:
            explicit OggWriter(u32 serial) : serial_(serial) {}

            /// Adds a packet; `granule` is the stream position after it (-1 for header packets), `eos` marks the last one.
            void packet(std::span<const u8> data, i64 granule, bool eos = false) {
                usize offset = 0;
                bool first_page = true;
                do {
                    // 255 segments per page; a packet that needs more continues on the next one.
                    const usize segments_needed = data.size() - offset == 0 ? 1 : (data.size() - offset) / 255 + 1;
                    const usize segments = std::min<usize>(segments_needed, 255);
                    const usize bytes = std::min<usize>(data.size() - offset, segments * 255);
                    const bool finishes = offset + bytes >= data.size() && (segments_needed <= 255);
                    std::vector<u8> page = {'O', 'g', 'g', 'S', 0, 0}; // magic, version, header type
                    u8 flags = 0;
                    if (!first_page) flags |= 0x01;
                    if (!started_ && first_page) flags |= 0x02;
                    if (eos && finishes) flags |= 0x04;
                    page[5] = flags;
                    const u64 granule_field = finishes ? static_cast<u64>(granule) : ~0ull;
                    for (int i = 0; i < 8; ++i) page.push_back(static_cast<u8>(granule_field >> (8 * i)));
                    for (int i = 0; i < 4; ++i) page.push_back(static_cast<u8>(serial_ >> (8 * i)));
                    for (int i = 0; i < 4; ++i) page.push_back(static_cast<u8>(sequence_ >> (8 * i)));
                    const usize crc_at = page.size();
                    page.insert(page.end(), 4, 0);
                    page.push_back(static_cast<u8>(segments));
                    usize left = bytes;
                    for (usize s = 0; s < segments; ++s) {
                        const usize lace = std::min<usize>(left, 255);
                        // A packet ending exactly on a 255 boundary needs a zero-length final segment.
                        page.push_back(static_cast<u8>(lace));
                        left -= lace;
                    }
                    page.insert(page.end(), data.begin() + static_cast<std::ptrdiff_t>(offset), data.begin() + static_cast<std::ptrdiff_t>(offset + bytes));
                    const u32 crc = ogg_crc(page);
                    for (int i = 0; i < 4; ++i) page[crc_at + i] = static_cast<u8>(crc >> (8 * i));
                    put(out_, std::span<const std::byte>(reinterpret_cast<const std::byte *>(page.data()), page.size()));
                    ++sequence_;
                    started_ = true;
                    first_page = false;
                    offset += bytes;
                } while (offset < data.size());
            }
            [[nodiscard]] Bytes take() { return std::move(out_); }

          private:
            u32 serial_;
            u32 sequence_ = 0;
            bool started_ = false;
            Bytes out_;
        };

        /// Samples (at 48 kHz) in one Opus packet, from its table-of-contents byte (RFC 6716 section 3.1).
        u32 opus_packet_samples(std::span<const std::byte> packet) {
            if (packet.empty()) return 0;
            const u8 toc = static_cast<u8>(packet[0]);
            const u32 config = toc >> 3;
            u32 frame;
            if (config < 12) frame = std::array<u32, 4>{480, 960, 1920, 2880}[config % 4];
            else if (config < 16) frame = (config % 2) == 0 ? 480 : 960;
            else frame = std::array<u32, 4>{120, 240, 480, 960}[config % 4];
            const u32 code = toc & 3;
            if (code == 0) return frame;
            if (code != 3) return frame * 2;
            return packet.size() > 1 ? frame * (static_cast<u8>(packet[1]) & 0x3F) : 0;
        }

        struct Rewrapped {
            Bytes bytes;
            UString extension;
        };

        std::expected<Rewrapped, UString> rewrap_opus(Demuxer &demuxer, const DemuxTrack &track) {
            std::vector<u8> head = track.codec_private;
            if (head.size() < 19 || std::memcmp(head.data(), "OpusHead", 8) != 0) {
                return std::unexpected("audio: the Opus track has no OpusHead setup data");
            }
            const u32 pre_skip = static_cast<u32>(head[10]) | (static_cast<u32>(head[11]) << 8);
            OggWriter ogg(0x53545552);
            ogg.packet(head, 0);
            std::vector<u8> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 6, 0, 0, 0, 'S', 't', 'u', 'r', 'd', 'y', 0, 0, 0, 0};
            ogg.packet(tags, 0);
            // One packet is held back so the last can carry the end-of-stream flag.
            std::optional<DemuxPacket> held;
            i64 position = static_cast<i64>(pre_skip);
            i64 held_end = 0;
            const auto flush = [&](bool eos) {
                if (!held) return;
                ogg.packet(std::span<const u8>(reinterpret_cast<const u8 *>(held->data.data()), held->data.size()), held_end, eos);
            };
            while (auto packet = demuxer.next(track.id)) {
                flush(false);
                position += static_cast<i64>(opus_packet_samples(packet->data));
                held = packet;
                held_end = position;
            }
            if (!held) return std::unexpected("audio: the Opus track has no packets");
            flush(true);
            return Rewrapped{ogg.take(), ".opus"};
        }

        std::expected<Rewrapped, UString> rewrap_vorbis(Demuxer &demuxer, const DemuxTrack &track) {
            // CodecPrivate: count-1, the sizes of the first two headers (Xiph lacing), then the three header packets.
            const std::vector<u8> &p = track.codec_private;
            if (p.size() < 3 || p[0] != 2) return std::unexpected("audio: the Vorbis track has no usable header packets");
            usize pos = 1;
            std::array<usize, 2> sizes{};
            for (usize &size : sizes) {
                while (pos < p.size() && p[pos] == 255) size += p[pos++];
                if (pos >= p.size()) return std::unexpected("audio: the Vorbis header sizes are damaged");
                size += p[pos++];
            }
            if (pos + sizes[0] + sizes[1] > p.size()) return std::unexpected("audio: the Vorbis headers are truncated");
            OggWriter ogg(0x53545556);
            const std::span<const u8> all(p);
            ogg.packet(all.subspan(pos, sizes[0]), 0);
            ogg.packet(all.subspan(pos + sizes[0], sizes[1]), 0);
            ogg.packet(all.subspan(pos + sizes[0] + sizes[1]), 0);
            std::optional<DemuxPacket> held;
            const f64 rate = track.sample_rate;
            const auto flush = [&](f64 end_seconds, bool eos) {
                if (!held) return;
                ogg.packet(std::span<const u8>(reinterpret_cast<const u8 *>(held->data.data()), held->data.size()), static_cast<i64>(std::llround(end_seconds * rate)), eos);
            };
            while (auto packet = demuxer.next(track.id)) {
                flush(packet->pts_seconds, false);
                held = packet;
            }
            if (!held) return std::unexpected("audio: the Vorbis track has no packets");
            flush(held->pts_seconds + std::max(held->duration_seconds, 0.0), true);
            return Rewrapped{ogg.take(), ".ogg"};
        }

        // ---- the rest -----------------------------------------------------------------------------------------------------

        std::expected<Rewrapped, UString> rewrap_aac(Demuxer &demuxer, const DemuxTrack &track) {
            if (track.codec_private.size() < 2) return std::unexpected("audio: the AAC track has no AudioSpecificConfig");
            const u8 object_type = track.codec_private[0] >> 3;
            const u8 frequency_index = static_cast<u8>(((track.codec_private[0] & 7) << 1) | (track.codec_private[1] >> 7));
            const u8 channel_config = (track.codec_private[1] >> 3) & 0x0F;
            Bytes out;
            while (auto packet = demuxer.next(track.id)) {
                const u32 length = static_cast<u32>(packet->data.size()) + 7;
                const std::array<u8, 7> header = {
                    0xFF, 0xF1, static_cast<u8>(((std::max<u8>(object_type, 1) - 1) << 6) | (frequency_index << 2) | (channel_config >> 2)),
                    static_cast<u8>(((channel_config & 3) << 6) | (length >> 11)), static_cast<u8>((length >> 3) & 0xFF), static_cast<u8>(((length & 7) << 5) | 0x1F), 0xFC};
                for (const u8 b : header) out.push_back(static_cast<std::byte>(b));
                put(out, packet->data);
            }
            if (out.empty()) return std::unexpected("audio: the AAC track has no packets");
            return Rewrapped{std::move(out), ".aac"};
        }

        std::expected<Rewrapped, UString> rewrap_raw(Demuxer &demuxer, const DemuxTrack &track, const std::vector<u8> &prefix, const char *extension) {
            Bytes out;
            put(out, prefix);
            while (auto packet = demuxer.next(track.id)) put(out, packet->data);
            if (out.size() <= prefix.size()) return std::unexpected("audio: the track has no packets");
            return Rewrapped{std::move(out), UString{extension}};
        }

        std::expected<Rewrapped, UString> rewrap_pcm(Demuxer &demuxer, const DemuxTrack &track) {
            // "pcm_s16le", "pcm_s24be", "pcm_f32le": bits, float or integer, endianness.
            const std::string_view name = track.codec.cpp_string_view();
            const bool is_float = name.find("pcm_f") == 0;
            const bool big = name.size() >= 2 && name.substr(name.size() - 2) == "be";
            const u32 bits = static_cast<u32>(std::atoi(std::string(name.substr(5, name.size() - 7)).c_str()));
            if ((bits != 16 && bits != 24 && bits != 32 && !(is_float && bits == 64)) || track.channels == 0) {
                return std::unexpected(UString{std::format("audio: PCM of {} is not supported", name)});
            }
            const u32 width = bits / 8;
            Bytes samples;
            while (auto packet = demuxer.next(track.id)) {
                const usize start = samples.size();
                put(samples, packet->data);
                if (big) {
                    for (usize i = start; i + width <= samples.size(); i += width) std::reverse(samples.begin() + static_cast<std::ptrdiff_t>(i), samples.begin() + static_cast<std::ptrdiff_t>(i + width));
                }
            }
            if (samples.empty()) return std::unexpected("audio: the PCM track has no data");
            Bytes wav;
            put_text(wav, "RIFF");
            put_le(wav, 36 + samples.size(), 4);
            put_text(wav, "WAVEfmt ");
            put_le(wav, 16, 4);
            put_le(wav, is_float ? 3 : 1, 2);
            put_le(wav, track.channels, 2);
            put_le(wav, track.sample_rate, 4);
            put_le(wav, static_cast<u64>(track.sample_rate) * track.channels * width, 4);
            put_le(wav, static_cast<u64>(track.channels) * width, 2);
            put_le(wav, bits, 2);
            put_text(wav, "data");
            put_le(wav, samples.size(), 4);
            wav.insert(wav.end(), samples.begin(), samples.end());
            return Rewrapped{std::move(wav), ".wav"};
        }

        std::expected<Rewrapped, UString> rewrap(Demuxer &demuxer, const DemuxTrack &track) {
            const std::string_view codec = track.codec.cpp_string_view();
            if (codec == "opus") return rewrap_opus(demuxer, track);
            if (codec == "vorbis") return rewrap_vorbis(demuxer, track);
            if (codec == "aac") return rewrap_aac(demuxer, track);
            if (codec == "mp3" || codec == "mp2") return rewrap_raw(demuxer, track, {}, ".mp3");
            if (codec == "flac") {
                std::vector<u8> prefix = track.codec_private;
                if (prefix.size() < 4 || std::memcmp(prefix.data(), "fLaC", 4) != 0) prefix.insert(prefix.begin(), {'f', 'L', 'a', 'C'});
                return rewrap_raw(demuxer, track, prefix, ".flac");
            }
            if (codec.rfind("pcm_", 0) == 0) return rewrap_pcm(demuxer, track);
            return std::unexpected(UString{std::format("audio: the {} audio in this container needs a decoder this build does not have", codec)});
        }

        class ContainerBackend final : public DecoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "Container (Matroska/WebM/MP4)"_ustr; }
            [[nodiscard]] std::vector<UString> extensions() const override { return {".mkv", ".mka", ".webm", ".mp4", ".m4a", ".m4b", ".mov"}; }
            [[nodiscard]] int probe(std::span<const std::byte> header, const UString &extension) const override {
                if (const auto kind = Demuxer::sniff(header)) {
                    // The OS decoders know AAC in M4A better than this does; this one wins everything they cannot do.
                    return *kind == ContainerKind::Matroska ? 80 : 35;
                }
                (void)extension;
                return 0;
            }
            [[nodiscard]] std::unique_ptr<StreamDecoder> open(EncodedBytes bytes) const override {
                auto demuxer = Demuxer::open(bytes);
                if (!demuxer) return nullptr;
                // The first track the engine can decode.
                for (const DemuxTrack &track : (*demuxer)->tracks()) {
                    (*demuxer)->seek(0.0);
                    auto wrapped = rewrap(**demuxer, track);
                    if (!wrapped) continue;
                    auto blob = Foundation::Io::ByteBlob::from_vector(std::move(wrapped->bytes));
                    if (auto decoder = DecoderRegistry::global().open(std::move(blob), wrapped->extension)) {
                        return std::move(*decoder);
                    }
                }
                return nullptr;
            }
        };
    } // namespace

    std::unique_ptr<DecoderBackend> make_container_backend() { return std::make_unique<ContainerBackend>(); }

} // namespace SFT::Audio
