// ISO base media (MP4 / M4A / MOV), progressive (moov sample tables) and fragmented (moof/traf/trun). Only the structure is read.

#include <Audio/Demux.hpp>
#include <Audio/Text.hpp>

#include <algorithm>
#include <cstring>
#include <map>

namespace SFT::Audio {

    namespace {
        constexpr u32 fourcc(const char (&s)[5]) { return (static_cast<u32>(static_cast<u8>(s[0])) << 24) | (static_cast<u32>(static_cast<u8>(s[1])) << 16) | (static_cast<u32>(static_cast<u8>(s[2])) << 8) | static_cast<u8>(s[3]); }

        struct Reader {
            std::span<const std::byte> data;
            usize pos = 0;
            [[nodiscard]] bool has(usize n) const noexcept { return pos + n <= data.size(); }
            u8 u8_() { return has(1) ? static_cast<u8>(data[pos++]) : (pos = data.size(), u8{0}); }
            u16 u16_() { const u16 hi = u8_(); return static_cast<u16>((hi << 8) | u8_()); }
            u32 u32_() { const u32 hi = u16_(); return (hi << 16) | u16_(); }
            u64 u64_() { const u64 hi = u32_(); return (hi << 32) | u32_(); }
            void skip(usize n) { pos = std::min(data.size(), pos + n); }
        };

        struct Box {
            u32 type = 0;
            usize start = 0;   ///< header start
            usize begin = 0;   ///< payload start
            usize end = 0;     ///< payload end
        };

        /// The boxes directly inside [begin, end).
        std::vector<Box> boxes_in(std::span<const std::byte> file, usize begin, usize end) {
            std::vector<Box> out;
            usize pos = begin;
            end = std::min(end, file.size());
            while (pos + 8 <= end) {
                Reader r{file, pos};
                u64 size = r.u32_();
                const u32 type = r.u32_();
                usize header = 8;
                if (size == 1) {
                    size = r.u64_();
                    header = 16;
                } else if (size == 0) {
                    size = end - pos;
                }
                if (size < header || pos + size > end) {
                    size = end - pos; // a truncated final box: take what is there
                    if (size < header) break;
                }
                out.push_back(Box{type, pos, pos + header, static_cast<usize>(pos + size)});
                pos += size;
            }
            return out;
        }

        const Box *find(const std::vector<Box> &boxes, u32 type) {
            const auto it = std::ranges::find(boxes, type, &Box::type);
            return it == boxes.end() ? nullptr : &*it;
        }

        struct TrackState {
            DemuxTrack track;
            u32 timescale = 1000;
            std::vector<u8> sample_codec_hint;
            u32 trex_duration = 0, trex_size = 0;
            bool pcm = false;
        };

        /// MPEG-4 descriptor length: 7 bits per byte, high bit means more follow.
        u32 descriptor_length(Reader &r) {
            u32 length = 0;
            for (int i = 0; i < 4; ++i) {
                const u8 b = r.u8_();
                length = (length << 7) | (b & 0x7F);
                if ((b & 0x80) == 0) break;
            }
            return length;
        }

        /// esds: the codec's object type and, for AAC, the AudioSpecificConfig.
        void parse_esds(std::span<const std::byte> payload, DemuxTrack &track) {
            Reader r{payload, 4}; // version + flags
            if (r.u8_() != 0x03) return;
            (void)descriptor_length(r);
            r.skip(2); // ES_ID
            const u8 flags = r.u8_();
            if (flags & 0x80) r.skip(2);
            if (flags & 0x40) r.skip(r.u8_());
            if (flags & 0x20) r.skip(2);
            if (r.u8_() != 0x04) return;
            (void)descriptor_length(r);
            const u8 object_type = r.u8_();
            r.skip(12); // stream type, buffer size, bitrates
            if (object_type == 0x40 || object_type == 0x66 || object_type == 0x67 || object_type == 0x68) track.codec = "aac";
            else if (object_type == 0x69 || object_type == 0x6B) track.codec = "mp3";
            else if (object_type == 0xA5) track.codec = "ac3";
            else if (object_type == 0xA6) track.codec = "eac3";
            else if (object_type == 0xDD) track.codec = "vorbis";
            if (r.u8_() == 0x05) {
                const u32 length = descriptor_length(r);
                if (r.has(length)) {
                    track.codec_private.assign(reinterpret_cast<const u8 *>(payload.data()) + r.pos, reinterpret_cast<const u8 *>(payload.data()) + r.pos + length);
                }
            }
        }

