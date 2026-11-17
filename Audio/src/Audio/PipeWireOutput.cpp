#include <Audio/PipeWireOutput.hpp>

#include <Audio/DeviceSink.hpp>
#include <Audio/Realtime.hpp>
#include <Audio/Text.hpp>

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <format>
#include <mutex>

// The real client needs Linux and PipeWire's client headers (fetched into the build by cmake/SturdyPipeWire.cmake, or installed). Without
// either, the stubs at the end of this file keep the symbols defined and report the system as absent, so DeviceSink needs no #ifdefs of
// its own and every platform links.
#if defined(__linux__) && __has_include(<pipewire/pipewire.h>) && __has_include(<spa/param/audio/format-utils.h>)
#define STURDY_AUDIO_HAS_PIPEWIRE 1
#else
#define STURDY_AUDIO_HAS_PIPEWIRE 0
#endif

#if STURDY_AUDIO_HAS_PIPEWIRE

#include <dlfcn.h>

// PipeWire's client headers are C and header-only where it matters (the SPA pod builder, the core/registry interface wrappers). Every
// function that lives in libpipewire itself is reached through the table below instead of being linked.
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

namespace SFT::Audio {

    namespace {

        // ---- libpipewire, bound at run time ---------------------------------------------------------------------------

        struct Api {
            void (*init)(int *, char ***) = nullptr;
            pw_thread_loop *(*thread_loop_new)(const char *, const spa_dict *) = nullptr;
            void (*thread_loop_destroy)(pw_thread_loop *) = nullptr;
            int (*thread_loop_start)(pw_thread_loop *) = nullptr;
            void (*thread_loop_stop)(pw_thread_loop *) = nullptr;
            void (*thread_loop_lock)(pw_thread_loop *) = nullptr;
            void (*thread_loop_unlock)(pw_thread_loop *) = nullptr;
            pw_loop *(*thread_loop_get_loop)(pw_thread_loop *) = nullptr;
            pw_context *(*context_new)(pw_loop *, pw_properties *, size_t) = nullptr;
            void (*context_destroy)(pw_context *) = nullptr;
            pw_core *(*context_connect)(pw_context *, pw_properties *, size_t) = nullptr;
            int (*core_disconnect)(pw_core *) = nullptr;
            void (*proxy_destroy)(pw_proxy *) = nullptr;
            pw_properties *(*properties_new)(const char *, ...) = nullptr;
            int (*properties_set)(pw_properties *, const char *, const char *) = nullptr;
            pw_stream *(*stream_new)(pw_core *, const char *, pw_properties *) = nullptr;
            void (*stream_destroy)(pw_stream *) = nullptr;
            void (*stream_add_listener)(pw_stream *, spa_hook *, const pw_stream_events *, void *) = nullptr;
            int (*stream_connect)(pw_stream *, enum pw_direction, uint32_t, enum pw_stream_flags, const spa_pod **, uint32_t) = nullptr;
            int (*stream_disconnect)(pw_stream *) = nullptr;
            int (*stream_set_active)(pw_stream *, bool) = nullptr;
            pw_buffer *(*stream_dequeue_buffer)(pw_stream *) = nullptr;
            int (*stream_queue_buffer)(pw_stream *, pw_buffer *) = nullptr;
            const char *(*stream_state_as_string)(enum pw_stream_state) = nullptr;
        };

        const Api *load_api() noexcept {
            static const Api *api = [] () -> const Api * {
                static Api table;
                // RTLD_LOCAL: nothing else in the process resolves against this handle. libpipewire stays loaded for the life of the
                // process (it keeps global state; pw_deinit is deliberately never called).
                void *library = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
                if (library == nullptr) {
                    return nullptr;
                }
                bool complete = true;
                auto bind = [&](auto &slot, const char *name) {
                    slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(dlsym(library, name));
                    complete = complete && slot != nullptr;
                };
                bind(table.init, "pw_init");
                bind(table.thread_loop_new, "pw_thread_loop_new");
                bind(table.thread_loop_destroy, "pw_thread_loop_destroy");
                bind(table.thread_loop_start, "pw_thread_loop_start");
                bind(table.thread_loop_stop, "pw_thread_loop_stop");
                bind(table.thread_loop_lock, "pw_thread_loop_lock");
                bind(table.thread_loop_unlock, "pw_thread_loop_unlock");
                bind(table.thread_loop_get_loop, "pw_thread_loop_get_loop");
                bind(table.context_new, "pw_context_new");
                bind(table.context_destroy, "pw_context_destroy");
                bind(table.context_connect, "pw_context_connect");
                bind(table.core_disconnect, "pw_core_disconnect");
                bind(table.proxy_destroy, "pw_proxy_destroy");
                bind(table.properties_new, "pw_properties_new");
                bind(table.properties_set, "pw_properties_set");
                bind(table.stream_new, "pw_stream_new");
                bind(table.stream_destroy, "pw_stream_destroy");
                bind(table.stream_add_listener, "pw_stream_add_listener");
                bind(table.stream_connect, "pw_stream_connect");
                bind(table.stream_disconnect, "pw_stream_disconnect");
                bind(table.stream_set_active, "pw_stream_set_active");
                bind(table.stream_dequeue_buffer, "pw_stream_dequeue_buffer");
                bind(table.stream_queue_buffer, "pw_stream_queue_buffer");
                bind(table.stream_state_as_string, "pw_stream_state_as_string");
                if (!complete) {
                    return nullptr;
                }
                table.init(nullptr, nullptr);
                return &table;
            }();
            return api;
        }

