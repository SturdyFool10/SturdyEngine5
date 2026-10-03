// AAC (M4A) export through the Media Foundation sink writer, which ships a licensed AAC encoder: the engine carries none.
// Not compiled on other platforms.

#include <Audio/EncodeCommon.hpp>

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace SFT::Audio {

    namespace {

        using Microsoft::WRL::ComPtr;

        bool ensure_media_foundation() {
            static const bool ready = [] {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
                    return false;
                }
                return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
            }();
            return ready;
        }

        class AacEncoder final : public detail::EncoderBase {
          public:
            explicit AacEncoder(const EncoderConfig &config) : config_(config) {}

            [[nodiscard]] std::expected<void, UString> start() {
                if (!ensure_media_foundation()) {
                    return std::unexpected("audio: Media Foundation is not available");
                }
                // The Windows AAC encoder takes mono, stereo or 5.1, at 44.1 or 48 kHz, at fixed bitrates.
                if (config_.channels != 1 && config_.channels != 2 && config_.channels != 6) {
                    return std::unexpected("audio: the Windows AAC encoder supports 1, 2 or 6 channels (set EncodeOptions::channels)");
                }
                temp_path_ = config_.path;
                temp_path_ += L".part.m4a";
                if (FAILED(MFCreateSinkWriterFromURL(temp_path_.c_str(), nullptr, nullptr, &writer_))) {
                    return std::unexpected("audio: could not create the M4A writer");
                }
                static constexpr std::array<u32, 4> kBytesPerSecond = {12000, 16000, 20000, 24000}; // 96, 128, 160, 192 kbps
                const u32 wanted = config_.options.bitrate_bps != 0 ? config_.options.bitrate_bps / 8 : 16000;
                u32 bytes_per_second = kBytesPerSecond[0];
                for (u32 candidate : kBytesPerSecond) {
                    if (std::abs(static_cast<i64>(candidate) - static_cast<i64>(wanted)) < std::abs(static_cast<i64>(bytes_per_second) - static_cast<i64>(wanted))) {
                        bytes_per_second = candidate;
                    }
                }
                ComPtr<IMFMediaType> out;
                MFCreateMediaType(&out);
                out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, config_.sample_rate);
                out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, config_.channels);
                out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes_per_second);
                out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
                out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
                if (FAILED(writer_->AddStream(out.Get(), &stream_))) {
                    return std::unexpected("audio: the AAC encoder rejected the format (rate must be 44100 or 48000)");
                }
                ComPtr<IMFMediaType> in;
                MFCreateMediaType(&in);
                in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, config_.sample_rate);
                in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, config_.channels);
                in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, config_.channels * 2);
                in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, config_.channels * 2 * config_.sample_rate);
                if (FAILED(writer_->SetInputMediaType(stream_, in.Get(), nullptr)) || FAILED(writer_->BeginWriting())) {
                    return std::unexpected("audio: could not start the AAC encoder");
                }
                return {};
            }

            bool write(const f32 *interleaved, usize frames) override {
                if (!error_.empty()) {
                    return false;
                }
                constexpr usize chunk = 4096;
                for (usize done = 0; done < frames; done += chunk) {
                    const usize n = std::min(chunk, frames - done);
                    const DWORD bytes = static_cast<DWORD>(n * config_.channels * 2);
                    ComPtr<IMFMediaBuffer> buffer;
                    if (FAILED(MFCreateMemoryBuffer(bytes, &buffer))) {
                        return fail("audio: out of memory in the AAC encoder");
                    }
                    BYTE *data = nullptr;
                    buffer->Lock(&data, nullptr, nullptr);
                    auto *pcm = reinterpret_cast<i16 *>(data);
                    for (usize i = 0; i < n * config_.channels; ++i) {
                        pcm[i] = static_cast<i16>(std::lrint(std::clamp(interleaved[done * config_.channels + i], -1.0f, 1.0f) * 32767.0f));
                    }
                    buffer->Unlock();
                    buffer->SetCurrentLength(bytes);
                    ComPtr<IMFSample> sample;
                    MFCreateSample(&sample);
                    sample->AddBuffer(buffer.Get());
                    sample->SetSampleTime(static_cast<LONGLONG>(written_frames_) * 10000000LL / config_.sample_rate);
                    sample->SetSampleDuration(static_cast<LONGLONG>(n) * 10000000LL / config_.sample_rate);
                    if (FAILED(writer_->WriteSample(stream_, sample.Get()))) {
                        return fail("audio: the AAC encoder failed while writing");
                    }
                    written_frames_ += n;
                }
                frames_ += frames;
                return true;
            }

            std::expected<void, UString> finish() override {
                if (!error_.empty()) {
                    return std::unexpected(error_);
                }
                const HRESULT result = writer_->Finalize();
                writer_.Reset();
                if (FAILED(result)) {
                    return std::unexpected("audio: could not finalise the M4A file");
                }
                std::error_code ec;
                std::filesystem::rename(temp_path_, config_.path, ec);
                if (ec) {
                    return std::unexpected("audio: cannot move the finished file into place");
                }
                return {};
            }

          private:
            EncoderConfig config_;
            std::filesystem::path temp_path_;
            ComPtr<IMFSinkWriter> writer_;
            DWORD stream_ = 0;
            u64 written_frames_ = 0;
        };

        class AacBackend final : public EncoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "AAC (Media Foundation)"_ustr; }
            [[nodiscard]] AudioFormat format() const override { return AudioFormat::Aac; }
            [[nodiscard]] u32 max_channels() const override { return 6; }
            [[nodiscard]] u32 constrain_sample_rate(u32 requested) const override { return requested <= 46050 ? 44100 : 48000; }
            [[nodiscard]] std::expected<std::unique_ptr<AudioEncoder>, UString> open(const EncoderConfig &config) const override {
                auto encoder = std::make_unique<AacEncoder>(config);
                if (auto started = encoder->start(); !started) {
                    return std::unexpected(started.error());
                }
                return std::unique_ptr<AudioEncoder>(std::move(encoder));
            }
        };

    } // namespace

    std::unique_ptr<EncoderBackend> make_platform_aac_encoder() { return std::make_unique<AacBackend>(); }
    std::unique_ptr<EncoderBackend> make_platform_alac_encoder() { return nullptr; } // no Apple Lossless encoder on Windows

} // namespace SFT::Audio
