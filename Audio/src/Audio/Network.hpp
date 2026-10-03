#pragma once

#include <Audio/Channels.hpp>
#include <Audio/Rtcp.hpp>
#include <Audio/Mixer.hpp>
#include <Audio/OpusCodec.hpp>
#include <Audio/Source.hpp>

#include <Foundation/Foundation.hpp>

#include <chrono>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// Audio over IP. Streams are RTP (RFC 3550) over UDP, unicast or multicast, carrying uncompressed 16/24-bit PCM (the same
/// payload AES67 and Dante-compatible gear use) or Opus; both work for any number of channels up to 255, so an 8-channel
/// surround feed, a 32-channel console snapshot and a stereo voice chat are the same code. A stream is described by a
/// `StreamDescription`, which converts to and from SDP so sessions can be exchanged with other software.
///
/// The pieces are layered so each can be used alone: `RtpPacketizer` / `RtpStreamDecoder` turn audio into packets and back
/// with no sockets (for tests, or to carry RTP over some other transport), `UdpSocket` is a small cross-platform socket, and
/// `RtpSender` / `RtpReceiver` / `NetworkOutputSink` put them together.
namespace SFT::Audio {

    // ---- sockets --------------------------------------------------------------------------------------------------------

    struct NetAddress {
        UString host; ///< numeric address or a name (resolved when used)
        u16 port = 0;
        [[nodiscard]] UString to_string() const { return std::format("{}:{}", host, port); }
    };

    struct UdpOptions {
        UString bind_address = "0.0.0.0"; ///< "::" for IPv6
        u16 bind_port = 0;                    ///< 0 picks a free port
        bool reuse_address = true;            ///< several receivers of one multicast stream on a host need this
        /// Multicast group to join after binding (receivers).
        UString multicast_group;
        u32 multicast_ttl = 4;
        bool multicast_loopback = true;
        u32 receive_buffer_bytes = 1u << 20;
        /// DSCP traffic class; 46 (Expedited Forwarding) is the usual choice for audio on managed networks, 0 leaves it default.
        u8 dscp = 0;
    };

    /// A datagram socket (IPv4 or IPv6) on POSIX and Winsock. Not thread safe except that `send_to` and `receive` may run on
    /// different threads.
    class UdpSocket {
      public:
        [[nodiscard]] static std::expected<std::unique_ptr<UdpSocket>, UString> open(const UdpOptions &options = {});
        ~UdpSocket();
        UdpSocket(const UdpSocket &) = delete;
        UdpSocket &operator=(const UdpSocket &) = delete;

        [[nodiscard]] u16 local_port() const noexcept;
        bool send_to(const NetAddress &destination, std::span<const u8> data);
        /// Waits up to `timeout_ms` (0 = poll) for a datagram. Returns its size, or nullopt when none arrived. `from`, when
        /// given, receives the sender.
        [[nodiscard]] std::optional<usize> receive(std::span<u8> buffer, int timeout_ms, NetAddress *from = nullptr);