        uint32_t to_spa(ChannelRole role) noexcept {
            switch (role) {
                case ChannelRole::FrontLeft: return SPA_AUDIO_CHANNEL_FL;
                case ChannelRole::FrontRight: return SPA_AUDIO_CHANNEL_FR;
                case ChannelRole::Center: return SPA_AUDIO_CHANNEL_FC;
                case ChannelRole::Lfe: return SPA_AUDIO_CHANNEL_LFE;
                case ChannelRole::FrontLeftWide: return SPA_AUDIO_CHANNEL_FLC;
                case ChannelRole::FrontRightWide: return SPA_AUDIO_CHANNEL_FRC;
                case ChannelRole::SideLeft: return SPA_AUDIO_CHANNEL_SL;
                case ChannelRole::SideRight: return SPA_AUDIO_CHANNEL_SR;
                case ChannelRole::RearLeft: return SPA_AUDIO_CHANNEL_RL;
                case ChannelRole::RearRight: return SPA_AUDIO_CHANNEL_RR;
                case ChannelRole::TopFrontLeft: return SPA_AUDIO_CHANNEL_TFL;
                case ChannelRole::TopFrontRight: return SPA_AUDIO_CHANNEL_TFR;
                case ChannelRole::TopMiddleLeft: return SPA_AUDIO_CHANNEL_TSL;
                case ChannelRole::TopMiddleRight: return SPA_AUDIO_CHANNEL_TSR;
                case ChannelRole::TopRearLeft: return SPA_AUDIO_CHANNEL_TRL;
                case ChannelRole::TopRearRight: return SPA_AUDIO_CHANNEL_TRR;
                case ChannelRole::Mono: return SPA_AUDIO_CHANNEL_MONO;
                case ChannelRole::BackCenter: return SPA_AUDIO_CHANNEL_RC;
                case ChannelRole::TopCenter: return SPA_AUDIO_CHANNEL_TC;
                case ChannelRole::TopFrontCenter: return SPA_AUDIO_CHANNEL_TFC;
                case ChannelRole::TopRearCenter: return SPA_AUDIO_CHANNEL_TRC;
                case ChannelRole::Lfe2: return SPA_AUDIO_CHANNEL_LFE2;
                // Floor-level positions have no SPA name: auxiliary channels keep them distinct.
                case ChannelRole::BottomFrontCenter: return SPA_AUDIO_CHANNEL_START_Aux + 2;
                case ChannelRole::BottomFrontLeft: return SPA_AUDIO_CHANNEL_START_Aux + 3;
                case ChannelRole::BottomFrontRight: return SPA_AUDIO_CHANNEL_START_Aux + 4;
                case ChannelRole::Count: break;
            }
            return SPA_AUDIO_CHANNEL_UNKNOWN;
        }

        /// A connection to the daemon with its own loop thread; shared by the enumeration and the output stream.
        struct Connection {
            const Api *api = nullptr;
            pw_thread_loop *loop = nullptr;
            pw_context *context = nullptr;
            pw_core *core = nullptr;

