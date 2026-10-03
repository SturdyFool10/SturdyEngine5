#include <Audio/Network.hpp>

#include <Audio/Realtime.hpp>
#include <Audio/Sink.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <ranges>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace SFT::Audio {

    // ================================================================================================================
    // Sockets
    // ================================================================================================================

    namespace {

#if defined(_WIN32)
        using SocketHandle = SOCKET;
        constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
        void close_socket(SocketHandle s) { closesocket(s); }
        UString socket_error() { return UString{std::format("error {}", WSAGetLastError())}; }
        void ensure_socket_library() {
            static const bool ready = [] {
                WSADATA data;
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            (void)ready;
        }
#else
        using SocketHandle = int;
        constexpr SocketHandle kInvalidSocket = -1;
        void close_socket(SocketHandle s) { ::close(s); }
        UString socket_error() { return text_from_bytes(std::strerror(errno)); }
        void ensure_socket_library() {}
#endif

        /// Resolves a numeric address or host name for `family` (AF_UNSPEC accepts either).
        bool resolve(const UString &host, u16 port, int family, bool passive, sockaddr_storage &out, socklen_t &length) {
            addrinfo hints{};
            hints.ai_family = family;
            hints.ai_socktype = SOCK_DGRAM;
            hints.ai_flags = passive ? AI_PASSIVE : 0;
            addrinfo *result = nullptr;
            const std::string service = std::to_string(port); // getaddrinfo wants a C string
            if (getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
                return false;
            }
            std::memcpy(&out, result->ai_addr, result->ai_addrlen);
            length = static_cast<socklen_t>(result->ai_addrlen);
            freeaddrinfo(result);
            return true;
        }

        UString numeric_host(const sockaddr_storage &address) {
            char buffer[NI_MAXHOST] = {};
            getnameinfo(reinterpret_cast<const sockaddr *>(&address), sizeof(sockaddr_storage), buffer, sizeof(buffer), nullptr, 0, NI_NUMERICHOST);
            return UString::from_c_str(buffer, sizeof(buffer));
        }

        u16 port_of(const sockaddr_storage &address) {
            if (address.ss_family == AF_INET6) {
                return ntohs(reinterpret_cast<const sockaddr_in6 *>(&address)->sin6_port);
            }
            return ntohs(reinterpret_cast<const sockaddr_in *>(&address)->sin_port);
        }

    } // namespace

    struct UdpSocket::Impl {
        SocketHandle handle = kInvalidSocket;
        int family = AF_INET;
        u16 port = 0;
        // The destination of the last send, resolved once.
        UString cached_host;
        u16 cached_port = 0;
        sockaddr_storage cached_address{};
        socklen_t cached_length = 0;
        bool cached_valid = false;
        std::mutex send_mutex;
    };

    UdpSocket::UdpSocket(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    UdpSocket::~UdpSocket() {
        if (impl_ && impl_->handle != kInvalidSocket) {
            close_socket(impl_->handle);
        }
    }

    u16 UdpSocket::local_port() const noexcept { return impl_->port; }

    std::expected<std::unique_ptr<UdpSocket>, UString> UdpSocket::open(const UdpOptions &options) {
        ensure_socket_library();
        sockaddr_storage bind_address{};
        socklen_t bind_length = 0;
        if (!resolve(options.bind_address, options.bind_port, AF_UNSPEC, true, bind_address, bind_length)) {
            return std::unexpected(UString{std::format("network: cannot resolve the bind address '{}'", options.bind_address)});
        }
        auto impl = std::make_unique<Impl>();
        impl->family = bind_address.ss_family;
        impl->handle = ::socket(impl->family, SOCK_DGRAM, 0);
        if (impl->handle == kInvalidSocket) {
            return std::unexpected(UString{std::format("network: cannot create a socket ({})", socket_error())});
        }
        const auto set_int = [&](int level, int name, int value) {
            return ::setsockopt(impl->handle, level, name, reinterpret_cast<const char *>(&value), sizeof(value)) == 0;
        };
        if (options.reuse_address) {
            set_int(SOL_SOCKET, SO_REUSEADDR, 1);
#if defined(SO_REUSEPORT)
            set_int(SOL_SOCKET, SO_REUSEPORT, 1);
#endif
        }
        set_int(SOL_SOCKET, SO_RCVBUF, static_cast<int>(options.receive_buffer_bytes));
        if (::bind(impl->handle, reinterpret_cast<const sockaddr *>(&bind_address), bind_length) != 0) {
            const UString error = socket_error();
            close_socket(impl->handle);
            impl->handle = kInvalidSocket;
            return std::unexpected(UString{std::format("network: cannot bind {}:{} ({})", options.bind_address, options.bind_port, error)});
        }
        sockaddr_storage local{};
        socklen_t local_length = sizeof(local);
        if (::getsockname(impl->handle, reinterpret_cast<sockaddr *>(&local), &local_length) == 0) {
            impl->port = port_of(local);
        }

        if (impl->family == AF_INET) {
            set_int(IPPROTO_IP, IP_MULTICAST_TTL, static_cast<int>(options.multicast_ttl));
            set_int(IPPROTO_IP, IP_MULTICAST_LOOP, options.multicast_loopback ? 1 : 0);
            if (options.dscp != 0) {
                set_int(IPPROTO_IP, IP_TOS, static_cast<int>(options.dscp) << 2);
            }
        } else {
            set_int(IPPROTO_IPV6, IPV6_MULTICAST_HOPS, static_cast<int>(options.multicast_ttl));
            set_int(IPPROTO_IPV6, IPV6_MULTICAST_LOOP, options.multicast_loopback ? 1 : 0);
#if defined(IPV6_TCLASS)
            if (options.dscp != 0) {
                set_int(IPPROTO_IPV6, IPV6_TCLASS, static_cast<int>(options.dscp) << 2);
            }
#endif
        }

        if (!options.multicast_group.empty()) {
            sockaddr_storage group{};
            socklen_t group_length = 0;
            if (!resolve(options.multicast_group, 0, impl->family, false, group, group_length)) {
                close_socket(impl->handle);
                impl->handle = kInvalidSocket;
                return std::unexpected(UString{std::format("network: cannot resolve the multicast group '{}'", options.multicast_group)});
            }
            bool joined = false;
            if (impl->family == AF_INET) {
                ip_mreq request{};
                request.imr_multiaddr = reinterpret_cast<const sockaddr_in *>(&group)->sin_addr;
                request.imr_interface.s_addr = htonl(INADDR_ANY);
                joined = ::setsockopt(impl->handle, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char *>(&request), sizeof(request)) == 0;
            } else {
                ipv6_mreq request{};
                request.ipv6mr_multiaddr = reinterpret_cast<const sockaddr_in6 *>(&group)->sin6_addr;
                request.ipv6mr_interface = 0;
                joined = ::setsockopt(impl->handle, IPPROTO_IPV6, IPV6_JOIN_GROUP, reinterpret_cast<const char *>(&request), sizeof(request)) == 0;
            }
            if (!joined) {
                const UString error = socket_error();
                close_socket(impl->handle);
                impl->handle = kInvalidSocket;
                return std::unexpected(UString{std::format("network: cannot join multicast group {} ({})", options.multicast_group, error)});
            }
        }
        return std::unique_ptr<UdpSocket>(new UdpSocket(std::move(impl)));
    }

    bool UdpSocket::send_to(const NetAddress &destination, std::span<const u8> data) {
        Impl &m = *impl_;
        std::scoped_lock lock(m.send_mutex);
        if (!m.cached_valid || m.cached_host != destination.host || m.cached_port != destination.port) {
            if (!resolve(destination.host, destination.port, m.family, false, m.cached_address, m.cached_length)) {
                m.cached_valid = false;
                return false;
            }
            m.cached_host = destination.host;
            m.cached_port = destination.port;
            m.cached_valid = true;
        }
        const auto sent = ::sendto(m.handle, reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()), 0,
                                   reinterpret_cast<const sockaddr *>(&m.cached_address), m.cached_length);
        return sent == static_cast<decltype(sent)>(data.size());
    }

    std::optional<usize> UdpSocket::receive(std::span<u8> buffer, int timeout_ms, NetAddress *from) {
        Impl &m = *impl_;
#if defined(_WIN32)
        WSAPOLLFD poll_descriptor{m.handle, POLLRDNORM, 0};
        const int ready = WSAPoll(&poll_descriptor, 1, timeout_ms);
#else
        pollfd poll_descriptor{m.handle, POLLIN, 0};
        const int ready = ::poll(&poll_descriptor, 1, timeout_ms);
#endif
        if (ready <= 0) {
            return std::nullopt;
        }
        sockaddr_storage sender{};
        socklen_t sender_length = sizeof(sender);
        const auto got = ::recvfrom(m.handle, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0,
                                    reinterpret_cast<sockaddr *>(&sender), &sender_length);
        if (got < 0) {
            return std::nullopt;
        }
        if (from != nullptr) {
            from->host = numeric_host(sender);
            from->port = port_of(sender);
        }
        return static_cast<usize>(got);
    }

    // ================================================================================================================
    // Stream description / SDP
    // ================================================================================================================

    namespace {

        u32 pcm_bytes(RtpCodec codec) { return codec == RtpCodec::Pcm16 ? 2u : 3u; }

        bool is_multicast_address(const UString &address) {
            if (address.contains(U':')) {
                return address.starts_with("ff"_ustr) || address.starts_with("FF"_ustr);
            }
            const int first = number_or<int>(address, 0);
            return first >= 224 && first <= 239;
        }

    } // namespace

    u32 StreamDescription::frames_per_packet(u32 mtu_payload) const {
        if (codec == RtpCodec::Opus) {
            return static_cast<u32>(std::lround(opus_frame_ms * 48.0f));
        }
        const u32 by_time = std::max(1u, static_cast<u32>(std::lround(packet_ms * static_cast<f32>(sample_rate) / 1000.0f)));
        const u32 by_mtu = std::max(1u, mtu_payload / std::max(1u, channels * pcm_bytes(codec)));
        return std::min(by_time, by_mtu);
    }

    UString StreamDescription::to_sdp(const UString &address, u16 port) const {
        const bool v6 = address.contains(U':');
        std::ostringstream sdp;
        sdp << "v=0\r\n"
            << "o=- 0 0 IN " << (v6 ? "IP6" : "IP4") << ' ' << address.cpp_string_view() << "\r\n"
            << "s=" << name.cpp_string_view() << "\r\n"
            << "c=IN " << (v6 ? "IP6" : "IP4") << ' ' << address.cpp_string_view();
        if (is_multicast_address(address)) {
            sdp << (v6 ? "" : "/32");
        }
        sdp << "\r\nt=0 0\r\n"
            << "m=audio " << port << " RTP/AVP " << static_cast<int>(payload_type) << "\r\n";
        const u32 clock = codec == RtpCodec::Opus ? 48000u : sample_rate;
        const char *encoding = codec == RtpCodec::Pcm16 ? "L16" : codec == RtpCodec::Pcm24 ? "L24" : "opus";
        sdp << "a=rtpmap:" << static_cast<int>(payload_type) << ' ' << encoding << '/' << clock << '/' << channels << "\r\n";
        if (codec == RtpCodec::Opus) {
            // RFC 7587 describes mono and stereo; beyond that the layout travels in an extension attribute.
            sdp << "a=fmtp:" << static_cast<int>(payload_type) << " stereo=" << (channels == 2 ? 1 : 0) << ";sprop-stereo=" << (channels == 2 ? 1 : 0)
                << ";useinbandfec=1\r\n";
            if (channels > 2) {
                sdp << "a=x-opus-multistream:family=" << opus.family << ";streams=" << opus.streams << ";coupled=" << opus.coupled << ";mapping=";
                for (u32 c = 0; c < channels; ++c) {
                    sdp << (c ? "," : "") << static_cast<int>(opus.mapping[c]);
                }
                sdp << "\r\n";
            }
            sdp << "a=ptime:" << static_cast<int>(opus_frame_ms) << "\r\n";
        } else {
            sdp << "a=ptime:" << packet_ms << "\r\n";
        }
        // Channel meaning, so a receiver can fold a 5.1 feed correctly instead of treating it as six unrelated tracks.
        if (layout.kind == ChannelKind::Speakers && layout.channels == channels) {
            char mask[16];
            std::snprintf(mask, sizeof(mask), "0x%X", wave_channel_mask(layout.speakers));
            sdp << "a=x-channel-layout:speakers:" << mask << "\r\n";
        } else if (layout.kind == ChannelKind::Ambisonic) {
            sdp << "a=x-channel-layout:ambisonic:" << layout.ambisonic_order << "\r\n";
        }
        return UString{sdp.str()};
    }

    std::expected<StreamDescription, UString> StreamDescription::from_sdp(const ustr &description, UString *address, u16 *port) {
        StreamDescription stream;
        stream.layout = ChannelLayoutInfo{};
        bool have_media = false, have_rtpmap = false;
        UString connection;
        UString layout_attribute, multistream_attribute;
        f32 ptime = 0.0f;
        for (const UString &raw : split(description, '\n')) {
            const UString line = trim_end(raw);
            const auto value_of = [&line](const ustr &prefix) -> std::optional<UString> {
                if (!line.starts_with(prefix)) {
                    return std::nullopt;
                }
                return line.substr(prefix.size());
            };
            if (const auto name = value_of("s="_ustr)) {
                stream.name = *name;
            } else if (const auto c = value_of("c="_ustr)) {
                // c=IN IP4 239.69.1.1/32
                const auto parts = split(*c, ' ');
                if (parts.size() >= 3) {
                    connection = split(parts[2], '/')[0];
                }
            } else if (line.starts_with("m=audio"_ustr)) {
                // m=audio 5004 RTP/AVP 97
                const auto parts = split(line.substr(2), ' ');
                if (parts.size() >= 4) {
                    have_media = true;
                    if (port != nullptr) {
                        *port = number_or<u16>(parts[1], 0);
                    }
                    stream.payload_type = number_or<u8>(parts[3], 0);
                }
            } else if (const auto rtpmap = value_of("a=rtpmap:"_ustr); rtpmap && have_media && !have_rtpmap) {
                // a=rtpmap:97 L24/48000/8
                const usize space = rtpmap->find(std::string_view{" "});
                if (space == UString::npos) continue;
                const auto fields = split(rtpmap->substr(space + 1), '/');
                const UString encoding = ascii_lower(fields[0]);
                if (encoding == "l16"_ustr) stream.codec = RtpCodec::Pcm16;
                else if (encoding == "l24"_ustr) stream.codec = RtpCodec::Pcm24;
                else if (encoding == "opus"_ustr) stream.codec = RtpCodec::Opus;
                else return std::unexpected(UString{std::format("network: unsupported SDP encoding '{}' (supported: L16, L24, opus)", fields[0])});
                stream.sample_rate = fields.size() > 1 ? number_or<u32>(fields[1], 0) : 48000u;
                stream.channels = fields.size() > 2 ? number_or<u32>(fields[2], 0) : 1u;
                have_rtpmap = true;
            } else if (const auto p = value_of("a=ptime:"_ustr)) {
                ptime = number_or<f32>(*p, 0.0f);
            } else if (const auto attribute = value_of("a=x-channel-layout:"_ustr)) {
                layout_attribute = *attribute;
            } else if (const auto attribute = value_of("a=x-opus-multistream:"_ustr)) {
                multistream_attribute = *attribute;
            }
        }
        if (!have_media || !have_rtpmap || stream.channels == 0 || stream.channels > max_source_channels || stream.sample_rate == 0) {
            return std::unexpected("network: the session description has no usable audio stream");
        }
        if (address != nullptr) {
            *address = connection;
        }
        if (ptime > 0.0f) {
            (stream.codec == RtpCodec::Opus ? stream.opus_frame_ms : stream.packet_ms) = ptime;
        }
        if (stream.codec == RtpCodec::Opus) {
            stream.sample_rate = 48000;
            stream.opus.family = stream.channels <= 2 ? 0 : 255;
            stream.opus.streams = stream.channels == 2 ? 1 : (stream.channels == 1 ? 1 : static_cast<int>(stream.channels));
            stream.opus.coupled = stream.channels == 2 ? 1 : 0;
            if (stream.channels <= 2) {
                stream.opus.mapping[0] = 0;
                stream.opus.mapping[1] = 1;
            }
            for (const UString &field : split(multistream_attribute, ';')) {
                const usize eq = field.find(std::string_view{"="});
                if (eq == UString::npos) continue;
                const UString key = field.substr(0, eq), value = field.substr(eq + 1);
                if (key == "family"_ustr) stream.opus.family = number_or<int>(value, 0);
                else if (key == "streams"_ustr) stream.opus.streams = number_or<int>(value, 0);
                else if (key == "coupled"_ustr) stream.opus.coupled = number_or<int>(value, 0);
                else if (key == "mapping"_ustr) {
                    const auto entries = split(value, ',');
                    for (const auto [slot, entry] : std::views::zip(stream.opus.mapping, entries)) {
                        slot = number_or<u8>(entry, 0);
                    }
                }
            }
        }
        // Channel meaning: our own attribute, else the conventional one for the count (AES67 gear sends 8 channels as 7.1 in
        // SMPTE order).
        if (layout_attribute.starts_with("speakers:"_ustr)) {
            u32 mask = 0;
            const std::string_view hex = layout_attribute.cpp_string_view().substr(9);
            std::from_chars(hex.data(), hex.data() + hex.size(), mask, 16);
            stream.layout = layout_from_wave_mask(mask, stream.channels);
        } else if (layout_attribute.starts_with("ambisonic:"_ustr)) {
            stream.layout = ChannelLayoutInfo::ambisonic(number_or<u32>(layout_attribute.substr(10), 0));
            if (stream.layout.channels != stream.channels) {
                stream.layout = ChannelLayoutInfo::discrete(stream.channels);
            }
        } else {
            stream.layout = stream.channels <= 2 ? ChannelLayoutInfo::guess(stream.channels) : ChannelLayoutInfo::discrete(stream.channels);
        }
        return stream;
    }

    // ================================================================================================================
    // Packetizer
    // ================================================================================================================

    struct RtpPacketizer::Impl {
        StreamDescription stream;
        PacketSink sink;
        u32 ssrc = 0;
        u16 sequence = 0;
        u32 timestamp = 0;
        bool first = true;
        u32 frames_per_packet = 0;
        std::vector<f32> pending;     // interleaved
        usize pending_start = 0;      // read offset into `pending`, in floats
        std::vector<u8> packet;
        std::unique_ptr<OpusEncoderHandle> opus;
        std::atomic<u64> packet_count{0}, byte_count{0};

        void write_header(u8 *out, bool marker) const {
            out[0] = 0x80;
            out[1] = static_cast<u8>((marker ? 0x80 : 0x00) | (stream.payload_type & 0x7F));
            out[2] = static_cast<u8>(sequence >> 8);
            out[3] = static_cast<u8>(sequence & 0xFF);
            out[4] = static_cast<u8>(timestamp >> 24);
            out[5] = static_cast<u8>(timestamp >> 16);
            out[6] = static_cast<u8>(timestamp >> 8);
            out[7] = static_cast<u8>(timestamp);
            out[8] = static_cast<u8>(ssrc >> 24);
            out[9] = static_cast<u8>(ssrc >> 16);
            out[10] = static_cast<u8>(ssrc >> 8);
            out[11] = static_cast<u8>(ssrc);
        }

        // Emits one packet from `frames` interleaved frames.
        bool emit(const f32 *frames_data, u32 frames) {
            const u32 channels = stream.channels;
            usize size = 12;
            if (stream.codec == RtpCodec::Opus) {
#if STURDY_AUDIO_OPUS
                const int bytes = opus->encode(frames_data, packet.data() + 12, packet.size() - 12);
                if (bytes < 0) {
                    return false;
                }
                size += static_cast<usize>(bytes);
#else
                return false;
#endif
            } else if (stream.codec == RtpCodec::Pcm16) {
                u8 *out = packet.data() + 12;
                for (usize i = 0; i < static_cast<usize>(frames) * channels; ++i) {
                    const i32 v = static_cast<i32>(std::lrint(std::clamp(frames_data[i], -1.0f, 1.0f) * 32767.0f));
                    out[2 * i] = static_cast<u8>((v >> 8) & 0xFF);
                    out[2 * i + 1] = static_cast<u8>(v & 0xFF);
                }
                size += static_cast<usize>(frames) * channels * 2;
            } else {
                u8 *out = packet.data() + 12;
                for (usize i = 0; i < static_cast<usize>(frames) * channels; ++i) {
                    const i32 v = static_cast<i32>(std::lrint(std::clamp(frames_data[i], -1.0f, 1.0f) * 8388607.0f));
                    out[3 * i] = static_cast<u8>((v >> 16) & 0xFF);
                    out[3 * i + 1] = static_cast<u8>((v >> 8) & 0xFF);
                    out[3 * i + 2] = static_cast<u8>(v & 0xFF);
                }
                size += static_cast<usize>(frames) * channels * 3;
            }
            write_header(packet.data(), first);
            first = false;
            sink(std::span<const u8>(packet.data(), size));
            ++sequence;
            timestamp += frames;
            packet_count.fetch_add(1, std::memory_order_relaxed);
            byte_count.fetch_add(size, std::memory_order_relaxed);
            return true;
        }
    };

    RtpPacketizer::RtpPacketizer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    RtpPacketizer::~RtpPacketizer() = default;

    std::expected<std::unique_ptr<RtpPacketizer>, UString> RtpPacketizer::create(const StreamDescription &requested, PacketSink sink, u32 ssrc) {
        auto impl = std::make_unique<Impl>();
        impl->stream = requested;
        StreamDescription &stream = impl->stream;
        if (stream.channels == 0 || stream.channels > max_source_channels) {
            return std::unexpected("network: a stream carries 1 to 255 channels");
        }
        if (stream.layout.channels != stream.channels) {
            stream.layout = stream.channels <= 2 ? ChannelLayoutInfo::guess(stream.channels) : ChannelLayoutInfo::discrete(stream.channels);
        }
        if (stream.codec == RtpCodec::Opus) {
#if STURDY_AUDIO_OPUS
            stream.sample_rate = 48000;
            OpusEncoderSettings settings;
            settings.channels = stream.channels;
            settings.layout = stream.layout;
            settings.application = stream.opus_application;
            settings.frame_ms = stream.opus_frame_ms;
            settings.bitrate_bps = stream.opus_bitrate_bps;
            settings.variable_bitrate = true;
            auto encoder = OpusEncoderHandle::create(settings);
            if (!encoder) {
                return std::unexpected(encoder.error());
            }
            impl->opus = std::move(*encoder);
            stream.opus = impl->opus->stream_layout();
            impl->packet.resize(12 + static_cast<usize>(std::max(stream.opus.streams, 1)) * 1500);
#else
            return std::unexpected("network: this build has no Opus codec");
#endif
        }
        if (stream.codec == RtpCodec::Opus) {
#if STURDY_AUDIO_OPUS
            impl->frames_per_packet = impl->opus->frame_size();
#endif
        } else {
            impl->frames_per_packet = stream.frames_per_packet();
            impl->packet.resize(12 + static_cast<usize>(impl->frames_per_packet) * stream.channels * pcm_bytes(stream.codec));
        }
        impl->sink = std::move(sink);
        std::random_device random;
        impl->ssrc = ssrc != 0 ? ssrc : (random() | 1u);
        impl->sequence = static_cast<u16>(random());
        impl->timestamp = random();
        return std::unique_ptr<RtpPacketizer>(new RtpPacketizer(std::move(impl)));
    }

    bool RtpPacketizer::write(const f32 *interleaved, usize frames) {
        Impl &m = *impl_;
        const u32 channels = m.stream.channels;
        m.pending.insert(m.pending.end(), interleaved, interleaved + frames * channels);
        const usize packet_samples = static_cast<usize>(m.frames_per_packet) * channels;
        while (m.pending.size() - m.pending_start >= packet_samples) {
            if (!m.emit(m.pending.data() + m.pending_start, m.frames_per_packet)) {
                return false;
            }
            m.pending_start += packet_samples;
        }
        if (m.pending_start > 0 && m.pending_start >= m.pending.size() / 2) {
            m.pending.erase(m.pending.begin(), m.pending.begin() + static_cast<std::ptrdiff_t>(m.pending_start));
            m.pending_start = 0;
        }
        return true;
    }

    void RtpPacketizer::flush() {
        Impl &m = *impl_;
        const usize left = m.pending.size() - m.pending_start;
        if (left == 0) {
            return;
        }
        const u32 channels = m.stream.channels;
        if (m.stream.codec == RtpCodec::Opus) {
            m.pending.resize(m.pending_start + static_cast<usize>(m.frames_per_packet) * channels, 0.0f); // a codec frame is fixed length
            m.emit(m.pending.data() + m.pending_start, m.frames_per_packet);
        } else {
            m.emit(m.pending.data() + m.pending_start, static_cast<u32>(left / channels));
        }
        m.pending.clear();
        m.pending_start = 0;
    }

    u64 RtpPacketizer::packets() const noexcept { return impl_->packet_count.load(std::memory_order_relaxed); }
    u64 RtpPacketizer::bytes() const noexcept { return impl_->byte_count.load(std::memory_order_relaxed); }
    u32 RtpPacketizer::ssrc() const noexcept { return impl_->ssrc; }
    u32 RtpPacketizer::rtp_timestamp() const noexcept { return impl_->timestamp; }
    const StreamDescription &RtpPacketizer::stream() const noexcept { return impl_->stream; }

    // ================================================================================================================
    // Stream decoder: jitter buffer, concealment, decode
    // ================================================================================================================

    struct RtpStreamDecoder::Impl {
        static constexpr usize kSlots = 256;
        struct Slot {
            bool valid = false;
            u16 sequence = 0;
            u32 timestamp = 0;
            Clock::time_point arrival{};
            u16 length = 0;
            std::array<u8, 1500> payload{};
        };

        StreamDescription stream;
        RtpDecoderOptions options;
        std::shared_ptr<LiveSource> source;
        std::mutex mutex;
        std::vector<Slot> slots = std::vector<Slot>(kSlots);

        bool started = false;
        u32 locked_ssrc = 0;
        u16 expected = 0;
        // Sequence bookkeeping for RTCP reports (RFC 3550 A.1).
        bool seq_started = false;
        u16 max_sequence = 0;
        u32 sequence_cycles = 0;
        u32 base_extended = 0;
        u64 prev_expected_total = 0, prev_received_total = 0;
        Clock::time_point last_packet{};
        RtpStats stats;
        // Interarrival jitter state (RFC 3550 6.4.1).
        bool have_transit = false;
        f64 last_transit = 0.0;
        f64 jitter = 0.0;

        std::unique_ptr<OpusDecoderHandle> opus;
        std::vector<f32> pcm;
        u32 last_frames = 0;

        [[nodiscard]] u32 clock_rate() const { return stream.codec == RtpCodec::Opus ? 48000u : stream.sample_rate; }

        void decode_payload(const u8 *data, usize length) {
            const u32 channels = stream.channels;
            if (stream.codec == RtpCodec::Opus) {
#if STURDY_AUDIO_OPUS
                const int frames = opus->decode(data, length, pcm.data());
                if (frames > 0) {
                    source->push(pcm.data(), static_cast<usize>(frames));
                    last_frames = static_cast<u32>(frames);
                }
#endif
                return;
            }
            const u32 width = pcm_bytes(stream.codec);
            const usize frames = length / (static_cast<usize>(channels) * width);
            if (frames == 0) {
                return;
            }
            pcm.resize(std::max(pcm.size(), frames * channels));
            if (stream.codec == RtpCodec::Pcm16) {
                for (usize i = 0; i < frames * channels; ++i) {
                    const i16 v = static_cast<i16>((static_cast<u16>(data[2 * i]) << 8) | data[2 * i + 1]);
                    pcm[i] = static_cast<f32>(v) / 32768.0f;
                }
            } else {
                for (usize i = 0; i < frames * channels; ++i) {
                    const i32 v = (static_cast<i32>((static_cast<u32>(data[3 * i]) << 24) | (static_cast<u32>(data[3 * i + 1]) << 16) |
                                                    (static_cast<u32>(data[3 * i + 2]) << 8))) >> 8;
                    pcm[i] = static_cast<f32>(v) / 8388608.0f;
                }
            }
            source->push(pcm.data(), frames);
            last_frames = static_cast<u32>(frames);
        }

        void conceal() {
            ++stats.packets_lost;
            const u32 channels = stream.channels;
            if (stream.codec == RtpCodec::Opus) {
#if STURDY_AUDIO_OPUS
                const u32 frames_wanted = last_frames != 0 ? last_frames : stream.frames_per_packet();
                const int frames = opus->decode(nullptr, 0, pcm.data(), frames_wanted);
                if (frames > 0) {
                    source->push(pcm.data(), static_cast<usize>(frames));
                    stats.concealed_frames += static_cast<u64>(frames);
                }
#endif
                return;
            }
            const u32 frames = last_frames != 0 ? last_frames : stream.frames_per_packet();
            pcm.assign(std::max(pcm.size(), static_cast<usize>(frames) * channels), 0.0f);
            std::fill(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(static_cast<usize>(frames) * channels), 0.0f);
            source->push(pcm.data(), frames);
            stats.concealed_frames += frames;
        }

        // Plays everything that is next in order, and gives up on a missing packet once a later one has waited out the jitter window.
        void release(Clock::time_point now) {
            const auto window = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<f32, std::milli>(options.jitter_ms));
            for (;;) {
                Slot &next = slots[expected % kSlots];
                if (next.valid && next.sequence == expected) {
                    decode_payload(next.payload.data(), next.length);
                    next.valid = false;
                    ++expected;
                    continue;
                }
                // Missing. Is a later packet waiting, and for how long?
                bool later = false;
                Clock::time_point oldest = now;
                for (usize k = 1; k < kSlots / 2; ++k) {
                    const Slot &slot = slots[(expected + k) % kSlots];
                    if (slot.valid && static_cast<u16>(slot.sequence - expected) == k) {
                        later = true;
                        oldest = std::min(oldest, slot.arrival);
                    }
                }
                if (later && now - oldest >= window) {
                    conceal();
                    ++expected;
                    continue;
                }
                break;
            }
        }
    };

    RtpStreamDecoder::RtpStreamDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    RtpStreamDecoder::~RtpStreamDecoder() = default;

    std::expected<std::unique_ptr<RtpStreamDecoder>, UString> RtpStreamDecoder::create(const StreamDescription &stream, u32 engine_rate, const RtpDecoderOptions &options,
                                                                                             f32 cushion_ms, f32 buffer_seconds) {
        if (stream.channels == 0 || stream.channels > max_source_channels) {
            return std::unexpected("network: a stream carries 1 to 255 channels");
        }
        auto impl = std::make_unique<Impl>();
        impl->stream = stream;
        impl->options = options;
        if (impl->stream.layout.channels != stream.channels) {
            impl->stream.layout = stream.channels <= 2 ? ChannelLayoutInfo::guess(stream.channels) : ChannelLayoutInfo::discrete(stream.channels);
        }
        if (stream.codec == RtpCodec::Opus) {
#if STURDY_AUDIO_OPUS
            impl->stream.sample_rate = 48000;
            auto decoder = OpusDecoderHandle::create(stream.channels, stream.opus);
            if (!decoder) {
                return std::unexpected(decoder.error());
            }
            impl->opus = std::move(*decoder);
            impl->pcm.resize(static_cast<usize>(OpusDecoderHandle::max_frame_samples) * stream.channels);
#else
            return std::unexpected("network: this build has no Opus codec");
#endif
        }
        const u32 rate = impl->stream.sample_rate;
        LiveSourceOptions live;
        live.prefill_frames = static_cast<u32>(std::max(cushion_ms, 0.0f) * static_cast<f32>(rate) / 1000.0f);
        // A live cushion bounds latency: a stalled consumer drops old audio instead of playing it late. A zero cushion means the
        // caller wants every frame (offline use, tests), so nothing is dropped.
        live.max_latency_frames = live.prefill_frames == 0 ? 0 : live.prefill_frames * 8 + static_cast<u32>(0.25 * rate);
        live.adaptive = live.prefill_frames > 0;
        impl->source = std::make_shared<LiveSource>(stream.channels, rate, engine_rate, static_cast<usize>(std::max(buffer_seconds, 0.2f) * static_cast<f32>(rate)), live);
        impl->source->set_layout(impl->stream.layout);
        return std::unique_ptr<RtpStreamDecoder>(new RtpStreamDecoder(std::move(impl)));
    }

    void RtpStreamDecoder::push_packet(std::span<const u8> packet, Clock::time_point arrival) {
        Impl &m = *impl_;
        std::scoped_lock lock(m.mutex);
        // Header: 12 bytes + CSRC list + optional extension; the last byte says how much padding to drop.
        if (packet.size() < 12 || (packet[0] >> 6) != 2) {
            ++m.stats.packets_invalid;
            return;
        }
        const u8 payload_type = packet[1] & 0x7F;
        if (payload_type != m.stream.payload_type) {
            ++m.stats.packets_invalid;
            return;
        }
        const u16 sequence = static_cast<u16>((packet[2] << 8) | packet[3]);
        const u32 timestamp = (static_cast<u32>(packet[4]) << 24) | (static_cast<u32>(packet[5]) << 16) | (static_cast<u32>(packet[6]) << 8) | packet[7];
        const u32 ssrc = (static_cast<u32>(packet[8]) << 24) | (static_cast<u32>(packet[9]) << 16) | (static_cast<u32>(packet[10]) << 8) | packet[11];
        usize offset = 12 + 4 * static_cast<usize>(packet[0] & 0x0F);
        if ((packet[0] & 0x10) != 0) {
            if (packet.size() < offset + 4) {
                ++m.stats.packets_invalid;
                return;
            }
            offset += 4 + 4 * static_cast<usize>((packet[offset + 2] << 8) | packet[offset + 3]);
        }
        usize end = packet.size();
        if ((packet[0] & 0x20) != 0 && end > offset) {
            end -= std::min<usize>(packet[end - 1], end - offset);
        }
        if (offset >= end || end - offset > 1500) {
            ++m.stats.packets_invalid;
            return;
        }

        // Which sender: lock to the first (or the configured one); a new sender takes over after a second of silence.
        if (m.options.ssrc != 0 && ssrc != m.options.ssrc) {
            ++m.stats.packets_invalid;
            return;
        }
        if (m.locked_ssrc == 0 || (ssrc != m.locked_ssrc && arrival - m.last_packet > std::chrono::seconds(1))) {
            m.locked_ssrc = ssrc;
            m.started = false;
            m.seq_started = false;
            for (auto &slot : m.slots) slot.valid = false;
            m.have_transit = false;
        } else if (ssrc != m.locked_ssrc) {
            ++m.stats.packets_invalid;
            return;
        }
        m.last_packet = arrival;
        ++m.stats.packets_received;
        if (!m.seq_started || ssrc != m.locked_ssrc) {
            m.seq_started = true;
            m.max_sequence = sequence;
            m.sequence_cycles = 0;
            m.base_extended = sequence;
            m.prev_expected_total = m.prev_received_total = 0;
        } else if (static_cast<i16>(sequence - m.max_sequence) > 0) {
            if (sequence < m.max_sequence) m.sequence_cycles += 0x10000;
            m.max_sequence = sequence;
        }

        // Interarrival jitter (RFC 3550): variation of (arrival time - media time) between packets.
        const f64 arrival_units = std::chrono::duration<f64>(arrival.time_since_epoch()).count() * static_cast<f64>(m.clock_rate());
        const f64 transit = arrival_units - static_cast<f64>(timestamp);
        if (m.have_transit) {
            const f64 d = std::fabs(transit - m.last_transit);
            m.jitter += (d - m.jitter) / 16.0;
            m.stats.jitter_ms = m.jitter * 1000.0 / static_cast<f64>(m.clock_rate());
        }
        m.last_transit = transit;
        m.have_transit = true;

        if (!m.started) {
            m.started = true;
            m.expected = sequence;
        }
        const i16 delta = static_cast<i16>(sequence - m.expected);
        if (delta < 0) {
            // Behind the playout point: a duplicate of something played, or a packet we gave up on.
            ++(delta > -static_cast<i16>(Impl::kSlots / 2) ? m.stats.packets_late : m.stats.packets_duplicate);
            if (delta <= -static_cast<i16>(Impl::kSlots / 2)) {
                // Far behind means the sender restarted its sequence numbers: begin again from here.
                for (auto &slot : m.slots) slot.valid = false;
                m.expected = sequence;
            } else {
                return;
            }
        } else if (delta >= static_cast<i16>(Impl::kSlots / 2)) {
            // A jump far ahead: resynchronise rather than conceal hundreds of packets.
            for (auto &slot : m.slots) slot.valid = false;
            m.expected = sequence;
        }
        Impl::Slot &slot = m.slots[sequence % Impl::kSlots];
        if (slot.valid && slot.sequence == sequence) {
            ++m.stats.packets_duplicate;
            return;
        }
        if (delta > 0) {
            ++m.stats.packets_reordered; // counted when it is ahead of the expected one (it may yet be filled in behind)
        }
        slot.valid = true;
        slot.sequence = sequence;
        slot.timestamp = timestamp;
        slot.arrival = arrival;
        slot.length = static_cast<u16>(end - offset);
        std::memcpy(slot.payload.data(), packet.data() + offset, slot.length);
        m.release(arrival);
    }

    void RtpStreamDecoder::tick(Clock::time_point now) {
        Impl &m = *impl_;
        std::scoped_lock lock(m.mutex);
        if (m.started) {
            m.release(now);
        }
    }

    std::shared_ptr<LiveSource> RtpStreamDecoder::source() const { return impl_->source; }
    const StreamDescription &RtpStreamDecoder::stream() const noexcept { return impl_->stream; }

    RtcpReportBlock RtpStreamDecoder::report_block() const {
        Impl &m = *impl_;
        std::scoped_lock lock(m.mutex);
        RtcpReportBlock block;
        if (!m.seq_started || m.locked_ssrc == 0) {
            return block;
        }
        block.ssrc = m.locked_ssrc;
        block.highest_sequence = m.sequence_cycles + m.max_sequence;
        const u64 expected_total = static_cast<u64>(block.highest_sequence - m.base_extended) + 1;
        const u64 received_total = m.stats.packets_received;
        block.cumulative_lost = static_cast<i32>(std::clamp<i64>(static_cast<i64>(expected_total) - static_cast<i64>(received_total), -0x800000, 0x7FFFFF));
        const i64 expected_interval = static_cast<i64>(expected_total - m.prev_expected_total);
        const i64 lost_interval = expected_interval - static_cast<i64>(received_total - m.prev_received_total);
        block.fraction_lost = expected_interval <= 0 || lost_interval <= 0 ? 0 : static_cast<u8>(std::min<i64>(255, (lost_interval << 8) / expected_interval));
        m.prev_expected_total = expected_total;
        m.prev_received_total = received_total;
        block.jitter = static_cast<u32>(m.stats.jitter_ms * 0.001 * static_cast<f64>(m.clock_rate()));
        return block;
    }

    RtpStats RtpStreamDecoder::stats() const {
        std::scoped_lock lock(impl_->mutex);
        return impl_->stats;
    }

    // ================================================================================================================
    // Sender / receiver
    // ================================================================================================================

    struct RtpSender::Impl {
        std::unique_ptr<UdpSocket> socket;
        std::unique_ptr<RtpPacketizer> packetizer;
        RtpSenderConfig config;
        std::chrono::steady_clock::time_point paced_start{};
        u64 paced_frames = 0;
        bool paced_started = false;
        // RTCP: sender reports out, receiver reports in.
        std::unique_ptr<UdpSocket> rtcp;
        UString cname;
        std::chrono::steady_clock::time_point next_report{};
        mutable std::mutex peer_mutex;
        RtpPeerStats peer;

        void service_rtcp() {
            if (!rtcp) return;
            std::array<u8, 1500> buffer{};
            while (const auto got = rtcp->receive(buffer, 0)) {
                const auto compound = parse_rtcp(std::span<const u8>(buffer.data(), *got));
                if (!compound) continue;
                const u64 arrival = ntp_now();
                std::scoped_lock lock(peer_mutex);
                for (const RtcpReceiverReport &report : compound->receiver_reports) {
                    for (const RtcpReportBlock &block : report.blocks) {
                        if (block.ssrc != packetizer->ssrc()) continue;
                        ++peer.reports_received;
                        peer.fraction_lost = static_cast<f32>(block.fraction_lost) / 256.0f;
                        peer.packets_lost = block.cumulative_lost;
                        peer.jitter_ms = 1000.0 * static_cast<f64>(block.jitter) / static_cast<f64>(std::max(1u, packetizer->stream().sample_rate));
                        if (const auto rtt = round_trip_seconds(block, arrival)) peer.round_trip_ms = *rtt * 1000.0;
                    }
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now < next_report || packetizer->packets() == 0) return;
            next_report = now + std::chrono::seconds(1);
            RtcpSenderReport report;
            report.ssrc = packetizer->ssrc();
            report.ntp_timestamp = ntp_now();
            report.rtp_timestamp = packetizer->rtp_timestamp();
            report.packet_count = static_cast<u32>(packetizer->packets());
            report.octet_count = static_cast<u32>(packetizer->bytes());
            NetAddress target = config.destination;
            target.port = static_cast<u16>(target.port + 1);
            rtcp->send_to(target, build_sender_report_packet(report, cname));
        }
    };

    RtpSender::RtpSender(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    RtpSender::~RtpSender() {
        if (impl_ && impl_->packetizer) {
            impl_->packetizer->flush();
        }
    }

    std::expected<std::unique_ptr<RtpSender>, UString> RtpSender::create(const RtpSenderConfig &config) {
        auto impl = std::make_unique<Impl>();
        impl->config = config;
        UdpOptions udp;
        udp.bind_port = config.local_port;
        udp.reuse_address = false;
        udp.dscp = config.dscp;
        udp.multicast_ttl = config.multicast_ttl;
        const bool v6 = config.destination.host.contains(U':');
        udp.bind_address = v6 ? "::" : "0.0.0.0";
        auto socket = UdpSocket::open(udp);
        if (!socket) {
            return std::unexpected(socket.error());
        }
        impl->socket = std::move(*socket);
        Impl *raw = impl.get();
        auto packetizer = RtpPacketizer::create(
            config.stream, [raw](std::span<const u8> packet) { raw->socket->send_to(raw->config.destination, packet); }, config.ssrc);
        if (!packetizer) {
            return std::unexpected(packetizer.error());
        }
        impl->packetizer = std::move(*packetizer);
        // RTCP rides a socket of its own (any port): receivers answer to wherever the sender report came from.
        UdpOptions control = udp;
        control.bind_port = 0;
        if (auto rtcp = UdpSocket::open(control)) {
            impl->rtcp = std::move(*rtcp);
        }
        impl->cname = UString{std::format("sturdy-{:08x}@{}", impl->packetizer->ssrc(), config.destination.host)};
        return std::unique_ptr<RtpSender>(new RtpSender(std::move(impl)));
    }

    bool RtpSender::write(const f32 *interleaved, usize frames) {
        Impl &m = *impl_;
        if (m.config.paced) {
            // Keep to the media clock: do not let the first packet of a burst leave before its time.
            const auto now = std::chrono::steady_clock::now();
            if (!m.paced_started) {
                m.paced_start = now;
                m.paced_started = true;
            }
            const f64 due = static_cast<f64>(m.paced_frames) / static_cast<f64>(m.packetizer->stream().sample_rate);
            const f64 elapsed = std::chrono::duration<f64>(now - m.paced_start).count();
            if (due > elapsed + 0.020) {
                std::this_thread::sleep_for(std::chrono::duration<f64>(due - elapsed - 0.010));
            }
            m.paced_frames += frames;
        }
        const bool ok = m.packetizer->write(interleaved, frames);
        m.service_rtcp();
        return ok;
    }

    RtpPeerStats RtpSender::peer_stats() const {
        std::scoped_lock lock(impl_->peer_mutex);
        return impl_->peer;
    }

    void RtpSender::flush() { impl_->packetizer->flush(); }
    u64 RtpSender::packets_sent() const noexcept { return impl_->packetizer->packets(); }
    u64 RtpSender::bytes_sent() const noexcept { return impl_->packetizer->bytes(); }
    const StreamDescription &RtpSender::stream() const noexcept { return impl_->packetizer->stream(); }
    UString RtpSender::sdp() const { return stream().to_sdp(impl_->config.destination.host, impl_->config.destination.port); }

    struct RtpReceiver::Impl {
        std::unique_ptr<UdpSocket> socket;
        std::unique_ptr<RtpStreamDecoder> decoder;
        std::thread worker;
        std::atomic<bool> stop{false};
        // RTCP (port + 1): sender reports in, receiver reports out.
        std::unique_ptr<UdpSocket> rtcp;
        UString cname;
        u32 own_ssrc = 0;
        mutable std::mutex info_mutex;
        RtpSenderInfo info;
        NetAddress sender_address;
        bool have_sender = false;
        u32 last_sr_middle = 0;
        std::chrono::steady_clock::time_point last_sr_arrival{};
        std::chrono::steady_clock::time_point next_report{};

        void service_rtcp() {
            if (!rtcp) return;
            std::array<u8, 1500> buffer{};
            NetAddress from;
            while (const auto got = rtcp->receive(buffer, 0, &from)) {
                const auto compound = parse_rtcp(std::span<const u8>(buffer.data(), *got));
                if (!compound) continue;
                std::scoped_lock lock(info_mutex);
                for (const RtcpSenderReport &report : compound->sender_reports) {
                    ++info.reports_received;
                    info.packets_sent = report.packet_count;
                    info.octets_sent = report.octet_count;
                    info.ntp_timestamp = report.ntp_timestamp;
                    last_sr_middle = ntp_middle(report.ntp_timestamp);
                    last_sr_arrival = std::chrono::steady_clock::now();
                    sender_address = from;
                    have_sender = true;
                }
                for (const auto &[ssrc, name] : compound->cnames) info.cname = name;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now < next_report) return;
            next_report = now + std::chrono::seconds(1);
            NetAddress target;
            RtcpReceiverReport report;
            report.ssrc = own_ssrc;
            RtcpReportBlock block = decoder->report_block();
            {
                std::scoped_lock lock(info_mutex);
                if (!have_sender || block.ssrc == 0) return;
                block.last_sender_report = last_sr_middle;
                block.delay_since_sr = static_cast<u32>(std::chrono::duration<f64>(now - last_sr_arrival).count() * 65536.0);
                target = sender_address;
            }
            report.blocks.push_back(block);
            rtcp->send_to(target, build_receiver_report_packet(report, cname));
        }

        void run() {
            RealtimeOptions options;
            options.raise_priority = true;
            prepare_realtime_thread(options);
            std::array<u8, 2048> buffer{};
            while (!stop.load(std::memory_order_acquire)) {
                if (const auto got = socket->receive(buffer, 5)) {
                    decoder->push_packet(std::span<const u8>(buffer.data(), *got));
                }
                decoder->tick();
                service_rtcp();
            }
            if (rtcp && have_sender) {
                rtcp->send_to(sender_address, build_goodbye_packet(own_ssrc));
            }
        }
    };

    RtpReceiver::RtpReceiver(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    RtpReceiver::~RtpReceiver() {
        if (impl_) {
            impl_->stop.store(true, std::memory_order_release);
            if (impl_->worker.joinable()) {
                impl_->worker.join();
            }
        }
    }

    std::expected<std::unique_ptr<RtpReceiver>, UString> RtpReceiver::create(const RtpReceiverConfig &config) {
        auto impl = std::make_unique<Impl>();
        UdpOptions udp;
        udp.bind_address = config.bind_address;
        udp.bind_port = config.port;
        udp.multicast_group = config.multicast_group;
        udp.reuse_address = !config.multicast_group.empty();
        auto socket = UdpSocket::open(udp);
        if (!socket) {
            return std::unexpected(socket.error());
        }
        impl->socket = std::move(*socket);
        auto decoder = RtpStreamDecoder::create(config.stream, config.engine_rate, config.decoder, config.cushion_ms);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        impl->decoder = std::move(*decoder);
        {
            // RTCP on the next port up, when it is free; without it the stream works the same, just unreported.
            UdpOptions control = udp;
            control.bind_port = static_cast<u16>(impl->socket->local_port() + 1);
            if (auto rtcp = UdpSocket::open(control)) {
                impl->rtcp = std::move(*rtcp);
            }
            std::random_device entropy;
            impl->own_ssrc = entropy();
            impl->cname = UString{std::format("sturdy-rx-{:08x}", impl->own_ssrc)};
        }
        Impl *raw = impl.get();
        impl->worker = std::thread([raw] { raw->run(); });
        return std::unique_ptr<RtpReceiver>(new RtpReceiver(std::move(impl)));
    }

    std::shared_ptr<LiveSource> RtpReceiver::source() const { return impl_->decoder->source(); }
    RtpStats RtpReceiver::stats() const { return impl_->decoder->stats(); }
    RtpSenderInfo RtpReceiver::sender_info() const {
        std::scoped_lock lock(impl_->info_mutex);
        return impl_->info;
    }
    u16 RtpReceiver::port() const noexcept { return impl_->socket->local_port(); }
    const StreamDescription &RtpReceiver::stream() const noexcept { return impl_->decoder->stream(); }

    namespace {
        /// An `RtpSender` as a sink, so the network output rides the same pump as every other sink.
        class RtpSink final : public AudioSink {
          public:
            explicit RtpSink(std::unique_ptr<RtpSender> sender) : sender_(std::move(sender)) {}
            void write(std::span<const f32> interleaved, u32 frames) override { sender_->write(interleaved.data(), frames); }
            [[nodiscard]] RtpSender &sender() { return *sender_; }

          private:
            std::unique_ptr<RtpSender> sender_;
        };
    } // namespace

    struct NetworkOutputSink::Impl {
        std::shared_ptr<RtpSink> sink;
        std::unique_ptr<SinkPump> pump;
    };

    NetworkOutputSink::NetworkOutputSink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    NetworkOutputSink::~NetworkOutputSink() = default;

    std::expected<std::unique_ptr<NetworkOutputSink>, UString> NetworkOutputSink::start(AudioEngine &engine, OutputId output, RtpSenderConfig config) {
        if (output == primary_output || output >= engine.output_count()) {
            return std::unexpected("network: send a secondary engine output (the primary one drives the clock); add one to the engine's output list");
        }
        const OutputDesc &desc = engine.config().outputs[output];
        const SpeakerLayout layout = desc.kind == OutputDesc::Kind::Binaural ? SpeakerLayout::stereo() : desc.layout;
        config.stream.channels = layout.channel_count();
        config.stream.sample_rate = engine.config().sample_rate;
        config.stream.layout = ChannelLayoutInfo::from_speakers(layout);
        auto sender = RtpSender::create(config);
        if (!sender) {
            return std::unexpected(sender.error());
        }
        auto impl = std::make_unique<Impl>();
        impl->sink = std::make_shared<RtpSink>(std::move(*sender));
        auto pump = SinkPump::start(engine, output, impl->sink);
        if (!pump) {
            return std::unexpected(pump.error());
        }
        impl->pump = std::move(*pump);
        return std::unique_ptr<NetworkOutputSink>(new NetworkOutputSink(std::move(impl)));
    }

    u64 NetworkOutputSink::packets_sent() const noexcept { return impl_->sink->sender().packets_sent(); }
    UString NetworkOutputSink::sdp() const { return impl_->sink->sender().sdp(); }

} // namespace SFT::Audio
