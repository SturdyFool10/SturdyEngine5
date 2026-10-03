// Ogg Opus (RFC 7845) through libopus and libogg: mono to 255 channels, sample-accurate seeking and trimming, Opus tags.

#include <Audio/Decoder.hpp>

#if STURDY_AUDIO_OPUS

#include <Audio/OpusCodec.hpp>

#include <ogg/ogg.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <Audio/VorbisComments.hpp>

namespace SFT::Audio {

    namespace {

        constexpr u64 kMaxPageBytes = 27 + 255 + 255 * 255;
        constexpr u32 kMaxFrameSamples = 5760; // 120 ms at 48 kHz, the longest Opus packet
        constexpr u64 kPreroll = 3840;         // 80 ms the decoder needs to converge after a seek (RFC 7845 section 4.6)

        struct PageInfo {
            u64 offset = 0;
            u64 size = 0;
            u8 type = 0;
            i64 granule = -1;
        };

        u32 le32(const std::byte *p) {
            return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
        }

        /// Parses an Ogg page header at exactly `offset`; false if there is no valid one there.
        bool page_at(const std::byte *data, u64 size, u64 offset, PageInfo &page) {
            if (offset + 27 > size || std::memcmp(data + offset, "OggS", 4) != 0) {
                return false;
            }
            const u32 segments = static_cast<u32>(data[offset + 26]);
            if (offset + 27 + segments > size) {
                return false;
            }
            u64 body = 0;
            for (u32 i = 0; i < segments; ++i) {
                body += static_cast<u8>(data[offset + 27 + i]);
            }
            page.offset = offset;
            page.size = 27 + segments + body;
            page.type = static_cast<u8>(data[offset + 5]);
            page.granule = static_cast<i64>(static_cast<u64>(le32(data + offset + 6)) | (static_cast<u64>(le32(data + offset + 10)) << 32));
            return offset + page.size <= size;
        }

        /// The first page that starts at or after `from` (resynchronising on the capture pattern).
        bool find_page(const std::byte *data, u64 size, u64 from, PageInfo &page) {
            const u64 limit = std::min(size, from + kMaxPageBytes * 2);
            for (u64 i = from; i + 27 <= limit; ++i) {
                if (data[i] == std::byte{'O'} && page_at(data, size, i, page)) {
                    return true;
                }
            }
            return false;
        }

        class OpusFileDecoder final : public StreamDecoder {
          public:
            ~OpusFileDecoder() override {
                ogg_stream_clear(&stream_);
                ogg_sync_clear(&sync_);
            }

            [[nodiscard]] bool open(EncodedBytes bytes) {
                bytes_ = std::move(bytes);
                data_ = bytes_->data();
                size_ = bytes_->size();
                ogg_sync_init(&sync_);
                if (!read_headers()) {
                    return false;
                }
                auto decoder = OpusDecoderHandle::create(info_.channels, stream_layout_, gain_q8_);
                if (!decoder) {
                    return false;
                }
                decoder_ = std::move(*decoder);
                pcm_.resize(static_cast<usize>(kMaxFrameSamples) * info_.channels);
                // Length: the granule position of the last page minus the pre-skip.
                u64 end = 0;
                const u64 tail = size_ > kMaxPageBytes * 2 ? size_ - kMaxPageBytes * 2 : 0;
                PageInfo page;
                u64 cursor = tail;
                while (find_page(data_, size_, cursor, page)) {
                    if (page.granule >= 0) {
                        end = static_cast<u64>(page.granule);
                    }
                    cursor = page.offset + page.size;
                }
                end_granule_ = end;
                info_.total_frames = end > pre_skip_ ? end - pre_skip_ : 0;
                start_decoding(data_begin_, 0);
                skip_ = pre_skip_;
                return true;
            }

            [[nodiscard]] const AudioStreamInfo &info() const override { return info_; }