            [[nodiscard]] static std::unique_ptr<Connection> connect(UString &why) {
                const Api *api = load_api();
                if (api == nullptr) {
                    why = "libpipewire is not installed";
                    return nullptr;
                }
                auto connection = std::make_unique<Connection>();
                connection->api = api;
                connection->loop = api->thread_loop_new("sturdy-pipewire", nullptr);
                if (connection->loop == nullptr) {
                    why = "could not create the PipeWire loop";
                    return nullptr;
                }
                connection->context = api->context_new(api->thread_loop_get_loop(connection->loop), nullptr, 0);
                if (connection->context == nullptr) {
                    why = "could not create the PipeWire context";
                    return nullptr;
                }
                if (api->thread_loop_start(connection->loop) < 0) {
                    why = "could not start the PipeWire loop";
                    return nullptr;
                }
                api->thread_loop_lock(connection->loop);
                connection->core = api->context_connect(connection->context, nullptr, 0);
                api->thread_loop_unlock(connection->loop);
                if (connection->core == nullptr) {
                    why = "no PipeWire daemon is running";
                    return nullptr;
                }
                return connection;
            }

            ~Connection() {
                if (api == nullptr) {
                    return;
                }
                if (loop != nullptr) {
                    api->thread_loop_stop(loop);
                }
                if (core != nullptr) {
                    api->core_disconnect(core);
                }
                if (context != nullptr) {
                    api->context_destroy(context);
                }
                if (loop != nullptr) {
                    api->thread_loop_destroy(loop);
                }
            }
        };

        // ---- registry enumeration -------------------------------------------------------------------------------------

        struct SinkEntry {
            std::string node_name;
            std::string description;
        };

        struct RegistryState {
            std::vector<SinkEntry> sinks;
            std::mutex mutex;
            std::condition_variable changed;
            bool done = false;
            int pending_sync = 0;
        };

        void registry_global(void *data, uint32_t, uint32_t, const char *type, uint32_t, const spa_dict *props) {
            auto *state = static_cast<RegistryState *>(data);
            if (props == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
                return;
            }
            const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
            if (media_class == nullptr || std::strcmp(media_class, "Audio/Sink") != 0) {
                return;
            }
            const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
            if (name == nullptr) {
                return;
            }
            std::lock_guard lock{state->mutex};
            state->sinks.push_back(SinkEntry{name, description != nullptr ? description : name});
        }

        void core_done(void *data, uint32_t id, int) {
            auto *state = static_cast<RegistryState *>(data);
            if (id != PW_ID_CORE) {
                return;
            }
            {
                std::lock_guard lock{state->mutex};
                state->done = true;
            }
            state->changed.notify_all();
        }

        std::vector<SinkEntry> list_sinks() {
            UString why;
            auto connection = Connection::connect(why);
            if (!connection) {
                return {};
            }
            RegistryState state;
            static const pw_registry_events registry_events = [] {
                pw_registry_events events{};
                events.version = PW_VERSION_REGISTRY_EVENTS;
                events.global = registry_global;
                return events;
            }();
            static const pw_core_events core_events = [] {
                pw_core_events events{};
                events.version = PW_VERSION_CORE_EVENTS;
                events.done = core_done;
                return events;
            }();
            spa_hook registry_hook{};
            spa_hook core_hook{};
            pw_registry *registry = nullptr;
            const Api &api = *connection->api;
            api.thread_loop_lock(connection->loop);
            pw_core_add_listener(connection->core, &core_hook, &core_events, &state);
            registry = pw_core_get_registry(connection->core, PW_VERSION_REGISTRY, 0);
            if (registry != nullptr) {
                pw_registry_add_listener(registry, &registry_hook, &registry_events, &state);
            }
            // The daemon answers "done" after it has sent every global that existed when the sync was requested.
            pw_core_sync(connection->core, PW_ID_CORE, 0);
            api.thread_loop_unlock(connection->loop);

            {
                std::unique_lock lock{state.mutex};
                state.changed.wait_for(lock, std::chrono::seconds(2), [&] { return state.done; });
            }
            api.thread_loop_lock(connection->loop);
            spa_hook_remove(&registry_hook);
            spa_hook_remove(&core_hook);
            if (registry != nullptr) {
                api.proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
            }
            api.thread_loop_unlock(connection->loop);
            std::lock_guard lock{state.mutex};
            return std::move(state.sinks);
        }

    } // namespace

    std::vector<AudioDeviceInfo> pipewire_playback_devices() {
        const std::vector<SinkEntry> sinks = list_sinks();
        std::vector<AudioDeviceInfo> devices;
        if (sinks.empty()) {
            return devices;
        }
        AudioDeviceInfo system_default;
        system_default.name = UString{"Default"};
        system_default.is_default = true;
        devices.push_back(std::move(system_default));
        for (const SinkEntry &sink : sinks) {
            AudioDeviceInfo info;
            info.name = text_from_bytes(sink.description);
            devices.push_back(std::move(info));
        }
        return devices;
    }