        /// dOps (big endian, no magic) as the OpusHead a decoder expects, so both containers hand over the same thing.
        std::vector<u8> opus_head_from_dops(std::span<const std::byte> payload) {
            Reader r{payload, 0};
            r.skip(1); // version
            const u8 channels = r.u8_();
            const u16 pre_skip = r.u16_();
            const u32 rate = r.u32_();
            const i16 gain = static_cast<i16>(r.u16_());
            const u8 family = r.u8_();
            std::vector<u8> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, channels};
            const auto le16 = [&](u16 v) { head.push_back(static_cast<u8>(v)); head.push_back(static_cast<u8>(v >> 8)); };
            le16(pre_skip);
            for (int i = 0; i < 4; ++i) head.push_back(static_cast<u8>(rate >> (8 * i)));
            le16(static_cast<u16>(gain));
            head.push_back(family);
            if (family != 0) {
                while (r.has(1) && head.size() < 21 + channels) head.push_back(r.u8_());
            }
            return head;
        }

        void parse_sample_entry(std::span<const std::byte> file, const Box &entry, TrackState &state) {
            DemuxTrack &track = state.track;
            Reader r{file, entry.begin};
            r.skip(6 + 2); // reserved, data reference index
            const u16 version = r.u16_();
            r.skip(2 + 4); // revision, vendor
            track.channels = r.u16_();
            track.bit_depth = r.u16_();
            r.skip(2 + 2);
            track.sample_rate = r.u32_() >> 16;
            if (version == 1) r.skip(16);
            const u32 type = entry.type;
            const auto bits = [&] { return std::to_string(track.bit_depth ? track.bit_depth : 16); };
            if (type == fourcc("Opus")) track.codec = "opus";
            else if (type == fourcc("fLaC")) track.codec = "flac";
            else if (type == fourcc("alac")) track.codec = "alac";
            else if (type == fourcc("ac-3")) track.codec = "ac3";
            else if (type == fourcc("ec-3")) track.codec = "eac3";
            else if (type == fourcc(".mp3")) track.codec = "mp3";
            else if (type == fourcc("sowt")) { track.codec = text_from_bytes("pcm_s" + bits() + "le"); state.pcm = true; }
            else if (type == fourcc("twos")) { track.codec = text_from_bytes("pcm_s" + bits() + "be"); state.pcm = true; }
            else if (type == fourcc("in24")) { track.codec = "pcm_s24be"; state.pcm = true; track.bit_depth = 24; }
            else if (type == fourcc("in32")) { track.codec = "pcm_s32be"; state.pcm = true; track.bit_depth = 32; }
            else if (type == fourcc("fl32")) { track.codec = "pcm_f32be"; state.pcm = true; track.bit_depth = 32; }
            else if (type == fourcc("mp4a")) track.codec = "aac"; // refined by the esds object type
            else {
                char name[5] = {static_cast<char>(type >> 24), static_cast<char>(type >> 16), static_cast<char>(type >> 8), static_cast<char>(type), 0};
                track.codec = text_from_bytes(name);
            }
            for (const Box &child : boxes_in(file, r.pos, entry.end)) {
                const auto payload = file.subspan(child.begin, child.end - child.begin);
                if (child.type == fourcc("esds")) {
                    parse_esds(payload, track);
                } else if (child.type == fourcc("dOps")) {
                    track.codec_private = opus_head_from_dops(payload);
                    if (track.codec_private.size() > 9) track.channels = track.codec_private[9];
                    track.sample_rate = 48000;
                } else if (child.type == fourcc("dfLa")) {
                    track.codec_private = {'f', 'L', 'a', 'C'};
                    track.codec_private.insert(track.codec_private.end(), reinterpret_cast<const u8 *>(payload.data()) + 4, reinterpret_cast<const u8 *>(payload.data()) + payload.size());
                } else if (child.type == fourcc("alac")) {
                    track.codec_private.assign(reinterpret_cast<const u8 *>(payload.data()) + std::min<usize>(4, payload.size()), reinterpret_cast<const u8 *>(payload.data()) + payload.size());
                } else if (child.type == fourcc("wave")) { // QuickTime wraps the real setup one level down
                    for (const Box &inner : boxes_in(file, child.begin, child.end)) {
                        if (inner.type == fourcc("esds")) parse_esds(file.subspan(inner.begin, inner.end - inner.begin), track);
                    }
                }
            }
        }

