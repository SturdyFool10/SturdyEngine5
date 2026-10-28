/// C ABI for scripts: language adapters, native script instances, host functions, calls, messages and hot reload.
/// A thin layer over `Engine/Script.hpp`; the callbacks of a script arrive here as C function pointers and become a `ScriptInstance`.

#include <FFI/AsyncNetSupport.hpp>

#include <Engine/CompositeGameLogic.hpp>
#include <Engine/Script.hpp>
#include <Foundation/Log.hpp>

#include <FFI/Sturdy.h>

using namespace SFT::Ffi::AsyncSupport;
using SFT::Ffi::ScopedHandle;
using SFT::Ffi::resolve_engine;
namespace E = SFT::Engine;

namespace {

    E::ScriptBytes to_script(const B::Bytes &in) {
        E::ScriptBytes out(in.size());
        if (!in.empty()) std::memcpy(out.data(), in.data(), in.size());
        return out;
    }
    B::Bytes to_binding(std::span<const std::byte> in) {
        B::Bytes out(in.size());
        if (!in.empty()) std::memcpy(out.data(), in.data(), in.size());
        return out;
    }
    const uint8_t *raw(std::span<const std::byte> in) { return reinterpret_cast<const uint8_t *>(in.data()); }
    std::string message_text(const E::GameLogicError &error) { return std::string(std::string_view{error.message}); }

    std::unexpected<E::GameLogicError> failed(std::string message) { return std::unexpected(E::GameLogicError{SFT::UString{message}}); }

    /// Runs a callback that writes into a sink and returns what it wrote.
    template <typename Fill>
    B::Bytes collect(Fill &&fill) {
        B::Bytes bytes;
        SturdyBytesSink sink{&bytes};
        fill(&sink);
        return bytes;
    }
    std::string as_text(const B::Bytes &bytes) { return std::string(bytes.begin(), bytes.end()); }

    // ---------------------------------------------------------------------------------------------------------------
    // C callbacks as a ScriptInstance
    // ---------------------------------------------------------------------------------------------------------------
    class CScriptInstance final : public E::ScriptInstance {
      public:
        CScriptInstance(const SturdyScriptInstance &table, std::shared_ptr<void> keep_alive) : table_(table), keep_alive_(std::move(keep_alive)) {}
        ~CScriptInstance() override {
            if (table_.destroy != nullptr) no_throw([&] { table_.destroy(table_.user_data); });
        }

        E::GameLogicResult start(E::Engine &engine) override {
            if (table_.start == nullptr) return {};
            const ScopedHandle handle{SFT::Ffi::HandleKind::Engine, &engine};
            B::Bytes error;
            SturdyBytesSink sink{&error};
            if (table_.start(SturdyEngine{handle.token()}, table_.user_data, &sink) == STURDY_FALSE) {
                return failed(error.empty() ? "the script's start callback reported failure" : as_text(error));
            }
            return {};
        }

        void update(E::Engine &engine, const E::ScriptTick &tick) override {
            if (table_.update == nullptr) return;
            const ScopedHandle handle{SFT::Ffi::HandleKind::Engine, &engine};
            const SturdyScriptTick view{tick.delta_seconds, tick.elapsed_seconds, tick.tick};
            table_.update(SturdyEngine{handle.token()}, &view, table_.user_data);
        }

        std::optional<E::RenderFrameParameters> request_render_frame(E::Engine &engine, SFT::Core::RenderSurfaceHandle surface, const SFT::Core::FrameInput &frame) override {
            if (table_.request_render_frame == nullptr) return std::nullopt;
            E::RenderFrameParameters parameters{};
            parameters.render_graph = E::RenderGraph::standard();
            if (frame.framebuffer_width != 0 && frame.framebuffer_height != 0) parameters.camera.set_viewport_size(frame.framebuffer_width, frame.framebuffer_height);
            SturdyFrameInput input{};
            input.struct_size = static_cast<uint32_t>(sizeof(SturdyFrameInput));
            input.delta_seconds = frame.delta_seconds;
            input.frame_index = frame.frame_index;
            input.framebuffer_width = frame.framebuffer_width;
            input.framebuffer_height = frame.framebuffer_height;
            input.live_resize = frame.live_resize ? STURDY_TRUE : STURDY_FALSE;
            const ScopedHandle engine_handle{SFT::Ffi::HandleKind::Engine, &engine};
            const ScopedHandle frame_handle{SFT::Ffi::HandleKind::Frame, &parameters};
            const SturdyBool render = table_.request_render_frame(SturdyEngine{engine_handle.token()}, SturdySurface{static_cast<uint64_t>(surface.window_id)}, &input,
                                                                   SturdyFrame{frame_handle.token()}, table_.user_data);
            if (render == STURDY_FALSE) return std::nullopt;
            return parameters;
        }

