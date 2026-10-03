/// C ABI for audio: engine setup, sounds and voices, entity-attached sources, user-fed streams, microphones, custom sinks
/// and effects. Everything forwards to the Audio package and `Engine/EcsAudio.hpp`; this file checks handles and arguments
/// and translates types.

#include <Foundation/Foundation.hpp>

#include <map>
#include <memory>
#include <mutex>

#include <Audio/DeviceSink.hpp>
#include <Audio/Effects.hpp>
#include <Audio/Sink.hpp>
#include <Audio/Sound.hpp>
#include <Audio/Stream.hpp>
#include <Engine/Engine.hpp>
#include <Engine/EcsAudio.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;
    using SFT::u64;

    /// Owns the objects behind one kind of owned handle. Tokens are never reused, so a released handle can only ever
    /// resolve to "expired".
    template <class T, HandleKind Kind>
    class Owned {
      public:
        [[nodiscard]] u64 add(std::unique_ptr<T> object) {
            const std::lock_guard lock{mutex_};
            const u64 token = mint_handle(Kind, object.get());
            items_.emplace(token, std::move(object));
            return token;
        }
        [[nodiscard]] SturdyResult get(u64 token, T **out) const noexcept {
            void *pointer = nullptr;
            const SturdyResult result = resolve_handle(token, Kind, &pointer);
            if (result == STURDY_OK) {
                *out = static_cast<T *>(pointer);
            }
            return result;
        }
        [[nodiscard]] SturdyResult release(u64 token) {
            T *object = nullptr;
            if (const SturdyResult result = get(token, &object); result != STURDY_OK) {
                return result;
            }
            revoke_handle(token);
            std::unique_ptr<T> doomed;
            {
                const std::lock_guard lock{mutex_};
                if (const auto it = items_.find(token); it != items_.end()) {
                    doomed = std::move(it->second);
                    items_.erase(it);
                }
            }
            return STURDY_OK; // `doomed` dies here, outside the lock: stopping a sink joins its thread
        }

      private:
        mutable std::mutex mutex_;
        std::map<u64, std::unique_ptr<T>> items_;
    };

    struct SoundBox { std::shared_ptr<const SFT::Audio::SampleBuffer> buffer; };
    struct VoiceBox { SFT::Audio::SoundHandle handle; };
    struct StreamBox { std::shared_ptr<SFT::Audio::LiveSource> source; };
    struct SinkBox {
        std::shared_ptr<SFT::Audio::CallbackSink> sink;
        std::unique_ptr<SFT::Audio::SinkPump> pump;
    };
    struct CaptureBox { std::unique_ptr<SFT::Audio::CaptureDevice> device; };

    Owned<SoundBox, HandleKind::AudioSound> g_sounds;
    Owned<VoiceBox, HandleKind::AudioVoice> g_voices;
    Owned<StreamBox, HandleKind::AudioStream> g_streams;
    Owned<SinkBox, HandleKind::AudioSink> g_sinks;
    Owned<CaptureBox, HandleKind::AudioCapture> g_captures;

    /// The engine's audio world, which must be enabled.
    [[nodiscard]] SturdyResult resolve_audio(SturdyEngine engine, SFT::Engine::AudioWorld **out) noexcept {
        SFT::Engine::Engine *resolved = nullptr;
        if (const SturdyResult result = resolve_engine(engine, &resolved); result != STURDY_OK) {
            return result;
        }
        SFT::Engine::AudioWorld &world = resolved->audio();
        if (!world.enabled()) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "audio is not enabled (call sturdy_audio_enable first)");
        }
        *out = &world;
        return STURDY_OK;
    }

    [[nodiscard]] SFT::Audio::PlayParams to_params(const SturdyPlayOptions *options) {
        SFT::Audio::PlayParams params;
        if (options == nullptr) {
            params.spatial = false;
            return params;
        }
        params.volume = options->volume > 0.0f ? options->volume : 1.0f;
        params.pitch = options->pitch > 0.0f ? options->pitch : 1.0f;
        params.spatial = options->spatial != 0;
        params.position = {options->position[0], options->position[1], options->position[2]};
        params.start_delay_seconds = options->delay_seconds;
        params.fade_in_seconds = options->fade_in_seconds;
        return params;
    }

    [[nodiscard]] SturdyResult check_options(const SturdyPlayOptions *options) noexcept {
        if (options != nullptr && options->struct_size < sizeof(SturdyPlayOptions)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyPlayOptions::struct_size is too small");
        }
        return STURDY_OK;
    }

    [[nodiscard]] SturdyResult hand_out_voice(SFT::Audio::SoundHandle handle, SturdyVoice *out_voice) {
        if (out_voice != nullptr) {
            out_voice->token = g_voices.add(std::make_unique<VoiceBox>(VoiceBox{std::move(handle)}));
        }
        return STURDY_OK;
    }

    template <class Body>
    [[nodiscard]] SturdyResult with_voice(SturdyVoice voice, Body &&body) {
        return guarded([&]() -> SturdyResult {
            VoiceBox *box = nullptr;
            if (const SturdyResult result = g_voices.get(voice.token, &box); result != STURDY_OK) {
                return result;
            }
            body(box->handle);
            return STURDY_OK;
        });
    }

    [[nodiscard]] SFT::Audio::SpeakerLayout to_layout(const SturdyAudioConfig &config) {
        using SFT::Audio::SpeakerLayout;
        switch (config.layout) {
            case STURDY_AUDIO_LAYOUT_MONO: return SpeakerLayout::mono();
            case STURDY_AUDIO_LAYOUT_QUAD: return SpeakerLayout::quad();
            case STURDY_AUDIO_LAYOUT_5_1: return SpeakerLayout::surround_5_1();
            case STURDY_AUDIO_LAYOUT_7_1: return SpeakerLayout::surround_7_1();
            case STURDY_AUDIO_LAYOUT_7_1_4: return SpeakerLayout::surround_7_1_4();
            case STURDY_AUDIO_LAYOUT_9_1_6: return SpeakerLayout::surround_9_1_6();
            case STURDY_AUDIO_LAYOUT_22_2: return SpeakerLayout::surround_22_2();
            case STURDY_AUDIO_LAYOUT_CHANNEL_COUNT: return SpeakerLayout::from_channel_count(config.channel_count);
            default: return SpeakerLayout::stereo();
        }
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_audio_enable(SturdyEngine engine, const SturdyAudioConfig *config) {
    return guarded([&]() -> SturdyResult {
        if (config == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "config must not be null");
        }
        if (config->struct_size < sizeof(SturdyAudioConfig)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyAudioConfig::struct_size is too small");
        }
        if (config->layout == STURDY_AUDIO_LAYOUT_CHANNEL_COUNT && (config->channel_count == 0 || config->channel_count > SFT::Audio::max_channels)) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "channel_count must be 1..32");
        }
        SFT::Engine::Engine *resolved = nullptr;
        if (const SturdyResult result = resolve_engine(engine, &resolved); result != STURDY_OK) {
            return result;
        }
        SFT::Audio::AudioEngineConfig audio;
        audio.outputs = {SFT::Audio::OutputDesc{SFT::Audio::OutputDesc::Kind::Speakers, "main", to_layout(*config)}};
        if (config->sample_rate != 0) audio.sample_rate = config->sample_rate;
        if (config->max_voices != 0) audio.max_voices = config->max_voices;
        if (config->max_physical_voices != 0) audio.max_physical_voices = config->max_physical_voices;
        if (config->mix_threads != 0) audio.mix_threads = config->mix_threads;
        const SFT::UString failure = resolved->audio().enable(std::move(audio), config->open_device != 0);
        if (!failure.empty()) {
            // The mixer still runs silently when the device would not open; report it without failing the call.
            return set_error(STURDY_ERROR_NOT_AVAILABLE, failure.cpp_string_view());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_disable(SturdyEngine engine) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::Engine *resolved = nullptr;
        if (const SturdyResult result = resolve_engine(engine, &resolved); result != STURDY_OK) {
            return result;
        }
        resolved->audio().disable();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_listener_attach(SturdyEngine engine, SturdyEntity entity, float priority) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::Engine *resolved = nullptr;
        if (const SturdyResult result = resolve_engine(engine, &resolved); result != STURDY_OK) {
            return result;
        }
        SFT::Ecs::World &world = resolved->ecs_world();
        const SFT::Ecs::Entity target{.index = entity.index, .generation = entity.generation};
        if (!world.is_alive(target)) {
            return set_error(STURDY_ERROR_ENTITY_NOT_ALIVE, "the entity is not alive");
        }
        world.add_component(target, SFT::Engine::AudioListener{.priority = priority, .enabled = true});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_stats(SturdyEngine engine, SturdyAudioStats *out_stats) {
    return guarded([&]() -> SturdyResult {
        if (out_stats == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "out_stats must not be null");
        }
        SFT::Engine::AudioWorld *audio = nullptr;
        if (const SturdyResult result = resolve_audio(engine, &audio); result != STURDY_OK) {
            return result;
        }
        const SFT::Audio::AudioStats stats = audio->engine()->stats();
        *out_stats = SturdyAudioStats{stats.voices, stats.physical, stats.virtualised, stats.master_peak};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_sound_load(SturdyEngine engine, const char *path, SturdySound *out_sound) {
    return guarded([&]() -> SturdyResult {
        if (path == nullptr || out_sound == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "path and out_sound must not be null");
        }
        SFT::Engine::AudioWorld *audio = nullptr;
        if (const SturdyResult result = resolve_audio(engine, &audio); result != STURDY_OK) {
            return result;
        }
        auto loaded = audio->sounds()->load(path);
        if (!loaded) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, loaded.error().cpp_string_view());
        }
        out_sound->token = g_sounds.add(std::make_unique<SoundBox>(SoundBox{std::move(*loaded)}));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_sound_release(SturdySound sound) {
    return guarded([&]() -> SturdyResult { return g_sounds.release(sound.token); });
}

SturdyResult STURDY_ABI_CALL sturdy_sound_play(SturdyEngine engine, SturdySound sound, const SturdyPlayOptions *options, SturdyVoice *out_voice) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult result = check_options(options); result != STURDY_OK) {
            return result;
        }
        SFT::Engine::AudioWorld *audio = nullptr;
        if (const SturdyResult result = resolve_audio(engine, &audio); result != STURDY_OK) {
            return result;
        }
        SoundBox *box = nullptr;
        if (const SturdyResult result = g_sounds.get(sound.token, &box); result != STURDY_OK) {
            return result;
        }
        return hand_out_voice(audio->sounds()->play_buffer(box->buffer, to_params(options), {}, options != nullptr && options->loop != 0), out_voice);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_stream_file(SturdyEngine engine, const char *path, const SturdyPlayOptions *options, SturdyVoice *out_voice) {
    return guarded([&]() -> SturdyResult {
        if (path == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "path must not be null");
        }
        if (const SturdyResult result = check_options(options); result != STURDY_OK) {
            return result;
        }
        SFT::Engine::AudioWorld *audio = nullptr;
        if (const SturdyResult result = resolve_audio(engine, &audio); result != STURDY_OK) {
            return result;
        }
        SFT::Audio::StreamingOptions streaming;
        streaming.loop = options != nullptr && options->loop != 0;
        auto handle = audio->sounds()->stream_file(path, to_params(options), streaming);
        if (!handle) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, handle.error().cpp_string_view());
        }
        return hand_out_voice(std::move(*handle), out_voice);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_voice_release(SturdyVoice voice) {
    return guarded([&]() -> SturdyResult { return g_voices.release(voice.token); });
}
SturdyResult STURDY_ABI_CALL sturdy_voice_set_volume(SturdyVoice voice, float volume) {
    return with_voice(voice, [&](const SFT::Audio::SoundHandle &h) { h.set_volume(volume); });
}
SturdyResult STURDY_ABI_CALL sturdy_voice_fade_volume(SturdyVoice voice, float target, float seconds) {
    return with_voice(voice, [&](const SFT::Audio::SoundHandle &h) { h.fade_volume(target, seconds); });
}
SturdyResult STURDY_ABI_CALL sturdy_voice_set_pitch(SturdyVoice voice, float pitch) {
    return with_voice(voice, [&](const SFT::Audio::SoundHandle &h) { h.set_pitch(pitch); });
}
SturdyResult STURDY_ABI_CALL sturdy_voice_set_position(SturdyVoice voice, const float position[3], const float *velocity) {
    if (position == nullptr) {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, "position must not be null");
    }
    return with_voice(voice, [&](const SFT::Audio::SoundHandle &h) {
        h.set_position({position[0], position[1], position[2]}, velocity != nullptr ? glm::vec3{velocity[0], velocity[1], velocity[2]} : glm::vec3{0.0f});
    });
}
SturdyResult STURDY_ABI_CALL sturdy_voice_stop(SturdyVoice voice, float fade_seconds) {
    return with_voice(voice, [&](const SFT::Audio::SoundHandle &h) { h.stop(fade_seconds); });
}
SturdyResult STURDY_ABI_CALL sturdy_voice_pause(SturdyVoice voice) {
    return with_voice(voice, [&](const SFT::Audio::SoundHandle &h) { h.pause(); });
}
SturdyResult STURDY_ABI_CALL sturdy_voice_resume(SturdyVoice voice) {
    return with_voice(voice, [&](const SFT::Audio::SoundHandle &h) { h.resume(); });
}
SturdyResult STURDY_ABI_CALL sturdy_voice_seek(SturdyVoice voice, double seconds) {
    return with_voice(voice, [&](const SFT::Audio::SoundHandle &h) { h.seek(seconds); });
}
SturdyResult STURDY_ABI_CALL sturdy_voice_state(SturdyVoice voice, SturdyBool *out_playing, SturdyBool *out_finished, double *out_position_seconds) {
    return with_voice(voice, [&](const SFT::Audio::SoundHandle &h) {
        if (out_playing != nullptr) *out_playing = h.playing() ? STURDY_TRUE : STURDY_FALSE;
        if (out_finished != nullptr) *out_finished = h.finished() ? STURDY_TRUE : STURDY_FALSE;
        if (out_position_seconds != nullptr) *out_position_seconds = h.position_seconds();
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_source_attach(SturdyEngine engine, SturdyEntity entity, SturdySound sound, SturdyAudioAttachment attachment,
                                                        const float offset[3], const SturdyPlayOptions *options) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult result = check_options(options); result != STURDY_OK) {
            return result;
        }
        SFT::Engine::Engine *resolved = nullptr;
        if (const SturdyResult result = resolve_engine(engine, &resolved); result != STURDY_OK) {
            return result;
        }
        SoundBox *box = nullptr;
        if (const SturdyResult result = g_sounds.get(sound.token, &box); result != STURDY_OK) {
            return result;
        }
        SFT::Ecs::World &world = resolved->ecs_world();
        const SFT::Ecs::Entity target{.index = entity.index, .generation = entity.generation};
        if (!world.is_alive(target)) {
            return set_error(STURDY_ERROR_ENTITY_NOT_ALIVE, "the entity is not alive");
        }
        SFT::Engine::AudioSource source;
        source.sound = box->buffer;
        source.attachment = attachment == STURDY_AUDIO_ATTACH_WORLD ? SFT::Engine::AudioAttachment::World
                            : attachment == STURDY_AUDIO_ATTACH_LISTENER ? SFT::Engine::AudioAttachment::Listener
                                                                         : SFT::Engine::AudioAttachment::Entity;
        if (offset != nullptr) {
            source.offset = {offset[0], offset[1], offset[2]};
        }
        if (options != nullptr) {
            source.volume = options->volume > 0.0f ? options->volume : 1.0f;
            source.pitch = options->pitch > 0.0f ? options->pitch : 1.0f;
            source.spatial = options->spatial != 0;
            source.loop = options->loop != 0;
        }
        world.add_component(target, std::move(source));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_stream_create(SturdyEngine engine, uint32_t channels, uint32_t sample_rate, float buffer_seconds,
                                                        SturdyAudioStream *out_stream) {
    return guarded([&]() -> SturdyResult {
        if (out_stream == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "out_stream must not be null");
        }
        if (channels == 0 || channels > SFT::Audio::max_source_channels || sample_rate == 0) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "a stream needs 1..256 channels and a sample rate");
        }
        SFT::Engine::AudioWorld *audio = nullptr;
        if (const SturdyResult result = resolve_audio(engine, &audio); result != STURDY_OK) {
            return result;
        }
        const SFT::u32 engine_rate = audio->engine()->config().sample_rate;
        const auto capacity = static_cast<SFT::usize>(std::max(buffer_seconds, 0.1f) * static_cast<float>(sample_rate));
        auto source = std::make_shared<SFT::Audio::LiveSource>(channels, sample_rate, engine_rate, capacity);
        out_stream->token = g_streams.add(std::make_unique<StreamBox>(StreamBox{std::move(source)}));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_stream_push(SturdyAudioStream stream, const float *interleaved, uint32_t frames, uint32_t *out_accepted) {
    return guarded([&]() -> SturdyResult {
        if (interleaved == nullptr && frames != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "interleaved must not be null");
        }
        StreamBox *box = nullptr;
        if (const SturdyResult result = g_streams.get(stream.token, &box); result != STURDY_OK) {
            return result;
        }
        const SFT::usize accepted = box->source->push(interleaved, frames);
        if (out_accepted != nullptr) {
            *out_accepted = static_cast<uint32_t>(accepted);
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_stream_close(SturdyAudioStream stream) {
    return guarded([&]() -> SturdyResult {
        StreamBox *box = nullptr;
        if (const SturdyResult result = g_streams.get(stream.token, &box); result != STURDY_OK) {
            return result;
        }
        box->source->close();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_stream_buffered_seconds(SturdyAudioStream stream, double *out_seconds) {
    return guarded([&]() -> SturdyResult {
        if (out_seconds == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "out_seconds must not be null");
        }
        StreamBox *box = nullptr;
        if (const SturdyResult result = g_streams.get(stream.token, &box); result != STURDY_OK) {
            return result;
        }
        *out_seconds = box->source->buffered_seconds();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_stream_play(SturdyEngine engine, SturdyAudioStream stream, const SturdyPlayOptions *options, SturdyVoice *out_voice) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult result = check_options(options); result != STURDY_OK) {
            return result;
        }
        SFT::Engine::AudioWorld *audio = nullptr;
        if (const SturdyResult result = resolve_audio(engine, &audio); result != STURDY_OK) {
            return result;
        }
        StreamBox *box = nullptr;
        if (const SturdyResult result = g_streams.get(stream.token, &box); result != STURDY_OK) {
            return result;
        }
        SFT::Audio::PlayParams params = to_params(options);
        params.source = box->source;
        return hand_out_voice(audio->sounds()->play(std::move(params)), out_voice);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_stream_release(SturdyAudioStream stream) {
    return guarded([&]() -> SturdyResult { return g_streams.release(stream.token); });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_capture_start(const char *device_name, uint32_t sample_rate, uint32_t channels, SturdyAudioCapture *out_capture) {
    return guarded([&]() -> SturdyResult {
        if (out_capture == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "out_capture must not be null");
        }
        SFT::Audio::CaptureConfig config;
        if (device_name != nullptr) {
            config.device_name = SFT::UString::from_c_str(device_name);
        }
        if (sample_rate != 0) config.sample_rate = sample_rate;
        config.channels = channels;
        auto device = SFT::Audio::CaptureDevice::start(config);
        if (!device) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, device.error().cpp_string_view());
        }
        out_capture->token = g_captures.add(std::make_unique<CaptureBox>(CaptureBox{std::move(*device)}));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_capture_info(SturdyAudioCapture capture, uint32_t *out_channels, uint32_t *out_sample_rate) {
    return guarded([&]() -> SturdyResult {
        CaptureBox *box = nullptr;
        if (const SturdyResult result = g_captures.get(capture.token, &box); result != STURDY_OK) {
            return result;
        }
        if (out_channels != nullptr) *out_channels = box->device->channels();
        if (out_sample_rate != nullptr) *out_sample_rate = box->device->sample_rate();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_capture_peak(SturdyAudioCapture capture, uint32_t channel, float *out_peak) {
    return guarded([&]() -> SturdyResult {
        if (out_peak == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "out_peak must not be null");
        }
        CaptureBox *box = nullptr;
        if (const SturdyResult result = g_captures.get(capture.token, &box); result != STURDY_OK) {
            return result;
        }
        *out_peak = box->device->take_channel_peak(channel);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_capture_play(SturdyEngine engine, SturdyAudioCapture capture, const SturdyPlayOptions *options, SturdyVoice *out_voice) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult result = check_options(options); result != STURDY_OK) {
            return result;
        }
        SFT::Engine::AudioWorld *audio = nullptr;
        if (const SturdyResult result = resolve_audio(engine, &audio); result != STURDY_OK) {
            return result;
        }
        CaptureBox *box = nullptr;
        if (const SturdyResult result = g_captures.get(capture.token, &box); result != STURDY_OK) {
            return result;
        }
        SFT::Audio::PlayParams params = to_params(options);
        params.source = box->device->source();
        return hand_out_voice(audio->sounds()->play(std::move(params)), out_voice);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_capture_stop(SturdyAudioCapture capture) {
    return guarded([&]() -> SturdyResult { return g_captures.release(capture.token); });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_sink_start(SturdyEngine engine, uint32_t output, SturdyAudioSinkCallback callback, void *user_data,
                                                     uint32_t block_frames, SturdyBool realtime, SturdyAudioSink *out_sink) {
    return guarded([&]() -> SturdyResult {
        if (callback == nullptr || out_sink == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "callback and out_sink must not be null");
        }
        SFT::Engine::AudioWorld *audio = nullptr;
        if (const SturdyResult result = resolve_audio(engine, &audio); result != STURDY_OK) {
            return result;
        }
        const SFT::u32 rate = audio->engine()->config().sample_rate;
        auto box = std::make_unique<SinkBox>();
        box->sink = std::make_shared<SFT::Audio::CallbackSink>(
            [callback, user_data, rate](std::span<const float> interleaved, SFT::u32 frames) {
                callback(user_data, interleaved.data(), frames, frames != 0 ? static_cast<uint32_t>(interleaved.size() / frames) : 0u, rate);
            });
        SFT::Audio::SinkPumpConfig config;
        if (block_frames != 0) config.block_frames = block_frames;
        config.realtime = realtime != 0;
        auto pump = SFT::Audio::SinkPump::start(*audio->engine(), output, box->sink, config);
        if (!pump) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, pump.error().cpp_string_view());
        }
        box->pump = std::move(*pump);
        out_sink->token = g_sinks.add(std::move(box));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_sink_stop(SturdyAudioSink sink) {
    return guarded([&]() -> SturdyResult { return g_sinks.release(sink.token); });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_master_effect_add(SturdyEngine engine, const char *kind, const char *const *names, const float *values, uint32_t count) {
    return guarded([&]() -> SturdyResult {
        if (kind == nullptr || (count != 0 && (names == nullptr || values == nullptr))) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "kind, names and values must not be null");
        }
        SFT::Engine::AudioWorld *audio = nullptr;
        if (const SturdyResult result = resolve_audio(engine, &audio); result != STURDY_OK) {
            return result;
        }
        std::optional<SFT::Audio::EffectKind> found;
        for (const SFT::Audio::EffectKind candidate : SFT::Audio::all_effect_kinds()) {
            if (SFT::Audio::effect_kind_token(candidate) == std::string_view{kind}) {
                found = candidate;
            }
        }
        if (!found) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "unknown effect kind");
        }
        SFT::Audio::EffectSpec spec(*found);
        for (uint32_t i = 0; i < count; ++i) {
            spec.set(SFT::ustr{names[i]}, values[i]);
        }
        if (!audio->engine()->add_effect(audio->engine()->master_bus(), spec)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "the effect could not be created (unknown parameter name?)");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_effect_count(uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "out_count must not be null");
        }
        *out_count = static_cast<uint32_t>(SFT::Audio::all_effect_kinds().size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_audio_effect_name(uint32_t index, char *buffer, size_t capacity, size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        const auto kinds = SFT::Audio::all_effect_kinds();
        if (index >= kinds.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "no effect with that index");
        }
        return copy_string_out(SFT::Audio::effect_kind_token(kinds[index]), buffer, capacity, out_length);
    });
}

} // extern "C"
