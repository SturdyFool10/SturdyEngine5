#include <Audio/Rtcp.hpp>
#include <Audio/Text.hpp>

#include <algorithm>
#include <ranges>

namespace SFT::Audio {

    namespace {
        constexpr u8 kSenderReport = 200, kReceiverReport = 201, kSourceDescription = 202, kGoodbye = 203;
        constexpr u8 kCname = 1;
        constexpr u64 kNtpUnixOffset = 2208988800ull; // seconds from 1900 to 1970

        class Writer {
          public:
            void u8_(u8 v) { out.push_back(v); }
            void u16_(u16 v) { u8_(static_cast<u8>(v >> 8)); u8_(static_cast<u8>(v)); }
            void u32_(u32 v) { u16_(static_cast<u16>(v >> 16)); u16_(static_cast<u16>(v)); }
            void u64_(u64 v) { u32_(static_cast<u32>(v >> 32)); u32_(static_cast<u32>(v)); }
            /// Starts a packet; returns where its length field lives.
            usize begin(u8 count, u8 type) {
                u8_(static_cast<u8>(0x80 | (count & 0x1F)));
                u8_(type);
                u16_(0);
                return out.size() - 2;
            }
            /// Pads to 32 bits and fills in the length (in 32-bit words minus one).
            void end(usize length_at, usize packet_start) {
                while ((out.size() - packet_start) % 4 != 0) u8_(0);
                const usize words = (out.size() - packet_start) / 4 - 1;
                out[length_at] = static_cast<u8>(words >> 8);
                out[length_at + 1] = static_cast<u8>(words);
            }
            std::vector<u8> out;
        };

        void write_block(Writer &w, const RtcpReportBlock &b) {
            w.u32_(b.ssrc);
            w.u8_(b.fraction_lost);
            const u32 lost = static_cast<u32>(std::clamp<i32>(b.cumulative_lost, -0x800000, 0x7FFFFF)) & 0xFFFFFF;
            w.u8_(static_cast<u8>(lost >> 16));
            w.u16_(static_cast<u16>(lost));
            w.u32_(b.highest_sequence);
            w.u32_(b.jitter);
            w.u32_(b.last_sender_report);
            w.u32_(b.delay_since_sr);
        }

        void write_sdes(Writer &w, u32 ssrc, const UString &cname) {
            const usize start = w.out.size();
            const usize length_at = w.begin(1, kSourceDescription);
            w.u32_(ssrc);
            const std::string_view text = cname.cpp_string_view().substr(0, 255);
            w.u8_(kCname);
            w.u8_(static_cast<u8>(text.size()));
            for (const char c : text) w.u8_(static_cast<u8>(c));
            w.u8_(0); // end of item list
            w.end(length_at, start);
        }

        class Reader {
          public:
            explicit Reader(std::span<const u8> data) : data_(data) {}
            [[nodiscard]] usize left() const noexcept { return data_.size() - pos_; }
            u8 u8_() { return data_[pos_++]; }
            u16 u16_() { const u16 hi = u8_(); return static_cast<u16>((hi << 8) | u8_()); }
            u32 u32_() { const u32 hi = u16_(); return (hi << 16) | u16_(); }
            u64 u64_() { const u64 hi = u32_(); return (hi << 32) | u32_(); }
            void skip(usize n) { pos_ += std::min(n, left()); }
          private:
            std::span<const u8> data_;
            usize pos_ = 0;
        };

        std::optional<RtcpReportBlock> read_block(Reader &r) {
            if (r.left() < 24) return std::nullopt;
            RtcpReportBlock b;
            b.ssrc = r.u32_();
            b.fraction_lost = r.u8_();
            u32 lost = (static_cast<u32>(r.u8_()) << 16);
            lost |= r.u16_();
            b.cumulative_lost = (lost & 0x800000) != 0 ? static_cast<i32>(lost | 0xFF000000u) : static_cast<i32>(lost);
            b.highest_sequence = r.u32_();
            b.jitter = r.u32_();
            b.last_sender_report = r.u32_();
            b.delay_since_sr = r.u32_();
            return b;
        }
    } // namespace

    std::vector<u8> build_sender_report_packet(const RtcpSenderReport &report, const UString &cname) {
        Writer w;
        const usize start = 0;
        const usize length_at = w.begin(static_cast<u8>(std::min<usize>(report.blocks.size(), 31)), kSenderReport);
        w.u32_(report.ssrc);
        w.u64_(report.ntp_timestamp);
        w.u32_(report.rtp_timestamp);
        w.u32_(report.packet_count);
        w.u32_(report.octet_count);
        for (const RtcpReportBlock &b : report.blocks | std::views::take(31)) write_block(w, b);
        w.end(length_at, start);
        write_sdes(w, report.ssrc, cname);
        return std::move(w.out);
    }