            u64 read(f32 *out, u64 frames) override {
                const u32 channels = info_.channels;
                u64 done = 0;
                while (done < frames) {
                    if (pcm_position_ >= pcm_frames_ && !decode_next()) {
                        break;
                    }
                    const u64 available = pcm_frames_ - pcm_position_;
                    u64 take = std::min<u64>(available, frames - done);
                    if (info_.total_frames > 0 && position_ + take > info_.total_frames) {
                        take = info_.total_frames > position_ ? info_.total_frames - position_ : 0;
                        if (take == 0) {
                            break;
                        }
                    }
                    std::memcpy(out + done * channels, pcm_.data() + pcm_position_ * channels, take * channels * sizeof(f32));
                    pcm_position_ += take;
                    done += take;
                    position_ += take;
                }
                return done;
            }

            bool seek(u64 frame) override {
                if (info_.total_frames > 0) {
                    frame = std::min(frame, info_.total_frames);
                }
                const u64 target = frame + pre_skip_;               // in decoder output samples, pre-skip included
                const u64 goal = target > kPreroll ? target - kPreroll : 0;
                // Bisect for the last page whose granule position is at or before `goal`.
                u64 low = data_begin_, high = size_;
                u64 start_page = data_begin_;
                while (high - low > 4096) {
                    const u64 mid = low + (high - low) / 2;
                    PageInfo page;
                    if (!find_page(data_, size_, mid, page) || page.offset >= high) {
                        high = mid;
                        continue;
                    }
                    if (page.granule < 0 || static_cast<u64>(page.granule) <= goal) {
                        low = page.offset;
                        if (page.granule >= 0) start_page = page.offset;
                    } else {
                        high = mid;
                    }
                }
                // From there, walk to a page that does not continue a packet; decoding starts cleanly on it, at the granule
                // position of the page before.
                u64 position = 0;
                u64 offset = start_page;
                PageInfo page, previous;
                bool have_previous = false;
                if (start_page > data_begin_ && page_at(data_, size_, start_page, previous) && previous.granule >= 0) {
                    have_previous = true;
                    position = static_cast<u64>(previous.granule);
                    offset = previous.offset + previous.size;
                } else {
                    offset = data_begin_;
                    position = 0;
                }
                while (page_at(data_, size_, offset, page)) {
                    if ((page.type & 0x01) == 0) {
                        break; // fresh packets begin here
                    }
                    if (page.granule >= 0) {
                        position = static_cast<u64>(page.granule);
                    }
                    offset = page.offset + page.size;
                }
                (void)have_previous;
                start_decoding(offset, position);
                skip_ = target > position ? target - position : 0;
                position_ = frame;
                decoder_->reset();
                return true;
            }

            [[nodiscard]] u64 position() const override { return position_; }

          private:
            // ---- headers --------------------------------------------------------------------------------------------

            bool read_headers() {
                ogg_page page;
                ogg_packet packet;
                bool have_head = false, have_tags = false, stream_ready = false;
                u64 offset = 0;
                u64 fed = 0;
                while (!have_tags) {
                    const int got = ogg_sync_pageout(&sync_, &page);
                    if (got == 0) {
                        if (fed >= size_) {
                            return false;
                        }
                        feed(fed, 4096);
                        continue;
                    }
                    if (got < 0) {
                        continue;
                    }
                    if (!stream_ready) {
                        ogg_stream_init(&stream_, ogg_page_serialno(&page));
                        stream_ready = true;
                        stream_initialised_ = true;
                    }
                    ogg_stream_pagein(&stream_, &page);
                    while (ogg_stream_packetout(&stream_, &packet) > 0) {
                        if (!have_head) {
                            if (!parse_head(packet)) {
                                return false;
                            }
                            have_head = true;
                        } else if (!have_tags) {
                            if (packet.bytes < 8 || std::memcmp(packet.packet, "OpusTags", 8) != 0) {
                                return false;
                            }
                            parse_tags(packet);
                            have_tags = true;
                            break;
                        }
                    }
                    offset += static_cast<u64>(page.header_len + page.body_len);
                }
                // `offset` counts bytes of whole pages consumed so far: the next page is the first with audio.
                data_begin_ = offset;
                serial_ = stream_.serialno;
                return have_head && info_.channels > 0;
            }

