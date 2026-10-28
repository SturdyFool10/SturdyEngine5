#include <Engine/Script.hpp>

#include <Engine/EngineModule.hpp>
#include <Foundation/Log.hpp>

#include <algorithm>
#include <deque>
#include <filesystem>

namespace SFT::Engine {

    namespace {
        GameLogicResult failure(std::string message) { return std::unexpected(GameLogicError{UString{message}}); }
        std::unexpected<GameLogicError> unexpected_error(std::string message) { return std::unexpected(GameLogicError{UString{message}}); }

        std::string to_std(const UString &text) { return std::string(std::string_view{text}); }

        std::mutex g_hosts_mutex;
        std::map<Engine *, std::unique_ptr<ScriptHost>> &hosts() {
            static auto *map = new std::map<Engine *, std::unique_ptr<ScriptHost>>(); // leaked: engines may outlive static destruction order
            return *map;
        }
    } // namespace

    // =================================================================================================================
    // Registry
    // =================================================================================================================
    ScriptRegistry &ScriptRegistry::global() {
        static ScriptRegistry *registry = new ScriptRegistry(); // leaked on purpose: adapters and host functions may be used during teardown
        return *registry;
    }

    bool ScriptRegistry::add_language(std::shared_ptr<ScriptLanguage> language) {
        if (!language) return false;
        const std::lock_guard lock{mutex_};
        return languages_.emplace(language->name(), std::move(language)).second;
    }
    bool ScriptRegistry::remove_language(std::string_view name) {
        std::shared_ptr<ScriptLanguage> removed;
        {
            const std::lock_guard lock{mutex_};
            const auto it = languages_.find(name);
            if (it == languages_.end()) return false;
            removed = std::move(it->second);
            languages_.erase(it);
        }
        return true; // `removed` is released outside the lock: an adapter's destructor may call back into the registry
    }
    std::shared_ptr<ScriptLanguage> ScriptRegistry::language(std::string_view name) const {
        const std::lock_guard lock{mutex_};
        const auto it = languages_.find(name);
        return it == languages_.end() ? nullptr : it->second;
    }
    std::shared_ptr<ScriptLanguage> ScriptRegistry::language_for_extension(std::string_view extension) const {
        const std::lock_guard lock{mutex_};
        for (const auto &[name, language] : languages_) {
            for (const std::string &candidate : language->extensions()) {
                if (candidate == extension) return language;
            }
        }
        return nullptr;
    }
    std::vector<std::string> ScriptRegistry::language_names() const {
        const std::lock_guard lock{mutex_};
        std::vector<std::string> names;
        for (const auto &[name, language] : languages_) names.push_back(name);
        return names;
    }

    bool ScriptRegistry::add_function(std::string name, ScriptHostFunction function) {
        if (name.empty() || !function) return false;
        const std::lock_guard lock{mutex_};
        return functions_.emplace(std::move(name), std::make_shared<ScriptHostFunction>(std::move(function))).second;
    }
    bool ScriptRegistry::remove_function(std::string_view name) {
        std::shared_ptr<ScriptHostFunction> removed;
        const std::lock_guard lock{mutex_};
        const auto it = functions_.find(name);
        if (it == functions_.end()) return false;
        removed = std::move(it->second);
        functions_.erase(it);
        return true;
    }
    ScriptValue<ScriptBytes> ScriptRegistry::call_function(std::string_view name, std::span<const std::byte> args) const {
        std::shared_ptr<ScriptHostFunction> function;
        {
            const std::lock_guard lock{mutex_};
            const auto it = functions_.find(name);
            if (it != functions_.end()) function = it->second;
        }
        if (!function) return unexpected_error("no host function '" + std::string(name) + "'");
        try {
            return (*function)(args);
        } catch (const std::exception &error) {
            return unexpected_error("host function '" + std::string(name) + "' threw: " + error.what());
        }
    }
    std::vector<std::string> ScriptRegistry::function_names() const {
        const std::lock_guard lock{mutex_};
        std::vector<std::string> names;
        for (const auto &[name, function] : functions_) names.push_back(name);
        return names;
    }

    // =================================================================================================================
    // Host
    // =================================================================================================================
    struct ScriptHost::Entry {
        std::string name;
        std::string language;
        std::string path;
        std::uint64_t generation = 1;
        bool healthy = true;
        std::string failure;
        std::unique_ptr<ScriptInstance> instance;
        std::set<std::string> topics;
    };

    struct ScriptHost::State {
        struct Queued {
            std::string from;
            std::string to;
            std::string topic;
            ScriptBytes payload;
        };