        void stop(E::Engine &engine) noexcept override {
            if (table_.stop == nullptr) return;
            const ScopedHandle handle{SFT::Ffi::HandleKind::Engine, &engine};
            table_.stop(SturdyEngine{handle.token()}, table_.user_data);
        }

        std::optional<E::ScriptBytes> save_state() override {
            if (table_.save_state == nullptr) return std::nullopt;
            B::Bytes state;
            SturdyBytesSink sink{&state};
            if (table_.save_state(table_.user_data, &sink) == STURDY_FALSE) return std::nullopt;
            return to_script(state);
        }

        bool restore_state(std::span<const std::byte> state) override {
            if (table_.restore_state == nullptr) return true;
            return table_.restore_state(table_.user_data, raw(state), state.size()) != STURDY_FALSE;
        }

        E::ScriptValue<E::ScriptBytes> call(std::string_view function, std::span<const std::byte> args) override {
            if (table_.call == nullptr) return E::ScriptInstance::call(function, args);
            const std::string name(function);
            B::Bytes result;
            B::Bytes error;
            SturdyBytesSink result_sink{&result};
            SturdyBytesSink error_sink{&error};
            if (table_.call(table_.user_data, name.c_str(), raw(args), args.size(), &result_sink, &error_sink) == STURDY_FALSE) {
                return failed(error.empty() ? "the script's '" + name + "' failed" : as_text(error));
            }
            return to_script(result);
        }

        void on_message(const E::ScriptMessage &message) override {
            if (table_.on_message == nullptr) return;
            const SturdyScriptMessage view{message.from.c_str(), message.topic.c_str(), raw(message.payload), message.payload.size()};
            table_.on_message(table_.user_data, &view);
        }

      private:
        SturdyScriptInstance table_;
        std::shared_ptr<void> keep_alive_; ///< the language adapter that made this instance, so it outlives it
    };

    bool valid_instance(const SturdyScriptInstance &instance) { return instance.struct_size == sizeof(SturdyScriptInstance); }

    // ---------------------------------------------------------------------------------------------------------------
    // A registered C language adapter
    // ---------------------------------------------------------------------------------------------------------------
    class CScriptLanguage final : public E::ScriptLanguage, public std::enable_shared_from_this<CScriptLanguage> {
      public:
        explicit CScriptLanguage(const SturdyScriptLanguage &table) : table_(table), name_(table.name != nullptr ? table.name : "") {
            for (uint32_t i = 0; i < table.extension_count && table.extensions != nullptr; ++i) {
                if (table.extensions[i] != nullptr) extensions_.emplace_back(table.extensions[i]);
            }
        }
        ~CScriptLanguage() override {
            if (table_.destroy != nullptr) no_throw([&] { table_.destroy(table_.user_data); });
        }

        /// Gives up responsibility for `user_data` (the registration did not happen).
        void disarm() noexcept { table_.destroy = nullptr; }

        std::string name() const override { return name_; }
        std::vector<std::string> extensions() const override { return extensions_; }