        struct Fragmenter {
            std::span<const std::byte> file;
            ContainerIndex &index;
            std::map<u32, TrackState> &tracks;
            std::map<u32, i64> next_dts; // continues across fragments when a traf has no tfdt

            void parse_moof(const Box &moof) {
                for (const Box &traf : boxes_in(file, moof.begin, moof.end)) {
                    if (traf.type != fourcc("traf")) continue;
                    parse_traf(moof, traf);
                }
            }

            void parse_traf(const Box &moof, const Box &traf) {
                const auto children = boxes_in(file, traf.begin, traf.end);
                const Box *tfhd = find(children, fourcc("tfhd"));
                if (tfhd == nullptr) return;
                Reader h{file, tfhd->begin};
                const u32 versioned_flags = h.u32_();
                const u32 flags = versioned_flags & 0xFFFFFF;
                const u32 track_id = h.u32_();
                const auto state = tracks.find(track_id);
                if (state == tracks.end()) return; // not an audio track we know
                u64 base = moof.start; // default-base-is-moof, and the only sensible default for fragments
                if (flags & 0x01) base = h.u64_();
                if (flags & 0x02) h.skip(4);
                u32 default_duration = state->second.trex_duration, default_size = state->second.trex_size;
                if (flags & 0x08) default_duration = h.u32_();
                if (flags & 0x10) default_size = h.u32_();
                i64 dts = next_dts.contains(track_id) ? next_dts[track_id] : 0;
                if (const Box *tfdt = find(children, fourcc("tfdt"))) {
                    Reader t{file, tfdt->begin};
                    const u8 version = static_cast<u8>(t.u32_() >> 24);
                    dts = static_cast<i64>(version == 1 ? t.u64_() : t.u32_());
                }
                for (const Box &trun : children) {
                    if (trun.type != fourcc("trun")) continue;
                    Reader r{file, trun.begin};
                    const u32 trun_flags = r.u32_() & 0xFFFFFF;
                    const u32 count = r.u32_();
                    i64 data_offset = 0;
                    if (trun_flags & 0x01) data_offset = static_cast<i32>(r.u32_());
                    if (trun_flags & 0x04) r.skip(4);
                    u64 offset = base + static_cast<u64>(data_offset);
                    for (u32 i = 0; i < count && r.has(1); ++i) {
                        const u32 duration = (trun_flags & 0x100) ? r.u32_() : default_duration;
                        const u32 size = (trun_flags & 0x200) ? r.u32_() : default_size;
                        if (trun_flags & 0x400) r.skip(4);
                        if (trun_flags & 0x800) r.skip(4);
                        if (offset + size > file.size()) return;
                        index.packets.push_back(PacketRef{offset, size, track_id, dts, duration, true});
                        offset += size;
                        dts += duration;
                    }
                }
                next_dts[track_id] = dts;
            }
        };

