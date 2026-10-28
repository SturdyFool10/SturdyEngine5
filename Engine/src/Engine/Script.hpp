#pragma once

// Scripts: units of game logic that can be written in any language and run side by side in one game.
//
// A *script* is a game-logic module (see `CompositeGameLogic`) whose body is a `ScriptInstance`: something with a lifecycle (start, a
// per-update tick, optional frame contribution, stop), the ability to be called by name, to receive messages and to hand its state to its own
// replacement when it is hot-reloaded. Where the instance comes from depends on the language:
//
//   * C++            -- derive from `ScriptInstance` (or from `GameLogic` directly, for a plain module).
//   * Rust, Java, C#, Zig, Go ... compiled -- build a `ScriptInstance` out of callbacks through the C ABI (`sturdy_script_module_create_native`)
//                       or ship a native plugin; the engine does not care which language filled the callbacks in.
//   * JavaScript, Lua, Python ... interpreted -- the *application* supplies the interpreter: it registers a `ScriptLanguage` adapter that turns
//                       source text (or bytecode) into a `ScriptInstance`. The engine ships no interpreter; it ships the seam.
//
// Threading contract (the point of the design): every callback of every script runs while one lock, the script lock, is held -- so an
// interpreter that is not thread-safe never sees two threads, whichever thread the engine happens to use (the update thread for `start`,
// `update`, messages and `stop`; a render thread for `request_render_frame`). `ScriptInstance::update` runs at the update boundary
// (`Engine::add_update_hook`): nothing else touches the world at that moment, so a script may use the whole engine API directly. Scripts that
// need real parallelism are native modules, which are never serialized.