        E::ScriptValue<std::unique_ptr<E::ScriptInstance>> load(const E::ScriptSource &source) override {
            std::vector<SturdyKeyValue> options;
            for (const auto &[key, value] : source.options) options.push_back({key.c_str(), value.c_str()});
            SturdyScriptSource view{};
            view.struct_size = sizeof(SturdyScriptSource);
            view.name = source.name.c_str();
            view.language = source.language.c_str();
            view.path = source.path.c_str();
            view.code = raw(source.code);
            view.code_size = source.code.size();
            view.options = options.data();
            view.option_count = static_cast<uint32_t>(options.size());
            SturdyScriptInstance instance{};
            instance.struct_size = sizeof(SturdyScriptInstance);
            B::Bytes error;
            SturdyBytesSink sink{&error};
            if (table_.load == nullptr || table_.load(table_.user_data, &view, &instance, &sink) == STURDY_FALSE) {
                return failed(error.empty() ? "the '" + name_ + "' adapter could not load the script" : as_text(error));
            }
            if (!valid_instance(instance)) {
                if (instance.destroy != nullptr) instance.destroy(instance.user_data);
                return failed("the '" + name_ + "' adapter returned an instance with the wrong struct_size");
            }
            return std::make_unique<CScriptInstance>(instance, shared_from_this());
        }

      private:
        SturdyScriptLanguage table_;
        std::string name_;
        std::vector<std::string> extensions_;
    };

    // ---------------------------------------------------------------------------------------------------------------
    // A GameLogic as a SturdyGameModule
    // ---------------------------------------------------------------------------------------------------------------
    struct ModuleBridge {
        std::string name;
        std::vector<std::string> after;
        std::vector<const char *> after_pointers;
        std::unique_ptr<E::GameLogic> logic;
    };

    SturdyBool STURDY_ABI_CALL bridge_initialize(SturdyEngine handle, void *user_data) {
        auto &bridge = *static_cast<ModuleBridge *>(user_data);
        SFT::Engine::Engine *engine = nullptr;
        if (resolve_engine(handle, &engine) != STURDY_OK) return STURDY_FALSE;
        const auto result = bridge.logic->on_engine_initialized(*engine);
        if (!result) {
            SFT::Foundation::log_error("{}", message_text(result.error()));
            SFT::Ffi::set_callback_failure_detail(message_text(result.error()));
            return STURDY_FALSE;
        }
        return STURDY_TRUE;
    }

    SturdyBool STURDY_ABI_CALL bridge_frame(SturdyEngine handle, SturdySurface surface, const SturdyFrameInput *input, SturdyFrame frame, void *user_data) {
        auto &bridge = *static_cast<ModuleBridge *>(user_data);
        SFT::Engine::Engine *engine = nullptr;
        void *target = nullptr;
        if (resolve_engine(handle, &engine) != STURDY_OK || SFT::Ffi::resolve_handle(frame.token, SFT::Ffi::HandleKind::Frame, &target) != STURDY_OK) return STURDY_FALSE;
        SFT::Core::FrameInput frame_input{};
        frame_input.delta_seconds = input->delta_seconds;
        frame_input.frame_index = input->frame_index;
        frame_input.framebuffer_width = input->framebuffer_width;
        frame_input.framebuffer_height = input->framebuffer_height;
        frame_input.live_resize = input->live_resize != STURDY_FALSE;
        SFT::Core::RenderSurfaceHandle surface_handle{};
        surface_handle.window_id = static_cast<decltype(surface_handle.window_id)>(surface.id);
        auto result = bridge.logic->request_render_frame(*engine, surface_handle, frame_input);
        if (!result) return STURDY_FALSE;
        *static_cast<E::RenderFrameParameters *>(target) = std::move(*result);
        return STURDY_TRUE;
    }

    void STURDY_ABI_CALL bridge_shutdown(SturdyEngine handle, void *user_data) {
        auto &bridge = *static_cast<ModuleBridge *>(user_data);
        SFT::Engine::Engine *engine = nullptr;
        if (resolve_engine(handle, &engine) == STURDY_OK) bridge.logic->on_shutdown(*engine);
    }

    void STURDY_ABI_CALL bridge_destroy(void *user_data) { delete static_cast<ModuleBridge *>(user_data); }

