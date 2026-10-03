// Builds small Matroska/WebM and MP4 files by hand and checks the demuxers and the container decoder against them.

#include <Audio/Decoder.hpp>
#include <Audio/Demux.hpp>
#include <Audio/OpusCodec.hpp>

#include <cmath>
#include <cstring>
#include <iostream>
#include <numbers>

using namespace SFT::Audio;
using SFT::u16;
using SFT::u32;
using SFT::u64;
using SFT::u8;
using SFT::usize;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }

    using Bytes = std::vector<u8>;

    void append(Bytes &out, const Bytes &more) { out.insert(out.end(), more.begin(), more.end()); }
    void be(Bytes &out, u64 value, int bytes) {
        for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<u8>(value >> (8 * i)));
    }

    // ---- EBML ----
    Bytes ebml_id(u32 id) {
        Bytes out;
        int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
        be(out, id, n);
        return out;
    }
    Bytes element(u32 id, const Bytes &payload) {
        Bytes out = ebml_id(id);
        out.push_back(0x08 | 0x00); // 8-byte size marker: 0x01 followed by 7 bytes; use the 1-byte form below instead
        out.pop_back();
        out.push_back(0x01);
        be(out, payload.size(), 7);
        append(out, payload);
        return out;
    }
    Bytes uint_element(u32 id, u64 value) {
        Bytes payload;
        be(payload, value, value > 0xFFFFFF ? 4 : value > 0xFFFF ? 3 : value > 0xFF ? 2 : 1);
        return element(id, payload);
    }
    Bytes string_element(u32 id, const char *text) { return element(id, Bytes(text, text + std::strlen(text))); }
    Bytes float_element(u32 id, double value) {
        Bytes payload;
        u64 bits;
        std::memcpy(&bits, &value, 8);
        be(payload, bits, 8);
        return element(id, payload);
    }

    // frames of audio as blocks of 20 ms each; `payloads[i]` is block i's data
    Bytes make_mkv(const char *codec_id, const Bytes &codec_private, u32 channels, double rate, int bit_depth, const std::vector<Bytes> &payloads, u32 block_ms, bool simple, bool unknown_cluster_size) {
        Bytes tracks_entry;
        append(tracks_entry, uint_element(0xD7, 3));
        append(tracks_entry, uint_element(0x83, 2));
        append(tracks_entry, string_element(0x86, codec_id));
        if (!codec_private.empty()) append(tracks_entry, element(0x63A2, codec_private));
        Bytes audio;
        append(audio, float_element(0xB5, rate));
        append(audio, uint_element(0x9F, channels));
        if (bit_depth) append(audio, uint_element(0x6264, static_cast<u64>(bit_depth)));
        append(tracks_entry, element(0xE1, audio));
        // A video track too, which must be ignored.
        Bytes video_entry;
        append(video_entry, uint_element(0xD7, 1));
        append(video_entry, uint_element(0x83, 1));
        append(video_entry, string_element(0x86, "V_VP9"));
        Bytes tracks = element(0xAE, video_entry);
        append(tracks, element(0xAE, tracks_entry));

        Bytes cluster_body;
        append(cluster_body, uint_element(0xE7, 0));
        for (usize i = 0; i < payloads.size(); ++i) {
            Bytes block;
            block.push_back(0x83); // track number 3 as a one-byte vint
            be(block, static_cast<u64>(i * block_ms) & 0xFFFF, 2);
            block.push_back(simple ? 0x80 : 0x00);
            append(block, payloads[i]);
            if (simple) {
                append(cluster_body, element(0xA3, block));
            } else {
                Bytes group = element(0xA1, block);
                append(group, uint_element(0x9B, block_ms));
                append(cluster_body, element(0xA0, group));
            }
            // interleave a video block that must be skipped
            Bytes video_block = {0x81, 0, 0, 0x80, 1, 2, 3};
            append(cluster_body, element(0xA3, video_block));
        }
        Bytes segment_body;
        Bytes info;
        append(info, uint_element(0x2AD7B1, 1000000));
        append(segment_body, element(0x1549A966, info));
        append(segment_body, element(0x1654AE6B, tracks));
        if (unknown_cluster_size) {
            Bytes cluster = ebml_id(0x1F43B675);
            cluster.push_back(0x01);
            for (int i = 0; i < 7; ++i) cluster.push_back(0xFF);
            append(cluster, cluster_body);
            append(segment_body, cluster);
        } else {
            append(segment_body, element(0x1F43B675, cluster_body));
        }
        Bytes file = element(0x1A45DFA3, string_element(0x4282, "webm"));
        append(file, element(0x18538067, segment_body));
        return file;
    }

    // ---- MP4 ----
    Bytes box(const char *type, const Bytes &payload) {
        Bytes out;
        be(out, payload.size() + 8, 4);
        out.insert(out.end(), type, type + 4);
        append(out, payload);
        return out;
    }
    Bytes full_box(const char *type, u32 flags_version, const Bytes &payload) {
        Bytes body;
        be(body, flags_version, 4);
        append(body, payload);
        return box(type, body);
    }

    struct Mp4Spec {
        const char *sample_type = "sowt";
        Bytes sample_children;
        u32 channels = 2, rate = 48000, bits = 16;
        std::vector<Bytes> samples; // one entry per sample (a PCM "sample" here is a whole chunk of frames)
        u32 sample_delta = 1024;
        u32 timescale = 48000;
        bool pcm_frames = false;    // true: samples are single PCM frames in one big chunk
        bool fragmented = false;
    };

    Bytes make_mp4(const Mp4Spec &spec) {
        Bytes ftyp;
        ftyp.insert(ftyp.end(), {'i', 's', 'o', 'm'});
        be(ftyp, 0, 4);
        Bytes file = box("ftyp", ftyp);

        Bytes entry;
        be(entry, 0, 6);
        be(entry, 1, 2);
        be(entry, 0, 2);
        be(entry, 0, 2);
        be(entry, 0, 4);
        be(entry, spec.channels, 2);
        be(entry, spec.bits, 2);
        be(entry, 0, 2);
        be(entry, 0, 2);
        be(entry, static_cast<u64>(spec.rate) << 16, 4);
        append(entry, spec.sample_children);
        Bytes stsd_body;
        be(stsd_body, 1, 4);
        append(stsd_body, box(spec.sample_type, entry));
        Bytes stsd = full_box("stsd", 0, stsd_body);

        Bytes mdat_payload;
        std::vector<u32> sizes;
        for (const Bytes &s : spec.samples) {
            append(mdat_payload, s);
            sizes.push_back(static_cast<u32>(s.size()));
        }

        Bytes stbl_body = stsd;
        if (!spec.fragmented) {
            Bytes stts;
            be(stts, 1, 4);
            be(stts, spec.samples.size(), 4);
            be(stts, spec.sample_delta, 4);
            append(stbl_body, full_box("stts", 0, stts));
            Bytes stsc;
            be(stsc, 1, 4);
            be(stsc, 1, 4);
            be(stsc, spec.samples.size(), 4); // everything in one chunk
            be(stsc, 1, 4);
            append(stbl_body, full_box("stsc", 0, stsc));
            Bytes stsz;
            if (spec.pcm_frames) {
                be(stsz, sizes.empty() ? 0 : sizes[0], 4);
                be(stsz, sizes.size(), 4);
            } else {
                be(stsz, 0, 4);
                be(stsz, sizes.size(), 4);
                for (u32 s : sizes) be(stsz, s, 4);
            }
            append(stbl_body, full_box("stsz", 0, stsz));
        } else {
            for (const char *t : {"stts", "stsc", "stco"}) {
                Bytes empty;
                be(empty, 0, 4);
                append(stbl_body, full_box(t, 0, empty));
            }
            Bytes stsz;
            be(stsz, 0, 8);
            append(stbl_body, full_box("stsz", 0, stsz));
        }
        // stco gets patched once the layout is known
        const usize stco_index = stbl_body.size();
        if (!spec.fragmented) {
            Bytes stco;
            be(stco, 1, 4);
            be(stco, 0, 4);
            append(stbl_body, full_box("stco", 0, stco));
        }

        const auto build_moov = [&](u32 chunk_offset) {
            Bytes stbl = stbl_body;
            if (!spec.fragmented) {
                // overwrite the offset in the last 4 bytes of stco
                for (int i = 0; i < 4; ++i) stbl[stco_index + 8 + 4 + 4 + i] = static_cast<u8>(chunk_offset >> (8 * (3 - i)));
            }
            Bytes minf = box("stbl", stbl);
            Bytes mdhd;
            be(mdhd, 0, 8);
            be(mdhd, spec.timescale, 4);
            be(mdhd, spec.samples.size() * spec.sample_delta, 4);
            be(mdhd, 0x55C4, 2);
            be(mdhd, 0, 2);
            Bytes hdlr;
            be(hdlr, 0, 4);
            hdlr.insert(hdlr.end(), {'s', 'o', 'u', 'n'});
            be(hdlr, 0, 12);
            hdlr.push_back(0);
            Bytes mdia = full_box("mdhd", 0, mdhd);
            append(mdia, full_box("hdlr", 0, hdlr));
            append(mdia, box("minf", minf));
            Bytes tkhd;
            be(tkhd, 0, 8);
            be(tkhd, 1, 4); // track id
            be(tkhd, 0, 68);
            Bytes trak = full_box("tkhd", 3, tkhd);
            append(trak, box("mdia", mdia));
            Bytes mvhd;
            be(mvhd, 0, 8);
            be(mvhd, spec.timescale, 4);
            be(mvhd, spec.samples.size() * spec.sample_delta, 4);
            be(mvhd, 0, 80);
            Bytes moov = full_box("mvhd", 0, mvhd);
            if (spec.fragmented) {
                Bytes trex;
                be(trex, 1, 4);
                be(trex, 1, 4);
                be(trex, spec.sample_delta, 4);
                be(trex, 0, 8);
                append(moov, box("mvex", full_box("trex", 0, trex)));
            }
            append(moov, box("trak", trak));
            return box("moov", moov);
        };

        if (!spec.fragmented) {
            const Bytes moov = build_moov(0);
            const u32 offset = static_cast<u32>(file.size() + moov.size() + 8);
            append(file, build_moov(offset));
            append(file, box("mdat", mdat_payload));
            return file;
        }
        append(file, build_moov(0));
        // two fragments, each with half of the samples
        const usize half = spec.samples.size() / 2;
        for (int part = 0; part < 2; ++part) {
            const usize begin = part == 0 ? 0 : half, end = part == 0 ? half : spec.samples.size();
            Bytes data;
            Bytes sizes_table;
            for (usize i = begin; i < end; ++i) {
                append(data, spec.samples[i]);
                be(sizes_table, spec.samples[i].size(), 4);
            }
            Bytes tfhd;
            be(tfhd, 1, 4);
            Bytes tfdt;
            be(tfdt, begin * spec.sample_delta, 4);
            const auto make_moof = [&](u32 data_offset) {
                Bytes trun;
                be(trun, end - begin, 4);
                be(trun, data_offset, 4);
                append(trun, sizes_table);
                Bytes traf = full_box("tfhd", 0x020000, tfhd);
                append(traf, full_box("tfdt", 0, tfdt));
                append(traf, full_box("trun", 0x000201, trun));
                Bytes mfhd;
                be(mfhd, static_cast<u64>(part + 1), 4);
                Bytes moof = full_box("mfhd", 0, mfhd);
                append(moof, box("traf", traf));
                return box("moof", moof);
            };
            const u32 moof_size = static_cast<u32>(make_moof(0).size());
            append(file, make_moof(moof_size + 8));
            append(file, box("mdat", data));
        }
        return file;
    }

    EncodedBytes blob(const Bytes &bytes) {
        std::vector<std::byte> copy(bytes.size());
        std::memcpy(copy.data(), bytes.data(), bytes.size());
        return SFT::Foundation::Io::ByteBlob::from_vector(std::move(copy));
    }

    // 16-bit stereo ramp so every sample is distinguishable.
    std::vector<Bytes> pcm_blocks(usize blocks, usize frames_per_block, bool big_endian) {
        std::vector<Bytes> out;
        for (usize b = 0; b < blocks; ++b) {
            Bytes block;
            for (usize f = 0; f < frames_per_block; ++f) {
                for (int c = 0; c < 2; ++c) {
                    const int16_t v = static_cast<int16_t>(((b * frames_per_block + f) * 7 + c * 1000) % 20000 - 10000);
                    const u16 u = static_cast<u16>(v);
                    if (big_endian) { block.push_back(static_cast<u8>(u >> 8)); block.push_back(static_cast<u8>(u)); }
                    else { block.push_back(static_cast<u8>(u)); block.push_back(static_cast<u8>(u >> 8)); }
                }
            }
            out.push_back(std::move(block));
        }
        return out;
    }
} // namespace

