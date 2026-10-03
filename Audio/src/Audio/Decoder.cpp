#include <Audio/Decoder.hpp>
#include <Audio/Demux.hpp>
#include <Foundation/Iter.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <map>

namespace SFT::Audio {

    namespace {

        UString lower_extension(const UString &hint) {
            UString e = ascii_lower(hint);
            if (!e.empty() && e.front() != U'.') {
                e.insert(0, U'.');
            }
            return e;
        }

        u32 read_u32_le(const std::byte *p) {
            return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
        }

    } // namespace

    // ---- registry ---------------------------------------------------------------------------------------------------

    void DecoderRegistry::add(std::unique_ptr<DecoderBackend> backend) {
        if (backend) {
            backends_.push_back(std::move(backend));
        }
    }

    DecoderRegistry DecoderRegistry::with_defaults() {
        DecoderRegistry registry;
        registry.add(make_miniaudio_backend()); // WAV (PCM/float/ADPCM), FLAC, MP3
        registry.add(make_aiff_backend());
        registry.add(make_vorbis_backend());
        registry.add(make_opus_backend());      // null without opusfile
        registry.add(make_container_backend()); // audio inside Matroska/WebM/MP4
        registry.add(make_platform_backend());  // AAC/M4A/ALAC/WMA through the OS, null elsewhere
        return registry;
    }

    DecoderRegistry &DecoderRegistry::global() {
        static DecoderRegistry registry = with_defaults();
        return registry;
    }

    std::vector<UString> DecoderRegistry::supported_extensions() const {
        std::vector<UString> all;
        for (const auto &backend : backends_) {
            for (UString &e : backend->extensions()) {
                if (!Foundation::iter(all).any([&e](const UString &existing) { return existing == e; })) {
                    all.push_back(std::move(e));
                }
            }
        }
        return all;
    }

    std::expected<std::unique_ptr<StreamDecoder>, UString> DecoderRegistry::open(EncodedBytes bytes, const UString &extension_hint) const {
        if (!bytes || bytes->empty()) {
            return std::unexpected("audio: the file is empty");
        }
        const UString extension = lower_extension(extension_hint);
        const std::span<const std::byte> header(bytes->data(), std::min<usize>(bytes->size(), 64));

        struct Candidate {
            int score;
            const DecoderBackend *backend;
        };
        std::vector<Candidate> candidates;
        for (const auto &backend : backends_) {
            int score = backend->probe(header, extension);
            if (score > 0) {
                candidates.push_back({score, backend.get()});
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.score > b.score; });

        UString tried;
        for (const Candidate &c : candidates) {
            if (auto decoder = c.backend->open(bytes)) {
                return decoder;
            }
            tried += tried.empty() ? UString{} : UString{", "};
            tried += c.backend->name();
        }
        if (tried.empty()) {
            return std::unexpected(extension.empty() ? UString{"audio: no decoder recognises this format"} : UString{std::format("audio: no decoder recognises this format ({})", extension)});
        }
        return std::unexpected(UString{std::format("audio: the data looked like a supported format but failed to open (tried: {})", tried)});
    }

    std::expected<EncodedBytes, UString> read_file_bytes(const std::filesystem::path &path, Foundation::Io::AccessHint hint) {
        auto blob = Foundation::Io::open_blob(path, hint);
        if (!blob) {
            return std::unexpected(UString{std::format("audio: {}", blob.error())});
        }
        return std::move(*blob);
    }

    std::expected<std::unique_ptr<StreamDecoder>, UString> DecoderRegistry::open_file(const std::filesystem::path &path) const {
        auto bytes = read_file_bytes(path);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return open(*bytes, text_from_bytes(path.extension().string()));
    }

    std::expected<std::shared_ptr<SampleBuffer>, UString> decode_all(StreamDecoder &decoder) {
        const AudioStreamInfo &info = decoder.info();
        if (info.channels == 0 || info.sample_rate == 0) {
            return std::unexpected("audio: the decoder reported no channels or sample rate");
        }
        auto samples = std::make_shared<std::vector<f32>>();
        if (info.total_frames > 0) {
            samples->reserve(static_cast<usize>(info.total_frames) * info.channels);
        }
        std::vector<f32> chunk(4096 * static_cast<usize>(info.channels));
        for (;;) {
            const u64 frames = decoder.read(chunk.data(), 4096);
            if (frames == 0) {
                break;
            }
            samples->insert(samples->end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(frames * info.channels));
        }
        auto buffer = std::make_shared<SampleBuffer>();
        buffer->channels = info.channels;
        buffer->layout = info.layout;
        buffer->sample_rate = info.sample_rate;
        buffer->samples = std::move(samples);
        buffer->markers = info.markers;
        buffer->loop = info.loop;
        return buffer;
    }

    std::expected<std::shared_ptr<SampleBuffer>, UString> load_sound_memory(EncodedBytes bytes, const UString &extension_hint) {
        auto decoder = DecoderRegistry::global().open(std::move(bytes), extension_hint);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        return decode_all(**decoder);
    }

    std::expected<std::shared_ptr<SampleBuffer>, UString> load_sound_file(const std::filesystem::path &path) {
        // Decoded start to finish once: read-ahead helps, and the file mapping is dropped as soon as the decode ends.
        auto bytes = read_file_bytes(path, Foundation::Io::AccessHint::Sequential);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return load_sound_memory(*bytes, text_from_bytes(path.extension().string()));
    }

