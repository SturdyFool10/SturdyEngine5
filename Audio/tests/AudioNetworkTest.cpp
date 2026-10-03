#include <Audio/Network.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <thread>

using namespace SFT::Audio;
using SFT::u32;
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

    std::vector<float> signal(u32 channels, usize frames, double base = 220.0, u32 rate = 48000, float amplitude = 0.4f) {
        std::vector<float> out(frames * channels);
        for (usize i = 0; i < frames; ++i) {
            for (u32 c = 0; c < channels; ++c) {
                out[i * channels + c] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * base * (c + 1) * static_cast<double>(i) / rate));
            }
        }
        return out;
    }

    std::vector<float> drain(LiveSource &source, u32 channels, usize max_frames) {
        std::vector<float> out;
        AudioBuffer block(channels, 512);
        for (;;) {
            const u32 got = source.read(block, 512);
            if (got == 0 || out.size() / channels >= max_frames) break;
            for (u32 i = 0; i < got; ++i)
                for (u32 c = 0; c < channels; ++c) out.push_back(block.data(c)[i]);
        }
        return out;
    }

    std::vector<std::vector<u8>> packetize(const StreamDescription &stream, const std::vector<float> &audio, usize chunk_frames = 480) {
        std::vector<std::vector<u8>> packets;
        auto packetizer = RtpPacketizer::create(stream, [&](std::span<const u8> p) { packets.emplace_back(p.begin(), p.end()); });
        if (!packetizer) {
            std::cerr << "  packetizer: " << packetizer.error() << '\n';
            return packets;
        }
        const u32 channels = stream.channels;
        for (usize i = 0; i < audio.size() / channels; i += chunk_frames) {
            (*packetizer)->write(audio.data() + i * channels, std::min(chunk_frames, audio.size() / channels - i));
        }
        (*packetizer)->flush();
        return packets;
    }
} // namespace

