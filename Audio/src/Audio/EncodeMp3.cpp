#include <Audio/EncodeCommon.hpp>

#include <Foundation/DynamicLibrary.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <mutex>

/// MP3 encoding through libmp3lame, loaded at run time. LAME is LGPL; loading it dynamically (never linking or shipping it)
/// keeps this engine's own licence untouched while letting a machine that has the library export MP3. Windows and macOS can
/// also encode through the OS (see the platform encoders).
namespace SFT::Audio {

    namespace {

        struct LameApi {
            Foundation::DynamicLibrary library;
            void *(*init)() = nullptr;
            int (*close)(void *) = nullptr;
            int (*set_in_samplerate)(void *, int) = nullptr;
            int (*set_out_samplerate)(void *, int) = nullptr;
            int (*set_num_channels)(void *, int) = nullptr;
            int (*set_mode)(void *, int) = nullptr;
            int (*set_brate)(void *, int) = nullptr;
            int (*set_vbr)(void *, int) = nullptr;
            int (*set_vbr_quality)(void *, float) = nullptr;
            int (*set_vbr_mean_bitrate)(void *, int) = nullptr;
            int (*set_quality)(void *, int) = nullptr;
            int (*init_params)(void *) = nullptr;
            int (*encode_float)(void *, const float *, int, unsigned char *, int) = nullptr;
            int (*encode_flush)(void *, unsigned char *, int) = nullptr;
            void (*id3_init)(void *) = nullptr;
            void (*id3_add_v2)(void *) = nullptr;
            void (*id3_title)(void *, const char *) = nullptr;
            void (*id3_artist)(void *, const char *) = nullptr;
            void (*id3_album)(void *, const char *) = nullptr;
            void (*id3_year)(void *, const char *) = nullptr;
            void (*id3_comment)(void *, const char *) = nullptr;
            int (*id3_genre)(void *, const char *) = nullptr;
            int (*id3_track)(void *, const char *) = nullptr;
            bool ok = false;
        };

        const LameApi &lame() {
            static LameApi api;
            static std::once_flag once;
            std::call_once(once, [] {
                std::vector<UString> candidates;
                if (const char *override_path = std::getenv("STURDY_LAME_PATH")) {
                    candidates.emplace_back(override_path);
                }
#if defined(_WIN32)
                for (const char *n : {"libmp3lame.dll", "libmp3lame-0.dll", "mp3lame.dll"}) candidates.emplace_back(n);
#elif defined(__APPLE__)
                for (const char *n : {"libmp3lame.dylib", "/opt/homebrew/lib/libmp3lame.dylib", "/usr/local/lib/libmp3lame.dylib"}) candidates.emplace_back(n);
#else
                for (const char *n : {"libmp3lame.so.0", "libmp3lame.so"}) candidates.emplace_back(n);
#endif
                for (const UString &name : candidates) {
                    if (api.library.load(name)) {
                        break;
                    }
                }
                if (!api.library.is_loaded()) {
                    return;
                }
                const auto &l = api.library;
#define SFT_LAME(member, name) api.member = l.symbol_as<decltype(api.member)>(name)
                SFT_LAME(init, "lame_init");
                SFT_LAME(close, "lame_close");
                SFT_LAME(set_in_samplerate, "lame_set_in_samplerate");
                SFT_LAME(set_out_samplerate, "lame_set_out_samplerate");
                SFT_LAME(set_num_channels, "lame_set_num_channels");
                SFT_LAME(set_mode, "lame_set_mode");
                SFT_LAME(set_brate, "lame_set_brate");
                SFT_LAME(set_vbr, "lame_set_VBR");
                SFT_LAME(set_vbr_quality, "lame_set_VBR_quality");
                SFT_LAME(set_vbr_mean_bitrate, "lame_set_VBR_mean_bitrate_kbps");
                SFT_LAME(set_quality, "lame_set_quality");
                SFT_LAME(init_params, "lame_init_params");
                SFT_LAME(encode_float, "lame_encode_buffer_interleaved_ieee_float");
                SFT_LAME(encode_flush, "lame_encode_flush");
                SFT_LAME(id3_init, "id3tag_init");
                SFT_LAME(id3_add_v2, "id3tag_add_v2");
                SFT_LAME(id3_title, "id3tag_set_title");
                SFT_LAME(id3_artist, "id3tag_set_artist");
                SFT_LAME(id3_album, "id3tag_set_album");
                SFT_LAME(id3_year, "id3tag_set_year");
                SFT_LAME(id3_comment, "id3tag_set_comment");
                SFT_LAME(id3_genre, "id3tag_set_genre");
                SFT_LAME(id3_track, "id3tag_set_track");
#undef SFT_LAME
                api.ok = api.init && api.close && api.set_in_samplerate && api.set_num_channels && api.init_params && api.encode_float && api.encode_flush;
            });
            return api;
        }

        class Mp3Encoder final : public detail::EncoderBase {
          public:
            explicit Mp3Encoder(const EncoderConfig &config) : config_(config) {}
            ~Mp3Encoder() override {
                if (handle_ != nullptr) {
                    lame().close(handle_);
                }
            }