    // ---- WAV metadata + writer -----------------------------------------------------------------------------------------

    void parse_wav_metadata(std::span<const std::byte> data, AudioStreamInfo &info) {
        const bool riff = data.size() >= 12 && (std::memcmp(data.data(), "RIFF", 4) == 0 || std::memcmp(data.data(), "RF64", 4) == 0);
        if (!riff || std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
            return;
        }
        std::map<u32, u64> cue_frames;
        std::map<u32, UString> cue_labels;
        usize offset = 12;
        while (offset + 8 <= data.size()) {
            const std::byte *chunk = data.data() + offset;
            const u32 size = read_u32_le(chunk + 4);
            const usize body = offset + 8;
            const usize available = std::min<usize>(size, data.size() - body);
            if (std::memcmp(chunk, "fmt ", 4) == 0 && available >= 16) {
                // WAVE_FORMAT_EXTENSIBLE carries the speaker mask: that is what makes a 6-channel file 5.1 rather than "6 tracks".
                const u32 channels = static_cast<u32>(static_cast<u16>(static_cast<u8>(data[body + 2]) | (static_cast<u8>(data[body + 3]) << 8)));
                const u32 tag = static_cast<u32>(static_cast<u8>(data[body]) | (static_cast<u8>(data[body + 1]) << 8));
                u32 mask = 0;
                if (tag == 0xFFFE && available >= 40) {
                    mask = read_u32_le(data.data() + body + 20);
                }
                if (channels > 0) {
                    info.layout = layout_from_wave_mask(mask, channels);
                }
            } else if (std::memcmp(chunk, "cue ", 4) == 0 && available >= 4) {
                const u32 count = read_u32_le(data.data() + body);
                for (u32 i = 0; i < count && 4 + (i + 1) * 24 <= available; ++i) {
                    const std::byte *point = data.data() + body + 4 + static_cast<usize>(i) * 24;
                    cue_frames[read_u32_le(point)] = read_u32_le(point + 20); // sample offset
                }
            } else if (std::memcmp(chunk, "LIST", 4) == 0 && available >= 4 && std::memcmp(data.data() + body, "INFO", 4) == 0) {
                usize sub = body + 4;
                while (sub + 8 <= body + available) {
                    const u32 sub_size = read_u32_le(data.data() + sub + 4);
                    if (sub + 8 + sub_size > data.size()) {
                        break;
                    }
                    const std::string id(reinterpret_cast<const char *>(data.data() + sub), 4);
                    const char *key = id == "INAM" ? "title" : id == "IART" ? "artist" : id == "IPRD" ? "album" : id == "ICMT" ? "comment"
                                      : id == "ICRD" ? "date" : id == "IGNR" ? "genre" : id == "ITRK" ? "track" : id == "ISFT" ? "software" : nullptr;
                    if (key != nullptr) {
                        const char *text = reinterpret_cast<const char *>(data.data() + sub + 8);
                        usize length = sub_size;
                        while (length > 0 && text[length - 1] == '\0') {
                            --length;
                        }
                        info.tags.emplace_back(key, text_from_bytes(std::string_view{text, length}));
                    }
                    sub += 8 + sub_size + (sub_size & 1u);
                }
            } else if (std::memcmp(chunk, "LIST", 4) == 0 && available >= 4 && std::memcmp(data.data() + body, "adtl", 4) == 0) {
                usize sub = body + 4;
                while (sub + 8 <= body + available) {
                    const u32 sub_size = read_u32_le(data.data() + sub + 4);
                    if (std::memcmp(data.data() + sub, "labl", 4) == 0 && sub_size >= 4 && sub + 8 + sub_size <= data.size()) {
                        const u32 id = read_u32_le(data.data() + sub + 8);
                        const char *text = reinterpret_cast<const char *>(data.data() + sub + 12);
                        usize length = sub_size - 4;
                        while (length > 0 && text[length - 1] == '\0') {
                            --length;
                        }
                        cue_labels[id] = text_from_bytes(std::string_view{text, length});
                    }
                    sub += 8 + sub_size + (sub_size & 1u);
                }
            } else if (std::memcmp(chunk, "smpl", 4) == 0 && available >= 36) {
                const u32 loops = read_u32_le(data.data() + body + 28);
                if (loops > 0 && available >= 36 + 24) {
                    const std::byte *loop = data.data() + body + 36;
                    const u64 start = read_u32_le(loop + 8);
                    const u64 end = static_cast<u64>(read_u32_le(loop + 12)) + 1; // stored inclusive
                    if (end > start) {
                        info.loop = LoopRegion{start, end};
                    }
                }
            }
            offset = body + size + (size & 1u);
        }
        for (const auto &[id, frame] : cue_frames) {
            const auto label = cue_labels.find(id);
            info.markers.push_back(AudioMarker{label != cue_labels.end() && !label->second.empty() ? label->second : UString{std::format("cue {}", id)}, frame});
        }
        std::sort(info.markers.begin(), info.markers.end(), [](const AudioMarker &a, const AudioMarker &b) { return a.frame < b.frame; });
    }

} // namespace SFT::Audio