            bool parse_head(const ogg_packet &packet) {
                const auto *p = packet.packet;
                if (packet.bytes < 19 || std::memcmp(p, "OpusHead", 8) != 0 || (p[8] >> 4) != 0) {
                    return false;
                }
                info_.channels = p[9];
                pre_skip_ = static_cast<u32>(p[10]) | (static_cast<u32>(p[11]) << 8);
                gain_q8_ = static_cast<i16>(static_cast<u16>(p[16]) | (static_cast<u16>(p[17]) << 8));
                const u8 family = p[18];
                info_.sample_rate = 48000; // Opus always decodes at 48 kHz
                info_.codec = "opus";
                if (info_.channels == 0) {
                    return false;
                }
                if (family == 0) {
                    if (info_.channels > 2) return false;
                    stream_layout_.family = 0;
                    stream_layout_.streams = 1;
                    stream_layout_.coupled = info_.channels == 2 ? 1 : 0;
                    stream_layout_.mapping[0] = 0;
                    stream_layout_.mapping[1] = 1;
                    info_.layout = ChannelLayoutInfo::guess(info_.channels);
                } else if (family == 1 || family == 2 || family == 255) {
                    if (packet.bytes < 21 + static_cast<long>(info_.channels)) return false;
                    stream_layout_.family = family;
                    stream_layout_.streams = p[19];
                    stream_layout_.coupled = p[20];
                    for (u32 c = 0; c < info_.channels; ++c) stream_layout_.mapping[c] = p[21 + c];
                    if (family == 1) {
                        info_.layout = vorbis_channel_layout(info_.channels);
                    } else if (family == 2) {
                        // (order + 1)^2 channels; two extra non-diegetic channels make it "discrete" for our purposes.
                        u32 order = 0;
                        while (ambisonic_channels(order + 1) <= info_.channels && order < 14) ++order;
                        info_.layout = ambisonic_channels(order) == info_.channels && order <= max_ambisonic_order ? ChannelLayoutInfo::ambisonic(order)
                                                                                                                     : ChannelLayoutInfo::discrete(info_.channels);
                    } else {
                        info_.layout = ChannelLayoutInfo::discrete(info_.channels);
                    }
                } else {
                    return false; // family 3 (projection) is not supported
                }
                return true;
            }

            void parse_tags(const ogg_packet &packet) {
                const auto *p = packet.packet;
                const long n = packet.bytes;
                long pos = 8;
                const auto read32 = [&](u32 &v) {
                    if (pos + 4 > n) return false;
                    v = le32(reinterpret_cast<const std::byte *>(p + pos));
                    pos += 4;
                    return true;
                };
                u32 vendor = 0;
                if (!read32(vendor) || pos + static_cast<long>(vendor) > n) return;
                pos += static_cast<long>(vendor);
                u32 count = 0;
                if (!read32(count)) return;
                VorbisCommentReader reader{info_};
                for (u32 i = 0; i < count; ++i) {
                    u32 length = 0;
                    if (!read32(length) || pos + static_cast<long>(length) > n) break;
                    reader.add(std::string_view(reinterpret_cast<const char *>(p + pos), length));
                    pos += static_cast<long>(length);
                }
                reader.finish();
            }

            // ---- packet stream -----------------------------------------------------------------------------------------

            void feed(u64 &fed, u64 amount) {
                const u64 n = std::min<u64>(amount, size_ - fed);
                char *buffer = ogg_sync_buffer(&sync_, static_cast<long>(n));
                std::memcpy(buffer, data_ + fed, n);
                ogg_sync_wrote(&sync_, static_cast<long>(n));
                fed += n;
            }

            /// Starts reading pages at byte `offset`, whose first packet begins at decoder sample `position`.
            void start_decoding(u64 offset, u64 position) {
                ogg_sync_reset(&sync_);
                if (stream_initialised_) {
                    ogg_stream_reset_serialno(&stream_, serial_);
                }
                fed_ = offset;
                sample_position_ = position;
                pcm_position_ = pcm_frames_ = 0;
                skip_ = 0;
                finished_ = false;
                if (decoder_) {
                    decoder_->reset();
                }
            }

