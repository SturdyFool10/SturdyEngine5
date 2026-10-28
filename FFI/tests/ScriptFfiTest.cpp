/// Exercises scripts through the C header only, the way a binding for an interpreter would: a language adapter written against
/// `SturdyScriptLanguage`, host functions, scripts loaded into a running composite, calls, messages, hot reload with state handoff, and
/// native (callback-built) scripts. Needs no window or GPU: the engine object is the headless one the other FFI tests use.

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <Async/Scheduler.hpp>
#include <Engine/CompositeGameLogic.hpp>
#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    int failures = 0;
    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "ScriptFfiTest: %s\n", description);
            ++failures;
        }
    }

    std::vector<std::string> g_log;
    bool logged(const std::string &line) {
        for (const auto &entry : g_log) {
            if (entry == line) return true;
        }
        return false;
    }
    int index_prefix(const std::string &prefix) {
        for (std::size_t i = 0; i < g_log.size(); ++i) {
            if (g_log[i].rfind(prefix, 0) == 0) return static_cast<int>(i);
        }
        return -1;
    }
    int index_of(const std::string &line) {
        for (std::size_t i = 0; i < g_log.size(); ++i) {
            if (g_log[i] == line) return static_cast<int>(i);
        }
        return -1;
    }

    const uint8_t *bytes_of(const char *s) { return reinterpret_cast<const uint8_t *>(s); }
    std::string str(SturdyAsyncOp op) {
        size_t length = 0;
        (void)sturdy_op_bytes(op, nullptr, 0, &length);
        std::string out(length, '\0');
        (void)sturdy_op_bytes(op, reinterpret_cast<uint8_t *>(out.data()), out.size(), &length);
        return out;
    }
    std::string field(SturdyAsyncOp op, const char *name) {
        size_t length = 0;
        if (sturdy_op_field(op, name, nullptr, 0, &length) != STURDY_OK) return "<none>";
        std::string out(length, '\0');
        (void)sturdy_op_field(op, name, out.data(), out.size(), &length);
        if (!out.empty()) out.pop_back();
        return out;
    }
    SturdyOpStatus status(SturdyAsyncOp op) {
        SturdyOpStatus s = STURDY_OP_PENDING;
        (void)sturdy_op_status(op, &s);
        return s;
    }

    // ---- the "mini" language: the source text is "<tag>" or "<tag>@<version>" ----------------------------------------------------
    struct Instance {
        std::string tag;
        std::string version;
        int ticks = 0;
        bool fail_start = false;
        bool refuse_state = false;
    };

    SturdyBool start(SturdyEngine, void *user_data, SturdyBytesSink *error) {
        auto *self = static_cast<Instance *>(user_data);
        if (self->fail_start) {
            const char *reason = "mini start failed";
            (void)sturdy_bytes_sink_set(error, bytes_of(reason), std::strlen(reason));
            return STURDY_FALSE;
        }
        g_log.push_back(self->tag + ":" + self->version + " start");
        return STURDY_TRUE;
    }
    void update(SturdyEngine, const SturdyScriptTick *tick, void *user_data) {
        auto *self = static_cast<Instance *>(user_data);
        ++self->ticks;
        g_log.push_back(self->tag + " tick" + std::to_string(tick->tick));
    }
    void stop(SturdyEngine, void *user_data) { g_log.push_back(static_cast<Instance *>(user_data)->tag + ":" + static_cast<Instance *>(user_data)->version + " stop"); }
    SturdyBool save_state(void *user_data, SturdyBytesSink *out) {
        const std::string state = std::to_string(static_cast<Instance *>(user_data)->ticks);
        (void)sturdy_bytes_sink_set(out, bytes_of(state.data()), state.size());
        return STURDY_TRUE;
    }
    SturdyBool restore_state(void *user_data, const uint8_t *state, size_t size) {
        auto *self = static_cast<Instance *>(user_data);
        if (self->refuse_state) return STURDY_FALSE;
        self->ticks = std::stoi(std::string(reinterpret_cast<const char *>(state), size));
        g_log.push_back(self->tag + ":" + self->version + " restored " + std::to_string(self->ticks));
        return STURDY_TRUE;
    }
    SturdyBool call(void *user_data, const char *function, const uint8_t *args, size_t args_size, SturdyBytesSink *result, SturdyBytesSink *error) {
        auto *self = static_cast<Instance *>(user_data);
        const std::string name = function;
        std::string out;
        if (name == "ticks") out = std::to_string(self->ticks);
        else if (name == "version") out = self->version;
        else if (name == "echo") out.assign(reinterpret_cast<const char *>(args), args_size);
        else {
            const char *reason = "mini: unknown function";
            (void)sturdy_bytes_sink_set(error, bytes_of(reason), std::strlen(reason));
            return STURDY_FALSE;
        }
        (void)sturdy_bytes_sink_set(result, bytes_of(out.data()), out.size());
        return STURDY_TRUE;
    }
    void on_message(void *user_data, const SturdyScriptMessage *message) {
        g_log.push_back(static_cast<Instance *>(user_data)->tag + " got " + message->topic + "=" + std::string(reinterpret_cast<const char *>(message->payload), message->payload_size) +
                        " from '" + message->from + "'");
    }
    int g_destroyed = 0;
    void destroy_instance(void *user_data) {
        ++g_destroyed;
        delete static_cast<Instance *>(user_data);
    }

    SturdyScriptInstance make_instance(Instance *state) {
        SturdyScriptInstance instance{};
        instance.struct_size = sizeof(SturdyScriptInstance);
        instance.user_data = state;
        instance.start = start;
        instance.update = update;
        instance.stop = stop;
        instance.save_state = save_state;
        instance.restore_state = restore_state;
        instance.call = call;
        instance.on_message = on_message;
        instance.destroy = destroy_instance;
        return instance;
    }

    int g_languages_destroyed = 0;
    SturdyBool mini_load(void *, const SturdyScriptSource *source, SturdyScriptInstance *out_instance, SturdyBytesSink *error) {
        const std::string code(reinterpret_cast<const char *>(source->code), source->code_size);
        if (code == "syntax error") {
            const char *reason = "mini: unexpected token";
            (void)sturdy_bytes_sink_set(error, bytes_of(reason), std::strlen(reason));
            return STURDY_FALSE;
        }
        auto *state = new Instance;
        const auto at = code.find('@');
        state->tag = at == std::string::npos ? code : code.substr(0, at);
        state->version = at == std::string::npos ? "v1" : code.substr(at + 1);
        state->fail_start = state->tag == "failing";
        state->refuse_state = state->tag == "stubborn";
        *out_instance = make_instance(state);
        return STURDY_TRUE;
    }
    void mini_destroy(void *) { ++g_languages_destroyed; }

    int g_host_destroyed = 0;
    SturdyBool shout(void *, const uint8_t *args, size_t size, SturdyBytesSink *result, SturdyBytesSink *error) {
        std::string text(reinterpret_cast<const char *>(args), size);
        if (text == "fail") {
            const char *reason = "shout refused";
            (void)sturdy_bytes_sink_set(error, bytes_of(reason), std::strlen(reason));
            return STURDY_FALSE;
        }
        for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        (void)sturdy_bytes_sink_set(result, bytes_of(text.data()), text.size());
        return STURDY_TRUE;
    }
    void host_destroy(void *) { ++g_host_destroyed; }

    SturdyScriptModuleDesc desc_of(const char *name, const char *code, const char *const *after = nullptr, uint32_t after_count = 0) {
        SturdyScriptModuleDesc desc{};
        desc.struct_size = sizeof(SturdyScriptModuleDesc);
        desc.source.struct_size = sizeof(SturdyScriptSource);
        desc.source.name = name;
        desc.source.language = "mini";
        desc.source.code = bytes_of(code);
        desc.source.code_size = std::strlen(code);
        desc.after = after;
        desc.after_count = after_count;
        return desc;
    }

    /// The C++ module every game needs at least one of; scripts are loaded beside it.
    class Host final : public SFT::Engine::GameLogic {
      public:
        SFT::Engine::GameLogicResult on_engine_initialized(SFT::Engine::Engine &) override { return {}; }
        std::optional<SFT::Engine::RenderFrameParameters> request_render_frame(SFT::Engine::Engine &, SFT::Core::RenderSurfaceHandle, const SFT::Core::FrameInput &) override {
            return std::nullopt;
        }
    };

} // namespace