    std::vector<u8> build_receiver_report_packet(const RtcpReceiverReport &report, const UString &cname) {
        Writer w;
        const usize length_at = w.begin(static_cast<u8>(std::min<usize>(report.blocks.size(), 31)), kReceiverReport);
        w.u32_(report.ssrc);
        for (const RtcpReportBlock &b : report.blocks | std::views::take(31)) write_block(w, b);
        w.end(length_at, 0);
        write_sdes(w, report.ssrc, cname);
        return std::move(w.out);
    }

    std::vector<u8> build_goodbye_packet(u32 ssrc) {
        Writer w;
        const usize length_at = w.begin(1, kGoodbye);
        w.u32_(ssrc);
        w.end(length_at, 0);
        return std::move(w.out);
    }

    std::optional<RtcpCompound> parse_rtcp(std::span<const u8> datagram) {
        RtcpCompound out;
        usize offset = 0;
        bool any = false;
        while (offset + 4 <= datagram.size()) {
            const u8 first = datagram[offset], type = datagram[offset + 1];
            if ((first >> 6) != 2) return std::nullopt;
            const usize length = (static_cast<usize>((datagram[offset + 2] << 8) | datagram[offset + 3]) + 1) * 4;
            if (offset + length > datagram.size()) return std::nullopt;
            const u8 count = first & 0x1F;
            Reader r(datagram.subspan(offset + 4, length - 4));
            switch (type) {
                case kSenderReport: {
                    if (r.left() < 24) return std::nullopt;
                    RtcpSenderReport sr;
                    sr.ssrc = r.u32_();
                    sr.ntp_timestamp = r.u64_();
                    sr.rtp_timestamp = r.u32_();
                    sr.packet_count = r.u32_();
                    sr.octet_count = r.u32_();
                    for (u8 i = 0; i < count; ++i) {
                        auto block = read_block(r);
                        if (!block) return std::nullopt;
                        sr.blocks.push_back(*block);
                    }
                    out.sender_reports.push_back(std::move(sr));
                    break;
                }
                case kReceiverReport: {
                    if (r.left() < 4) return std::nullopt;
                    RtcpReceiverReport rr;
                    rr.ssrc = r.u32_();
                    for (u8 i = 0; i < count; ++i) {
                        auto block = read_block(r);
                        if (!block) return std::nullopt;
                        rr.blocks.push_back(*block);
                    }
                    out.receiver_reports.push_back(std::move(rr));
                    break;
                }
                case kSourceDescription: {
                    for (u8 chunk = 0; chunk < count && r.left() >= 4; ++chunk) {
                        const u32 ssrc = r.u32_();
                        while (r.left() >= 1) {
                            const u8 item = r.u8_();
                            if (item == 0) break;
                            if (r.left() < 1) break;
                            const u8 size = r.u8_();
                            if (r.left() < size) return std::nullopt;
                            std::string text;
                            for (u8 i = 0; i < size; ++i) text.push_back(static_cast<char>(r.u8_()));
                            if (item == kCname) out.cnames.emplace_back(ssrc, text_from_bytes(text));
                        }
                        // Chunks are padded to 32 bits; the reader already consumed the terminator, skip to the boundary by length of the packet's own padding.
                        break;
                    }
                    break;
                }
                case kGoodbye:
                    for (u8 i = 0; i < count && r.left() >= 4; ++i) out.goodbyes.push_back(r.u32_());
                    break;
                default: break; // APP, feedback and extended reports are not needed here
            }
            any = true;
            offset += length;
        }
        if (!any || offset != datagram.size()) return std::nullopt;
        return out;
    }

    u64 ntp_now() {
        const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count();
        const u64 seconds = static_cast<u64>(micros / 1000000) + kNtpUnixOffset;
        const u64 fraction = (static_cast<u64>(micros % 1000000) << 32) / 1000000u;
        return (seconds << 32) | fraction;
    }

    std::optional<f64> round_trip_seconds(const RtcpReportBlock &block, u64 arrival_ntp) {
        if (block.last_sender_report == 0) return std::nullopt;
        const u32 arrival = ntp_middle(arrival_ntp);
        const u32 rtt = arrival - block.last_sender_report - block.delay_since_sr; // wraps correctly in 32-bit arithmetic
        if (rtt > 0x80000000u) return std::nullopt;                                 // negative: clocks or a stale report
        return static_cast<f64>(rtt) / 65536.0;
    }

} // namespace SFT::Audio
