// AAC / M4A / ALAC / CAF / MP4 audio through AudioToolbox (the system's licensed decoders).

#include <Audio/Decoder.hpp>

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cstring>

namespace SFT::Audio {

    namespace {

        OSStatus read_proc(void *client, SInt64 position, UInt32 request, void *buffer, UInt32 *actual);
        SInt64 size_proc(void *client);

        class AudioToolboxDecoder final : public StreamDecoder {
          public:
            ~AudioToolboxDecoder() override {
                if (ext_ != nullptr) {
                    ExtAudioFileDispose(ext_);
                }
                if (file_ != nullptr) {
                    AudioFileClose(file_);
                }
            }

            [[nodiscard]] bool open(EncodedBytes bytes) {
                bytes_ = std::move(bytes);
                if (AudioFileOpenWithCallbacks(this, read_proc, nullptr, size_proc, nullptr, 0, &file_) != noErr) {
                    return false;
                }
                if (ExtAudioFileWrapAudioFileID(file_, false, &ext_) != noErr) {
                    return false;
                }
                AudioStreamBasicDescription file_format{};
                UInt32 size = sizeof(file_format);
                if (ExtAudioFileGetProperty(ext_, kExtAudioFileProperty_FileDataFormat, &size, &file_format) != noErr ||
                    file_format.mChannelsPerFrame == 0 || file_format.mSampleRate <= 0.0) {
                    return false;
                }
                info_.channels = file_format.mChannelsPerFrame;
                info_.sample_rate = static_cast<u32>(file_format.mSampleRate);
                info_.codec = "audiotoolbox";

                AudioStreamBasicDescription client{};
                client.mSampleRate = file_format.mSampleRate;
                client.mFormatID = kAudioFormatLinearPCM;
                client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
                client.mBitsPerChannel = 32;
                client.mChannelsPerFrame = file_format.mChannelsPerFrame;
                client.mFramesPerPacket = 1;
                client.mBytesPerFrame = file_format.mChannelsPerFrame * 4;
                client.mBytesPerPacket = client.mBytesPerFrame;
                if (ExtAudioFileSetProperty(ext_, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client) != noErr) {
                    return false;
                }
                SInt64 length = 0;
                size = sizeof(length);
                if (ExtAudioFileGetProperty(ext_, kExtAudioFileProperty_FileLengthFrames, &size, &length) == noErr && length > 0) {
                    info_.total_frames = static_cast<u64>(length);
                }
                return true;
            }

            [[nodiscard]] const AudioStreamInfo &info() const override { return info_; }

            u64 read(f32 *out, u64 frames) override {
                u64 done = 0;
                while (done < frames) {
                    AudioBufferList list{};
                    list.mNumberBuffers = 1;
                    list.mBuffers[0].mNumberChannels = info_.channels;
                    list.mBuffers[0].mData = out + done * info_.channels;
                    list.mBuffers[0].mDataByteSize = static_cast<UInt32>((frames - done) * info_.channels * sizeof(f32));
                    UInt32 count = static_cast<UInt32>(frames - done);
                    if (ExtAudioFileRead(ext_, &count, &list) != noErr || count == 0) {
                        break;
                    }
                    done += count;
                }
                position_ += done;
                return done;
            }

            bool seek(u64 frame) override {
                if (ExtAudioFileSeek(ext_, static_cast<SInt64>(frame)) != noErr) {
                    return false;
                }
                position_ = frame;
                return true;
            }

            [[nodiscard]] u64 position() const override { return position_; }
            [[nodiscard]] const EncodedBytes &bytes() const { return bytes_; }

          private:
            EncodedBytes bytes_;
            AudioFileID file_ = nullptr;
            ExtAudioFileRef ext_ = nullptr;
            AudioStreamInfo info_;
            u64 position_ = 0;
        };

        OSStatus read_proc(void *client, SInt64 position, UInt32 request, void *buffer, UInt32 *actual) {
            const EncodedBytes &bytes = static_cast<AudioToolboxDecoder *>(client)->bytes();
            const SInt64 size = static_cast<SInt64>(bytes->size());
            if (position >= size) {
                *actual = 0;
                return noErr;
            }
            const UInt32 count = static_cast<UInt32>(std::min<SInt64>(request, size - position));
            std::memcpy(buffer, bytes->data() + position, count);
            *actual = count;
            return noErr;
        }

        SInt64 size_proc(void *client) { return static_cast<SInt64>(static_cast<AudioToolboxDecoder *>(client)->bytes()->size()); }

        class AudioToolboxBackend final : public DecoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "AudioToolbox (AAC, M4A, ALAC, CAF)"_ustr; }
            [[nodiscard]] std::vector<std::string_view> extensions() const override { return {".m4a", ".mp4", ".aac", ".caf", ".m4b", ".alac", ".3gp"}; }
            [[nodiscard]] int probe(std::span<const std::byte> header, const ustr &extension) const override {
                if (header.size() >= 12 && std::memcmp(header.data() + 4, "ftyp", 4) == 0) {
                    return 85;
                }
                if (header.size() >= 4 && std::memcmp(header.data(), "caff", 4) == 0) {
                    return 95;
                }
                if (header.size() >= 2 && static_cast<unsigned>(header[0]) == 0xFF && (static_cast<unsigned>(header[1]) & 0xF6) == 0xF0) {
                    return 50;
                }
                for (std::string_view e : extensions()) {
                    if (e == extension) {
                        return 10;
                    }
                }
                return 0;
            }
            [[nodiscard]] std::unique_ptr<StreamDecoder> open(EncodedBytes bytes) const override {
                auto decoder = std::make_unique<AudioToolboxDecoder>();
                return decoder->open(std::move(bytes)) ? std::move(decoder) : nullptr;
            }
        };

    } // namespace

    std::unique_ptr<DecoderBackend> make_platform_backend() { return std::make_unique<AudioToolboxBackend>(); }

} // namespace SFT::Audio