int main() {
    // ---- Matroska: the demuxer ----
    {
        const auto payloads = pcm_blocks(10, 960, false); // 10 blocks of 20 ms at 48 kHz
        for (bool simple : {true, false}) {
            for (bool unknown : {false, true}) {
                const Bytes file = make_mkv("A_PCM/INT/LIT", {}, 2, 48000.0, 16, payloads, 20, simple, unknown);
                auto demuxer = Demuxer::open(blob(file));
                check(demuxer.has_value(), "a hand-built Matroska file opens");
                if (!demuxer) continue;
                const auto tracks = (*demuxer)->tracks();
                check(tracks.size() == 1 && tracks[0].codec == "pcm_s16le"_ustr && tracks[0].channels == 2 && tracks[0].sample_rate == 48000 && tracks[0].bit_depth == 16, "the audio track is described (the video track is ignored)");
                check((*demuxer)->packet_count() == 10 && (*demuxer)->kind() == ContainerKind::Matroska, "every audio block is a packet, video blocks are skipped");
                const auto first = (*demuxer)->next();
                check(first && first->pts_seconds == 0.0 && first->data.size() == 960 * 4, "the first packet is at 0 and carries its bytes");
                (*demuxer)->seek(0.105);
                const auto after = (*demuxer)->next();
                check(after && std::fabs(after->pts_seconds - 0.100) < 1e-9, "seek lands on the packet at or before the time");
                check(simple == (after && after->keyframe) || !simple, "keyframe flags come from SimpleBlock flags");
            }
        }
        // decode through the registry
        const Bytes file = make_mkv("A_PCM/INT/LIT", {}, 2, 48000.0, 16, payloads, 20, true, false);
        auto decoder = DecoderRegistry::global().open(blob(file), ".mkv");
        check(decoder.has_value(), "the decoder registry opens PCM in Matroska");
        if (decoder) {
            check((*decoder)->info().channels == 2 && (*decoder)->info().sample_rate == 48000 && (*decoder)->info().total_frames == 9600, "with the right format and length");
            std::vector<float> out(9600 * 2);
            const u64 got = (*decoder)->read(out.data(), 9600);
            check(got == 9600, "all frames decode");
            const float expected = static_cast<float>(static_cast<int16_t>((5 * 960 + 3) * 7 % 20000 - 10000)) / 32768.0f;
            check(got == 9600 && std::fabs(out[(5 * 960 + 3) * 2] - expected) < 1.0f / 32768.0f, "and the samples are exact");
        }
    }

    // ---- Matroska with Opus (a WebM soundtrack) ----
#if STURDY_AUDIO_ENCODE_OPUS
    {
        OpusEncoderSettings settings;
        settings.channels = 2;
        settings.layout = ChannelLayoutInfo::guess(2);
        auto encoder = OpusEncoderHandle::create(settings);
        check(encoder.has_value(), "an Opus encoder builds");
        if (encoder) {
            std::vector<Bytes> packets;
            std::vector<float> frame(960 * 2);
            std::vector<u8> out(4000);
            for (int p = 0; p < 50; ++p) {
                for (int i = 0; i < 960; ++i) frame[i * 2] = frame[i * 2 + 1] = 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * static_cast<float>(p * 960 + i) / 48000.0f);
                const int size = (*encoder)->encode(frame.data(), out.data(), out.size());
                packets.emplace_back(out.begin(), out.begin() + size);
            }
            Bytes head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2};
            const u32 pre_skip = (*encoder)->pre_skip();
            head.push_back(static_cast<u8>(pre_skip));
            head.push_back(static_cast<u8>(pre_skip >> 8));
            be(head, 0, 0);
            head.insert(head.end(), {0x80, 0xBB, 0, 0, 0, 0, 0});
            const Bytes file = make_mkv("A_OPUS", head, 2, 48000.0, 0, packets, 20, true, false);
            auto decoder = DecoderRegistry::global().open(blob(file), ".webm");
            check(decoder.has_value(), "Opus in WebM opens through the registry");
            if (decoder) {
                check((*decoder)->info().channels == 2 && (*decoder)->info().codec == "opus"_ustr, "as stereo Opus");
                std::vector<float> pcm(48000 * 2);
                const u64 got = (*decoder)->read(pcm.data(), 48000);
                double energy = 0.0;
                for (u64 i = 24000 * 2; i < got * 2; ++i) energy += static_cast<double>(pcm[i]) * pcm[i];
                check(got >= 47000 && energy / static_cast<double>(got * 2 - 48000) > 0.05, "a second of the tone decodes");
            }
        }
    }
