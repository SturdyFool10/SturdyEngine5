#pragma once

#include <Foundation/Foundation.hpp>

#include <string>

#include <chrono>
#include <optional>
#include <span>
#include <vector>

/// RTCP (RFC 3550 section 6): the control stream that runs beside RTP. Senders say what they have sent and when (sender
/// reports, which let a receiver line the stream up with a wall clock), receivers say what they got (loss, jitter) and how
/// long ago they heard the last sender report, from which the sender works out the round-trip time.
///
/// Pure packet building and parsing, no sockets; `RtpSender` and `RtpReceiver` use it.
namespace SFT::Audio {

    /// How one source was received (the report block both report types carry).
    struct RtcpReportBlock {
        u32 ssrc = 0;                 ///< the source being reported on
        u8 fraction_lost = 0;         ///< lost / expected since the last report, in 1/256ths
        i32 cumulative_lost = 0;      ///< 24-bit signed on the wire
        u32 highest_sequence = 0;     ///< extended highest sequence number received
        u32 jitter = 0;               ///< interarrival jitter in RTP timestamp units
        u32 last_sender_report = 0;   ///< middle 32 bits of the NTP time of the last SR heard (0 = none)
        u32 delay_since_sr = 0;       ///< 1/65536 s between hearing that SR and sending this report
    };

    struct RtcpSenderReport {
        u32 ssrc = 0;
        u64 ntp_timestamp = 0;        ///< seconds since 1900 in the high 32 bits, fraction in the low 32
        u32 rtp_timestamp = 0;        ///< the RTP clock at the same instant
        u32 packet_count = 0;
        u32 octet_count = 0;
        std::vector<RtcpReportBlock> blocks;
    };

    struct RtcpReceiverReport {
        u32 ssrc = 0;
        std::vector<RtcpReportBlock> blocks;
    };

    /// Everything one compound RTCP datagram said.
    struct RtcpCompound {
        std::vector<RtcpSenderReport> sender_reports;
        std::vector<RtcpReceiverReport> receiver_reports;
        std::vector<std::pair<u32, UString>> cnames; ///< SDES CNAME items by SSRC
        std::vector<u32> goodbyes;                   ///< SSRCs that said BYE
    };

    /// Serialises a compound packet: a report first (SR or RR), then SDES with the CNAME, as the RFC requires.
    [[nodiscard]] std::vector<u8> build_sender_report_packet(const RtcpSenderReport &report, const UString &cname);
    [[nodiscard]] std::vector<u8> build_receiver_report_packet(const RtcpReceiverReport &report, const UString &cname);
    [[nodiscard]] std::vector<u8> build_goodbye_packet(u32 ssrc);

    /// Parses a compound packet; nothing when it is not valid RTCP (bad version, lengths running past the datagram).
    [[nodiscard]] std::optional<RtcpCompound> parse_rtcp(std::span<const u8> datagram);

    /// NTP timestamp for the system clock now (seconds since 1900, 32.32 fixed point).
    [[nodiscard]] u64 ntp_now();
    /// The middle 32 bits of an NTP timestamp: what report blocks use to refer to a sender report.
    [[nodiscard]] constexpr u32 ntp_middle(u64 ntp) noexcept { return static_cast<u32>(ntp >> 16); }
    /// Round-trip time from a report block that answers one of our sender reports, given the NTP time the block arrived
    /// (RFC 3550 6.4.1: arrival - last_sender_report - delay_since_sr). Nothing when the block refers to no SR.
    [[nodiscard]] std::optional<f64> round_trip_seconds(const RtcpReportBlock &block, u64 arrival_ntp);

} // namespace SFT::Audio