    // ---- the output stream ------------------------------------------------------------------------------------------

    struct PipeWireOutput::Impl {
        std::unique_ptr<Connection> connection;
        pw_stream *stream = nullptr;
        spa_hook stream_hook{};
        AudioEngine *engine = nullptr;
        OutputId output = 0;
        u32 channels = 0;
        u32 block_frames = 0;
        u32 sample_rate = 0;

        std::mutex mutex;
        std::condition_variable changed;
        pw_stream_state state = PW_STREAM_STATE_UNCONNECTED;
        std::string error;

        static void on_state_changed(void *data, enum pw_stream_state, enum pw_stream_state state, const char *error) {
            auto *self = static_cast<Impl *>(data);
            {
                std::lock_guard lock{self->mutex};
                self->state = state;
                if (error != nullptr) {
                    self->error = error;
                }
            }
            self->changed.notify_all();
        }

        // PipeWire's real-time thread (PW_STREAM_FLAG_RT_PROCESS): fill the buffer straight from the engine.
        static void on_process(void *data) {
            auto *self = static_cast<Impl *>(data);
            prepare_realtime_thread(RealtimeOptions{.period_seconds = static_cast<f64>(self->block_frames) / static_cast<f64>(self->sample_rate)});
            pw_buffer *buffer = load_api()->stream_dequeue_buffer(self->stream);
            if (buffer == nullptr) {
                return;
            }
            spa_data &data0 = buffer->buffer->datas[0];
            if (data0.data == nullptr) {
                load_api()->stream_queue_buffer(self->stream, buffer);
                return;
            }
            const u32 stride = static_cast<u32>(sizeof(f32)) * self->channels;
            const u32 frames = data0.maxsize / stride;
            self->engine->pull(self->output, static_cast<f32 *>(data0.data), frames);
            data0.chunk->offset = 0;
            data0.chunk->stride = static_cast<int32_t>(stride);
            data0.chunk->size = frames * stride;
            load_api()->stream_queue_buffer(self->stream, buffer);
        }
    };

