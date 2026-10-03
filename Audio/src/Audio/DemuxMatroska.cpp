// Matroska / WebM: an EBML tree. Only the structure is read; block payloads stay where they are in the file.

#include <Audio/Demux.hpp>

#include <Audio/Text.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>

namespace SFT::Audio {

    namespace {
        // Element ids (with their length marker bits, as the spec writes them).
        constexpr u32 kEbml = 0x1A45DFA3, kSegment = 0x18538067, kInfo = 0x1549A966, kTracks = 0x1654AE6B, kCluster = 0x1F43B675;
        constexpr u32 kCues = 0x1C53BB6B, kTags = 0x1254C367, kChapters = 0x1043A770, kSeekHead = 0x114D9B74, kAttachments = 0x1941A469;
        constexpr u32 kTimecodeScale = 0x2AD7B1, kDuration = 0x4489;
        constexpr u32 kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackType = 0x83, kCodecId = 0x86, kCodecPrivate = 0x63A2, kLanguage = 0x22B59C;
        constexpr u32 kAudio = 0xE1, kSamplingFrequency = 0xB5, kChannels = 0x9F, kBitDepth = 0x6264, kDefaultDuration = 0x23E383;
        constexpr u32 kClusterTimecode = 0xE7, kSimpleBlock = 0xA3, kBlockGroup = 0xA0, kBlock = 0xA1, kBlockDuration = 0x9B;
        constexpr u64 kUnknownSize = ~0ull;

        class Cursor {
          public:
            Cursor(std::span<const std::byte> data, usize begin, usize end) : data_(data), pos_(begin), end_(std::min(end, data.size())) {}
            [[nodiscard]] bool done() const noexcept { return pos_ >= end_; }
            [[nodiscard]] usize pos() const noexcept { return pos_; }
            [[nodiscard]] usize end() const noexcept { return end_; }
            void seek(usize p) noexcept { pos_ = p; }

            /// A variable-length element id (marker bits kept).
            std::optional<u32> id() {
                if (done()) return std::nullopt;
                const u8 first = byte(pos_);
                if (first == 0) return std::nullopt;
                const u32 length = static_cast<u32>(std::countl_zero(first)) + 1;
                if (length > 4 || pos_ + length > end_) return std::nullopt;
                u32 value = 0;
                for (u32 i = 0; i < length; ++i) value = (value << 8) | byte(pos_ + i);
                pos_ += length;
                return value;
            }
            /// A variable-length size (marker removed); `kUnknownSize` for the all-ones pattern.
            std::optional<u64> size() {
                if (done()) return std::nullopt;
                const u8 first = byte(pos_);
                if (first == 0) return std::nullopt;
                const u32 length = static_cast<u32>(std::countl_zero(first)) + 1;
                if (pos_ + length > end_) return std::nullopt;
                u64 value = first & (0xFFu >> length);
                bool all_ones = value == (0xFFu >> length);
                for (u32 i = 1; i < length; ++i) {
                    const u8 b = byte(pos_ + i);
                    value = (value << 8) | b;
                    all_ones = all_ones && b == 0xFF;
                }
                pos_ += length;
                return all_ones ? kUnknownSize : value;
            }
            [[nodiscard]] u8 byte(usize at) const noexcept { return static_cast<u8>(data_[at]); }
            [[nodiscard]] std::span<const std::byte> data() const noexcept { return data_; }

          private:
            std::span<const std::byte> data_;
            usize pos_;
            usize end_;
        };

        u64 read_uint(std::span<const std::byte> payload) {
            u64 v = 0;
            for (const std::byte b : payload) v = (v << 8) | static_cast<u8>(b);
            return v;
        }

        f64 read_float(std::span<const std::byte> payload) {
            if (payload.size() == 4) {
                u32 bits = static_cast<u32>(read_uint(payload));
                f32 f;
                std::memcpy(&f, &bits, 4);
                return f;
            }
            if (payload.size() == 8) {
                u64 bits = read_uint(payload);
                f64 f;
                std::memcpy(&f, &bits, 8);
                return f;
            }
            return 0.0;
        }

