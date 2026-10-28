// Scripts: language adapters, the start/update/stop lifecycle in module order, calls, messages, host functions, error containment and hot
// reload with state handoff. Pure CPU: a toy language stands in for Lua or JavaScript.

#include <Engine/Script.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace SFT;
using namespace SFT::Engine;
using EngineT = SFT::Engine::Engine;

namespace {

    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }

    ScriptBytes bytes(std::string_view text) {
        ScriptBytes out(text.size());
        if (!text.empty()) std::memcpy(out.data(), text.data(), text.size());
        return out;
    }
    std::string text(std::span<const std::byte> data) { return std::string(reinterpret_cast<const char *>(data.data()), data.size()); }
    std::string message_of(const GameLogicError &error) { return std::string(std::string_view{error.message}); }

    std::vector<std::string> g_log;

    /// The toy language: the "source" is a script name tag; the script appends events to g_log, counts ticks, and exposes `count`, `boom`,
    /// `ping` (sends to the script named in the args) and `host` (calls the host function "twice").
    class ToyScript final : public ScriptInstance {
      public:
        ToyScript(std::string tag, std::string version) : tag_(std::move(tag)), version_(std::move(version)) {}

        GameLogicResult start(EngineT &) override {
            if (tag_ == "failing") return std::unexpected(GameLogicError{UString{"toy start failed"}});
            g_log.push_back(tag_ + ":" + version_ + " start");
            return {};
        }
        void update(EngineT &, const ScriptTick &tick) override {
            ++ticks_;
            last_tick_ = tick.tick;
            g_log.push_back(tag_ + " update" + std::to_string(tick.tick));
            if (throws_on_update_) throw std::runtime_error("boom in update");
        }
        void stop(EngineT &) noexcept override { g_log.push_back(tag_ + ":" + version_ + " stop"); }
        std::optional<ScriptBytes> save_state() override { return bytes(std::to_string(ticks_)); }
        bool restore_state(std::span<const std::byte> state) override {
            if (tag_ == "stubborn") return false;
            ticks_ = std::stoi(text(state));
            g_log.push_back(tag_ + ":" + version_ + " restored " + std::to_string(ticks_));
            return true;
        }
        ScriptValue<ScriptBytes> call(std::string_view function, std::span<const std::byte> args) override {
            if (function == "count") return bytes(std::to_string(ticks_));
            if (function == "version") return bytes(version_);
            if (function == "arm") {
                throws_on_update_ = true;
                return ScriptBytes{};
            }
            if (function == "host") return ScriptRegistry::global().call_function("twice", args);
            return ScriptInstance::call(function, args);
        }
        void on_message(const ScriptMessage &message) override {
            g_log.push_back(tag_ + " got " + message.topic + "=" + text(message.payload) + " from '" + message.from + "'");
        }

      private:
        std::string tag_;
        std::string version_;
        int ticks_ = 0;
        std::uint64_t last_tick_ = 0;
        bool throws_on_update_ = false;
    };

    class ToyLanguage final : public ScriptLanguage {
      public:
        std::string name() const override { return "toy"; }
        std::vector<std::string> extensions() const override { return {".toy"}; }
        ScriptValue<std::unique_ptr<ScriptInstance>> load(const ScriptSource &source) override {
            const std::string code = text(source.code);
            if (code == "syntax error") return std::unexpected(GameLogicError{UString{"toy: unexpected token"}});
            // The "source" is `tag` or `tag@version`.
            const auto at = code.find('@');
            return std::make_unique<ToyScript>(at == std::string::npos ? code : code.substr(0, at), at == std::string::npos ? "v1" : code.substr(at + 1));
        }
    };

    ScriptSource toy(std::string name, std::string code) {
        ScriptSource source;
        source.name = std::move(name);
        source.language = "toy";
        source.code = bytes(code);
        return source;
    }

    bool log_has(const std::string &entry) {
        for (const auto &line : g_log) {
            if (line == entry) return true;
        }
        return false;
    }
    std::size_t log_index(const std::string &entry) {
        for (std::size_t i = 0; i < g_log.size(); ++i) {
            if (g_log[i] == entry) return i;
        }
        return static_cast<std::size_t>(-1);
    }

} // namespace

