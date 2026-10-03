#include <Audio/Encode.hpp>

#include <Audio/EncodeCommon.hpp>
#include <Audio/Resample.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace SFT::Audio {

    // ---- format names ---------------------------------------------------------------------------------------------------

    ustr format_name(AudioFormat format) noexcept {
        switch (format) {
            case AudioFormat::Wav: return "WAV"_ustr;
            case AudioFormat::Wave64: return "Wave64"_ustr;
            case AudioFormat::Aiff: return "AIFF"_ustr;
            case AudioFormat::Flac: return "FLAC"_ustr;
            case AudioFormat::OggVorbis: return "Ogg Vorbis"_ustr;
            case AudioFormat::Opus: return "Opus"_ustr;
            case AudioFormat::Mp3: return "MP3"_ustr;
            case AudioFormat::Aac: return "AAC"_ustr;
            case AudioFormat::Alac: return "ALAC"_ustr;
            case AudioFormat::RawPcm: return "raw PCM"_ustr;
        }
        return "?"_ustr;
    }

    ustr format_extension(AudioFormat format) noexcept {
        switch (format) {
            case AudioFormat::Wav: return ".wav"_ustr;
            case AudioFormat::Wave64: return ".w64"_ustr;
            case AudioFormat::Aiff: return ".aiff"_ustr;
            case AudioFormat::Flac: return ".flac"_ustr;
            case AudioFormat::OggVorbis: return ".ogg"_ustr;
            case AudioFormat::Opus: return ".opus"_ustr;
            case AudioFormat::Mp3: return ".mp3"_ustr;
            case AudioFormat::Aac: return ".m4a"_ustr;
            case AudioFormat::Alac: return ".m4a"_ustr;
            case AudioFormat::RawPcm: return ".pcm"_ustr;
        }
        return ""_ustr;
    }

    std::optional<AudioFormat> format_from_extension(const UString &extension) noexcept {
        UString e;
        for (char32_t c : extension) {
            e.push_back(c < 128 ? static_cast<char32_t>(std::tolower(static_cast<int>(c))) : c);
        }
        if (!e.empty() && e.front() == U'.') {
            e.erase(0, 1);
        }
        if (e == "wav"_ustr || e == "wave"_ustr || e == "bwf"_ustr) return AudioFormat::Wav;
        if (e == "w64"_ustr) return AudioFormat::Wave64;
        if (e == "aif"_ustr || e == "aiff"_ustr || e == "aifc"_ustr) return AudioFormat::Aiff;
        if (e == "flac"_ustr) return AudioFormat::Flac;
        if (e == "ogg"_ustr || e == "oga"_ustr) return AudioFormat::OggVorbis;
        if (e == "opus"_ustr) return AudioFormat::Opus;
        if (e == "mp3"_ustr) return AudioFormat::Mp3;
        if (e == "m4a"_ustr || e == "aac"_ustr || e == "mp4"_ustr) return AudioFormat::Aac;
        if (e == "pcm"_ustr || e == "raw"_ustr) return AudioFormat::RawPcm;
        return std::nullopt;
    }

    // ---- registry ---------------------------------------------------------------------------------------------------------

    void EncoderRegistry::add(std::unique_ptr<EncoderBackend> backend) {
        if (!backend) {
            return;
        }
        // A later registration of the same format replaces the earlier one: that is how a project swaps in its own codec.
        std::erase_if(backends_, [&](const auto &b) { return b->format() == backend->format(); });
        backends_.push_back(std::move(backend));
    }

    EncoderRegistry EncoderRegistry::with_defaults() {
        EncoderRegistry registry;
        registry.add(make_wav_encoder());
        registry.add(make_wave64_encoder());
        registry.add(make_aiff_encoder());
        registry.add(make_raw_encoder());
        registry.add(make_flac_encoder());
        registry.add(make_vorbis_encoder());
        registry.add(make_opus_encoder());
        registry.add(make_mp3_encoder());
        registry.add(make_platform_aac_encoder());
        registry.add(make_platform_alac_encoder());
        return registry;
    }

    EncoderRegistry &EncoderRegistry::global() {
        static EncoderRegistry registry = with_defaults();
        return registry;
    }

    const EncoderBackend *EncoderRegistry::find(AudioFormat format) const {
        for (const auto &b : backends_) {
            if (b->format() == format) {
                return b.get();
            }
        }
        return nullptr;
    }

    std::vector<AudioFormat> EncoderRegistry::available() const {
        std::vector<AudioFormat> out;
        for (const auto &b : backends_) {
            out.push_back(b->format());
        }
        return out;
    }

    // ---- the conversion front end -----------------------------------------------------------------------------------------

    namespace {

        SpeakerLayout speaker_layout_for(u32 channels) {
            const ChannelLayoutInfo guessed = ChannelLayoutInfo::guess(channels);
            if (guessed.kind == ChannelKind::Speakers) {
                return guessed.speakers;
            }
            SpeakerLayout layout;
            layout.name = UString{std::format("{} channels", channels)};
            layout.speakers.assign(channels, Speaker{ChannelRole::Mono, 0.0f, 0.0f});
            return layout;
        }

        /// Wraps a backend encoder: remixes channels, resamples, applies gain, then hands over blocks in the backend's format.
        class ConvertingEncoder final : public AudioEncoder {
          public:
            ConvertingEncoder(std::unique_ptr<AudioEncoder> inner, u32 in_channels, u32 in_rate, u32 out_channels, u32 out_rate, ChannelMatrix matrix, f32 gain)
                : inner_(std::move(inner)), in_channels_(in_channels), out_channels_(out_channels), in_rate_(in_rate), out_rate_(out_rate),
                  matrix_(std::move(matrix)), gain_(gain), remix_(in_channels != out_channels || !matrix_.taps.empty()) {
                if (in_rate != out_rate) {
                    resampler_ = std::make_unique<Resampler>(out_channels, in_rate, out_rate, ResampleQuality::High);
                }
            }

            bool write(const f32 *interleaved, usize frames) override {
                constexpr usize chunk = 4096;
                for (usize done = 0; done < frames; done += chunk) {
                    const usize n = std::min(chunk, frames - done);
                    const f32 *source = interleaved + done * in_channels_;
                    const f32 *block = source;
                    if (remix_) {
                        mixed_.assign(n * out_channels_, 0.0f);
                        for (usize i = 0; i < n; ++i) {
                            for (const MatrixTap &t : matrix_.taps) {
                                mixed_[i * out_channels_ + t.destination] += source[i * in_channels_ + t.source] * t.gain;
                            }
                        }
                        block = mixed_.data();
                    }
                    if (gain_ != 1.0f) {
                        scaled_.assign(block, block + n * out_channels_);
                        for (f32 &s : scaled_) {
                            s *= gain_;
                        }
                        block = scaled_.data();
                    }
                    frames_ += n;
                    if (resampler_) {
                        out_.clear();
                        resampler_->process(block, n, out_);
                        out_frames_ += out_.size() / out_channels_;
                        if (!out_.empty() && !inner_->write(out_.data(), out_.size() / out_channels_)) {
                            return false;
                        }
                    } else if (!inner_->write(block, n)) {
                        return false;
                    }
                }
                return true;
            }

            std::expected<void, UString> finish() override {
                if (resampler_) {
                    out_.clear();
                    resampler_->flush(out_);
                    // The flush pushes silence through the filter; keep only the frames the input really covers, so the file is
                    // exactly as long as the audio it came from.
                    const u64 exact = static_cast<u64>(std::llround(static_cast<f64>(frames_) * static_cast<f64>(out_rate_) / static_cast<f64>(in_rate_)));
                    const u64 allowed = exact > out_frames_ ? exact - out_frames_ : 0;
                    out_.resize(std::min<usize>(out_.size(), static_cast<usize>(allowed) * out_channels_));
                    if (!out_.empty()) {
                        inner_->write(out_.data(), out_.size() / out_channels_);
                    }
                }
                return inner_->finish();
            }
            [[nodiscard]] const UString &error() const override { return inner_->error(); }
            [[nodiscard]] u64 frames_written() const override { return frames_; }

          private:
            std::unique_ptr<AudioEncoder> inner_;
            u32 in_channels_, out_channels_, in_rate_, out_rate_;
            ChannelMatrix matrix_;
            f32 gain_;
            bool remix_;
            std::unique_ptr<Resampler> resampler_;
            std::vector<f32> mixed_, scaled_, out_;
            u64 frames_ = 0, out_frames_ = 0;
        };

    } // namespace

    std::expected<std::unique_ptr<AudioEncoder>, UString> open_encoder(const std::filesystem::path &path, u32 channels, u32 sample_rate,
                                                                           const EncodeOptions &requested, const std::optional<ChannelLayoutInfo> &layout) {
        if (channels == 0 || sample_rate == 0) {
            return std::unexpected("audio: an encoder needs at least one channel and a sample rate");
        }
        const EncoderBackend *backend = EncoderRegistry::global().find(requested.format);
        if (backend == nullptr) {
            return std::unexpected(UString{std::format("audio: this build cannot write {} (the codec was left out of the build or needs a system library that was not found)",
                                                format_name(requested.format))});
        }
        const ChannelLayoutInfo in_layout = layout && layout->channels == channels ? *layout : ChannelLayoutInfo::guess(channels);

        const u32 out_channels = requested.channels > 0 ? requested.channels : channels;
        if (out_channels > backend->max_channels()) {
            return std::unexpected(UString{std::format("audio: {} carries at most {} channels, the audio has {} (set EncodeOptions::channels to mix down, or pick WAV/Wave64/Opus)",
                                                format_name(requested.format), backend->max_channels(), out_channels)});
        }
        const u32 wanted_rate = requested.sample_rate > 0 ? requested.sample_rate : sample_rate;
        const u32 out_rate = backend->constrain_sample_rate(wanted_rate);

        ChannelMatrix matrix;
        ChannelLayoutInfo out_layout = in_layout;
        if (out_channels != channels) {
            const SpeakerLayout target = speaker_layout_for(out_channels);
            matrix = make_channel_matrix(in_layout, target);
            out_layout = ChannelLayoutInfo::guess(out_channels);
        }

        EncoderConfig config;
        config.path = path;
        config.channels = out_channels;
        config.sample_rate = out_rate;
        config.options = requested;
        config.layout = out_layout;
        // Marker positions are in the frames of the audio the caller writes; the file will hold converted frames.
        if (out_rate != sample_rate) {
            const f64 scale = static_cast<f64>(out_rate) / static_cast<f64>(sample_rate);
            for (AudioMarker &m : config.options.markers) {
                m.frame = static_cast<u64>(std::llround(static_cast<f64>(m.frame) * scale));
            }
            if (config.options.loop) {
                config.options.loop->start = static_cast<u64>(std::llround(static_cast<f64>(config.options.loop->start) * scale));
                config.options.loop->end = static_cast<u64>(std::llround(static_cast<f64>(config.options.loop->end) * scale));
            }
        }
        auto inner = backend->open(config);
        if (!inner) {
            return std::unexpected(inner.error());
        }
        if (out_channels == channels && out_rate == sample_rate && requested.gain == 1.0f) {
            return std::move(*inner);
        }
        return std::unique_ptr<AudioEncoder>(std::make_unique<ConvertingEncoder>(std::move(*inner), channels, sample_rate, out_channels, out_rate,
                                                                                 std::move(matrix), requested.gain));
    }

    std::expected<void, UString> encode_buffer(const std::filesystem::path &path, const SampleBuffer &buffer, EncodeOptions options) {
        if (!buffer.samples || buffer.channels == 0 || buffer.sample_rate == 0) {
            return std::unexpected("audio: nothing to write");
        }
        if (options.markers.empty()) {
            options.markers = buffer.markers;
        }
        if (!options.loop) {
            options.loop = buffer.loop;
        }
        const std::optional<ChannelLayoutInfo> layout = buffer.layout.channels == buffer.channels ? std::optional(buffer.layout) : std::nullopt;
        auto encoder = open_encoder(path, buffer.channels, buffer.sample_rate, options, layout);
        if (!encoder) {
            return std::unexpected(encoder.error());
        }
        // Blocks keep the converting path's temporaries small even for hour-long buffers.
        constexpr usize chunk_frames = 1 << 16;
        const usize frames = static_cast<usize>(buffer.frames());
        for (usize done = 0; done < frames; done += chunk_frames) {
            const usize n = std::min(chunk_frames, frames - done);
            if (!(*encoder)->write(buffer.samples->data() + done * buffer.channels, n)) {
                return std::unexpected((*encoder)->error());
            }
        }
        return (*encoder)->finish();
    }

    std::expected<void, UString> transcode(StreamDecoder &decoder, const std::filesystem::path &output, EncodeOptions options, const ProgressCallback &progress) {
        const AudioStreamInfo &info = decoder.info();
        if (info.channels == 0 || info.sample_rate == 0) {
            return std::unexpected("audio: the source has no channels or sample rate");
        }
        if (options.tags.empty()) {
            options.tags = info.tags;
        }
        if (options.markers.empty()) {
            options.markers = info.markers;
        }
        if (!options.loop) {
            options.loop = info.loop;
        }
        const std::optional<ChannelLayoutInfo> layout = info.layout.channels == info.channels ? std::optional(info.layout) : std::nullopt;
        auto encoder = open_encoder(output, info.channels, info.sample_rate, options, layout);
        if (!encoder) {
            return std::unexpected(encoder.error());
        }
        constexpr u64 chunk_frames = 8192;
        std::vector<f32> block(static_cast<usize>(chunk_frames) * info.channels);
        u64 total = 0;
        for (;;) {
            const u64 got = decoder.read(block.data(), chunk_frames);
            if (got == 0) {
                break;
            }
            if (!(*encoder)->write(block.data(), static_cast<usize>(got))) {
                return std::unexpected((*encoder)->error());
            }
            total += got;
            if (progress && !progress(info.total_frames > 0 ? static_cast<f32>(static_cast<f64>(total) / static_cast<f64>(info.total_frames)) : 0.0f)) {
                return std::unexpected("audio: cancelled");
            }
        }
        return (*encoder)->finish();
    }

    std::expected<void, UString> transcode(const std::filesystem::path &input, const std::filesystem::path &output, EncodeOptions options,
                                               const ProgressCallback &progress) {
        auto decoder = DecoderRegistry::global().open_file(input);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        return transcode(**decoder, output, std::move(options), progress);
    }

    // ---- the historical entry point, now a thin wrapper -----------------------------------------------------------------------

    std::expected<void, UString> write_wav(const std::filesystem::path &path, const SampleBuffer &buffer, bool float32) {
        EncodeOptions options;
        options.format = AudioFormat::Wav;
        options.sample_format = float32 ? SampleFormat::Float32 : SampleFormat::Int16;
        return encode_buffer(path, buffer, std::move(options));
    }

} // namespace SFT::Audio
