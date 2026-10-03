// AAC and Apple Lossless (M4A) export through AudioToolbox's ExtAudioFile, which ships both encoders. Not compiled elsewhere.

#include <Audio/EncodeCommon.hpp>

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>

namespace SFT::Audio {

    namespace {

        class ToolboxEncoder final : public detail::EncoderBase {
          public:
            ToolboxEncoder(const EncoderConfig &config, bool lossless) : config_(config), lossless_(lossless) {}
            ~ToolboxEncoder() override {
                if (file_ != nullptr) {
                    ExtAudioFileDispose(file_);
                    std::error_code ec;
                    std::filesystem::remove(temp_path_, ec); // an unfinished export leaves nothing behind
                }
            }

            [[nodiscard]] std::expected<void, UString> start() {
                temp_path_ = config_.path;
                temp_path_ += ".part.m4a";
                AudioStreamBasicDescription file_format{};
                file_format.mSampleRate = config_.sample_rate;
                file_format.mChannelsPerFrame = config_.channels;
                if (lossless_) {
                    file_format.mFormatID = kAudioFormatAppleLossless;
                    file_format.mFormatFlags = config_.options.sample_format == SampleFormat::Int16 || config_.options.sample_format == SampleFormat::UInt8
                                                   ? kAppleLosslessFormatFlag_16BitSourceData
                                                   : kAppleLosslessFormatFlag_24BitSourceData;
                } else {
                    file_format.mFormatID = kAudioFormatMPEG4AAC;
                }
                CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8 *>(temp_path_.c_str()),
                                                                       static_cast<CFIndex>(temp_path_.string().size()), false);
                const OSStatus created = ExtAudioFileCreateWithURL(url, kAudioFileM4AType, &file_format, nullptr, kAudioFileFlags_EraseFile, &file_);
                CFRelease(url);
                if (created != noErr) {
                    file_ = nullptr;
                    return std::unexpected("audio: AudioToolbox cannot create this M4A (check the channel count and sample rate)");
                }
                // We hand it interleaved 32-bit float.
                AudioStreamBasicDescription client{};
                client.mSampleRate = config_.sample_rate;
                client.mFormatID = kAudioFormatLinearPCM;
                client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
                client.mChannelsPerFrame = config_.channels;
                client.mBitsPerChannel = 32;
                client.mBytesPerFrame = 4 * config_.channels;
                client.mFramesPerPacket = 1;
                client.mBytesPerPacket = client.mBytesPerFrame;
                if (ExtAudioFileSetProperty(file_, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client) != noErr) {
                    return std::unexpected("audio: AudioToolbox rejected the input format");
                }
                if (!lossless_ && config_.options.bitrate_bps > 0) {
                    AudioConverterRef converter = nullptr;
                    UInt32 size = sizeof(converter);
                    if (ExtAudioFileGetProperty(file_, kExtAudioFileProperty_AudioConverter, &size, &converter) == noErr && converter != nullptr) {
                        const UInt32 rate = config_.options.bitrate_bps;
                        AudioConverterSetProperty(converter, kAudioConverterEncodeBitRate, sizeof(rate), &rate);
                        // Changing the converter requires the file to pick the new configuration up.
                        CFArrayRef config_array = nullptr;
                        ExtAudioFileSetProperty(file_, kExtAudioFileProperty_ConverterConfig, sizeof(config_array), &config_array);
                    }
                }
                return {};
            }

            bool write(const f32 *interleaved, usize frames) override {
                if (!error_.empty()) {
                    return false;
                }
                constexpr usize chunk = 8192;
                for (usize done = 0; done < frames; done += chunk) {
                    const UInt32 n = static_cast<UInt32>(std::min(chunk, frames - done));
                    AudioBufferList list{};
                    list.mNumberBuffers = 1;
                    list.mBuffers[0].mNumberChannels = config_.channels;
                    list.mBuffers[0].mDataByteSize = n * config_.channels * sizeof(f32);
                    list.mBuffers[0].mData = const_cast<f32 *>(interleaved + done * config_.channels);
                    if (ExtAudioFileWrite(file_, n, &list) != noErr) {
                        return fail("audio: AudioToolbox failed while writing");
                    }
                }
                frames_ += frames;
                return true;
            }

            std::expected<void, UString> finish() override {
                if (!error_.empty()) {
                    return std::unexpected(error_);
                }
                const OSStatus status = ExtAudioFileDispose(file_);
                file_ = nullptr;
                if (status != noErr) {
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
            bool lossless_;
            std::filesystem::path temp_path_;
            ExtAudioFileRef file_ = nullptr;
        };

        class ToolboxBackend final : public EncoderBackend {
          public:
            explicit ToolboxBackend(bool lossless) : lossless_(lossless) {}
            [[nodiscard]] ustr name() const override { return lossless_ ? "Apple Lossless (AudioToolbox)"_ustr : "AAC (AudioToolbox)"_ustr; }
            [[nodiscard]] AudioFormat format() const override { return lossless_ ? AudioFormat::Alac : AudioFormat::Aac; }
            [[nodiscard]] u32 max_channels() const override { return lossless_ ? 8 : 48; }
            [[nodiscard]] std::expected<std::unique_ptr<AudioEncoder>, UString> open(const EncoderConfig &config) const override {
                auto encoder = std::make_unique<ToolboxEncoder>(config, lossless_);
                if (auto started = encoder->start(); !started) {
                    return std::unexpected(started.error());
                }
                return std::unique_ptr<AudioEncoder>(std::move(encoder));
            }

          private:
            bool lossless_;
        };

    } // namespace

    std::unique_ptr<EncoderBackend> make_platform_aac_encoder() { return std::make_unique<ToolboxBackend>(false); }
    std::unique_ptr<EncoderBackend> make_platform_alac_encoder() { return std::make_unique<ToolboxBackend>(true); }

} // namespace SFT::Audio