#include <Engine/CompositeGameLogic.hpp>
#include <Engine/GameLogic.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SFT::Engine {

    using ScriptBytes = std::vector<std::byte>;
    template <typename T>
    using ScriptValue = std::expected<T, GameLogicError>;

    /// Where a script comes from.
    struct ScriptSource {
        /// Unique; the script is a module of this name, so `after` lists and the module requests use it.
        std::string name;
        /// The language adapter to use ("lua", "js", ...). Empty: chosen from `path`'s extension.
        std::string language;
        /// A file the adapter may read (and the extension used to pick a language).
        std::string path;
        /// The source text or bytecode. Empty: the adapter loads `path`.
        ScriptBytes code;
        /// Adapter-specific settings (a module root, strict mode, a sandbox profile ...).
        std::vector<std::pair<std::string, std::string>> options;
    };

    /// One engine update as a script sees it.
    struct ScriptTick {
        double delta_seconds = 0;
        double elapsed_seconds = 0;
        std::uint64_t tick = 0;
    };

    /// A message between scripts (or from the host application).
    struct ScriptMessage {
        /// The sending script; empty when the host application sent it.
        std::string from;
        std::string topic;
        ScriptBytes payload;
    };

    /// A running script. Every method is called with the script lock held and never concurrently with another method of any script.
    class ScriptInstance {
      public:
        virtual ~ScriptInstance() = default;

        /// Runs once when the script starts (on the runtime thread, with the script registered as a module, so what it registers goes away
        /// with it). Failing aborts the load and the message reaches the caller.
        [[nodiscard]] virtual GameLogicResult start(Engine &engine) = 0;

        /// Runs once per engine update, at the update boundary, in the module start order.
        virtual void update(Engine &engine, const ScriptTick &tick) {
            (void)engine;
            (void)tick;
        }

        /// Contributes to a frame like `GameLogic::request_render_frame`. May run on a render thread (the script lock is held).
        [[nodiscard]] virtual std::optional<RenderFrameParameters> request_render_frame(Engine &engine, Core::RenderSurfaceHandle surface,
                                                                                         const Core::FrameInput &frame) {
            (void)engine;
            (void)surface;
            (void)frame;
            return std::nullopt;
        }

        /// Runs once when the script is unloaded or replaced, after `save_state`.
        virtual void stop(Engine &engine) noexcept { (void)engine; }

        /// Called when the script is about to be replaced by a reload: whatever this returns is given to the replacement's `restore_state`
        /// before its `start`. Return nothing for "no state".
        [[nodiscard]] virtual std::optional<ScriptBytes> save_state() { return std::nullopt; }
        /// Receives the previous version's state. Returning false aborts the reload and the old version stays unloaded.
        [[nodiscard]] virtual bool restore_state(std::span<const std::byte> state) {
            (void)state;
            return true;
        }

        /// Calls a function the script exposes. The default exposes none.
        [[nodiscard]] virtual ScriptValue<ScriptBytes> call(std::string_view function, std::span<const std::byte> args) {
            (void)args;
            return std::unexpected(GameLogicError{UString{std::string("no function '") + std::string(function) + "'"}});
        }

        /// A message sent to this script or published on a topic it subscribed to. Delivered at the update boundary, in send order.
        virtual void on_message(const ScriptMessage &message) { (void)message; }
    };

    /// Turns source into a running script: the seam an application uses to bring an interpreter (QuickJS, V8, Lua, ...) into the engine.
    class ScriptLanguage {
      public:
        virtual ~ScriptLanguage() = default;
        /// The name `ScriptSource::language` refers to ("lua", "js").
        [[nodiscard]] virtual std::string name() const = 0;
        /// File extensions (with the dot) that select this language when a source names only a path.
        [[nodiscard]] virtual std::vector<std::string> extensions() const { return {}; }
        /// Parses / compiles `source` into an instance. Called under the script lock; the instance must not start running yet.
        [[nodiscard]] virtual ScriptValue<std::unique_ptr<ScriptInstance>> load(const ScriptSource &source) = 0;
    };

    /// A function the host application exposes to every script, by name, over byte strings. How a script reaches it is the adapter's business
    /// (Lua `host.call("name", ...)`, a JS `sturdy.call`); the engine only provides the registry.
    using ScriptHostFunction = std::function<ScriptValue<ScriptBytes>(std::span<const std::byte> args)>;

    /// Process-wide registries: language adapters and host functions. Registered before the engine exists (the usual case: the application sets
    /// them up, then starts the runtime) or while it runs.
    class ScriptRegistry {
      public:
        [[nodiscard]] static ScriptRegistry &global();

        /// Registers an adapter. False when a language of that name is already registered.
        bool add_language(std::shared_ptr<ScriptLanguage> language);
        /// Forgets a language; scripts already running keep their instances (and so the adapter) alive until they are unloaded.
        bool remove_language(std::string_view name);
        [[nodiscard]] std::shared_ptr<ScriptLanguage> language(std::string_view name) const;
        /// The language that claims `extension` (".lua"), if any.
        [[nodiscard]] std::shared_ptr<ScriptLanguage> language_for_extension(std::string_view extension) const;
        [[nodiscard]] std::vector<std::string> language_names() const;

        /// Registers a function scripts can call by name. False when the name is taken.
        bool add_function(std::string name, ScriptHostFunction function);
        bool remove_function(std::string_view name);
        [[nodiscard]] ScriptValue<ScriptBytes> call_function(std::string_view name, std::span<const std::byte> args) const;
        [[nodiscard]] std::vector<std::string> function_names() const;

      private:
        mutable std::mutex mutex_;
        std::map<std::string, std::shared_ptr<ScriptLanguage>, std::less<>> languages_;
        std::map<std::string, std::shared_ptr<ScriptHostFunction>, std::less<>> functions_;
    };

    /// What `ScriptHost::scripts` reports.
    struct ScriptInfo {
        std::string name;
        std::string language;
        std::string path;
        /// 1 for the first version loaded under this name, +1 for each reload.
        std::uint64_t generation = 0;
        /// False once a callback threw: the script stays loaded but is no longer ticked; reload it.
        bool healthy = true;
        std::string failure;
    };

    /// The scripts of one running engine: call them, message them, list them, reload them.
    class ScriptHost {
      public:
        /// The host attached to `engine` (created on first use).
        [[nodiscard]] static ScriptHost &of(Engine &engine);
        /// The host attached to `engine`, or null when no script was ever loaded on it.
        [[nodiscard]] static ScriptHost *find(Engine &engine) noexcept;

        ScriptHost(const ScriptHost &) = delete;
        ScriptHost &operator=(const ScriptHost &) = delete;
        ~ScriptHost();

        [[nodiscard]] std::vector<ScriptInfo> scripts() const;
        [[nodiscard]] std::optional<ScriptInfo> info(std::string_view script) const;

        /// Calls `function` of `script` now. Takes the script lock, so it is safe from any thread (it waits while another script callback runs)
        /// and from inside a script callback.
        [[nodiscard]] ScriptValue<ScriptBytes> call(std::string_view script, std::string_view function, std::span<const std::byte> args);

        /// Queues a message for `to`, delivered at the next update boundary. False when no such script is loaded.
        bool send(std::string from, std::string to, std::string topic, ScriptBytes payload);
        /// Queues a message for every script subscribed to `topic` (except `from`). Returns how many will get it.
        std::size_t publish(std::string from, std::string topic, ScriptBytes payload);
        bool subscribe(std::string script, std::string topic);
        bool unsubscribe(std::string_view script, std::string_view topic);
        /// Delivers queued messages now (the update hook does this every update). Takes the script lock.
        void flush_messages();

        /// Replaces the script called `source.name` with a freshly loaded one: same place in the start order and the same options. The old
        /// version's `save_state` goes to the new one's `restore_state` before its `start`. If the new one cannot be loaded, the old one keeps
        /// running and `done` carries the error. Runs at the next update boundary.
        void request_reload(Engine &engine, ScriptSource source, std::function<void(GameLogicResult)> done = {});

        // ---- used by the script modules ----
        struct Entry;

      private:
        friend class ScriptModule;
        explicit ScriptHost(Engine &engine);
        /// The update hook body: deliver messages, then tick every healthy script in start order.
        void run_update(Engine &engine, double delta_seconds);
        /// Drops this host when it holds no scripts (so a stale one cannot outlive its engine). `*this` is destroyed by this call when it holds nothing.
        void release_if_idle(Engine &engine);

        struct State;
        std::unique_ptr<State> state_;
    };

    /// Makes a module that loads `source` through its language adapter when the game starts. `language` null: resolved from
    /// `ScriptRegistry::global()` by `source.language` (or the path's extension) at startup. `inherit_state`: take the state a previous
    /// version of this script saved (used by reloads).
    [[nodiscard]] std::unique_ptr<GameLogic> make_script_module(ScriptSource source, std::shared_ptr<ScriptLanguage> language = {}, bool inherit_state = false);

    /// Makes a module around an instance built by `factory` (a script that needs no language adapter: compiled code, or a C++ class).
    [[nodiscard]] std::unique_ptr<GameLogic> make_native_script_module(std::string name, std::function<ScriptValue<std::unique_ptr<ScriptInstance>>()> factory,
                                                                       bool inherit_state = false);

    /// Reloads a native script: replaces `name` with an instance from `factory`, handing state over like `ScriptHost::request_reload`.
    void request_native_script_reload(Engine &engine, std::string name, std::function<ScriptValue<std::unique_ptr<ScriptInstance>>()> factory,
                                      std::function<void(GameLogicResult)> done = {});

} // namespace SFT::Engine