        void add_sample_table(std::span<const std::byte> file, const Box &stbl, TrackState &state, ContainerIndex &index) {
            const auto boxes = boxes_in(file, stbl.begin, stbl.end);
            const Box *stsd = find(boxes, fourcc("stsd")), *stts = find(boxes, fourcc("stts")), *stsc = find(boxes, fourcc("stsc"));
            const Box *stsz = find(boxes, fourcc("stsz")), *stco = find(boxes, fourcc("stco")), *co64 = find(boxes, fourcc("co64"));
            if (stsd != nullptr) {
                Reader r{file, stsd->begin};
                r.skip(4);
                const u32 entries = r.u32_();
                if (entries > 0) {
                    const auto first = boxes_in(file, r.pos, stsd->end);
                    if (!first.empty()) parse_sample_entry(file, first.front(), state);
                }
            }
            if (stts == nullptr || stsc == nullptr || stsz == nullptr || (stco == nullptr && co64 == nullptr)) return; // fragmented: samples come from moof
            std::vector<std::pair<u32, u32>> time_runs;
            {
                Reader r{file, stts->begin};
                r.skip(4);
                const u32 n = r.u32_();
                for (u32 i = 0; i < n && r.has(8); ++i) { const u32 count = r.u32_(); time_runs.emplace_back(count, r.u32_()); }
            }
            struct ChunkRun { u32 first_chunk, samples; };
            std::vector<ChunkRun> chunk_runs;
            {
                Reader r{file, stsc->begin};
                r.skip(4);
                const u32 n = r.u32_();
                for (u32 i = 0; i < n && r.has(12); ++i) { const u32 first = r.u32_(); const u32 samples = r.u32_(); r.skip(4); chunk_runs.push_back({first, samples}); }
            }
            u32 constant_size = 0, sample_count = 0;
            std::vector<u32> sizes;
            {
                Reader r{file, stsz->begin};
                r.skip(4);
                constant_size = r.u32_();
                sample_count = r.u32_();
                if (constant_size == 0) {
                    for (u32 i = 0; i < sample_count && r.has(4); ++i) sizes.push_back(r.u32_());
                }
            }
            std::vector<u64> chunk_offsets;
            {
                const Box &box = stco != nullptr ? *stco : *co64;
                Reader r{file, box.begin};
                r.skip(4);
                const u32 n = r.u32_();
                for (u32 i = 0; i < n && r.has(stco != nullptr ? 4 : 8); ++i) chunk_offsets.push_back(stco != nullptr ? r.u32_() : r.u64_());
            }
            // Walk chunks, assigning samples; time comes from the run-length coded deltas.
            usize run = 0;
            u32 run_left = time_runs.empty() ? 0 : time_runs[0].first;
            auto next_delta = [&]() -> u32 {
                while (run < time_runs.size() && run_left == 0) {
                    ++run;
                    run_left = run < time_runs.size() ? time_runs[run].first : 0;
                }
                if (run >= time_runs.size()) return 0;
                --run_left;
                return time_runs[run].second;
            };
            i64 dts = 0;
            u32 sample = 0;
            const bool whole_chunks = state.pcm && constant_size != 0; // PCM "samples" are single frames: one packet per chunk instead
            for (usize chunk = 0; chunk < chunk_offsets.size() && sample < sample_count; ++chunk) {
                u32 in_chunk = chunk_runs.empty() ? 1 : chunk_runs.front().samples;
                for (const ChunkRun &cr : chunk_runs) {
                    if (cr.first_chunk <= chunk + 1) in_chunk = cr.samples;
                }
                in_chunk = std::min(in_chunk, sample_count - sample);
                u64 offset = chunk_offsets[chunk];
                if (whole_chunks) {
                    u32 duration = 0;
                    for (u32 i = 0; i < in_chunk; ++i) duration += next_delta();
                    const u64 bytes = static_cast<u64>(in_chunk) * constant_size;
                    if (offset + bytes <= file.size()) index.packets.push_back(PacketRef{offset, static_cast<u32>(bytes), state.track.id, dts, duration, true});
                    dts += duration;
                    sample += in_chunk;
                    continue;
                }
                for (u32 i = 0; i < in_chunk; ++i, ++sample) {
                    const u32 size = constant_size != 0 ? constant_size : sizes[sample];
                    const u32 duration = next_delta();
                    if (offset + size <= file.size()) index.packets.push_back(PacketRef{offset, size, state.track.id, dts, duration, true});
                    offset += size;
                    dts += duration;
                }
            }
        }
    } // namespace

    std::expected<ContainerIndex, UString> parse_mp4(std::span<const std::byte> file) {
        ContainerIndex index;
        index.kind = ContainerKind::Mp4;
        std::map<u32, TrackState> tracks;
        std::vector<Box> moofs;
        f64 movie_duration = 0.0;
        bool saw_moov = false;
        for (const Box &top : boxes_in(file, 0, file.size())) {
            if (top.type == fourcc("moof")) {
                moofs.push_back(top);
                continue;
            }
            if (top.type != fourcc("moov")) continue;
            saw_moov = true;
            const auto moov = boxes_in(file, top.begin, top.end);
            if (const Box *mvhd = find(moov, fourcc("mvhd"))) {
                Reader r{file, mvhd->begin};
                const u8 version = static_cast<u8>(r.u32_() >> 24);
                const u32 scale = version == 1 ? (r.skip(16), r.u32_()) : (r.skip(8), r.u32_());
                const u64 duration = version == 1 ? r.u64_() : r.u32_();
                if (scale != 0) movie_duration = static_cast<f64>(duration) / scale;
            }
            std::map<u32, std::pair<u32, u32>> trex; // track -> default duration, size
            if (const Box *mvex = find(moov, fourcc("mvex"))) {
                for (const Box &b : boxes_in(file, mvex->begin, mvex->end)) {
                    if (b.type != fourcc("trex")) continue;
                    Reader r{file, b.begin};
                    r.skip(4);
                    const u32 id = r.u32_();
                    r.skip(4);
                    const u32 duration = r.u32_(), size = r.u32_();
                    trex[id] = {duration, size};
                }
            }
            for (const Box &trak : moov) {
                if (trak.type != fourcc("trak")) continue;
                const auto parts = boxes_in(file, trak.begin, trak.end);
                const Box *tkhd = find(parts, fourcc("tkhd")), *mdia = find(parts, fourcc("mdia"));
                if (tkhd == nullptr || mdia == nullptr) continue;
                const auto media = boxes_in(file, mdia->begin, mdia->end);
                const Box *hdlr = find(media, fourcc("hdlr")), *mdhd = find(media, fourcc("mdhd")), *minf = find(media, fourcc("minf"));
                if (hdlr == nullptr || mdhd == nullptr || minf == nullptr) continue;
                Reader handler{file, hdlr->begin};
                handler.skip(8);
                if (handler.u32_() != fourcc("soun")) continue;
                TrackState state;
                {
                    Reader r{file, tkhd->begin};
                    const u8 version = static_cast<u8>(r.u32_() >> 24);
                    r.skip(version == 1 ? 16 : 8);
                    state.track.id = r.u32_();
                }
                {
                    Reader r{file, mdhd->begin};
                    const u8 version = static_cast<u8>(r.u32_() >> 24);
                    r.skip(version == 1 ? 16 : 8);
                    state.timescale = std::max<u32>(r.u32_(), 1u);
                    const u64 duration = version == 1 ? r.u64_() : r.u32_();
                    state.track.duration_seconds = static_cast<f64>(duration) / state.timescale;
                    const u16 language = r.u16_();
                    if (language != 0) {
                        const char code[4] = {static_cast<char>(((language >> 10) & 0x1F) + 0x60), static_cast<char>(((language >> 5) & 0x1F) + 0x60), static_cast<char>((language & 0x1F) + 0x60), 0};
                        state.track.language = text_from_bytes(code);
                    }
                }
                if (const auto it = trex.find(state.track.id); it != trex.end()) {
                    state.trex_duration = it->second.first;
                    state.trex_size = it->second.second;
                }
                if (const Box *stbl = find(boxes_in(file, minf->begin, minf->end), fourcc("stbl"))) {
                    add_sample_table(file, *stbl, state, index);
                }
                tracks.emplace(state.track.id, std::move(state));
            }
        }
        if (!saw_moov) {
            return std::unexpected("audio: the MP4 file has no moov box (an unfinished or truncated recording)");
        }
        Fragmenter fragmenter{file, index, tracks, {}};
        for (const Box &moof : moofs) fragmenter.parse_moof(moof);
        for (auto &[id, state] : tracks) {
            index.tracks.push_back(std::move(state.track));
            index.tick_seconds.push_back(1.0 / state.timescale);
        }
        f64 last = 0.0;
        for (const PacketRef &p : index.packets) {
            const auto slot = std::ranges::find(index.tracks, p.track, &DemuxTrack::id);
            if (slot != index.tracks.end()) last = std::max(last, (static_cast<f64>(p.pts_ticks) + p.duration_ticks) * index.tick_seconds[static_cast<usize>(slot - index.tracks.begin())]);
        }
        index.duration_seconds = std::max(movie_duration, last);
        for (DemuxTrack &track : index.tracks) {
            if (track.duration_seconds <= 0.0) track.duration_seconds = index.duration_seconds;
        }
        return index;
    }

} // namespace SFT::Audio