        struct Element {
            u32 id = 0;
            usize payload_begin = 0;
            usize payload_end = 0;
            bool unknown_size = false;
        };

        /// The next element at the cursor, its payload clamped to the cursor's range.
        std::optional<Element> next_element(Cursor &c) {
            const auto id = c.id();
            if (!id) return std::nullopt;
            const auto size = c.size();
            if (!size) return std::nullopt;
            Element e;
            e.id = *id;
            e.payload_begin = c.pos();
            e.unknown_size = *size == kUnknownSize;
            e.payload_end = e.unknown_size ? c.end() : static_cast<usize>(std::min<u64>(*size, c.end() - c.pos()) + c.pos());
            return e;
        }

        bool is_top_level(u32 id) {
            return id == kCluster || id == kCues || id == kTags || id == kChapters || id == kSeekHead || id == kTracks || id == kInfo || id == kAttachments || id == kSegment;
        }

        std::string codec_name(const std::string &id, u32 bits) {
            if (id == "A_OPUS") return "opus";
            if (id == "A_VORBIS") return "vorbis";
            if (id == "A_FLAC") return "flac";
            if (id.rfind("A_AAC", 0) == 0) return "aac";
            if (id == "A_MPEG/L3") return "mp3";
            if (id == "A_MPEG/L2") return "mp2";
            if (id == "A_AC3") return "ac3";
            if (id == "A_EAC3") return "eac3";
            if (id == "A_ALAC") return "alac";
            if (id == "A_PCM/INT/LIT") return "pcm_s" + std::to_string(bits ? bits : 16) + "le";
            if (id == "A_PCM/INT/BIG") return "pcm_s" + std::to_string(bits ? bits : 16) + "be";
            if (id == "A_PCM/FLOAT/IEEE") return bits == 64 ? "pcm_f64le" : "pcm_f32le";
            return id;
        }

        struct Parser {
            std::span<const std::byte> file;
            ContainerIndex index;
            f64 timecode_scale_ns = 1000000.0;
            f64 duration_ticks = 0.0;
            std::map<u32, usize> track_slot; // Matroska track number -> position in index.tracks
            std::map<u32, i64> default_duration_ns;

            void parse_tracks(Cursor c) {
                while (!c.done()) {
                    const auto e = next_element(c);
                    if (!e) return;
                    if (e->id == kTrackEntry) parse_track_entry(Cursor(file, e->payload_begin, e->payload_end));
                    c.seek(e->payload_end);
                }
            }

            void parse_track_entry(Cursor c) {
                DemuxTrack track;
                u64 type = 0;
                std::string codec_id;
                f64 rate = 8000.0;
                i64 default_duration = 0;
                while (!c.done()) {
                    const auto e = next_element(c);
                    if (!e) break;
                    const std::span<const std::byte> payload = file.subspan(e->payload_begin, e->payload_end - e->payload_begin);
                    switch (e->id) {
                        case kTrackNumber: track.id = static_cast<u32>(read_uint(payload)); break;
                        case kTrackType: type = read_uint(payload); break;
                        case kCodecId: codec_id.assign(reinterpret_cast<const char *>(payload.data()), payload.size()); break;
                        case kCodecPrivate: track.codec_private.assign(reinterpret_cast<const u8 *>(payload.data()), reinterpret_cast<const u8 *>(payload.data()) + payload.size()); break;
                        case kLanguage: track.language = text_from_bytes(std::string_view(reinterpret_cast<const char *>(payload.data()), payload.size())); break;
                        case kDefaultDuration: default_duration = static_cast<i64>(read_uint(payload)); break;
                        case kAudio: {
                            Cursor audio(file, e->payload_begin, e->payload_end);
                            while (!audio.done()) {
                                const auto a = next_element(audio);
                                if (!a) break;
                                const auto p = file.subspan(a->payload_begin, a->payload_end - a->payload_begin);
                                if (a->id == kSamplingFrequency) rate = read_float(p);
                                else if (a->id == kChannels) track.channels = static_cast<u32>(read_uint(p));
                                else if (a->id == kBitDepth) track.bit_depth = static_cast<u32>(read_uint(p));
                                audio.seek(a->payload_end);
                            }
                            break;
                        }
                        default: break;
                    }
                    c.seek(e->payload_end);
                }
                if (type != 2) return; // 2 = audio
                track.sample_rate = static_cast<u32>(std::lround(rate));
                if (track.channels == 0) track.channels = 1;
                track.codec = text_from_bytes(codec_name(codec_id, track.bit_depth));
                track_slot[track.id] = index.tracks.size();
                default_duration_ns[track.id] = default_duration;
                index.tracks.push_back(std::move(track));
            }