int main() {
    auto &registry = ScriptRegistry::global();

    // ---- the registry ----
    {
        check(registry.add_language(std::make_shared<ToyLanguage>()), "register a language");
        check(!registry.add_language(std::make_shared<ToyLanguage>()), "a second language of the same name is refused");
        check(registry.language("toy") != nullptr && registry.language("lua") == nullptr, "lookup by name");
        check(registry.language_for_extension(".toy") != nullptr && registry.language_for_extension(".lua") == nullptr, "lookup by extension");
        check(registry.add_function("twice", [](std::span<const std::byte> args) -> ScriptValue<ScriptBytes> {
            std::string doubled = text(args);
            return bytes(doubled + doubled);
        }), "register a host function");
        check(!registry.add_function("twice", [](std::span<const std::byte>) -> ScriptValue<ScriptBytes> { return ScriptBytes{}; }), "duplicate host function refused");
        auto doubled = registry.call_function("twice", bytes("ab"));
        check(doubled && text(*doubled) == "abab", "call a host function");
        check(!registry.call_function("missing", {}) && message_of(registry.call_function("missing", {}).error()).find("missing") != std::string::npos, "an unknown host function is an error naming it");
    }

    // ---- lifecycle, ordering, calls, messages ----
    {
        g_log.clear();
        EngineT engine;
        CompositeGameLogic game;
        check(game.add("hud", make_script_module(toy("hud", "hud")), {.after = {"gameplay"}, .contributes_scene = false}).has_value(), "add hud");
        check(game.add("gameplay", make_script_module(toy("gameplay", "gameplay@v1"))).has_value(), "add gameplay");
        check(game.on_engine_initialized(engine).has_value(), "the game starts");
        check(g_log.size() == 2 && g_log[0] == "gameplay:v1 start" && g_log[1] == "hud:v1 start", "scripts start in module order (dependencies first)");

        auto *host = ScriptHost::find(engine);
        check(host != nullptr && host->scripts().size() == 2, "the host lists both scripts");
        check(host && host->info("gameplay") && host->info("gameplay")->language == "toy" && host->info("gameplay")->generation == 1, "info reports language and generation");

        g_log.clear();
        engine.update(0.016);
        check(g_log.size() == 2 && g_log[0] == "gameplay update1" && g_log[1] == "hud update1", "every update ticks scripts in start order");
        engine.update(0.016);
        auto count = host->call("gameplay", "count", {});
        check(count && text(*count) == "2", "call a script function");
        check(!host->call("gameplay", "nope", {}) && !host->call("ghost", "count", {}), "unknown function / script are errors");
        auto via_host = host->call("hud", "host", bytes("xy"));
        check(via_host && text(*via_host) == "xyxy", "a script reaches a host function");

        g_log.clear();
        check(host->send("hud", "gameplay", "hello", bytes("world")), "send to a loaded script");
        check(!host->send("hud", "ghost", "hello", {}), "send to nobody fails");
        check(g_log.empty(), "messages wait for the update boundary");
        check(host->subscribe("hud", "news") && host->subscribe("gameplay", "news"), "subscribe");
        check(host->publish("hud", "news", bytes("extra")) == 1, "publish reaches the other subscriber, not the sender");
        engine.update(0.016);
        check(log_has("gameplay got hello=world from 'hud'") && log_has("gameplay got news=extra from 'hud'"), "messages are delivered in the next update");
        check(log_index("gameplay got hello=world from 'hud'") < log_index("gameplay update3"), "and before the scripts tick");
        check(host->unsubscribe("gameplay", "news") && host->publish("hud", "news", {}) == 0, "unsubscribe");

        // An exception contains itself: the script stops ticking, the others carry on.
        g_log.clear();
        (void)host->call("hud", "arm", {});
        engine.update(0.016);
        engine.update(0.016);
        check(host->info("hud") && !host->info("hud")->healthy && host->info("hud")->failure.find("boom in update") != std::string::npos, "a throwing script is marked unhealthy with its message");
        check(log_has("gameplay update5") && !log_has("hud update5"), "the others keep ticking, the failed one does not");

        // Reload with state handoff.
        g_log.clear();
        bool reloaded = false;
        host->request_reload(engine, toy("gameplay", "gameplay@v2"), [&](GameLogicResult r) { reloaded = r.has_value(); });
        check(g_log.empty(), "a reload waits for the update boundary");
        engine.update(0.016);
        check(reloaded, "the reload succeeded");
        check(log_index("gameplay:v1 stop") < log_index("gameplay:v2 restored 5") && log_index("gameplay:v2 restored 5") < log_index("gameplay:v2 start"), "stop, then restore the old state, then start");
        auto version = host->call("gameplay", "version", {});
        check(version && text(*version) == "v2" && host->info("gameplay")->generation == 2, "the new version answers and the generation advanced");
        auto carried = host->call("gameplay", "count", {});
        check(carried && std::stoi(text(*carried)) >= 5, "the tick count survived the reload");

        // A reload that does not parse leaves the old one running.
        reloaded = true;
        std::string failure_text;
        host->request_reload(engine, toy("gameplay", "syntax error"), [&](GameLogicResult r) {
            reloaded = r.has_value();
            if (!r) failure_text = message_of(r.error());
        });
        check(!reloaded && failure_text.find("unexpected token") != std::string::npos, "a parse error is reported at once");
        engine.update(0.016);
        check(host->call("gameplay", "version", {}).value_or(ScriptBytes{}) == bytes("v2"), "and the running version is untouched");

        // A new version that refuses the old state: the reload fails and the script stays unloaded.
        failure_text.clear();
        host->request_reload(engine, toy("gameplay", "stubborn"), [&](GameLogicResult r) {
            if (!r) failure_text = message_of(r.error());
        });
        engine.update(0.016);
        check(failure_text.find("refused") != std::string::npos, "refusing the state fails the reload");
        check(!host->call("gameplay", "version", {}).has_value(), "and the script is gone");

        g_log.clear();
        game.on_shutdown(engine);
        check(log_has("hud:v1 stop") && ScriptHost::find(engine) == nullptr, "shutdown stops the scripts and drops the host");
    }

    // ---- startup failures ----
    {
        EngineT engine;
        CompositeGameLogic game;
        check(game.add("ok", make_script_module(toy("ok", "fine"))).has_value() && game.add("bad", make_script_module(toy("bad", "failing")), {.after = {"ok"}}).has_value(), "add");
        g_log.clear();
        const auto started = game.on_engine_initialized(engine);
        check(!started && message_of(started.error()).find("toy start failed") != std::string::npos, "a script whose start fails fails the game, naming it");
        check(log_has("fine:v1 stop"), "the scripts that had started are stopped again");

        CompositeGameLogic unknown;
        check(unknown.add("x", make_script_module(toy("x", "t"), nullptr)).has_value(), "add");
        ScriptSource missing = toy("y", "t");
        missing.language = "cobol";
        check(unknown.add("y", make_script_module(missing)).has_value(), "add y");
        const auto unresolved = unknown.on_engine_initialized(engine);
        check(!unresolved && message_of(unresolved.error()).find("cobol") != std::string::npos, "an unregistered language is reported by name");
        unknown.on_shutdown(engine);
    }

    // ---- path-based language choice and native scripts ----
    {
        g_log.clear();
        EngineT engine;
        CompositeGameLogic game;
        ScriptSource by_extension;
        by_extension.name = "scripted";
        by_extension.path = "game/main.toy";
        by_extension.code = bytes("scripted@by-extension");
        check(game.add("scripted", make_script_module(by_extension)).has_value(), "add by extension");
        check(game.add("native", make_native_script_module("native", [] { return ScriptValue<std::unique_ptr<ScriptInstance>>(std::make_unique<ToyScript>("native", "cpp")); })).has_value(), "add native");
        check(game.on_engine_initialized(engine).has_value(), "start");
        check(log_has("scripted:by-extension start") && log_has("native:cpp start"), "the language came from the extension; a native script needs none");
        auto *host = ScriptHost::find(engine);
        check(host && host->info("scripted")->language == "toy" && host->info("native")->language == "native", "info reports the language");

        engine.update(0.016);
        bool done = false;
        request_native_script_reload(engine, "native", [] { return ScriptValue<std::unique_ptr<ScriptInstance>>(std::make_unique<ToyScript>("native", "cpp2")); }, [&](GameLogicResult r) { done = r.has_value(); });
        engine.update(0.016);
        check(done && log_has("native:cpp2 restored 1"), "a native script reloads with state handoff too");
        game.on_shutdown(engine);
    }

    // ---- unloading through the module API ----
    {
        EngineT engine;
        CompositeGameLogic game;
        check(game.add("a", make_script_module(toy("a", "a"))).has_value() && game.add("b", make_script_module(toy("b", "b"))).has_value(), "add");
        check(game.on_engine_initialized(engine).has_value(), "start");
        game.request_unload(engine, "a");
        engine.update(0.016);
        auto *host = ScriptHost::find(engine);
        check(host && host->scripts().size() == 1 && host->scripts()[0].name == "b", "unloading removes the script from the host");
        check(!host->call("a", "count", {}), "and it can no longer be called");
        game.on_shutdown(engine);
    }

    if (failures == 0) std::printf("ScriptTest passed\n");
    return failures == 0 ? 0 : 1;
}