    SturdyGameModule module_of(std::string name, const char *const *after, uint32_t after_count, bool contributes_scene, std::unique_ptr<E::GameLogic> logic) {
        auto *bridge = new ModuleBridge{std::move(name), {}, {}, std::move(logic)};
        for (uint32_t i = 0; i < after_count && after != nullptr; ++i) bridge->after.emplace_back(after[i] != nullptr ? after[i] : "");
        for (const auto &entry : bridge->after) bridge->after_pointers.push_back(entry.c_str());
        SturdyGameModule module{};
        module.struct_size = sizeof(SturdyGameModule);
        module.after_count = static_cast<uint32_t>(bridge->after_pointers.size());
        module.name = bridge->name.c_str();
        module.after = bridge->after_pointers.empty() ? nullptr : bridge->after_pointers.data();
        module.contributes_scene = contributes_scene ? STURDY_TRUE : STURDY_FALSE;
        module.logic.struct_size = sizeof(SturdyGameLogic);
        module.logic.user_data = bridge;
        module.logic.on_engine_initialized = bridge_initialize;
        module.logic.request_render_frame = bridge_frame;
        module.logic.on_shutdown = bridge_shutdown;
        module.logic.destroy = bridge_destroy;
        return module;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Conversions
    // ---------------------------------------------------------------------------------------------------------------
    SturdyResult check_source(const SturdyScriptSource &source) {
        if (source.struct_size != sizeof(SturdyScriptSource)) return SFT::Ffi::set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyScriptSource size does not match this engine build");
        if (source.name == nullptr || source.name[0] == '\0') return SFT::Ffi::set_error(STURDY_ERROR_INVALID_ARGUMENT, "a script needs a non-empty name");
        if (source.code == nullptr && source.code_size != 0) return SFT::Ffi::set_error(STURDY_ERROR_INVALID_ARGUMENT, "code must not be null when code_size is nonzero");
        if (source.options == nullptr && source.option_count != 0) return SFT::Ffi::set_error(STURDY_ERROR_INVALID_ARGUMENT, "options must not be null when option_count is nonzero");
        return STURDY_OK;
    }

    E::ScriptSource source_of(const SturdyScriptSource &c) {
        E::ScriptSource source;
        source.name = c.name;
        source.language = text(c.language);
        source.path = text(c.path);
        source.code = to_script(bytes_of(c.code, c.code_size));
        for (const auto &[key, value] : fields_of(c.options, c.option_count)) source.options.emplace_back(key, value);
        return source;
    }

    SturdyResult check_after(const char *const *after, uint32_t count) {
        if (count != 0 && after == nullptr) return SFT::Ffi::set_error(STURDY_ERROR_INVALID_ARGUMENT, "after must not be null when after_count is nonzero");
        for (uint32_t i = 0; i < count; ++i) {
            if (after[i] == nullptr) return SFT::Ffi::set_error(STURDY_ERROR_INVALID_ARGUMENT, "after entries must not be null");
        }
        return STURDY_OK;
    }

    SturdyResult finished_bytes(E::ScriptValue<E::ScriptBytes> result, SturdyAsyncOp *out_op) {
        auto op = std::make_shared<B::Op>();
        if (result) {
            B::Value value;
            value.status = B::OpStatus::Ok;
            value.bytes = to_binding(*result);
            op->complete(std::move(value));
        } else {
            op->fail(SFT::Async::IoErrorCode::Unknown, message_text(result.error()));
        }
        return publish_op(std::move(op), out_op);
    }

    SturdyResult with_engine(SturdyEngine handle, SFT::Engine::Engine **out_engine) { return resolve_engine(handle, out_engine); }

    struct HostFunctionHolder {
        SturdyScriptHostFn function = nullptr;
        void *user_data = nullptr;
        void(STURDY_ABI_CALL *destroy)(void *) = nullptr;
        HostFunctionHolder() = default;
        HostFunctionHolder(const HostFunctionHolder &) = delete;
        HostFunctionHolder &operator=(const HostFunctionHolder &) = delete;
        ~HostFunctionHolder() {
            if (destroy != nullptr) no_throw([&] { destroy(user_data); });
        }
    };

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_script_language_register(const SturdyScriptLanguage *language) {
    return run([&]() -> SturdyResult {
        if (language == nullptr || language->name == nullptr || language->name[0] == '\0') return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a language needs a name");
        if (language->struct_size != sizeof(SturdyScriptLanguage)) return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyScriptLanguage size does not match this engine build");
        if (language->load == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a language needs a load callback");
        if (E::ScriptRegistry::global().language(language->name) != nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a language of that name is already registered");
        auto adapter = std::make_shared<CScriptLanguage>(*language);
        if (!E::ScriptRegistry::global().add_language(adapter)) {
            adapter->disarm(); // lost a race for the name: not taken, so the caller keeps user_data
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a language of that name is already registered");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_language_unregister(const char *name) {
    return run([&]() -> SturdyResult {
        if (name == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
        return E::ScriptRegistry::global().remove_language(name) ? STURDY_OK : set_error(STURDY_ERROR_NOT_AVAILABLE, "no such language");
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_language_count(uint32_t *out_count) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_count, "output pointer must not be null");
        *out_count = static_cast<uint32_t>(E::ScriptRegistry::global().language_names().size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_language_name(uint32_t index, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        const auto names = E::ScriptRegistry::global().language_names();
        if (index >= names.size()) return set_error(STURDY_ERROR_OUT_OF_RANGE, "language index is out of range");
        return SFT::Ffi::copy_string_out(names[index], buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_register_host_function(const char *name, SturdyScriptHostFn function, void *user_data, void(STURDY_ABI_CALL *destroy)(void *user_data)) {
    return run([&]() -> SturdyResult {
        if (name == nullptr || name[0] == '\0' || function == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a host function needs a name and a callback");
        auto holder = std::make_shared<HostFunctionHolder>();
        holder->function = function;
        holder->user_data = user_data;
        holder->destroy = destroy;
        const bool added = E::ScriptRegistry::global().add_function(name, [holder](std::span<const std::byte> args) -> E::ScriptValue<E::ScriptBytes> {
            B::Bytes error;
            B::Bytes result;
            SturdyBytesSink result_sink{&result};
            SturdyBytesSink error_sink{&error};
            if (holder->function(holder->user_data, raw(args), args.size(), &result_sink, &error_sink) == STURDY_FALSE) return failed(error.empty() ? "the host function failed" : as_text(error));
            return to_script(result);
        });
        if (!added) {
            holder->destroy = nullptr; // not taken: the caller keeps user_data
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a host function of that name is already registered");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_unregister_host_function(const char *name) {
    return run([&]() -> SturdyResult {
        if (name == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
        return E::ScriptRegistry::global().remove_function(name) ? STURDY_OK : set_error(STURDY_ERROR_NOT_AVAILABLE, "no such host function");
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_call_host_function(const char *name, const uint8_t *args, size_t args_size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (name == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
        const E::ScriptBytes input = to_script(bytes_of(args, args_size));
        return finished_bytes(E::ScriptRegistry::global().call_function(name, input), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_module_create(const SturdyScriptModuleDesc *desc, SturdyGameModule *out_module) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_module, "output pointer must not be null");
        if (desc == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc must not be null");
        if (desc->struct_size != sizeof(SturdyScriptModuleDesc)) return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyScriptModuleDesc size does not match this engine build");
        if (const auto bad = check_source(desc->source); bad != STURDY_OK) return bad;
        if (const auto bad = check_after(desc->after, desc->after_count); bad != STURDY_OK) return bad;
        *out_module = module_of(desc->source.name, desc->after, desc->after_count, desc->contributes_scene != STURDY_FALSE, E::make_script_module(source_of(desc->source)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_module_create_native(const SturdyNativeScriptDesc *desc, SturdyGameModule *out_module) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_module, "output pointer must not be null");
        if (desc == nullptr || desc->name == nullptr || desc->name[0] == '\0') return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a script needs a name");
        if (desc->struct_size != sizeof(SturdyNativeScriptDesc) || !valid_instance(desc->instance)) return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "a struct size does not match this engine build");
        if (const auto bad = check_after(desc->after, desc->after_count); bad != STURDY_OK) return bad;
        // The factory runs once, at startup, and hands the instance over; if the game never starts, destroying the module destroys it.
        auto pending = std::make_shared<std::unique_ptr<E::ScriptInstance>>(std::make_unique<CScriptInstance>(desc->instance, nullptr));
        *out_module = module_of(desc->name, desc->after, desc->after_count, desc->contributes_scene != STURDY_FALSE,
                                E::make_native_script_module(desc->name, [pending]() -> E::ScriptValue<std::unique_ptr<E::ScriptInstance>> {
                                    if (!*pending) return failed("the script instance was already used");
                                    return std::move(*pending);
                                }));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_module_discard(SturdyGameModule *module) {
    return run([&]() -> SturdyResult {
        if (module == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "module must not be null");
        if (module->logic.destroy != nullptr) module->logic.destroy(module->logic.user_data);
        *module = SturdyGameModule{};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_load(SturdyEngine engine, const SturdyScriptModuleDesc *desc) {
    return run([&]() -> SturdyResult {
        SturdyGameModule module{};
        if (const auto made = sturdy_script_module_create(desc, &module); made != STURDY_OK) return made;
        const auto requested = sturdy_module_request_load(engine, &module);
        if (requested != STURDY_OK) (void)sturdy_script_module_discard(&module); // a refused request took nothing
        return requested;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_reload(SturdyEngine engine, const SturdyScriptSource *source) {
    return run([&]() -> SturdyResult {
        if (source == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "source must not be null");
        if (const auto bad = check_source(*source); bad != STURDY_OK) return bad;
        SFT::Engine::Engine *resolved = nullptr;
        if (const auto bad = with_engine(engine, &resolved); bad != STURDY_OK) return bad;
        if (E::CompositeGameLogic::of(*resolved) == nullptr) return set_error(STURDY_ERROR_NOT_AVAILABLE, "no game is running on this engine; reloading needs sturdy_runtime_run_modules");
        // `done` runs before request_reload returns when the new version does not load: that error goes to the caller. Later outcomes
        // (the swap itself) arrive at the update boundary and go to the failure queue.
        auto synchronous = std::make_shared<std::atomic<bool>>(true);
        auto parse_error = std::make_shared<std::string>();
        auto recorder = SFT::Ffi::module_failure_recorder(*resolved);
        E::ScriptHost::of(*resolved).request_reload(*resolved, source_of(*source), [synchronous, parse_error, recorder](E::GameLogicResult result) {
            if (result) return;
            if (synchronous->load()) *parse_error = message_text(result.error());
            else recorder(std::move(result));
        });
        synchronous->store(false);
        if (!parse_error->empty()) return set_error(STURDY_ERROR_INVALID_ARGUMENT, parse_error->c_str());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_reload_native(SturdyEngine engine, const char *name, const SturdyScriptInstance *instance) {
    return run([&]() -> SturdyResult {
        if (name == nullptr || name[0] == '\0' || instance == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name and instance must not be null");
        if (!valid_instance(*instance)) return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyScriptInstance size does not match this engine build");
        SFT::Engine::Engine *resolved = nullptr;
        if (const auto bad = with_engine(engine, &resolved); bad != STURDY_OK) return bad;
        if (E::CompositeGameLogic::of(*resolved) == nullptr) return set_error(STURDY_ERROR_NOT_AVAILABLE, "no game is running on this engine; reloading needs sturdy_runtime_run_modules");
        auto pending = std::make_shared<std::unique_ptr<E::ScriptInstance>>(std::make_unique<CScriptInstance>(*instance, nullptr));
        E::request_native_script_reload(
            *resolved, name,
            [pending]() -> E::ScriptValue<std::unique_ptr<E::ScriptInstance>> {
                if (!*pending) return failed("the script instance was already used");
                return std::move(*pending);
            },
            SFT::Ffi::module_failure_recorder(*resolved));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_unload(SturdyEngine engine, const char *name) { return sturdy_module_request_unload(engine, name); }

SturdyResult STURDY_ABI_CALL sturdy_script_count(SturdyEngine engine, uint32_t *out_count) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_count, "output pointer must not be null");
        SFT::Engine::Engine *resolved = nullptr;
        if (const auto bad = with_engine(engine, &resolved); bad != STURDY_OK) return bad;
        const auto *host = E::ScriptHost::find(*resolved);
        *out_count = host != nullptr ? static_cast<uint32_t>(host->scripts().size()) : 0;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_name(SturdyEngine engine, uint32_t index, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        SFT::Engine::Engine *resolved = nullptr;
        if (const auto bad = with_engine(engine, &resolved); bad != STURDY_OK) return bad;
        const auto *host = E::ScriptHost::find(*resolved);
        const auto scripts = host != nullptr ? host->scripts() : std::vector<E::ScriptInfo>{};
        if (index >= scripts.size()) return set_error(STURDY_ERROR_OUT_OF_RANGE, "script index is out of range");
        return SFT::Ffi::copy_string_out(scripts[index].name, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_info(SturdyEngine engine, const char *name, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (name == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
        SFT::Engine::Engine *resolved = nullptr;
        if (const auto bad = with_engine(engine, &resolved); bad != STURDY_OK) return bad;
        const auto *host = E::ScriptHost::find(*resolved);
        const auto info = host != nullptr ? host->info(name) : std::nullopt;
        if (!info) return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such script");
        return publish_fields({{"language", info->language}, {"path", info->path}, {"generation", std::to_string(info->generation)}, {"healthy", info->healthy ? "1" : "0"}, {"failure", info->failure}}, out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_call(SturdyEngine engine, const char *script, const char *function, const uint8_t *args, size_t args_size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (script == nullptr || function == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "script and function must not be null");
        SFT::Engine::Engine *resolved = nullptr;
        if (const auto bad = with_engine(engine, &resolved); bad != STURDY_OK) return bad;
        auto *host = E::ScriptHost::find(*resolved);
        if (host == nullptr) return finished_bytes(failed("no scripts are loaded"), out_op);
        const E::ScriptBytes input = to_script(bytes_of(args, args_size));
        return finished_bytes(host->call(script, function, input), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_send(SturdyEngine engine, const char *from, const char *to, const char *topic, const uint8_t *payload, size_t size) {
    return run([&]() -> SturdyResult {
        if (to == nullptr || topic == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "to and topic must not be null");
        SFT::Engine::Engine *resolved = nullptr;
        if (const auto bad = with_engine(engine, &resolved); bad != STURDY_OK) return bad;
        auto *host = E::ScriptHost::find(*resolved);
        if (host == nullptr || !host->send(text(from), to, topic, to_script(bytes_of(payload, size)))) return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such script");
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_publish(SturdyEngine engine, const char *from, const char *topic, const uint8_t *payload, size_t size, uint32_t *out_receivers) {
    return run([&]() -> SturdyResult {
        if (topic == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "topic must not be null");
        SFT::Engine::Engine *resolved = nullptr;
        if (const auto bad = with_engine(engine, &resolved); bad != STURDY_OK) return bad;
        auto *host = E::ScriptHost::find(*resolved);
        const std::size_t receivers = host != nullptr ? host->publish(text(from), topic, to_script(bytes_of(payload, size))) : 0;
        if (out_receivers != nullptr) *out_receivers = static_cast<uint32_t>(receivers);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_subscribe(SturdyEngine engine, const char *script, const char *topic) {
    return run([&]() -> SturdyResult {
        if (script == nullptr || topic == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "script and topic must not be null");
        SFT::Engine::Engine *resolved = nullptr;
        if (const auto bad = with_engine(engine, &resolved); bad != STURDY_OK) return bad;
        auto *host = E::ScriptHost::find(*resolved);
        return host != nullptr && host->subscribe(script, topic) ? STURDY_OK : set_error(STURDY_ERROR_NOT_AVAILABLE, "no such script");
    });
}

SturdyResult STURDY_ABI_CALL sturdy_script_unsubscribe(SturdyEngine engine, const char *script, const char *topic) {
    return run([&]() -> SturdyResult {
        if (script == nullptr || topic == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "script and topic must not be null");
        SFT::Engine::Engine *resolved = nullptr;
        if (const auto bad = with_engine(engine, &resolved); bad != STURDY_OK) return bad;
        auto *host = E::ScriptHost::find(*resolved);
        return host != nullptr && host->unsubscribe(script, topic) ? STURDY_OK : set_error(STURDY_ERROR_NOT_AVAILABLE, "no such subscription");
    });
}

} // extern "C"
