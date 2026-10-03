// AAC / M4A / MP4 audio / WMA / ADTS through Windows Media Foundation, which ships a licensed decoder for each, so the
// engine carries none of those codecs itself.

#include <Audio/Decoder.hpp>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace SFT::Audio {

    namespace {

        using Microsoft::WRL::ComPtr;

        // COM and Media Foundation are initialised once for the process (decoders may be created on any thread).
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

        class MediaFoundationDecoder final : public StreamDecoder {
          public:
            [[nodiscard]] bool open(EncodedBytes bytes) {
                if (!ensure_media_foundation()) {
                    return false;
                }
                bytes_ = std::move(bytes);
                ComPtr<IStream> stream;
                stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE *>(bytes_->data()), static_cast<UINT>(bytes_->size())));
                if (!stream) {
                    return false;
                }
                ComPtr<IMFByteStream> byte_stream;
                if (FAILED(MFCreateMFByteStreamOnStream(stream.Get(), &byte_stream))) {
                    return false;
                }
                if (FAILED(MFCreateSourceReaderFromByteStream(byte_stream.Get(), nullptr, &reader_))) {
                    return false;
                }
                reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
                reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);

                ComPtr<IMFMediaType> native;
                if (FAILED(reader_->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &native))) {
                    return false;
                }
                UINT32 channels = 0, rate = 0;
                native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
                native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                if (channels == 0 || rate == 0) {
                    return false;
                }

                ComPtr<IMFMediaType> type;
                if (FAILED(MFCreateMediaType(&type))) {
                    return false;
                }
                type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
                type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
                type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
                type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
                type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 4);
                type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 4);
                type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
                if (FAILED(reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, type.Get()))) {
                    return false;
                }

                info_.channels = channels;
                info_.sample_rate = rate;
                info_.codec = "media foundation";
                PROPVARIANT duration;
                PropVariantInit(&duration);
                if (SUCCEEDED(reader_->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) &&
                    duration.vt == VT_UI8) {
                    info_.total_frames = static_cast<u64>(duration.uhVal.QuadPart) * rate / 10000000ull;
                }
                PropVariantClear(&duration);
                return true;
            }

            [[nodiscard]] const AudioStreamInfo &info() const override { return info_; }

            u64 read(f32 *out, u64 frames) override {
                const usize wanted = static_cast<usize>(frames) * info_.channels;
                usize written = 0;
                while (written < wanted) {
                    if (pending_position_ >= pending_.size()) {
                        pending_.clear();
                        pending_position_ = 0;
                        if (!fetch()) {
                            break;
                        }
                    }
                    const usize take = std::min(pending_.size() - pending_position_, wanted - written);
                    std::memcpy(out + written, pending_.data() + pending_position_, take * sizeof(f32));
                    pending_position_ += take;
                    written += take;
                }
                const u64 produced = written / info_.channels;
                position_ += produced;
                return produced;
            }

            bool seek(u64 frame) override {
                PROPVARIANT position;
                PropVariantInit(&position);
                position.vt = VT_I8;
                position.hVal.QuadPart = static_cast<LONGLONG>(frame * 10000000ull / info_.sample_rate);
                const HRESULT result = reader_->SetCurrentPosition(GUID_NULL, position);
                PropVariantClear(&position);
                if (FAILED(result)) {
                    return false;
                }
                pending_.clear();
                pending_position_ = 0;
                position_ = frame;
                return true;
            }

            [[nodiscard]] u64 position() const override { return position_; }

          private:
            bool fetch() {
                DWORD flags = 0;
                ComPtr<IMFSample> sample;
                if (FAILED(reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample))) {
                    return false;
                }
                if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 && !sample) {
                    return false;
                }
                if (!sample) {
                    return (flags & MF_SOURCE_READERF_ENDOFSTREAM) == 0 ? fetch() : false; // a gap marker: try the next sample
                }
                ComPtr<IMFMediaBuffer> buffer;
                if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
                    return false;
                }
                BYTE *data = nullptr;
                DWORD length = 0;
                if (FAILED(buffer->Lock(&data, nullptr, &length))) {
                    return false;
                }
                const usize floats = length / sizeof(f32);
                pending_.resize(floats);
                std::memcpy(pending_.data(), data, floats * sizeof(f32));
                buffer->Unlock();
                return floats > 0;
            }

            EncodedBytes bytes_;
            ComPtr<IMFSourceReader> reader_;
            AudioStreamInfo info_;
            std::vector<f32> pending_;
            usize pending_position_ = 0;
            u64 position_ = 0;
        };

        class MediaFoundationBackend final : public DecoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "Windows Media Foundation (AAC, M4A, WMA)"_ustr; }
            [[nodiscard]] std::vector<std::string_view> extensions() const override { return {".m4a", ".mp4", ".aac", ".wma", ".m4b", ".3gp"}; }
            [[nodiscard]] int probe(std::span<const std::byte> header, const ustr &extension) const override {
                if (header.size() >= 12 && std::memcmp(header.data() + 4, "ftyp", 4) == 0) {
                    return 85; // MP4 family (AAC, ALAC where installed)
                }
                static constexpr unsigned char asf[8] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11};
                if (header.size() >= 8 && std::memcmp(header.data(), asf, 8) == 0) {
                    return 90; // ASF / WMA
                }
                if (header.size() >= 2 && static_cast<unsigned>(header[0]) == 0xFF && (static_cast<unsigned>(header[1]) & 0xF6) == 0xF0) {
                    return 50; // ADTS AAC (layer bits 00)
                }
                for (std::string_view e : extensions()) {
                    if (e == extension) {
                        return 10;
                    }
                }
                return 0;
            }
            [[nodiscard]] std::unique_ptr<StreamDecoder> open(EncodedBytes bytes) const override {
                auto decoder = std::make_unique<MediaFoundationDecoder>();
                return decoder->open(std::move(bytes)) ? std::move(decoder) : nullptr;
            }
        };

    } // namespace

    std::unique_ptr<DecoderBackend> make_platform_backend() { return std::make_unique<MediaFoundationBackend>(); }

} // namespace SFT::Audio