        /// The script lock: held for every script callback, recursive so a script may call another (or itself) from inside a callback.
        mutable std::recursive_mutex gil;
        std::vector<std::shared_ptr<Entry>> entries;
        std::mutex queue_mutex;
        std::deque<Queued> queue;
        std::map<std::string, ScriptBytes> stash;
        std::map<std::string, std::uint64_t> generations;
        Engine::UpdateHookId hook = 0;
        double elapsed = 0;
        std::uint64_t tick = 0;
    };

    ScriptHost::ScriptHost(Engine &) : state_(std::make_unique<State>()) {}
    ScriptHost::~ScriptHost() = default;

    ScriptHost &ScriptHost::of(Engine &engine) {
        const std::lock_guard lock{g_hosts_mutex};
        auto &slot = hosts()[&engine];
        if (!slot) slot.reset(new ScriptHost(engine));
        return *slot;
    }

    ScriptHost *ScriptHost::find(Engine &engine) noexcept {
        const std::lock_guard lock{g_hosts_mutex};
        const auto it = hosts().find(&engine);
        return it == hosts().end() ? nullptr : it->second.get();
    }

    namespace {
        /// Runs a script callback; an exception marks the script unhealthy instead of unwinding into the engine.
        template <typename Fn>
        bool guarded(ScriptHost::Entry &entry, const char *what, Fn &&fn) {
            try {
                fn();
                return true;
            } catch (const std::exception &error) {
                entry.healthy = false;
                entry.failure = std::string(what) + ": " + error.what();
            } catch (...) {
                entry.healthy = false;
                entry.failure = std::string(what) + ": unknown exception";
            }
            Foundation::log_error("script '{}' failed in {}", entry.name, entry.failure);
            return false;
        }
    } // namespace

    std::vector<ScriptInfo> ScriptHost::scripts() const {
        const std::lock_guard lock{state_->gil};
        std::vector<ScriptInfo> out;
        for (const auto &entry : state_->entries) out.push_back({entry->name, entry->language, entry->path, entry->generation, entry->healthy, entry->failure});
        return out;
    }

    std::optional<ScriptInfo> ScriptHost::info(std::string_view script) const {
        for (ScriptInfo &item : scripts()) {
            if (item.name == script) return std::move(item);
        }
        return std::nullopt;
    }

    ScriptValue<ScriptBytes> ScriptHost::call(std::string_view script, std::string_view function, std::span<const std::byte> args) {
        const std::lock_guard lock{state_->gil};
        for (const auto &entry : state_->entries) {
            if (entry->name != script) continue;
            if (!entry->instance) break;
            try {
                return entry->instance->call(function, args);
            } catch (const std::exception &error) {
                return unexpected_error("script '" + std::string(script) + "' threw in '" + std::string(function) + "': " + error.what());
            }
        }
        return unexpected_error("no script named '" + std::string(script) + "'");
    }

    bool ScriptHost::send(std::string from, std::string to, std::string topic, ScriptBytes payload) {
        {
            const std::lock_guard lock{state_->gil};
            const bool exists = std::any_of(state_->entries.begin(), state_->entries.end(), [&](const auto &e) { return e->name == to; });
            if (!exists) return false;
        }
        const std::lock_guard lock{state_->queue_mutex};
        state_->queue.push_back({std::move(from), std::move(to), std::move(topic), std::move(payload)});
        return true;
    }

    std::size_t ScriptHost::publish(std::string from, std::string topic, ScriptBytes payload) {
        std::vector<std::string> targets;
        {
            const std::lock_guard lock{state_->gil};
            for (const auto &entry : state_->entries) {
                if (entry->name != from && entry->topics.contains(topic)) targets.push_back(entry->name);
            }
        }
        const std::lock_guard lock{state_->queue_mutex};
        for (std::string &target : targets) state_->queue.push_back({from, std::move(target), topic, payload});
        return targets.size();
    }

    bool ScriptHost::subscribe(std::string script, std::string topic) {
        const std::lock_guard lock{state_->gil};
        for (const auto &entry : state_->entries) {
            if (entry->name == script) {
                entry->topics.insert(std::move(topic));
                return true;
            }
        }
        return false;
    }

    bool ScriptHost::unsubscribe(std::string_view script, std::string_view topic) {
        const std::lock_guard lock{state_->gil};
        for (const auto &entry : state_->entries) {
            if (entry->name == script) return entry->topics.erase(std::string(topic)) > 0;
        }
        return false;
    }

    void ScriptHost::flush_messages() {
        const std::lock_guard lock{state_->gil};
        // Messages sent while delivering wait for the next flush, so a pair of scripts answering each other cannot spin one update forever.
        std::deque<State::Queued> batch;
        {
            const std::lock_guard queue_lock{state_->queue_mutex};
            batch.swap(state_->queue);
        }
        for (State::Queued &message : batch) {
            for (const auto &entry : state_->entries) {
                if (entry->name != message.to || !entry->healthy || !entry->instance) continue;
                const ScriptMessage view{message.from, message.topic, message.payload};
                guarded(*entry, "on_message", [&] { entry->instance->on_message(view); });
                break;
            }
        }
    }

