/// Exercises the audio C ABI against a real `Engine`, headless (no playback device): setup with rare layouts, sounds and voices,
/// user-fed streams, a custom sink on the primary output, entity-attached sources, effects and handle lifetime rules.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <thread>
#include <vector>

#include <Audio/Decoder.hpp>
#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    int failures = 0;
    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "AudioFfiTest: %s\n", description);
            ++failures;
        }
    }

    struct SinkProbe {
        std::atomic<uint64_t> frames{0};
        std::atomic<uint32_t> channels{0};
        std::atomic<float> peak{0.0f};
    };

    void STURDY_ABI_CALL on_audio(void *user, const float *interleaved, uint32_t frames, uint32_t channels, uint32_t) {
        auto *probe = static_cast<SinkProbe *>(user);
        probe->channels = channels;
        float peak = probe->peak.load();
        for (uint32_t i = 0; i < frames * channels; ++i) peak = std::max(peak, std::fabs(interleaved[i]));
        probe->peak = peak;
        probe->frames += frames;
    }

    bool wait_until(const std::function<bool()> &done) {
        for (int i = 0; i < 400 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return done();
    }
} // namespace

int main() {
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::ScopedHandle;

    SFT::Engine::Engine engine_object;
    const ScopedHandle engine_handle{HandleKind::Engine, &engine_object};
    const SturdyEngine engine{engine_handle.token()};

    SturdyAudioStats stats{};
    check(sturdy_audio_stats(engine, &stats) == STURDY_ERROR_NOT_AVAILABLE, "audio calls fail cleanly before enable");

    SturdyAudioConfig config{};
    config.struct_size = sizeof(config);
    config.layout = STURDY_AUDIO_LAYOUT_CHANNEL_COUNT;
    config.channel_count = 11; // a rare layout
    config.open_device = STURDY_FALSE;
    check(sturdy_audio_enable(engine, &config) == STURDY_OK, "audio enables without a device on an 11-speaker layout");
    config.channel_count = 99;
    check(sturdy_audio_enable(engine, &config) == STURDY_ERROR_OUT_OF_RANGE, "an impossible channel count is rejected");
    config.channel_count = 11;
    config.struct_size = 4;
    check(sturdy_audio_enable(engine, &config) == STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "a short struct is rejected");

    // A custom sink on the primary output drives the mix with no device.
    SinkProbe probe;
    SturdyAudioSink sink{};
    check(sturdy_audio_sink_start(engine, 0, on_audio, &probe, 512, STURDY_TRUE, &sink) == STURDY_OK, "a sink starts on the primary output");

    // A stream fed from the outside (voice chat, video soundtrack), two channels at 44.1 kHz.
    SturdyAudioStream stream{};
    check(sturdy_audio_stream_create(engine, 2, 44100, 1.0f, &stream) == STURDY_OK, "a stream is created");
    std::vector<float> tone(44100 * 2);
    for (size_t i = 0; i < 44100; ++i) tone[i * 2] = tone[i * 2 + 1] = 0.4f * std::sin(2.0f * std::numbers::pi_v<float> * 330.0f * static_cast<float>(i) / 44100.0f);
    uint32_t accepted = 0;
    check(sturdy_audio_stream_push(stream, tone.data(), 44100, &accepted) == STURDY_OK && accepted == 44100, "frames are accepted");
    SturdyPlayOptions options{};
    options.struct_size = sizeof(options);
    options.volume = 1.0f;
    SturdyVoice voice{};
    check(sturdy_audio_stream_play(engine, stream, &options, &voice) == STURDY_OK, "the stream plays");
    check(wait_until([&] { return probe.peak.load() > 0.05f; }), "the stream's audio reaches the sink");
    check(probe.channels == 11, "the sink is delivered the layout's eleven channels");

    SturdyBool playing = STURDY_FALSE, finished = STURDY_FALSE;
    double position = 0.0;
    check(sturdy_voice_state(voice, &playing, &finished, &position) == STURDY_OK && finished == STURDY_FALSE, "voice state is readable");
    check(sturdy_voice_set_volume(voice, 0.5f) == STURDY_OK && sturdy_voice_pause(voice) == STURDY_OK && sturdy_voice_resume(voice) == STURDY_OK, "voice controls");
    check(sturdy_audio_stream_close(stream) == STURDY_OK, "the stream closes");
    double buffered = 0.0;
    check(sturdy_audio_stream_buffered_seconds(stream, &buffered) == STURDY_OK, "buffered time is readable");

    // A sound from a file, attached to an entity.
    const auto path = std::filesystem::temp_directory_path() / "sturdy_ffi_audio_test.wav";
    {
        SFT::Audio::SampleBuffer buffer;
        buffer.channels = 1;
        buffer.sample_rate = 48000;
        auto samples = std::make_shared<std::vector<float>>(48000);
        for (size_t i = 0; i < samples->size(); ++i) (*samples)[i] = 0.3f * std::sin(0.06f * static_cast<float>(i));
        buffer.samples = samples;
        check(SFT::Audio::write_wav(path, buffer).has_value(), "the test file is written");
    }
    SturdySound sound{};
    check(sturdy_sound_load(engine, path.string().c_str(), &sound) == STURDY_OK, "a sound loads");
    SturdyVoice second{};
    check(sturdy_sound_play(engine, sound, nullptr, &second) == STURDY_OK, "a sound plays with default options");
    check(sturdy_audio_stats(engine, &stats) == STURDY_OK && stats.voices >= 1, "stats count the voices");

    const SFT::Ecs::Entity spawned = engine_object.ecs_world().spawn(SFT::Engine::WorldTransform{});
    const SturdyEntity entity{spawned.index, spawned.generation};
    const float offset[3] = {0.0f, 1.0f, 0.0f};
    check(sturdy_audio_source_attach(engine, entity, sound, STURDY_AUDIO_ATTACH_ENTITY, offset, &options) == STURDY_OK, "a source attaches to an entity (parenting)");
    check(sturdy_audio_listener_attach(engine, entity, 1.0f) == STURDY_OK, "an entity becomes the listener");
    check(static_cast<bool>(engine_object.ecs_world().get_component<SFT::Engine::AudioSource>(spawned)), "the source component exists");
    check(sturdy_audio_source_attach(engine, SturdyEntity{999, 1}, sound, STURDY_AUDIO_ATTACH_WORLD, nullptr, nullptr) == STURDY_ERROR_ENTITY_NOT_ALIVE, "a dead entity is reported");

    // Effects by catalogue name.
    uint32_t effects = 0;
    check(sturdy_audio_effect_count(&effects) == STURDY_OK && effects >= 15, "the effect catalogue is listed");
    char name[64] = {};
    bool has_noise_reduction = false;
    for (uint32_t i = 0; i < effects; ++i) {
        size_t length = 0;
        check(sturdy_audio_effect_name(i, name, sizeof(name), &length) == STURDY_OK, "effect names are readable");
        has_noise_reduction |= std::strcmp(name, "noise_reduction") == 0;
    }
    check(has_noise_reduction, "noise reduction is in the catalogue");
    const char *names[] = {"reduction"};
    const float values[] = {18.0f};
    check(sturdy_audio_master_effect_add(engine, "noise_reduction", names, values, 1) == STURDY_OK, "an effect is added by name");
    const char *typo[] = {"redution"};
    check(sturdy_audio_master_effect_add(engine, "noise_reduction", typo, values, 1) == STURDY_ERROR_INVALID_ARGUMENT, "a misspelt parameter is reported");
    check(sturdy_audio_master_effect_add(engine, "no_such_effect", nullptr, nullptr, 0) == STURDY_ERROR_NOT_AVAILABLE, "an unknown effect is reported");

    // Lifetime: released handles expire, never alias.
    check(sturdy_voice_release(voice) == STURDY_OK && sturdy_voice_release(voice) != STURDY_OK, "a voice releases once");
    check(sturdy_voice_set_volume(voice, 1.0f) != STURDY_OK, "a released voice is refused");
    check(sturdy_voice_release(second) == STURDY_OK && sturdy_sound_release(sound) == STURDY_OK && sturdy_audio_stream_release(stream) == STURDY_OK, "everything releases");
    check(sturdy_audio_sink_stop(sink) == STURDY_OK, "the sink stops");
    check(sturdy_sound_play(engine, sound, nullptr, nullptr) != STURDY_OK, "a released sound is refused");
    check(sturdy_audio_disable(engine) == STURDY_OK, "audio disables");
    std::filesystem::remove(path);
    return failures == 0 ? 0 : 1;
}