int main() {
    // ---- exact PCM through the packetizer and decoder, stereo L24 ------------------------------------------------------------
    {
        StreamDescription stream;
        stream.codec = RtpCodec::Pcm24;
        stream.channels = 2;
        const auto audio = signal(2, 9600);
        const auto packets = packetize(stream, audio);
        check(packets.size() == 9600 / 48, "1 ms packets: 48 frames each at 48 kHz");
        check(packets[0].size() == 12 + 48 * 2 * 3, "an L24 stereo packet is 12 + 288 bytes");
        auto decoder = RtpStreamDecoder::create(stream, 48000, {}, 0.0f);
        check(decoder.has_value(), "decoder builds");
        const auto t0 = RtpStreamDecoder::Clock::now();
        for (const auto &p : packets) (*decoder)->push_packet(p, t0);
        const auto out = drain(*(*decoder)->source(), 2, 100000);
        check(out.size() == audio.size(), "every frame arrives");
        float worst = 0;
        for (usize i = 0; i < std::min(out.size(), audio.size()); ++i) worst = std::max(worst, std::fabs(out[i] - audio[i]));
        check(worst < 2e-7f, "and is exact to 24 bits");
        check((*decoder)->stats().packets_received == packets.size() && (*decoder)->stats().packets_lost == 0, "no loss counted");
    }

    // ---- loss and reordering ------------------------------------------------------------------------------------------------------
    {
        StreamDescription stream;
        stream.codec = RtpCodec::Pcm16;
        stream.channels = 2;
        const auto audio = signal(2, 48000);
        auto packets = packetize(stream, audio);
        check(packets.size() == 1000, "one second is 1000 packets");
        // Swap neighbours (reordering) and drop packet 500.
        for (usize i = 100; i + 1 < 300; i += 2) std::swap(packets[i], packets[i + 1]);
        packets.erase(packets.begin() + 500);
        RtpDecoderOptions options;
        options.jitter_ms = 10.0f;
        auto decoder = RtpStreamDecoder::create(stream, 48000, options, 0.0f, 4.0f);
        auto t = RtpStreamDecoder::Clock::now();
        for (const auto &p : packets) {
            (*decoder)->push_packet(p, t);
            t += std::chrono::microseconds(1000);
            (*decoder)->tick(t);
        }
        (*decoder)->tick(t + std::chrono::milliseconds(50));
        const RtpStats stats = (*decoder)->stats();
        check(stats.packets_lost == 1, "the dropped packet is declared lost after the jitter window");
        check(stats.packets_reordered > 0 && stats.packets_late == 0, "swapped packets are reordered in time, not discarded");
        const auto out = drain(*(*decoder)->source(), 2, 100000);
        check(out.size() == audio.size(), "the lost packet's time is concealed, so the stream keeps its length");
        bool silent = true;
        for (usize i = 500 * 48 * 2; i < 501 * 48 * 2 && i < out.size(); ++i) silent = silent && std::fabs(out[i]) < 1e-6f;
        check(silent, "PCM loss is concealed with silence");
        check(std::fabs(out[600 * 96 + 10] - audio[600 * 96 + 10]) < 1e-3f, "audio after the gap is in sync");
    }

    // ---- SDP round trip, many channels and layouts ------------------------------------------------------------------------------------
    {
        StreamDescription stream;
        stream.codec = RtpCodec::Pcm24;
        stream.channels = 6;
        stream.layout = ChannelLayoutInfo::from_speakers(SpeakerLayout::surround_5_1());
        stream.payload_type = 98;
        stream.packet_ms = 0.25f;
        const UString sdp = stream.to_sdp("239.69.1.2", 5004);
        UString address;
        SFT::u16 port = 0;
        const auto parsed = StreamDescription::from_sdp(sdp, &address, &port);
        check(parsed && parsed->codec == RtpCodec::Pcm24 && parsed->channels == 6 && parsed->payload_type == 98 && address == UString{"239.69.1.2"} && port == 5004, "SDP round trip");
        check(parsed && parsed->layout.kind == ChannelKind::Speakers && parsed->layout.speakers.speakers[3].role == ChannelRole::Lfe, "the speaker layout travels in the SDP");
        check(parsed && std::fabs(parsed->packet_ms - 0.25f) < 1e-4f, "and so does the packet time");

        const std::string aes67 =
            "v=0\r\no=- 1 1 IN IP4 192.168.1.20\r\ns=Console\r\nc=IN IP4 239.1.2.3/32\r\nt=0 0\r\nm=audio 5004 RTP/AVP 97\r\n"
            "a=rtpmap:97 L24/48000/8\r\na=ptime:1\r\n";
        const auto desk = StreamDescription::from_sdp(aes67, &address, &port);
        check(desk && desk->channels == 8 && desk->codec == RtpCodec::Pcm24 && address == UString{"239.1.2.3"} && desk->frames_per_packet() == 48, "an AES67-style session description is understood");
        check(!StreamDescription::from_sdp("v=0\r\ns=x\r\n").has_value(), "garbage is rejected");

        // 64 channels of L24 still fit a packet (192 bytes per frame).
        StreamDescription wide;
        wide.codec = RtpCodec::Pcm24;
        wide.channels = 64;
        check(wide.frames_per_packet() == 7, "wide streams get fewer frames per packet so they fit the MTU");
        const auto wide_audio = signal(64, 4800);
        const auto wide_packets = packetize(wide, wide_audio, 100);
        check(!wide_packets.empty() && wide_packets[0].size() <= 1400, "64-channel packets stay under the MTU");
        auto wide_decoder = RtpStreamDecoder::create(wide, 48000, {}, 0.0f);
        const auto now = RtpStreamDecoder::Clock::now();
        for (const auto &p : wide_packets) (*wide_decoder)->push_packet(p, now);
        const auto wide_out = drain(*(*wide_decoder)->source(), 64, 100000);
        check(wide_out.size() == wide_audio.size() && std::fabs(wide_out[63 + 64 * 100] - wide_audio[63 + 64 * 100]) < 2e-7f, "64 channels arrive intact");
    }

#if STURDY_AUDIO_OPUS
    // ---- Opus: stereo and 8-channel surround through RTP -----------------------------------------------------------------------------------
    {
        for (u32 channels : {2u, 8u}) {
            StreamDescription stream;
            stream.codec = RtpCodec::Opus;
            stream.channels = channels;
            stream.sample_rate = 48000;
            stream.layout = channels == 8 ? ChannelLayoutInfo::from_speakers(SpeakerLayout::surround_7_1()) : ChannelLayoutInfo::guess(2);
            stream.opus_bitrate_bps = channels == 8 ? 256000 : 96000;
            stream.payload_type = 97;
            const auto audio = signal(channels, 48000, 300.0);
            auto packets = packetize(stream, audio);
            check(packets.size() >= 49, "Opus sends one packet per 20 ms frame");
            // The description a receiver needs comes from the sender's side; a real receiver gets it as SDP.
            auto packetizer = RtpPacketizer::create(stream, [](std::span<const u8>) {});
            const std::string sdp = (*packetizer)->stream().to_sdp("127.0.0.1", 6000);
            const auto parsed = StreamDescription::from_sdp(sdp);
            check(parsed && parsed->codec == RtpCodec::Opus && parsed->channels == channels, "an Opus description round trips");
            auto decoder = RtpStreamDecoder::create(*parsed, 48000, {}, 0.0f, 4.0f);
            check(decoder.has_value(), "Opus decoder builds from the SDP");
            if (!decoder) continue;
            const auto now = RtpStreamDecoder::Clock::now();
            for (const auto &p : packets) (*decoder)->push_packet(p, now);
            const auto out = drain(*(*decoder)->source(), channels, 200000);
            check(out.size() / channels >= 47000, "the decoded stream has the audio's length");
            // Energy per channel: each channel carries its own tone, so every one must come out audible.
            bool all = out.size() > static_cast<usize>(channels) * 10000;
            for (u32 c = 0; all && c < channels; ++c) {
                double e = 0;
                for (usize i = 5000; i < 20000; ++i) e += static_cast<double>(out[i * channels + c]) * out[i * channels + c];
                all = e / 15000.0 > 0.01;
            }
            check(all, "every channel of the Opus stream carries its signal");
            // Lose a packet: Opus conceals it with its own PLC rather than silence.
            auto lossy = packets;
            lossy.erase(lossy.begin() + 20);
            auto plc = RtpStreamDecoder::create(*parsed, 48000, RtpDecoderOptions{.jitter_ms = 5.0f}, 0.0f, 4.0f);
            auto t = RtpStreamDecoder::Clock::now();
            for (const auto &p : lossy) {
                (*plc)->push_packet(p, t);
                t += std::chrono::milliseconds(20);
                (*plc)->tick(t);
            }
            (*plc)->tick(t + std::chrono::milliseconds(100));
            check((*plc)->stats().packets_lost == 1 && (*plc)->stats().concealed_frames == 960, "a lost Opus packet is concealed for exactly one frame");
        }
    }
#endif

    // ---- RTCP packets ----------------------------------------------------------------------------------------------------------------------
    {
        RtcpSenderReport sr;
        sr.ssrc = 0xDEADBEEF;
        sr.ntp_timestamp = 0x0123456789ABCDEFull;
        sr.rtp_timestamp = 48000;
        sr.packet_count = 100;
        sr.octet_count = 123456;
        sr.blocks.push_back(RtcpReportBlock{0x11223344, 25, -5, 0x00010002, 77, 0xAABBCCDD, 65536});
        const auto bytes = build_sender_report_packet(sr, "alice@host");
        check(bytes.size() % 4 == 0, "RTCP packets are whole 32-bit words");
        const auto parsed = parse_rtcp(bytes);
        check(parsed && parsed->sender_reports.size() == 1 && parsed->sender_reports[0].ntp_timestamp == sr.ntp_timestamp && parsed->sender_reports[0].octet_count == 123456, "a sender report round trips");
        check(parsed && parsed->sender_reports[0].blocks.size() == 1 && parsed->sender_reports[0].blocks[0].cumulative_lost == -5 && parsed->sender_reports[0].blocks[0].fraction_lost == 25 &&
                  parsed->sender_reports[0].blocks[0].highest_sequence == 0x00010002,
              "report blocks keep a negative 24-bit loss count");
        check(parsed && parsed->cnames.size() == 1 && parsed->cnames[0].first == 0xDEADBEEF && parsed->cnames[0].second == "alice@host"_ustr, "the CNAME rides along in SDES");
        RtcpReceiverReport rr;
        rr.ssrc = 7;
        rr.blocks.push_back(RtcpReportBlock{0xDEADBEEF, 0, 0, 5, 0, 0, 0});
        const auto rr_parsed = parse_rtcp(build_receiver_report_packet(rr, "bob"));
        check(rr_parsed && rr_parsed->receiver_reports.size() == 1 && rr_parsed->receiver_reports[0].blocks[0].highest_sequence == 5, "a receiver report round trips");
        const auto bye = parse_rtcp(build_goodbye_packet(42));
        check(bye && bye->goodbyes.size() == 1 && bye->goodbyes[0] == 42, "BYE round trips");
        auto truncated = bytes;
        truncated.resize(truncated.size() - 3);
        check(!parse_rtcp(truncated).has_value() && !parse_rtcp(std::vector<u8>{1, 2, 3, 4}).has_value(), "malformed RTCP is rejected");
        // Round trip: we sent an SR at NTP time T; the receiver heard it, waited one second, replied; the reply arrives 1.25 s after T.
        const u64 sent = 5ull << 32;
        RtcpReportBlock block{1, 0, 0, 0, 0, ntp_middle(sent), 65536};
        const auto rtt = round_trip_seconds(block, sent + (static_cast<u64>(5) << 30)); // + 1.25 s
        check(rtt && std::fabs(*rtt - 0.25) < 1e-3, "round-trip time comes from LSR and DLSR");
        check(!round_trip_seconds(RtcpReportBlock{}, sent).has_value(), "a block that answers no sender report has no round trip");
    }

    // ---- real UDP on the loopback interface ----------------------------------------------------------------------------------------------
    {
        StreamDescription stream;
        stream.codec = RtpCodec::Pcm16;
        stream.channels = 12;
        RtpReceiverConfig receive;
        receive.stream = stream;
        receive.bind_address = "127.0.0.1";
        receive.port = 0;
        receive.cushion_ms = 0.0f;
        auto receiver = RtpReceiver::create(receive);
        if (!receiver) {
            std::cerr << "  (skipping UDP test: " << receiver.error() << ")\n";
        } else {
            RtpSenderConfig send;
            send.destination = {"127.0.0.1", (*receiver)->port()};
            send.stream = stream;
            send.dscp = 0;
            auto sender = RtpSender::create(send);
            check(sender.has_value(), "the sender opens a socket");
            if (sender) {
                const auto audio = signal(12, 4800);
                (*sender)->write(audio.data(), 4800);
                (*sender)->flush();
                // Wait for the packets to cross the loopback and be decoded.
                for (int i = 0; i < 200 && (*receiver)->source()->buffered_frames() < 4800; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                check((*sender)->packets_sent() > 0, "packets were sent");
                check((*receiver)->source()->buffered_frames() >= 4700, "12-channel audio crosses a real UDP socket");
                check((*receiver)->stats().packets_received >= (*sender)->packets_sent() - 2, "nearly every packet is received on loopback");
                // RTCP: sender reports reach the receiver, receiver reports (loss, jitter, round trip) come back.
                const auto chunk = signal(12, 480);
                for (int i = 0; i < 80 && ((*sender)->peer_stats().reports_received == 0 || (*receiver)->sender_info().reports_received == 0); ++i) {
                    (*sender)->write(chunk.data(), 480);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                const RtpPeerStats peer = (*sender)->peer_stats();
                const RtpSenderInfo info = (*receiver)->sender_info();
                check(info.reports_received > 0 && info.packets_sent > 0 && !info.cname.empty(), "the receiver hears the sender's reports and CNAME");
                check(peer.reports_received > 0, "the sender hears the receiver's reports");
                check(peer.packets_lost == 0 && peer.fraction_lost == 0.0f && peer.round_trip_ms < 1000.0, "reports describe a clean loopback with a sane round trip");
            }
        }
    }

    // ---- an engine output over the network ---------------------------------------------------------------------------------------------------
    {
        AudioEngineConfig config;
        config.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::stereo()},
                          OutputDesc{OutputDesc::Kind::Speakers, "stream", SpeakerLayout::surround_5_1()}};
        AudioEngine engine(config);
        StreamDescription expected;
        expected.codec = RtpCodec::Pcm24;
        expected.channels = 6;
        RtpReceiverConfig receive;
        receive.stream = expected;
        receive.bind_address = "127.0.0.1";
        receive.cushion_ms = 0.0f;
        auto receiver = RtpReceiver::create(receive);
        if (receiver) {
            RtpSenderConfig send;
            send.destination = {"127.0.0.1", (*receiver)->port()};
            send.stream.codec = RtpCodec::Pcm24;
            send.dscp = 0;
            auto sink = NetworkOutputSink::start(engine, 1, send);
            check(sink.has_value(), "a secondary output starts streaming");
            if (sink) {
                // A tone on the second output's front-left channel.
                auto tone = std::make_shared<SampleBuffer>();
                tone->channels = 1;
                tone->sample_rate = 48000;
                tone->samples = std::make_shared<std::vector<float>>(signal(1, 48000, 440.0));
                PlayParams p;
                p.source = std::make_shared<BufferSource>(tone, 48000, true);
                p.spatial = false;
                p.bus = engine.master_bus(1);
                engine.play(std::move(p));
                std::vector<float> device(2 * 480);
                for (int i = 0; i < 40; ++i) {
                    engine.pull(0, device.data(), 480); // the primary output's device would do this
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                for (int i = 0; i < 100 && (*receiver)->source()->buffered_frames() < 9000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                check((*sink)->packets_sent() > 100, "the output was packetised as the engine produced it");
                check((*receiver)->source()->buffered_frames() > 9000, "and a receiver got it");
                check((*sink)->sdp().contains(std::string_view{"L24/48000/6"}), "the sink publishes an SDP for its 5.1 stream");
                AudioBuffer block(6, 2048);
                (*receiver)->source()->read(block, 2048);
                check(block.peak() > 0.1f, "the received 5.1 stream carries the tone");
            }
        }
    }

    return failures == 0 ? 0 : 1;
}