    PipeWireOutput::PipeWireOutput(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    PipeWireOutput::~PipeWireOutput() {
        if (!impl_ || !impl_->connection) {
            return;
        }
        const Api &api = *impl_->connection->api;
        api.thread_loop_lock(impl_->connection->loop);
        if (impl_->stream != nullptr) {
            spa_hook_remove(&impl_->stream_hook);
            api.stream_destroy(impl_->stream); // no more process callbacks once this returns
            impl_->stream = nullptr;
        }
        api.thread_loop_unlock(impl_->connection->loop);
    }

    std::expected<std::unique_ptr<PipeWireOutput>, UString> PipeWireOutput::open(AudioEngine &engine, OutputId output, const SpeakerLayout &layout,
                                                                                   const UString &device) {
        UString why;
        auto connection = Connection::connect(why);
        if (!connection) {
            return std::unexpected(UString{std::format("PipeWire: {}", why)});
        }
        const Api &api = *connection->api;

        auto impl = std::make_unique<Impl>();
        impl->engine = &engine;
        impl->output = output;
        impl->channels = layout.channel_count();
        impl->block_frames = engine.config().block_frames;
        impl->sample_rate = engine.config().sample_rate;

        // `device` is a description; the daemon's target is the node name. A name this daemon does not have is an error, not a silent
        // fall back to the default sink: DeviceSink then tries the next audio system, which may be the one that listed it. (Resolved
        // before anything is allocated so the early return leaves nothing behind.)
        std::string target_node;
        const std::string wanted = std::string(device.c_str());
        if (!wanted.empty() && wanted != "Default") {
            for (const SinkEntry &sink : list_sinks()) {
                if (sink.description == wanted || sink.node_name == wanted) {
                    target_node = sink.node_name;
                    break;
                }
            }
            if (target_node.empty()) {
                return std::unexpected(UString{std::format("PipeWire: no output named '{}'", wanted)});
            }
        }

        pw_properties *properties = api.properties_new(nullptr, nullptr);
        api.properties_set(properties, PW_KEY_MEDIA_TYPE, "Audio");
        api.properties_set(properties, PW_KEY_MEDIA_CATEGORY, "Playback");
        api.properties_set(properties, PW_KEY_MEDIA_ROLE, "Game");
        api.properties_set(properties, PW_KEY_NODE_NAME, engine.config().outputs[output].name.c_str());
        const std::string latency = std::format("{}/{}", impl->block_frames, impl->sample_rate);
        api.properties_set(properties, PW_KEY_NODE_LATENCY, latency.c_str());
        // Output 0 drives the engine clock; a stream that PipeWire would pause when idle would stop it.
        api.properties_set(properties, PW_KEY_NODE_ALWAYS_PROCESS, "true");
        if (!target_node.empty()) {
            api.properties_set(properties, PW_KEY_TARGET_OBJECT, target_node.c_str());
        }

        api.thread_loop_lock(connection->loop);
        impl->stream = api.stream_new(connection->core, "Sturdy Engine", properties);
        if (impl->stream == nullptr) {
            api.thread_loop_unlock(connection->loop);
            return std::unexpected(UString{"PipeWire: could not create a stream"});
        }
        static const pw_stream_events events = [] {
            pw_stream_events table{};
            table.version = PW_VERSION_STREAM_EVENTS;
            table.state_changed = &Impl::on_state_changed;
            table.process = &Impl::on_process;
            return table;
        }();
        api.stream_add_listener(impl->stream, &impl->stream_hook, &events, impl.get());

        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = impl->sample_rate;
        info.channels = std::min<u32>(impl->channels, SPA_AUDIO_MAX_CHANNELS);
        for (u32 c = 0; c < info.channels; ++c) {
            info.position[c] = to_spa(layout.speakers[c].role);
        }
        u8 pod_buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
        const spa_pod *params[1];
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
        // INACTIVE until start(): secondary outputs run before the primary one begins feeding their FIFOs.
        const int connected = api.stream_connect(impl->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                                 static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                                              PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_INACTIVE),
                                                 params, 1);
        api.thread_loop_unlock(connection->loop);
        if (connected < 0) {
            api.thread_loop_lock(connection->loop);
            spa_hook_remove(&impl->stream_hook);
            api.stream_destroy(impl->stream);
            impl->stream = nullptr;
            api.thread_loop_unlock(connection->loop);
            return std::unexpected(UString{"PipeWire: could not connect the stream"});
        }

        // Wait until the daemon has linked the stream to a sink (PAUSED); without a session manager (or without a sink) it never does.
        {
            std::unique_lock lock{impl->mutex};
            impl->changed.wait_for(lock, std::chrono::seconds(3), [&] {
                return impl->state == PW_STREAM_STATE_PAUSED || impl->state == PW_STREAM_STATE_STREAMING || impl->state == PW_STREAM_STATE_ERROR;
            });
            if (impl->state != PW_STREAM_STATE_PAUSED && impl->state != PW_STREAM_STATE_STREAMING) {
                const std::string reason = impl->error.empty() ? std::string(api.stream_state_as_string(impl->state)) : impl->error;
                lock.unlock();
                api.thread_loop_lock(connection->loop);
                spa_hook_remove(&impl->stream_hook);
                api.stream_destroy(impl->stream);
                impl->stream = nullptr;
                api.thread_loop_unlock(connection->loop);
                return std::unexpected(UString{std::format("PipeWire: the stream was not linked to an output ({})", reason)});
            }
        }
        impl->connection = std::move(connection);
        return std::unique_ptr<PipeWireOutput>(new PipeWireOutput(std::move(impl)));
    }

    bool PipeWireOutput::start() noexcept {
        const Api &api = *impl_->connection->api;
        api.thread_loop_lock(impl_->connection->loop);
        const int result = api.stream_set_active(impl_->stream, true);
        api.thread_loop_unlock(impl_->connection->loop);
        return result >= 0;
    }

    f64 PipeWireOutput::latency_seconds() const noexcept {
        return 2.0 * static_cast<f64>(impl_->block_frames) / static_cast<f64>(impl_->sample_rate);
    }

} // namespace SFT::Audio

#else // no PipeWire client on this platform / no headers: the system reports itself absent

namespace SFT::Audio {

    struct PipeWireOutput::Impl {};

    std::vector<AudioDeviceInfo> pipewire_playback_devices() { return {}; }

    PipeWireOutput::PipeWireOutput(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    PipeWireOutput::~PipeWireOutput() = default;

    std::expected<std::unique_ptr<PipeWireOutput>, UString> PipeWireOutput::open(AudioEngine &, OutputId, const SpeakerLayout &, const UString &) {
        return std::unexpected(UString{"PipeWire: not built into this program"});
    }

    bool PipeWireOutput::start() noexcept { return false; }

    f64 PipeWireOutput::latency_seconds() const noexcept { return 0.0; }

} // namespace SFT::Audio

#endif