            void add_block(usize begin, usize end, i64 cluster_timecode, bool simple_block, u64 block_duration_ticks, bool has_duration) {
                Cursor c(file, begin, end);
                const auto number = c.size();
                if (!number || c.pos() + 3 > end) return;
                const auto slot = track_slot.find(static_cast<u32>(*number));
                if (slot == track_slot.end()) return;
                const usize at = c.pos();
                const i16 relative = static_cast<i16>((c.byte(at) << 8) | c.byte(at + 1));
                const u8 flags = c.byte(at + 2);
                usize payload = at + 3;
                const u32 lacing = (flags >> 1) & 3;
                std::vector<u32> sizes;
                u32 frame_count = 1;
                if (lacing != 0) {
                    if (payload >= end) return;
                    frame_count = static_cast<u32>(c.byte(payload++)) + 1;
                    if (lacing == 1) { // Xiph: sizes as runs of 255
                        for (u32 i = 0; i + 1 < frame_count; ++i) {
                            u32 size = 0;
                            while (payload < end) {
                                const u8 b = c.byte(payload++);
                                size += b;
                                if (b != 255) break;
                            }
                            sizes.push_back(size);
                        }
                    } else if (lacing == 3) { // EBML: first size absolute, the rest signed deltas
                        Cursor lace(file, payload, end);
                        const auto first = lace.size();
                        if (!first) return;
                        i64 previous = static_cast<i64>(*first);
                        sizes.push_back(static_cast<u32>(previous));
                        for (u32 i = 1; i + 1 < frame_count; ++i) {
                            const usize before = lace.pos();
                            const auto raw = lace.size();
                            if (!raw) return;
                            const u32 length = static_cast<u32>(lace.pos() - before);
                            const i64 bias = (i64{1} << (7 * length - 1)) - 1;
                            previous += static_cast<i64>(*raw) - bias;
                            if (previous < 0) return;
                            sizes.push_back(static_cast<u32>(previous));
                        }
                        payload = lace.pos();
                    }
                }
                usize remaining = end > payload ? end - payload : 0;
                if (lacing == 2) {
                    sizes.assign(frame_count - 1, static_cast<u32>(remaining / frame_count));
                }
                u64 used = 0;
                for (const u32 s : sizes) used += s;
                if (used > remaining) return;
                sizes.push_back(static_cast<u32>(remaining - used));
                const i64 track_default_ns = default_duration_ns[static_cast<u32>(*number)];
                const f64 ticks_per_ns = 1.0 / timecode_scale_ns;
                u32 duration_ticks = has_duration ? static_cast<u32>(block_duration_ticks / frame_count) : static_cast<u32>(static_cast<f64>(track_default_ns) * ticks_per_ns);
                i64 pts = cluster_timecode + relative;
                usize offset = payload;
                for (const u32 size : sizes) {
                    index.packets.push_back(PacketRef{offset, size, static_cast<u32>(*number), pts, duration_ticks, !simple_block || (flags & 0x80) != 0});
                    offset += size;
                    pts += duration_ticks;
                }
            }