            [[nodiscard]] std::expected<void, UString> start() {
                const LameApi &api = lame();
                handle_ = api.init();
                if (handle_ == nullptr) {
                    return std::unexpected("audio: libmp3lame could not be initialised");
                }
                const EncodeOptions &o = config_.options;
                api.set_in_samplerate(handle_, static_cast<int>(config_.sample_rate));
                if (api.set_out_samplerate) api.set_out_samplerate(handle_, static_cast<int>(config_.sample_rate));
                api.set_num_channels(handle_, static_cast<int>(config_.channels));
                if (api.set_mode) api.set_mode(handle_, config_.channels == 1 ? 3 /*MONO*/ : 1 /*JOINT_STEREO*/);
                if (api.set_quality) api.set_quality(handle_, 2); // algorithm effort, not bitrate: 2 is "near best"
                if (o.bitrate_bps > 0 && !o.variable_bitrate) {
                    if (api.set_vbr) api.set_vbr(handle_, 0);
                    if (api.set_brate) api.set_brate(handle_, static_cast<int>(o.bitrate_bps / 1000));
                } else if (o.bitrate_bps > 0) {
                    if (api.set_vbr) api.set_vbr(handle_, 3 /*ABR*/);
                    if (api.set_vbr_mean_bitrate) api.set_vbr_mean_bitrate(handle_, static_cast<int>(o.bitrate_bps / 1000));
                } else {
                    if (api.set_vbr) api.set_vbr(handle_, 4 /*vbr_mtrh*/);
                    // quality 0..1 -> LAME's V9 (smallest) .. V0 (best); the default V4 is about 165 kbps.
                    const f32 v = o.quality >= 0.0f ? 9.0f - std::clamp(o.quality, 0.0f, 1.0f) * 9.0f : 4.0f;
                    if (api.set_vbr_quality) api.set_vbr_quality(handle_, v);
                }
                if (api.id3_init) {
                    api.id3_init(handle_);
                    if (api.id3_add_v2) api.id3_add_v2(handle_);
                    for (const auto &[key, value] : o.tags) {
                        if (key == "title"_ustr && api.id3_title) api.id3_title(handle_, value.c_str());
                        else if (key == "artist"_ustr && api.id3_artist) api.id3_artist(handle_, value.c_str());
                        else if (key == "album"_ustr && api.id3_album) api.id3_album(handle_, value.c_str());
                        else if (key == "date"_ustr && api.id3_year) api.id3_year(handle_, value.substr(0, 4).c_str());
                        else if (key == "comment"_ustr && api.id3_comment) api.id3_comment(handle_, value.c_str());
                        else if (key == "genre"_ustr && api.id3_genre) api.id3_genre(handle_, value.c_str());
                        else if (key == "track"_ustr && api.id3_track) api.id3_track(handle_, value.c_str());
                    }
                }
                if (api.init_params(handle_) < 0) {
                    return std::unexpected("audio: libmp3lame rejected these settings (sample rate or bitrate not valid for MP3)");
                }
                auto file = Foundation::Io::FileWriter::create(config_.path);
                if (!file) {
                    return std::unexpected(io_error(file.error()));
                }
                file_ = std::move(*file);
                return {};
            }

            bool write(const f32 *interleaved, usize frames) override {
                return write_chunked(interleaved, frames, config_.channels, [this](const f32 *chunk, usize n) {
                    // LAME's worst case: 1.25 * samples + 7200 bytes per call.
                    out_.resize(static_cast<usize>(1.25 * static_cast<f64>(n) + 7200.0));
                    const int bytes = lame().encode_float(handle_, chunk, static_cast<int>(n), out_.data(), static_cast<int>(out_.size()));
                    if (bytes < 0) {
                        return fail("audio: libmp3lame failed while encoding");
                    }
                    return bytes == 0 || file_.write(out_.data(), static_cast<usize>(bytes)) || fail(io_error(file_.error()));
                });
            }

            std::expected<void, UString> finish() override {
                if (!error_.empty()) {
                    return std::unexpected(error_);
                }
                out_.resize(7200);
                const int bytes = lame().encode_flush(handle_, out_.data(), static_cast<int>(out_.size()));
                if (bytes > 0 && !file_.write(out_.data(), static_cast<usize>(bytes))) {
                    return std::unexpected(io_error(file_.error()));
                }
                return file_.commit();
            }

          private:
            EncoderConfig config_;
            Foundation::Io::FileWriter file_;
            void *handle_ = nullptr;
            std::vector<unsigned char> out_;
        };

        class Mp3Backend final : public EncoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "MP3 (libmp3lame, loaded at run time)"_ustr; }
            [[nodiscard]] AudioFormat format() const override { return AudioFormat::Mp3; }
            [[nodiscard]] u32 max_channels() const override { return 2; }
            [[nodiscard]] u32 constrain_sample_rate(u32 requested) const override {
                constexpr std::array<u32, 9> rates{8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
                u32 best = rates[0];
                for (u32 r : rates) {
                    if (std::abs(static_cast<i64>(r) - static_cast<i64>(requested)) < std::abs(static_cast<i64>(best) - static_cast<i64>(requested))) {
                        best = r;
                    }
                }
                return best;
            }
            [[nodiscard]] std::expected<std::unique_ptr<AudioEncoder>, UString> open(const EncoderConfig &config) const override {
                if (!lame().ok) {
                    return std::unexpected("audio: MP3 export needs libmp3lame (not found; install it or set STURDY_LAME_PATH to its location)");
                }
                auto encoder = std::make_unique<Mp3Encoder>(config);
                if (auto started = encoder->start(); !started) {
                    return std::unexpected(started.error());
                }
                return std::unique_ptr<AudioEncoder>(std::move(encoder));
            }
        };

    } // namespace

    std::unique_ptr<EncoderBackend> make_mp3_encoder() { return std::make_unique<Mp3Backend>(); }

} // namespace SFT::Audio