#endif

    // ---- MP4: PCM, fragmented PCM, AAC (demux only) ----
    {
        Mp4Spec pcm;
        pcm.samples = pcm_blocks(1, 9600, false);
        pcm.pcm_frames = true;
        pcm.samples = {};
        // one "sample" per frame (4 bytes), all in one chunk: the demuxer must still return a single packet
        std::vector<Bytes> frames;
        for (const Bytes &block : pcm_blocks(1, 9600, false)) {
            for (usize i = 0; i + 4 <= block.size(); i += 4) frames.emplace_back(block.begin() + static_cast<std::ptrdiff_t>(i), block.begin() + static_cast<std::ptrdiff_t>(i + 4));
        }
        pcm.samples = frames;
        pcm.sample_delta = 1;
        const Bytes file = make_mp4(pcm);
        auto demuxer = Demuxer::open(blob(file));
        check(demuxer.has_value() && (*demuxer)->kind() == ContainerKind::Mp4, "a hand-built MP4 opens");
        if (demuxer) {
            check((*demuxer)->tracks().size() == 1 && (*demuxer)->tracks()[0].codec == "pcm_s16le"_ustr && (*demuxer)->tracks()[0].language == "und"_ustr, "the sowt track is PCM with its language");
            check((*demuxer)->packet_count() == 1, "PCM frames are indexed per chunk, not per frame");
            check(std::fabs((*demuxer)->duration_seconds() - 0.2) < 1e-6, "duration comes from the sample tables");
        }
        auto decoder = DecoderRegistry::global().open(blob(file), ".m4a");
        check(decoder.has_value() && (*decoder)->info().total_frames == 9600, "PCM in MP4 decodes");

        Mp4Spec fragmented = pcm;
        fragmented.samples = pcm_blocks(10, 960, false);
        fragmented.sample_delta = 960;
        fragmented.pcm_frames = false;
        fragmented.fragmented = true;
        const Bytes frag_file = make_mp4(fragmented);
        auto frag = Demuxer::open(blob(frag_file));
        check(frag.has_value() && (*frag)->packet_count() == 10, "fragmented MP4 lists the packets of every fragment");
        if (frag) {
            u64 bytes = 0;
            double last = -1.0;
            bool ordered = true;
            while (auto packet = (*frag)->next()) {
                bytes += packet->data.size();
                ordered = ordered && packet->pts_seconds > last;
                last = packet->pts_seconds;
            }
            check(bytes == 10 * 960 * 4 && ordered && std::fabs(last - 9 * 0.02) < 1e-6, "fragment packets are complete and in time order");
        }
        auto frag_decoder = DecoderRegistry::global().open(blob(frag_file), ".mp4");
        check(frag_decoder.has_value() && (*frag_decoder)->info().total_frames == 9600, "fragmented PCM decodes");

        // AAC: the esds is understood and the packets come out; decoding needs an OS codec, so only the demuxer is tested.
        Mp4Spec aac;
        aac.sample_type = "mp4a";
        // esds: ES_Descriptor(03) { ES_ID, flags, DecoderConfig(04){ 0x40, stream type, ..., DecoderSpecific(05){ 0x12 0x10 } } }
        const Bytes asc = {0x12, 0x10}; // AAC-LC, 44.1 kHz, stereo
        Bytes dcd = {0x40, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        dcd.insert(dcd.end(), {0x05, static_cast<u8>(asc.size())});
        append(dcd, asc);
        Bytes es = {0x00, 0x01, 0x00};
        es.push_back(0x04);
        es.push_back(static_cast<u8>(dcd.size()));
        append(es, dcd);
        Bytes esds_body = {0x03, static_cast<u8>(es.size())};
        append(esds_body, es);
        aac.sample_children = full_box("esds", 0, esds_body);
        aac.rate = 44100;
        aac.timescale = 44100;
        for (int i = 0; i < 20; ++i) aac.samples.push_back(Bytes(100 + static_cast<usize>(i), static_cast<u8>(i)));
        auto aac_demuxer = Demuxer::open(blob(make_mp4(aac)));
        check(aac_demuxer.has_value(), "AAC in MP4 opens in the demuxer");
        if (aac_demuxer) {
            const auto &track = (*aac_demuxer)->tracks()[0];
            check(track.codec == "aac"_ustr && track.codec_private == asc && track.channels == 2 && track.sample_rate == 44100, "the AudioSpecificConfig is extracted");
            const auto p = (*aac_demuxer)->next();
            (void)(*aac_demuxer)->next();
            const auto q = (*aac_demuxer)->next();
            check(p && q && p->data.size() == 100 && q->data.size() == 102 && std::fabs(q->pts_seconds - 2.0 * 1024.0 / 44100.0) < 1e-9, "packet sizes and times come from the sample tables");
        }
    }

    // ---- garbage and truncation ----
    {
        check(!Demuxer::open(blob(Bytes{1, 2, 3, 4, 5, 6, 7, 8})).has_value(), "unknown data is not a container");
        Bytes truncated = make_mkv("A_PCM/INT/LIT", {}, 2, 48000.0, 16, pcm_blocks(10, 960, false), 20, true, false);
        truncated.resize(truncated.size() / 2);
        auto partial = Demuxer::open(blob(truncated));
        check(partial.has_value() && (*partial)->packet_count() > 0 && (*partial)->packet_count() < 10, "a truncated recording still yields the packets it has");
    }
    return failures == 0 ? 0 : 1;
}