      private:
        struct Impl;
        explicit UdpSocket(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

    // ---- stream description -----------------------------------------------------------------------------------------------

    enum class RtpCodec : u8 {
        Pcm16,    ///< "L16": 16-bit big-endian PCM
        Pcm24,    ///< "L24": 24-bit big-endian PCM (AES67's native format)
        Opus,     ///< "opus": mono/stereo per RFC 7587; multichannel as a Sturdy extension (see to_sdp)
    };

    struct StreamDescription {
        RtpCodec codec = RtpCodec::Pcm24;
        u32 sample_rate = 48000;
        u32 channels = 2;
        /// What the channels are; carried in the SDP so the receiver maps them correctly.
        ChannelLayoutInfo layout;
        u8 payload_type = 96;
        /// Packet duration. PCM defaults to 1 ms (AES67); Opus packets are one frame (`opus_frame_ms`).
        f32 packet_ms = 1.0f;
        UString name = "SturdyEngine audio";

        // ---- Opus
        OpusStreamLayout opus;
        u32 opus_bitrate_bps = 0;
        OpusApplication opus_application = OpusApplication::Audio;
        f32 opus_frame_ms = 20.0f;

        /// Frames carried by each packet (PCM), clamped so a packet fits `mtu_payload` bytes.
        [[nodiscard]] u32 frames_per_packet(u32 mtu_payload = 1388) const;
        /// A session description for this stream sent to `address:port` (multicast addresses get a TTL).
        [[nodiscard]] UString to_sdp(const UString &address, u16 port) const;
        /// Parses an SDP written by `to_sdp` (or by AES67/ST 2110-30 style gear: L16/L24, up to 8 channels, ptime). `address`
        /// and `port`, when given, receive the connection endpoint.
        [[nodiscard]] static std::expected<StreamDescription, UString> from_sdp(const ustr &sdp, UString *address = nullptr, u16 *port = nullptr);
    };

    // ---- packets ----------------------------------------------------------------------------------------------------------

    /// Turns audio into RTP packets: PCM is packed big-endian, Opus is encoded a frame at a time. Output goes to a callback
    /// per packet (a socket, a test, another transport).
    class RtpPacketizer {
      public:
        using PacketSink = std::function<void(std::span<const u8> packet)>;
        [[nodiscard]] static std::expected<std::unique_ptr<RtpPacketizer>, UString> create(const StreamDescription &stream, PacketSink sink, u32 ssrc = 0);
        ~RtpPacketizer();

        /// Accepts interleaved frames at the stream's rate and channel count; whole packets are emitted as they fill, any
        /// remainder waits for the next call. Returns false on an encoder error.
        bool write(const f32 *interleaved, usize frames);
        /// Emits what is left as a final (padded) packet.
        void flush();
        [[nodiscard]] u64 packets() const noexcept;
        [[nodiscard]] u64 bytes() const noexcept;
        [[nodiscard]] u32 ssrc() const noexcept;
        /// The RTP clock of the next packet (what a sender report pairs with the wall clock).
        [[nodiscard]] u32 rtp_timestamp() const noexcept;
        [[nodiscard]] const StreamDescription &stream() const noexcept;

      private:
        struct Impl;
        explicit RtpPacketizer(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

    struct RtpStats {
        u64 packets_received = 0;
        u64 packets_lost = 0;       ///< never arrived before their playout deadline
        u64 packets_late = 0;       ///< arrived after being given up on (discarded)
        u64 packets_duplicate = 0;
        u64 packets_reordered = 0;  ///< arrived out of order but in time
        u64 packets_invalid = 0;    ///< not RTP, wrong payload type, or malformed
        u64 concealed_frames = 0;
        f64 jitter_ms = 0.0;        ///< RFC 3550 interarrival jitter
    };

    struct RtpDecoderOptions {
        /// How long to wait for a missing packet before giving up on it and concealing the gap. Larger absorbs more network
        /// jitter and reordering at the cost of latency (it adds to the live source's cushion).
        f32 jitter_ms = 20.0f;
        /// SSRC to accept; 0 locks onto the first sender heard (a new sender replaces it after a second of silence).
        u32 ssrc = 0;
    };

    /// Turns RTP packets back into audio: reorders within the jitter window, conceals lost packets (silence for PCM, the
    /// codec's own concealment for Opus), decodes, and feeds a `LiveSource` the engine can play. Thread safe: `push_packet`
    /// and `tick` may be called from one network thread while the mixer reads the source.
    class RtpStreamDecoder {
      public:
        using Clock = std::chrono::steady_clock;
        [[nodiscard]] static std::expected<std::unique_ptr<RtpStreamDecoder>, UString> create(const StreamDescription &stream, u32 engine_rate,
                                                                                                  const RtpDecoderOptions &options = {}, f32 cushion_ms = 30.0f,
                                                                                                  f32 buffer_seconds = 2.0f);
        ~RtpStreamDecoder();

        /// Feeds one datagram received at `arrival`.
        void push_packet(std::span<const u8> packet, Clock::time_point arrival = Clock::now());
        /// Gives up on packets that have missed their deadline at `now`; call regularly (a receive loop does it for you).
        void tick(Clock::time_point now = Clock::now());
        [[nodiscard]] std::shared_ptr<LiveSource> source() const;
        [[nodiscard]] RtpStats stats() const;
        /// What a receiver report needs to say about the sender it is locked to (zero ssrc: nobody heard yet).
        [[nodiscard]] RtcpReportBlock report_block() const;
        [[nodiscard]] const StreamDescription &stream() const noexcept;

      private:
        struct Impl;
        explicit RtpStreamDecoder(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

    // ---- send / receive over UDP ----------------------------------------------------------------------------------------

    struct RtpSenderConfig {
        NetAddress destination;
        StreamDescription stream;
        /// Local port to send from (0 = any).
        u16 local_port = 0;
        u8 dscp = 46;
        u32 multicast_ttl = 4;
        u32 ssrc = 0;
        /// Hold `write` back so audio leaves at its natural rate even when the caller produces it faster (streaming a file).
        /// Leave off when the caller already runs in real time (a microphone, the engine's output).
        bool paced = false;
    };

    /// What the far end's receiver reports said (RTCP receiver reports), as of the latest one.
    struct RtpPeerStats {
        u64 reports_received = 0;
        f64 round_trip_ms = 0.0;   ///< 0 until a report answers one of our sender reports
        f32 fraction_lost = 0.0f;  ///< 0..1 since the previous report
        i32 packets_lost = 0;      ///< cumulative
        f64 jitter_ms = 0.0;
    };

    class RtpSender {
      public:
        [[nodiscard]] static std::expected<std::unique_ptr<RtpSender>, UString> create(const RtpSenderConfig &config);
        ~RtpSender();

        bool write(const f32 *interleaved, usize frames);
        void flush();
        [[nodiscard]] u64 packets_sent() const noexcept;
        [[nodiscard]] u64 bytes_sent() const noexcept;
        [[nodiscard]] const StreamDescription &stream() const noexcept;
        /// The SDP a receiver needs to join this stream.
        [[nodiscard]] UString sdp() const;
        /// RTCP feedback from the receiver(s): loss, jitter and round-trip time. Sender reports go out about once a second
        /// to the destination port + 1 while `write` is being called, and receiver reports come back to the same socket.
        [[nodiscard]] RtpPeerStats peer_stats() const;

      private:
        struct Impl;
        explicit RtpSender(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

    struct RtpReceiverConfig {
        StreamDescription stream;
        u16 port = 0;
        UString bind_address = "0.0.0.0";
        /// Join this multicast group (empty = unicast).
        UString multicast_group;
        u32 engine_rate = 48000;
        RtpDecoderOptions decoder;
        /// Held back before playback starts, on top of the jitter window.
        f32 cushion_ms = 30.0f;
    };

    /// What the sender's reports said (RTCP sender reports).
    struct RtpSenderInfo {
        u64 reports_received = 0;
        u64 packets_sent = 0;      ///< by the sender, as it counted them
        u64 octets_sent = 0;
        u64 ntp_timestamp = 0;     ///< the sender's wall clock at its latest report
        UString cname;
    };

    /// Receives a stream on a worker thread. `source()` is a `LiveSource` to hand to `AudioEngine::play`.
    class RtpReceiver {
      public:
        [[nodiscard]] static std::expected<std::unique_ptr<RtpReceiver>, UString> create(const RtpReceiverConfig &config);
        ~RtpReceiver();

        [[nodiscard]] std::shared_ptr<LiveSource> source() const;
        [[nodiscard]] RtpStats stats() const;
        /// What the sender said about itself in RTCP sender reports (port + 1); empty until one arrives.
        [[nodiscard]] RtpSenderInfo sender_info() const;
        /// The port actually bound (useful when `port` was 0).
        [[nodiscard]] u16 port() const noexcept;
        [[nodiscard]] const StreamDescription &stream() const noexcept;

      private:
        struct Impl;
        explicit RtpReceiver(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

    /// Sends one of the engine's secondary outputs onto the network as it is produced (the engine's primary output must be
    /// pulled by a device or by your own loop; secondary outputs follow it). Define the output in the engine config with the
    /// speaker layout you want to send (stereo, 5.1, 7.1.4 ...); the stream's channels and layout follow it.
    class NetworkOutputSink {
      public:
        [[nodiscard]] static std::expected<std::unique_ptr<NetworkOutputSink>, UString> start(AudioEngine &engine, OutputId output, RtpSenderConfig config);
        ~NetworkOutputSink();

        [[nodiscard]] u64 packets_sent() const noexcept;
        [[nodiscard]] UString sdp() const;

      private:
        struct Impl;
        explicit NetworkOutputSink(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

} // namespace SFT::Audio
