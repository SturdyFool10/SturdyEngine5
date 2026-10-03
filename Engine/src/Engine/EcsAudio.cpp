#include <Engine/EcsAudio.hpp>

#include <Engine/AssetManager.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace SFT::Engine {

    std::expected<std::shared_ptr<Audio::ComputeMixBackend>, UString> create_gpu_audio_mixer(RHI::RhiDevice *device, const AudioGpu::GpuMixerConfig &config) {
        if (device == nullptr) {
            return std::unexpected("no graphics device (the renderer is not initialised)");
        }
        auto mixer = AudioGpu::GpuComputeMixer::create(*device, config);
        if (!mixer) {
            return std::unexpected(std::move(mixer.error()));
        }
        return std::shared_ptr<Audio::ComputeMixBackend>(std::move(*mixer));
    }

    UString AudioWorld::enable(Audio::AudioEngineConfig config, bool open_device, const Audio::DeviceSinkConfig &devices) {
        disable();
        engine_ = std::make_unique<Audio::AudioEngine>(std::move(config));
        if (acoustics_) {
            engine_->set_acoustics_delay_capacity(delay_capacity_);
        }
        sounds_ = std::make_unique<Audio::SoundManager>(*engine_, /*pump_engine=*/false); // end_frame pumps
        if (!open_device) {
            return {};
        }
        auto sink = Audio::DeviceSink::start(*engine_, devices);
        if (!sink) {
            return std::move(sink.error()); // the mixer still runs (silently), so headless tools and tests keep working
        }
        sink_ = std::move(*sink);
        return {};
    }

    void AudioWorld::disable() {
        finish_acoustics_job();
        sink_.reset(); // stops the device callbacks before the engine they pull from goes away
        sounds_.reset();
        engine_.reset();
        finished_.clear();
        tracked_.clear();
        has_listener_ = false;
    }

    void AudioWorld::begin_frame() {
        best_priority_ = -1e30f;
        has_listener_ = false;
        tracked_.clear();
    }

    void AudioWorld::offer_listener(f32 priority, const glm::mat4 &transform) {
        if (priority > best_priority_) {
            best_priority_ = priority;
            best_transform_ = transform;
            has_listener_ = true;
        }
    }

    void AudioWorld::end_frame(f32 delta_seconds) {
        if (!engine_) {
            return;
        }
        if (has_listener_) {
            Audio::ListenerState next;
            next.position = glm::vec3(best_transform_[3]);
            // Strip scale before reading the orientation.
            const glm::vec3 x = glm::normalize(glm::vec3(best_transform_[0]));
            const glm::vec3 y = glm::normalize(glm::vec3(best_transform_[1]));
            const glm::vec3 z = glm::normalize(glm::vec3(best_transform_[2]));
            next.rotation = glm::normalize(glm::quat_cast(glm::mat3(x, y, z)));
            if (delta_seconds > 1e-5f && listener_ready_) {
                next.velocity = (next.position - listener_.position) / delta_seconds;
            }
            listener_ = next;
            listener_ready_ = true;
            engine_->set_listener(listener_);
        }

        finished_.clear();
        for (Audio::VoiceId id : engine_->pump()) {
            finished_.insert(id);
        }
        sounds_->update(); // runs waypoint / loop / finish callbacks on this (the game) thread

        poll_acoustics_job();
        if (acoustics_ && !tracked_.empty() && !acoustics_task_) {
            acoustics_clock_ += delta_seconds;
            if (acoustics_clock_ >= acoustics_interval_) {
                acoustics_clock_ = 0.0f;
                // Which voices to evaluate this pass: the ones whose answer is most out of date and most worth having. A voice that has
                // not moved since its last evaluation (with a listener that has not either) keeps its answer; the rest compete by
                // how stale they are, how close they are, and whether they are moving.
                struct Candidate {
                    f32 priority;
                    usize index;
                };
                std::vector<Candidate> candidates;
                candidates.reserve(tracked_.size());
                for (usize i = 0; i < tracked_.size(); ++i) {
                    const TrackedVoice &t = tracked_[i];
                    EvaluationState &state = evaluated_[t.voice];
                    ++state.age;
                    const bool moved = glm::distance(t.position, state.position) > 0.05f || glm::distance(listener_.position, state.listener) > 0.05f ||
                                       glm::dot(t.velocity, t.velocity) > 0.01f || !state.evaluated;
                    if (!moved && state.age < 200) {
                        continue;
                    }
                    const f32 distance = glm::distance(t.position, listener_.position);
                    candidates.push_back({static_cast<f32>(state.age) / (1.0f + distance) * (state.evaluated ? 1.0f : 1000.0f), i});
                }
                const usize count = std::min<usize>(acoustics_budget_, candidates.size());
                std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count), candidates.end(),
                                  [](const Candidate &a, const Candidate &b) { return a.priority > b.priority; });
                std::vector<Audio::AcousticsQuery> queries;
                std::vector<Audio::VoiceId> voices;
                queries.reserve(count);
                voices.reserve(count);
                for (usize c = 0; c < count; ++c) {
                    const TrackedVoice &t = tracked_[candidates[c].index];
                    EvaluationState &state = evaluated_[t.voice];
                    state = EvaluationState{t.position, listener_.position, 0, true};
                    Audio::AcousticsQuery query;
                    query.source = t.position;
                    query.listener = listener_.position;
                    query.source_radius = t.radius;
                    query.source_velocity = t.velocity;
                    query.listener_velocity = listener_.velocity;
                    query.enabled_effects = t.effects;
                    query.muffling_scale = t.muffling_scale;
                    query.reverb_scale = t.reverb_scale;
                    query.delay_scale = t.delay_scale;
                    query.doppler_scale = t.doppler_scale;
                    queries.push_back(query);
                    voices.push_back(t.voice);
                }
                // Forget voices that are no longer tracked.
                if (evaluated_.size() > tracked_.size() * 2 + 64) {
                    std::unordered_set<Audio::VoiceId> alive;
                    for (const TrackedVoice &t : tracked_) alive.insert(t.voice);
                    std::erase_if(evaluated_, [&](const auto &entry) { return !alive.contains(entry.first); });
                }
                auto pending = std::make_shared<PendingAcoustics>();
                pending->voices = std::move(voices);
                pending->results.resize(count);
                const bool want_room = reverb_bus_ != Audio::no_bus;
                if (acoustics_async_ && Async::Scheduler::is_running()) {
                    acoustics_pending_ = pending;
                    acoustics_task_ = Async::Scheduler::spawn([provider = acoustics_, queries = std::move(queries), pending, want_room]() mutable {
                        provider->evaluate(queries, pending->results);
                        pending->has_room = want_room && provider->listener_room(pending->room);
                    });
                } else {
                    acoustics_->evaluate(queries, pending->results);
                    pending->has_room = want_room && acoustics_->listener_room(pending->room);
                    apply_acoustics(*pending);
                }
            }
        }
    }

    void AudioWorld::apply_acoustics(const PendingAcoustics &done) {
        if (!engine_) {
            return;
        }
        for (usize i = 0; i < done.voices.size(); ++i) {
            engine_->set_acoustics(done.voices[i], done.results[i]);
        }
        // The reverb bus takes on the listener's room: decay from the measured RT60, duller when the highs die first.
        const Audio::RoomEstimate &room = done.room;
        if (reverb_bus_ != Audio::no_bus && done.has_room && room.rt60 > 0.0f) {
            reverb_rt60_ += (room.rt60 - (reverb_rt60_ > 0.0f ? reverb_rt60_ : room.rt60)) * 0.4f;
            reverb_rt60_ = reverb_rt60_ > 0.0f ? reverb_rt60_ : room.rt60;
            const f32 high_ratio = room.rt60_bands[1] > 1e-3f ? room.rt60_bands[2] / room.rt60_bands[1] : 1.0f;
            reverb_damping_ += (std::clamp(1.0f - high_ratio, 0.0f, 0.9f) - reverb_damping_) * 0.4f;
            engine_->set_effect_parameter(reverb_bus_, 0, "rt60", reverb_rt60_);
            engine_->set_effect_parameter(reverb_bus_, 0, "damping", reverb_damping_);
        }
    }

    void AudioWorld::finish_acoustics_job() {
        if (!acoustics_task_) {
            return;
        }
        acoustics_task_->wait();
        acoustics_task_.reset();
        if (auto pending = std::move(acoustics_pending_)) {
            apply_acoustics(*pending);
        }
    }

    void AudioWorld::poll_acoustics_job() {
        if (acoustics_task_ && acoustics_task_->is_done()) {
            finish_acoustics_job();
        }
    }

    std::shared_ptr<const Audio::SampleBuffer> sound_buffer(const AssetManager &assets, Asset sound) {
        auto buffer = assets.sound_buffer(sound);
        return buffer ? *buffer : nullptr;
    }

    void update_audio_source(AudioWorld &world, Ecs::Entity entity, AudioSource &source, const glm::mat4 &transform, f32 delta_seconds) {
        Audio::AudioEngine *engine = world.engine();
        if (engine == nullptr) {
            return;
        }
        if (source.voice != 0 && (world.voice_finished(source.voice) || source.handle.finished())) {
            source.voice = 0;
            source.handle = Audio::SoundHandle{};
            source.finished = true;
        }

        glm::vec3 position = source.offset;
        Audio::SourceSpace space = Audio::SourceSpace::World;
        switch (source.attachment) {
            case AudioAttachment::World: break;
            case AudioAttachment::Entity: position = glm::vec3(transform * glm::vec4(source.offset, 1.0f)); break;
            case AudioAttachment::Listener: space = Audio::SourceSpace::ListenerRelative; break;
        }
        glm::vec3 velocity(0.0f);
        if (space == Audio::SourceSpace::World) {
            if (source.has_previous_position && delta_seconds > 1e-5f) {
                velocity = (position - source.previous_position) / delta_seconds;
            }
            source.previous_position = position;
            source.has_previous_position = true;
        }

        if (source.voice == 0) {
            if (source.finished || !source.play_on_spawn) {
                return;
            }
            std::shared_ptr<Audio::DataSource> data = source.stream;
            if (!data && source.sound) {
                data = std::make_shared<Audio::BufferSource>(source.sound, engine->config().sample_rate, source.loop && !source.loop_region);
            }
            if (!data) {
                return;
            }
            Audio::PlayParams params;
            params.source = std::move(data);
            params.bus = source.bus;
            params.volume = source.volume;
            params.pitch = source.pitch;
            params.space = space;
            params.spatial = source.spatial;
            params.position = position;
            params.velocity = velocity;
            params.distance = source.distance;
            params.spread = source.spread;
            params.priority = source.priority;
            params.doppler_factor = source.doppler_factor;
            params.panner = source.panner;
            params.aux_bus = source.aux_bus;
            params.aux_send = source.aux_send;
            params.use_acoustics = source.use_acoustics;
            params.markers = source.markers;
            params.loop = source.loop_region;
            params.start_seconds = source.start_seconds;
            if (source.use_file_markers && source.sound) {
                Audio::apply_file_metadata(params, *source.sound, source.loop);
            }

            // Marker, loop, jump and end events become ECS events for this entity.
            Audio::SoundCallbacks callbacks;
            AudioWorld *const audio = &world;
            callbacks.on_marker = [audio, entity](const Audio::SoundHandle &, const Audio::MarkerHit &hit) {
                audio->push_event(AudioSourceEvent{entity, AudioSourceEvent::Kind::Marker, hit.name, hit.id, hit.seconds});
            };
            callbacks.on_loop = [audio, entity](const Audio::SoundHandle &, f64 seconds) {
                audio->push_event(AudioSourceEvent{entity, AudioSourceEvent::Kind::Loop, {}, 0, seconds});
            };
            callbacks.on_jump = [audio, entity](const Audio::SoundHandle &, f64 seconds) {
                audio->push_event(AudioSourceEvent{entity, AudioSourceEvent::Kind::Jump, {}, 0, seconds});
            };
            callbacks.on_finished = [audio, entity](const Audio::SoundHandle &handle) {
                audio->push_event(AudioSourceEvent{entity, AudioSourceEvent::Kind::Finished, {}, 0, handle.position_seconds()});
            };
            source.handle = world.sounds()->play(std::move(params), std::move(callbacks));
            source.voice = source.handle.voice();
            source.stream.reset();
            source.applied_volume = source.volume;
            source.applied_pitch = source.pitch;
        } else {
            if (source.spatial) {
                engine->set_position(source.voice, position, velocity);
            }
            if (source.volume != source.applied_volume) {
                engine->set_volume(source.voice, source.volume);
                source.applied_volume = source.volume;
            }
            if (source.pitch != source.applied_pitch) {
                engine->set_pitch(source.voice, source.pitch);
                source.applied_pitch = source.pitch;
            }
        }

        if (source.voice != 0 && source.spatial && space == Audio::SourceSpace::World && source.use_acoustics) {
            AudioWorld::TrackedVoice tracked;
            tracked.voice = source.voice;
            tracked.position = position;
            tracked.radius = source.acoustic_radius;
            tracked.velocity = velocity;
            tracked.effects = source.acoustic_effects;
            tracked.muffling_scale = source.muffling_scale;
            tracked.reverb_scale = source.reverb_scale;
            tracked.delay_scale = source.delay_scale;
            tracked.doppler_scale = source.doppler_scale;
            world.track(tracked);
        }
    }

} // namespace SFT::Engine