    // =================================================================================================================
    // Modules
    // =================================================================================================================
    class ScriptModule final : public GameLogic {
      public:
        struct Meta {
            std::string name;
            std::string language;
            std::string path;
            /// Filled in by the factory when the language is only known once it resolves one (from the path's extension).
            std::shared_ptr<std::string> resolved_language;
        };
        using Factory = std::function<ScriptValue<std::unique_ptr<ScriptInstance>>()>;

        ScriptModule(Meta meta, Factory factory, bool inherit_state) : meta_(std::move(meta)), factory_(std::move(factory)), inherit_(inherit_state) {}

        [[nodiscard]] GameLogicResult on_engine_initialized(Engine &engine) override {
            ScriptHost &host = ScriptHost::of(engine);
            host_ = &host;
            ScriptHost::State &state = *host.state_;
            const std::lock_guard lock{state.gil};

            auto made = factory_();
            if (!made) return failure("script '" + meta_.name + "': " + to_std(made.error().message));
            auto entry = std::make_shared<ScriptHost::Entry>();
            entry->name = meta_.name;
            entry->language = meta_.resolved_language && !meta_.resolved_language->empty() ? *meta_.resolved_language : meta_.language;
            entry->path = meta_.path;
            entry->instance = std::move(*made);

            std::optional<ScriptBytes> inherited;
            if (inherit_) {
                if (const auto it = state.stash.find(meta_.name); it != state.stash.end()) {
                    inherited = std::move(it->second);
                    state.stash.erase(it);
                }
                entry->generation = state.generations[meta_.name] + 1;
            } else {
                state.stash.erase(meta_.name);
            }
            state.generations[meta_.name] = entry->generation;

            if (inherited) {
                bool accepted = false;
                if (!guarded(*entry, "restore_state", [&] { accepted = entry->instance->restore_state(*inherited); }) || !accepted) {
                    return failure("script '" + meta_.name + "': the new version refused the previous version's state" + (entry->failure.empty() ? "" : " (" + entry->failure + ")"));
                }
            }
            GameLogicResult started;
            if (!guarded(*entry, "start", [&] { started = entry->instance->start(engine); })) return failure("script '" + meta_.name + "': " + entry->failure);
            if (!started) return failure("script '" + meta_.name + "': " + to_std(started.error().message));

            entry_ = entry;
            state.entries.push_back(entry);
            if (state.hook == 0) {
                state.hook = engine.add_update_hook([host_ptr = &host, engine_ptr = &engine](f64 delta) { host_ptr->run_update(*engine_ptr, delta); });
            }
            return {};
        }

        [[nodiscard]] std::optional<RenderFrameParameters> request_render_frame(Engine &engine, Core::RenderSurfaceHandle surface, const Core::FrameInput &frame) override {
            if (host_ == nullptr || !entry_) return std::nullopt;
            const std::lock_guard lock{host_->state_->gil};
            if (!entry_->instance || !entry_->healthy) return std::nullopt;
            std::optional<RenderFrameParameters> result;
            guarded(*entry_, "request_render_frame", [&] { result = entry_->instance->request_render_frame(engine, surface, frame); });
            return result;
        }

        void on_shutdown(Engine &engine) noexcept override {
            if (host_ == nullptr || !entry_) return;
            ScriptHost *host = host_;
            {
                const std::lock_guard lock{host->state_->gil};
                auto &entries = host->state_->entries;
                entries.erase(std::remove(entries.begin(), entries.end(), entry_), entries.end());
                if (entry_->instance) {
                    std::optional<ScriptBytes> saved;
                    guarded(*entry_, "save_state", [&] { saved = entry_->instance->save_state(); });
                    if (saved) host->state_->stash[meta_.name] = std::move(*saved);
                    guarded(*entry_, "stop", [&] { entry_->instance->stop(engine); });
                    entry_->instance.reset();
                }
                if (entries.empty() && host->state_->hook != 0) {
                    (void)engine.remove_update_hook(host->state_->hook);
                    host->state_->hook = 0;
                }
            }
            entry_.reset();
            host_ = nullptr;
            host->release_if_idle(engine);
        }

      private:
        Meta meta_;
        Factory factory_;
        bool inherit_;
        ScriptHost *host_ = nullptr;
        std::shared_ptr<ScriptHost::Entry> entry_;
    };