int main() {
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::ScopedHandle;

    // ---- languages and host functions ----
    const char *const extensions[] = {".mini"};
    SturdyScriptLanguage language{};
    language.struct_size = sizeof(SturdyScriptLanguage);
    language.name = "mini";
    language.extensions = extensions;
    language.extension_count = 1;
    language.load = mini_load;
    language.destroy = mini_destroy;
    check(sturdy_script_language_register(nullptr) == STURDY_ERROR_INVALID_ARGUMENT, "a null language is refused");
    SturdyScriptLanguage bad = language;
    bad.struct_size = 1;
    check(sturdy_script_language_register(&bad) == STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "a wrong struct size is refused");
    check(sturdy_script_language_register(&language) == STURDY_OK, "register the language");
    check(sturdy_script_language_register(&language) == STURDY_ERROR_INVALID_ARGUMENT && g_languages_destroyed == 0, "a duplicate is refused and leaves user_data with the caller");
    uint32_t count = 0;
    char name[32];
    size_t length = 0;
    check(sturdy_script_language_count(&count) == STURDY_OK && count >= 1, "count");
    check(sturdy_script_language_name(0, name, sizeof(name), &length) == STURDY_OK && std::string(name) == "mini", "name");

    check(sturdy_script_register_host_function("shout", shout, nullptr, host_destroy) == STURDY_OK, "register a host function");
    check(sturdy_script_register_host_function("shout", shout, nullptr, host_destroy) == STURDY_ERROR_INVALID_ARGUMENT && g_host_destroyed == 0, "a duplicate is refused without destroying user_data");
    SturdyAsyncOp op{0};
    check(sturdy_script_call_host_function("shout", bytes_of("hey"), 3, &op) == STURDY_OK && status(op) == STURDY_OP_OK && str(op) == "HEY", "call a host function");
    (void)sturdy_op_release(op);
    check(sturdy_script_call_host_function("shout", bytes_of("fail"), 4, &op) == STURDY_OK && status(op) == STURDY_OP_FAILED, "a failing host function fails the op");
    (void)sturdy_op_release(op);
    check(sturdy_script_call_host_function("nope", nullptr, 0, &op) == STURDY_OK && status(op) == STURDY_OP_FAILED, "an unknown one too");
    (void)sturdy_op_release(op);

    // ---- modules that never run are discarded cleanly ----
    {
        g_destroyed = 0;
        SturdyGameModule module{};
        const SturdyScriptModuleDesc desc = desc_of("unused", "unused");
        check(sturdy_script_module_create(&desc, &module) == STURDY_OK && std::string(module.name) == "unused", "create a module");
        check(sturdy_script_module_discard(&module) == STURDY_OK && module.name == nullptr, "discard it");
        SturdyScriptModuleDesc wrong = desc;
        wrong.source.name = "";
        check(sturdy_script_module_create(&wrong, &module) == STURDY_ERROR_INVALID_ARGUMENT, "an empty name is refused");
        wrong = desc;
        wrong.struct_size = 3;
        check(sturdy_script_module_create(&wrong, &module) == STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "a wrong desc size is refused");
    }

    // ---- scripts in a running game ----
    {
        g_log.clear();
        g_destroyed = 0;
        SFT::Engine::Engine engine_object;
        const ScopedHandle engine_handle{HandleKind::Engine, &engine_object};
        const SturdyEngine engine{engine_handle.token()};
        SFT::Engine::CompositeGameLogic composite;
        check(composite.add("host", std::make_unique<Host>()).has_value(), "add a C++ module (a game needs at least one)");
        check(composite.on_engine_initialized(engine_object).has_value(), "the composite starts");

        const char *const gameplay_after[] = {"physics"};
        check(sturdy_script_load(engine, &(const SturdyScriptModuleDesc &)desc_of("physics", "physics@v1")) == STURDY_OK, "load physics");
        const SturdyScriptModuleDesc gameplay = desc_of("gameplay", "gameplay@v1", gameplay_after, 1);
        check(sturdy_script_load(engine, &gameplay) == STURDY_OK, "load gameplay (after physics)");
        engine_object.update(0.016);
        check(index_of("physics:v1 start") >= 0 && index_of("physics:v1 start") < index_of("gameplay:v1 start"), "scripts start in dependency order");
        check(sturdy_script_count(engine, &count) == STURDY_OK && count == 2, "two scripts");
        check(sturdy_script_name(engine, 1, name, sizeof(name), &length) == STURDY_OK && std::string(name) == "gameplay", "names in start order");

        g_log.clear();
        engine_object.update(0.016);
        check(index_of("physics tick2") >= 0 && index_of("physics tick2") < index_of("gameplay tick2"), "scripts tick every update, in order");

        check(sturdy_script_info(engine, "gameplay", &op) == STURDY_OK && field(op, "language") == "mini" && field(op, "generation") == "1" && field(op, "healthy") == "1", "info");
        (void)sturdy_op_release(op);
        check(sturdy_script_info(engine, "ghost", &op) == STURDY_ERROR_NOT_AVAILABLE, "info for an unknown script");

        check(sturdy_script_call(engine, "gameplay", "ticks", nullptr, 0, &op) == STURDY_OK && str(op) == "2", "call a script");
        (void)sturdy_op_release(op);
        check(sturdy_script_call(engine, "gameplay", "echo", bytes_of("ping"), 4, &op) == STURDY_OK && str(op) == "ping", "call with arguments");
        (void)sturdy_op_release(op);
        check(sturdy_script_call(engine, "gameplay", "nope", nullptr, 0, &op) == STURDY_OK && status(op) == STURDY_OP_FAILED, "an unknown function fails the op");
        (void)sturdy_op_release(op);

        g_log.clear();
        check(sturdy_script_send(engine, nullptr, "gameplay", "greet", bytes_of("hello"), 5) == STURDY_OK, "send");
        check(sturdy_script_send(engine, nullptr, "ghost", "greet", nullptr, 0) == STURDY_ERROR_NOT_AVAILABLE, "send to nobody");
        check(sturdy_script_subscribe(engine, "physics", "news") == STURDY_OK && sturdy_script_subscribe(engine, "gameplay", "news") == STURDY_OK, "subscribe");
        uint32_t receivers = 0;
        check(sturdy_script_publish(engine, "physics", "news", bytes_of("flash"), 5, &receivers) == STURDY_OK && receivers == 1, "publish");
        engine_object.update(0.016);
        check(logged("gameplay got greet=hello from ''") && logged("gameplay got news=flash from 'physics'"), "messages arrive at the update boundary");

        // Reload with state handoff.
        g_log.clear();
        SturdyScriptSource next{};
        next.struct_size = sizeof(SturdyScriptSource);
        next.name = "gameplay";
        next.language = "mini";
        next.code = bytes_of("gameplay@v2");
        next.code_size = 11;
        check(sturdy_script_reload(engine, &next) == STURDY_OK, "request a reload");
        engine_object.update(0.016);
        check(index_prefix("gameplay:v1 stop") >= 0 && index_prefix("gameplay:v1 stop") < index_prefix("gameplay:v2 restored ") && index_prefix("gameplay:v2 restored ") < index_of("gameplay:v2 start"), "stop, restore the state, start");
        check(sturdy_script_call(engine, "gameplay", "version", nullptr, 0, &op) == STURDY_OK && str(op) == "v2", "the new version answers");
        (void)sturdy_op_release(op);
        check(sturdy_script_info(engine, "gameplay", &op) == STURDY_OK && field(op, "generation") == "2", "generation 2");
        (void)sturdy_op_release(op);

        SturdyScriptSource broken = next;
        broken.code = bytes_of("syntax error");
        broken.code_size = 12;
        check(sturdy_script_reload(engine, &broken) == STURDY_ERROR_INVALID_ARGUMENT, "a parse error is reported to the caller");
        engine_object.update(0.016);
        check(sturdy_script_call(engine, "gameplay", "version", nullptr, 0, &op) == STURDY_OK && str(op) == "v2", "and the old version keeps running");
        (void)sturdy_op_release(op);

        // A native script, built from callbacks.
        g_log.clear();
        SturdyNativeScriptDesc native{};
        native.struct_size = sizeof(SturdyNativeScriptDesc);
        native.name = "native";
        auto *native_state = new Instance;
        native_state->tag = "native";
        native_state->version = "c1";
        native.instance = make_instance(native_state);
        SturdyGameModule native_module{};
        check(sturdy_script_module_create_native(&native, &native_module) == STURDY_OK, "create a native script module");
        check(sturdy_module_request_load(engine, &native_module) == STURDY_OK, "load it into the game");
        engine_object.update(0.016);
        check(logged("native:c1 start"), "the native script started");
        auto *replacement = new Instance;
        replacement->tag = "native";
        replacement->version = "c2";
        const SturdyScriptInstance replacement_table = make_instance(replacement);
        check(sturdy_script_reload_native(engine, "native", &replacement_table) == STURDY_OK, "reload it with another instance");
        engine_object.update(0.016);
        check(logged("native:c2 start") && sturdy_script_info(engine, "native", &op) == STURDY_OK && field(op, "language") == "native", "the replacement runs");
        (void)sturdy_op_release(op);

        // A script whose start fails does not load, and says why.
        SturdyScriptModuleDesc failing = desc_of("failing", "failing");
        check(sturdy_script_load(engine, &failing) == STURDY_OK, "request a script that will fail to start");
        engine_object.update(0.016);
        char message[256] = {};
        const auto taken = sturdy_module_take_failure(engine, message, sizeof(message), &length);
        check(taken == STURDY_OK && std::string(message).find("mini start failed") != std::string::npos, "the failure reaches sturdy_module_take_failure");

        check(sturdy_script_unload(engine, "native") == STURDY_OK, "unload");
        engine_object.update(0.016);
        check(sturdy_script_count(engine, &count) == STURDY_OK && count == 2, "the unloaded script is gone");

        composite.on_shutdown(engine_object);
        check(logged("gameplay:v2 stop") && logged("physics:v1 stop"), "shutdown stops the scripts");
    }
    check(g_destroyed >= 5, "every instance was destroyed");

    // ---- teardown of registries ----
    check(sturdy_script_unregister_host_function("shout") == STURDY_OK && g_host_destroyed == 1, "unregistering a host function releases its user_data");
    check(sturdy_script_language_unregister("mini") == STURDY_OK && g_languages_destroyed == 1, "unregistering a language releases its user_data once nothing uses it");
    check(sturdy_script_language_unregister("mini") == STURDY_ERROR_NOT_AVAILABLE, "unregistering twice");

    SFT::Async::Scheduler::shutdown();
    if (failures != 0) {
        (void)std::fprintf(stderr, "ScriptFfiTest: %d check(s) failed\n", failures);
        return 1;
    }
    (void)std::printf("ScriptFfiTest passed\n");
    return 0;
}