            bool next_packet(ogg_packet &packet) {
                for (;;) {
                    const int got = ogg_stream_packetout(&stream_, &packet);
                    if (got > 0) {
                        return true;
                    }
                    if (got < 0) {
                        continue; // a gap: libogg resyncs on the next packet
                    }
                    ogg_page page;
                    int page_status = ogg_sync_pageout(&sync_, &page);
                    while (page_status == 0) {
                        if (fed_ >= size_) {
                            return false;
                        }
                        feed(fed_, 65536);
                        page_status = ogg_sync_pageout(&sync_, &page);
                    }
                    if (page_status < 0) {
                        continue;
                    }
                    if (static_cast<u32>(ogg_page_serialno(&page)) == serial_) {
                        ogg_stream_pagein(&stream_, &page);
                    }
                }
            }

            // Decodes the next packet into `pcm_`, applying pre-skip/seek discarding. False at the end of the stream.
            bool decode_next() {
                while (!finished_) {
                    ogg_packet packet;
                    if (!next_packet(packet)) {
                        finished_ = true;
                        return false;
                    }
                    const int frames = decoder_->decode(packet.packet, static_cast<usize>(packet.bytes), pcm_.data());
                    if (frames <= 0) {
                        continue; // a corrupt packet: skip it rather than ending playback
                    }
                    u64 drop = std::min<u64>(skip_, static_cast<u64>(frames));
                    skip_ -= drop;
                    sample_position_ += static_cast<u64>(frames);
                    // Never run past the final granule position (the encoder pads the last packet).
                    u64 keep_end = static_cast<u64>(frames);
                    if (end_granule_ > 0 && sample_position_ > end_granule_) {
                        const u64 excess = sample_position_ - end_granule_;
                        keep_end = excess >= keep_end ? 0 : keep_end - excess;
                    }
                    if (keep_end <= drop) {
                        continue;
                    }
                    pcm_position_ = drop;
                    pcm_frames_ = keep_end;
                    return true;
                }
                return false;
            }

            EncodedBytes bytes_;
            const std::byte *data_ = nullptr;
            u64 size_ = 0;
            ogg_sync_state sync_{};
            ogg_stream_state stream_{};
            bool stream_initialised_ = false;
            u32 serial_ = 0;
            u64 data_begin_ = 0, fed_ = 0;
            std::unique_ptr<OpusDecoderHandle> decoder_;
            OpusStreamLayout stream_layout_;
            i16 gain_q8_ = 0;
            u32 pre_skip_ = 0;
            u64 end_granule_ = 0;
            AudioStreamInfo info_;
            std::vector<f32> pcm_;
            u64 pcm_position_ = 0, pcm_frames_ = 0;
            u64 sample_position_ = 0; // decoder samples (pre-skip included) emitted up to the end of the last decoded packet
            u64 skip_ = 0;
            u64 position_ = 0;
            bool finished_ = false;
        };

        class OpusBackend final : public DecoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "Opus (libopus)"_ustr; }
            [[nodiscard]] std::vector<UString> extensions() const override { return {".opus"}; }
            [[nodiscard]] int probe(std::span<const std::byte> header, const UString &extension) const override {
                if (header.size() >= 4 && std::memcmp(header.data(), "OggS", 4) == 0) {
                    for (usize i = 0; i + 8 <= header.size(); ++i) {
                        if (std::memcmp(header.data() + i, "OpusHead", 8) == 0) {
                            return 100;
                        }
                    }
                    return 0;
                }
                return extension == ".opus"_ustr ? 10 : 0;
            }
            [[nodiscard]] std::unique_ptr<StreamDecoder> open(EncodedBytes bytes) const override {
                auto decoder = std::make_unique<OpusFileDecoder>();
                return decoder->open(std::move(bytes)) ? std::move(decoder) : nullptr;
            }
        };

    } // namespace

    std::unique_ptr<DecoderBackend> make_opus_backend() { return std::make_unique<OpusBackend>(); }

} // namespace SFT::Audio

#else

namespace SFT::Audio {
    std::unique_ptr<DecoderBackend> make_opus_backend() { return nullptr; }
} // namespace SFT::Audio

#endif