    void ScriptHost::run_update(Engine &engine, double delta_seconds) {
        const std::lock_guard lock{state_->gil};
        ++state_->tick;
        state_->elapsed += delta_seconds;
        flush_messages();
        const ScriptTick tick{delta_seconds, state_->elapsed, state_->tick};
        const std::vector<std::shared_ptr<Entry>> snapshot = state_->entries;
        for (const auto &entry : snapshot) {
            if (!entry->healthy || !entry->instance) continue;
            guarded(*entry, "update", [&] { entry->instance->update(engine, tick); });
        }
    }

    void ScriptHost::release_if_idle(Engine &engine) {
        const std::lock_guard lock{g_hosts_mutex};
        const auto it = hosts().find(&engine);
        if (it == hosts().end() || it->second.get() != this) return;
        {
            const std::lock_guard gil{state_->gil};
            if (!state_->entries.empty()) return;
        }
        hosts().erase(it); // destroys *this: the caller must not touch it afterwards
    }

    namespace {
        ScriptValue<std::pair<std::shared_ptr<ScriptLanguage>, ScriptSource>> resolve_language(ScriptSource source, std::shared_ptr<ScriptLanguage> language) {
            if (!language && !source.language.empty()) language = ScriptRegistry::global().language(source.language);
            if (!language && source.language.empty() && !source.path.empty()) {
                language = ScriptRegistry::global().language_for_extension(std::filesystem::path(source.path).extension().string());
            }
            if (!language) {
                return unexpected_error("script '" + source.name + "': no language adapter for " +
                                        (source.language.empty() ? "path '" + source.path + "'" : "'" + source.language + "'") + " is registered");
            }
            if (source.language.empty()) source.language = language->name();
            return std::make_pair(std::move(language), std::move(source));
        }
    } // namespace

    std::unique_ptr<GameLogic> make_script_module(ScriptSource source, std::shared_ptr<ScriptLanguage> language, bool inherit_state) {
        ScriptModule::Meta meta{source.name, source.language, source.path, std::make_shared<std::string>()};
        // The language is resolved when the game starts, so an adapter registered after the module was built (but before the runtime runs) works.
        auto factory = [source = std::move(source), language = std::move(language), resolved_language = meta.resolved_language]() mutable -> ScriptValue<std::unique_ptr<ScriptInstance>> {
            auto resolved = resolve_language(source, language);
            if (!resolved) return std::unexpected(resolved.error());
            *resolved_language = resolved->second.language;
            return resolved->first->load(resolved->second);
        };
        return std::make_unique<ScriptModule>(std::move(meta), std::move(factory), inherit_state);
    }

    std::unique_ptr<GameLogic> make_native_script_module(std::string name, std::function<ScriptValue<std::unique_ptr<ScriptInstance>>()> factory, bool inherit_state) {
        return std::make_unique<ScriptModule>(ScriptModule::Meta{std::move(name), "native", {}, nullptr}, std::move(factory), inherit_state);
    }

    void request_native_script_reload(Engine &engine, std::string name, std::function<ScriptValue<std::unique_ptr<ScriptInstance>>()> factory,
                                      std::function<void(GameLogicResult)> done) {
        CompositeGameLogic *composite = CompositeGameLogic::of(engine);
        if (composite == nullptr) {
            if (done) done(failure("scripts can only be reloaded in a running game made of modules"));
            return;
        }
        composite->request_reload(engine, name, make_native_script_module(name, std::move(factory), true), std::move(done));
    }

    void ScriptHost::request_reload(Engine &engine, ScriptSource source, std::function<void(GameLogicResult)> done) {
        CompositeGameLogic *composite = CompositeGameLogic::of(engine);
        if (composite == nullptr) {
            if (done) done(failure("scripts can only be reloaded in a running game made of modules"));
            return;
        }
        // Load the new version first: if it does not parse, the old one is never disturbed.
        std::unique_ptr<ScriptInstance> loaded;
        {
            const std::lock_guard lock{state_->gil};
            auto resolved = resolve_language(source, nullptr);
            if (!resolved) {
                if (done) done(std::unexpected(resolved.error()));
                return;
            }
            source = std::move(resolved->second);
            auto made = resolved->first->load(source);
            if (!made) {
                if (done) done(failure("script '" + source.name + "': " + to_std(made.error().message)));
                return;
            }
            loaded = std::move(*made);
        }
        auto holder = std::make_shared<std::unique_ptr<ScriptInstance>>(std::move(loaded));
        ScriptModule::Meta meta{source.name, source.language, source.path, nullptr};
        auto module = std::make_unique<ScriptModule>(
            std::move(meta), [holder]() -> ScriptValue<std::unique_ptr<ScriptInstance>> { return std::move(*holder); }, true);
        composite->request_reload(engine, source.name, std::move(module), std::move(done));
    }

} // namespace SFT::Engine