            void parse_cluster(usize begin, usize end, bool unknown_size) {
                Cursor c(file, begin, end);
                i64 timecode = 0;
                while (!c.done()) {
                    const usize start = c.pos();
                    const auto e = next_element(c);
                    if (!e) return;
                    if (unknown_size && is_top_level(e->id)) {
                        c.seek(start);
                        break;
                    }
                    if (e->id == kClusterTimecode) {
                        const auto payload = file.subspan(e->payload_begin, e->payload_end - e->payload_begin);
                        timecode = static_cast<i64>(read_uint(payload));
                    } else if (e->id == kSimpleBlock) {
                        add_block(e->payload_begin, e->payload_end, timecode, /*simple_block=*/true, 0, false);
                    } else if (e->id == kBlockGroup) {
                        Cursor group(file, e->payload_begin, e->payload_end);
                        usize block_begin = 0, block_end = 0;
                        u64 duration = 0;
                        bool has_duration = false;
                        while (!group.done()) {
                            const auto g = next_element(group);
                            if (!g) break;
                            if (g->id == kBlock) {
                                block_begin = g->payload_begin;
                                block_end = g->payload_end;
                            } else if (g->id == kBlockDuration) {
                                duration = read_uint(file.subspan(g->payload_begin, g->payload_end - g->payload_begin));
                                has_duration = true;
                            }
                            group.seek(g->payload_end);
                        }
                        if (block_end > block_begin) add_block(block_begin, block_end, timecode, /*simple_block=*/false, duration, has_duration);
                    }
                    c.seek(e->payload_end);
                }
                cluster_end = c.pos();
            }

            usize cluster_end = 0;
        };
    } // namespace

    std::expected<ContainerIndex, UString> parse_matroska(std::span<const std::byte> file) {
        Parser parser;
        parser.file = file;
        parser.index.kind = ContainerKind::Matroska;
        Cursor top(file, 0, file.size());
        std::optional<Element> segment;
        while (!top.done()) {
            const auto e = next_element(top);
            if (!e) break;
            if (e->id == kSegment) {
                segment = e;
                break;
            }
            top.seek(e->payload_end);
        }
        if (!segment) {
            return std::unexpected("audio: no Segment in the Matroska file");
        }
        Cursor c(file, segment->payload_begin, segment->payload_end);
        bool tracks_seen = false;
        while (!c.done()) {
            const auto e = next_element(c);
            if (!e) break;
            switch (e->id) {
                case kInfo: {
                    Cursor info(file, e->payload_begin, e->payload_end);
                    while (!info.done()) {
                        const auto i = next_element(info);
                        if (!i) break;
                        const auto p = file.subspan(i->payload_begin, i->payload_end - i->payload_begin);
                        if (i->id == kTimecodeScale) parser.timecode_scale_ns = static_cast<f64>(read_uint(p));
                        else if (i->id == kDuration) parser.duration_ticks = read_float(p);
                        info.seek(i->payload_end);
                    }
                    break;
                }
                case kTracks:
                    parser.parse_tracks(Cursor(file, e->payload_begin, e->payload_end));
                    tracks_seen = true;
                    break;
                case kCluster: {
                    if (!tracks_seen) return std::unexpected("audio: Matroska clusters before the track list");
                    parser.parse_cluster(e->payload_begin, e->payload_end, e->unknown_size);
                    if (e->unknown_size) {
                        c.seek(parser.cluster_end);
                        continue;
                    }
                    break;
                }
                default: break;
            }
            c.seek(e->payload_end);
        }
        ContainerIndex index = std::move(parser.index);
        const f64 tick = parser.timecode_scale_ns * 1e-9;
        index.tick_seconds.assign(index.tracks.size(), tick);
        f64 last = 0.0;
        for (const PacketRef &p : index.packets) {
            last = std::max(last, (static_cast<f64>(p.pts_ticks) + p.duration_ticks) * tick);
        }
        index.duration_seconds = parser.duration_ticks > 0.0 ? parser.duration_ticks * tick : last;
        for (DemuxTrack &track : index.tracks) track.duration_seconds = index.duration_seconds;
        return index;
    }

} // namespace SFT::Audio
