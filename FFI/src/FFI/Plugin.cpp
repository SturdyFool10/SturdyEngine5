/// C ABI implementation of native plugins (see "Native plugins" in Sturdy.h).
///
/// A plugin is a shared library with one exported entry point. This file owns the library, collects the modules the entry point
/// adds, and keeps the library open until the last module it handed to the engine has been destroyed. It is layered entirely on the
/// public module ABI (`sturdy_runtime_run_modules`, `sturdy_module_request_*`): the engine never learns that a module came from a
/// library, which is what lets a Rust, a C++ and a Zig plugin share one game with no special cases.
///
/// Handing a module out replaces its `user_data` with a record owned here and its callbacks with trampolines that forward to the
/// plugin's own. That costs one indirection per call and buys the one thing needed for safe unloading: the `destroy` trampoline knows
/// when the plugin's code can no longer be running.

#include <Foundation/DynamicLibrary.hpp>
#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <FFI/AbiSupport.hpp>
#include <FFI/Sturdy.h>

#if defined(_WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

using SFT::Ffi::guarded;
using SFT::Ffi::HandleKind;
using SFT::Ffi::set_error;

namespace {

    class Plugin;

    /// One module a plugin added, and the trampolines' view of it.
    struct ModuleRecord {
        std::string name;
        std::vector<std::string> after;
        std::vector<const char *> after_pointers;
        bool contributes_scene = true;
        SturdyGameLogic original{};
        Plugin *owner = nullptr;
        /// Set while the module is out with the engine: the plugin (hence its library) outlives the module's `destroy`.
        std::shared_ptr<Plugin> keepalive;
        bool handed_out = false;
    };

    SturdyBool STURDY_ABI_CALL trampoline_initialized(SturdyEngine engine, void *user_data);
    SturdyBool STURDY_ABI_CALL trampoline_frame(SturdyEngine engine, SturdySurface surface, const SturdyFrameInput *input,
                                                SturdyFrame frame, void *user_data);
    void STURDY_ABI_CALL trampoline_shutdown(SturdyEngine engine, void *user_data);
    void STURDY_ABI_CALL trampoline_destroy(void *user_data);

    class Plugin : public std::enable_shared_from_this<Plugin> {
      public:
        Plugin() = default;
        Plugin(const Plugin &) = delete;
        Plugin &operator=(const Plugin &) = delete;

        /// Runs `UNLOAD` and closes the library. By now every module handed out has been destroyed (that is what kept this alive),
        /// and the ones never handed out are destroyed first so `UNLOAD` really is after all of them.
        ~Plugin() {
            for (const auto &record : records) {
                if (!record->handed_out && record->original.destroy != nullptr) {
                    record->original.destroy(record->original.user_data);
                }
            }
            records.clear();
            if (entry != nullptr && loaded_ok) {
                accepting = false;
                const SturdyPluginHost host = make_host();
                entry(&host, STURDY_PLUGIN_UNLOAD, state);
            }
            library.unload();
            if (!shadow_path.empty()) {
                std::error_code ignored;
                std::filesystem::remove(shadow_path, ignored);
            }
        }

        [[nodiscard]] SturdyPluginHost make_host() noexcept {
            SturdyPluginHost host{};
            host.struct_size = static_cast<uint32_t>(sizeof(SturdyPluginHost));
            host.abi_major = STURDY_ABI_VERSION_MAJOR;
            host.abi_minor = STURDY_ABI_VERSION_MINOR;
            host.context = this;
            host.path = loaded_path.c_str();
            host.prior_state = prior_state;
            host.add_module = &Plugin::host_add_module;
            host.set_info = &Plugin::host_set_info;
            host.set_state = &Plugin::host_set_state;
            return host;
        }

        static SturdyResult STURDY_ABI_CALL host_add_module(void *context, const SturdyGameModule *module) {
            return guarded([&]() -> SturdyResult { return static_cast<Plugin *>(context)->add_module(module); });
        }

        static SturdyResult STURDY_ABI_CALL host_set_info(void *context, const char *language_text, const char *plugin_name,
                                                          const char *version_text) {
            return guarded([&]() -> SturdyResult {
                Plugin &self = *static_cast<Plugin *>(context);
                if (language_text != nullptr) {
                    self.language = language_text;
                }
                if (plugin_name != nullptr) {
                    self.name = plugin_name;
                }
                if (version_text != nullptr) {
                    self.version = version_text;
                }
                return STURDY_OK;
            });
        }

        static void STURDY_ABI_CALL host_set_state(void *context, void *new_state) {
            static_cast<Plugin *>(context)->state = new_state;
        }

        [[nodiscard]] SturdyResult add_module(const SturdyGameModule *module) {
            if (!accepting) {
                return set_error(STURDY_ERROR_NOT_AVAILABLE, "modules can only be added while the plugin's entry point handles STURDY_PLUGIN_LOAD");
            }
            if (module == nullptr) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, "module must not be null");
            }
            if (module->struct_size != sizeof(SturdyGameModule) || module->logic.struct_size != sizeof(SturdyGameLogic)) {
                return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyGameModule or SturdyGameLogic size does not match this engine build");
            }
            if (module->name == nullptr || module->name[0] == '\0') {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, "every game module needs a non-empty name");
            }
            if (module->after_count != 0 && module->after == nullptr) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, "after must not be null when after_count is nonzero");
            }
            for (uint32_t i = 0; i < module->after_count; ++i) {
                if (module->after[i] == nullptr) {
                    return set_error(STURDY_ERROR_INVALID_ARGUMENT, "after entries must not be null");
                }
                if (std::string_view{module->after[i]} == module->name) {
                    return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a game module cannot start after itself");
                }
            }
            if (find(module->name) != nullptr) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, "two modules of one plugin share a name");
            }

            auto record = std::make_unique<ModuleRecord>();
            record->name = module->name;
            for (uint32_t i = 0; i < module->after_count; ++i) {
                record->after.emplace_back(module->after[i]);
            }
            for (const std::string &dependency : record->after) {
                record->after_pointers.push_back(dependency.c_str());
            }
            record->contributes_scene = module->contributes_scene != STURDY_FALSE;
            record->original = module->logic;
            record->owner = this;
            records.push_back(std::move(record));
            return STURDY_OK;
        }

        [[nodiscard]] ModuleRecord *find(std::string_view module_name) const {
            for (const auto &record : records) {
                if (record->name == module_name) {
                    return record.get();
                }
            }
            return nullptr;
        }

        /// Builds the descriptor the engine consumes for `record` and ties the plugin's lifetime to it.
        [[nodiscard]] SturdyGameModule hand_out(ModuleRecord &record) {
            record.handed_out = true;
            record.keepalive = shared_from_this();
            SturdyGameModule module{};
            module.struct_size = static_cast<uint32_t>(sizeof(SturdyGameModule));
            module.after_count = static_cast<uint32_t>(record.after_pointers.size());
            module.name = record.name.c_str();
            module.after = record.after_pointers.empty() ? nullptr : record.after_pointers.data();
            module.contributes_scene = record.contributes_scene ? STURDY_TRUE : STURDY_FALSE;
            module.logic.struct_size = static_cast<uint32_t>(sizeof(SturdyGameLogic));
            module.logic.user_data = &record;
            module.logic.on_engine_initialized = record.original.on_engine_initialized != nullptr ? &trampoline_initialized : nullptr;
            module.logic.request_render_frame = record.original.request_render_frame != nullptr ? &trampoline_frame : nullptr;
            module.logic.on_shutdown = record.original.on_shutdown != nullptr ? &trampoline_shutdown : nullptr;
            module.logic.destroy = &trampoline_destroy;
            return module;
        }

        /// Hands out every module once; false when that already happened.
        [[nodiscard]] bool take_all(std::vector<SturdyGameModule> &out) {
            if (taken) {
                return false;
            }
            taken = true;
            for (const auto &record : records) {
                out.push_back(hand_out(*record));
            }
            return true;
        }

        SFT::Foundation::DynamicLibrary library;
        SturdyPluginEntryFn entry = nullptr;
        std::string loaded_path;
        std::filesystem::path shadow_path;
        std::string name;
        std::string language;
        std::string version;
        void *state = nullptr;
        void *prior_state = nullptr;
        bool accepting = false;
        bool loaded_ok = false;
        bool taken = false;
        std::vector<std::unique_ptr<ModuleRecord>> records;
    };

    SturdyBool STURDY_ABI_CALL trampoline_initialized(SturdyEngine engine, void *user_data) {
        const auto *record = static_cast<ModuleRecord *>(user_data);
        return record->original.on_engine_initialized(engine, record->original.user_data);
    }

    SturdyBool STURDY_ABI_CALL trampoline_frame(SturdyEngine engine, SturdySurface surface, const SturdyFrameInput *input,
                                                SturdyFrame frame, void *user_data) {
        const auto *record = static_cast<ModuleRecord *>(user_data);
        return record->original.request_render_frame(engine, surface, input, frame, record->original.user_data);
    }

    void STURDY_ABI_CALL trampoline_shutdown(SturdyEngine engine, void *user_data) {
        const auto *record = static_cast<ModuleRecord *>(user_data);
        record->original.on_shutdown(engine, record->original.user_data);
    }

    void STURDY_ABI_CALL trampoline_destroy(void *user_data) {
        auto *record = static_cast<ModuleRecord *>(user_data);
        // The plugin reference is dropped last and `record` is not touched after it: that may be the final reference, which
        // destroys the plugin, its records (this one included), runs UNLOAD and closes the library.
        std::shared_ptr<Plugin> keep = std::move(record->keepalive);
        if (record->original.destroy != nullptr) {
            record->original.destroy(record->original.user_data);
        }
        keep.reset();
    }

    std::mutex g_plugins_mutex;
    std::map<uint64_t, std::shared_ptr<Plugin>> g_plugins;

    [[nodiscard]] SturdyResult resolve_plugin(SturdyPlugin handle, std::shared_ptr<Plugin> *out) {
        void *pointer = nullptr;
        if (const SturdyResult resolved = SFT::Ffi::resolve_handle(handle.token, HandleKind::Plugin, &pointer); resolved != STURDY_OK) {
            return resolved;
        }
        const std::lock_guard<std::mutex> lock{g_plugins_mutex};
        const auto found = g_plugins.find(handle.token);
        if (found == g_plugins.end()) {
            return set_error(STURDY_ERROR_HANDLE_EXPIRED, "the plugin was released");
        }
        *out = found->second;
        return STURDY_OK;
    }

    /// Destroys every descriptor in `modules` from `first` on (their `destroy` trampolines), for a request that failed part way.
    void destroy_unconsumed(std::vector<SturdyGameModule> &modules, std::size_t first) {
        for (std::size_t i = first; i < modules.size(); ++i) {
            modules[i].logic.destroy(modules[i].logic.user_data);
        }
    }

    std::atomic<uint64_t> g_shadow_counter{0};

    /// A plugin leaves the engine's functions undefined, and the loader resolves them against the process's *global* symbol scope. A
    /// host that opened the engine library privately (Python's ctypes, the JVM, .NET and dlopen's default all do) would leave them
    /// unresolvable, so the engine library is promoted to the global scope once, before the first plugin opens. Already loaded, so this
    /// maps nothing new. Windows needs no such step: a plugin DLL imports the engine's functions by name from the library it links.
    void make_engine_symbols_global() {
#if !defined(_WIN32)
        static std::once_flag once;
        std::call_once(once, [] {
            Dl_info info{};
            if (dladdr(reinterpret_cast<void *>(&sturdy_plugin_load), &info) != 0 && info.dli_fname != nullptr) {
                (void)dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
            }
        });
#endif
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_plugin_load(const SturdyPluginLoadDesc *desc, SturdyPlugin *out_plugin) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_plugin == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and out_plugin must not be null");
        }
        if (desc->struct_size != sizeof(SturdyPluginLoadDesc)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyPluginLoadDesc size does not match this engine build");
        }
        if (desc->path == nullptr || desc->path[0] == '\0') {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "path must not be empty");
        }
        std::shared_ptr<Plugin> previous;
        if (desc->previous.token != 0) {
            if (const SturdyResult resolved = resolve_plugin(desc->previous, &previous); resolved != STURDY_OK) {
                return resolved;
            }
        }

        auto plugin = std::make_shared<Plugin>();
        const std::filesystem::path source{desc->path};
        plugin->name = source.stem().string();
        plugin->loaded_path = source.string();
        if ((desc->flags & STURDY_PLUGIN_SHADOW_COPY) != 0) {
            std::error_code error;
            const std::filesystem::path copy =
                std::filesystem::temp_directory_path(error) /
                ("sturdy_plugin_" + std::to_string(static_cast<long long>(getpid())) + "_" + std::to_string(g_shadow_counter++) + "_" +
                 source.filename().string());
            std::filesystem::copy_file(source, copy, std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                return set_error(STURDY_ERROR_NOT_AVAILABLE, "cannot make a shadow copy of the plugin: " + error.message());
            }
            plugin->shadow_path = copy;
            plugin->loaded_path = copy.string();
        }
        make_engine_symbols_global();
        if (!plugin->library.load(plugin->loaded_path)) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE,
                             "cannot open plugin '" + std::string{desc->path} + "': " + std::string{plugin->library.last_error().cpp_string_view()});
        }
        plugin->entry = plugin->library.symbol_as<SturdyPluginEntryFn>("sturdy_plugin_entry");
        if (plugin->entry == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "plugin '" + std::string{desc->path} + "' exports no sturdy_plugin_entry");
        }

        if (previous) {
            plugin->prior_state = previous->state;
            previous->state = nullptr; // ownership moves to the new generation (restored below if it refuses)
        }
        plugin->accepting = true;
        const SturdyPluginHost host = plugin->make_host();
        const SturdyResult entry_result = plugin->entry(&host, STURDY_PLUGIN_LOAD, nullptr);
        plugin->accepting = false;
        if (entry_result != STURDY_OK) {
            if (previous) {
                previous->state = plugin->prior_state;
            }
            // Modules already added are destroyed with the plugin; UNLOAD is not called for a load that was refused.
            plugin->entry = nullptr;
            return set_error(STURDY_ERROR_INITIALIZATION_FAILED, "plugin '" + std::string{desc->path} + "' refused to load");
        }
        plugin->loaded_ok = true;
        plugin->prior_state = nullptr;

        void *const pointer = plugin.get();
        const uint64_t token = SFT::Ffi::mint_handle(HandleKind::Plugin, pointer);
        {
            const std::lock_guard<std::mutex> lock{g_plugins_mutex};
            g_plugins.emplace(token, std::move(plugin));
        }
        out_plugin->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_plugin_release(SturdyPlugin handle) {
    return guarded([&]() -> SturdyResult {
        std::shared_ptr<Plugin> plugin;
        if (const SturdyResult resolved = resolve_plugin(handle, &plugin); resolved != STURDY_OK) {
            return resolved;
        }
        SFT::Ffi::revoke_handle(handle.token);
        {
            const std::lock_guard<std::mutex> lock{g_plugins_mutex};
            g_plugins.erase(handle.token);
        }
        // The last reference may be this one, which runs UNLOAD and closes the library; otherwise the last module's destroy does.
        plugin.reset();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_plugin_language(SturdyPlugin handle, char *buffer, size_t capacity, size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        std::shared_ptr<Plugin> plugin;
        if (const SturdyResult resolved = resolve_plugin(handle, &plugin); resolved != STURDY_OK) {
            return resolved;
        }
        return SFT::Ffi::copy_string_out(plugin->language, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_plugin_name(SturdyPlugin handle, char *buffer, size_t capacity, size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        std::shared_ptr<Plugin> plugin;
        if (const SturdyResult resolved = resolve_plugin(handle, &plugin); resolved != STURDY_OK) {
            return resolved;
        }
        return SFT::Ffi::copy_string_out(plugin->name, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_plugin_module_count(SturdyPlugin handle, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        std::shared_ptr<Plugin> plugin;
        if (const SturdyResult resolved = resolve_plugin(handle, &plugin); resolved != STURDY_OK) {
            return resolved;
        }
        *out_count = static_cast<uint32_t>(plugin->records.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_plugin_module_name(SturdyPlugin handle, uint32_t index, char *buffer, size_t capacity,
                                                       size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        std::shared_ptr<Plugin> plugin;
        if (const SturdyResult resolved = resolve_plugin(handle, &plugin); resolved != STURDY_OK) {
            return resolved;
        }
        if (index >= plugin->records.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "module index is out of range");
        }
        return SFT::Ffi::copy_string_out(plugin->records[index]->name, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_plugin_take_modules(SturdyPlugin handle, SturdyGameModule *out_modules, uint32_t capacity,
                                                        uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr || (capacity != 0 && out_modules == nullptr)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "out_count must not be null, and out_modules not when capacity is nonzero");
        }
        std::shared_ptr<Plugin> plugin;
        if (const SturdyResult resolved = resolve_plugin(handle, &plugin); resolved != STURDY_OK) {
            return resolved;
        }
        *out_count = static_cast<uint32_t>(plugin->records.size());
        if (plugin->taken) {
            return set_error(STURDY_ERROR_BUSY, "this plugin's modules were already handed out");
        }
        if (capacity < plugin->records.size()) {
            return set_error(STURDY_ERROR_BUFFER_TOO_SMALL, "out_modules is too small for the plugin's modules");
        }
        std::vector<SturdyGameModule> modules;
        (void)plugin->take_all(modules);
        std::copy(modules.begin(), modules.end(), out_modules);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_plugin_request_load(SturdyEngine engine, SturdyPlugin handle) {
    return guarded([&]() -> SturdyResult {
        std::shared_ptr<Plugin> plugin;
        if (const SturdyResult resolved = resolve_plugin(handle, &plugin); resolved != STURDY_OK) {
            return resolved;
        }
        std::vector<SturdyGameModule> modules;
        if (!plugin->take_all(modules)) {
            return set_error(STURDY_ERROR_BUSY, "this plugin's modules were already handed out");
        }
        for (std::size_t i = 0; i < modules.size(); ++i) {
            if (const SturdyResult requested = sturdy_module_request_load(engine, &modules[i]); requested != STURDY_OK) {
                destroy_unconsumed(modules, i); // a refused request took nothing, so these are still ours to destroy
                return requested;
            }
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_plugin_request_reload(SturdyEngine engine, SturdyPlugin previous_handle, SturdyPlugin next_handle) {
    return guarded([&]() -> SturdyResult {
        std::shared_ptr<Plugin> previous;
        std::shared_ptr<Plugin> next;
        if (const SturdyResult resolved = resolve_plugin(previous_handle, &previous); resolved != STURDY_OK) {
            return resolved;
        }
        if (const SturdyResult resolved = resolve_plugin(next_handle, &next); resolved != STURDY_OK) {
            return resolved;
        }
        if (previous == next) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "previous and next must be different plugins");
        }
        std::vector<SturdyGameModule> modules;
        if (!next->take_all(modules)) {
            return set_error(STURDY_ERROR_BUSY, "the new plugin's modules were already handed out");
        }
        for (std::size_t i = 0; i < modules.size(); ++i) {
            SturdyResult requested = STURDY_OK;
            if (previous->find(modules[i].name) != nullptr) {
                requested = sturdy_module_request_reload(engine, modules[i].name, &modules[i].logic);
            } else {
                requested = sturdy_module_request_load(engine, &modules[i]);
            }
            if (requested != STURDY_OK) {
                destroy_unconsumed(modules, i);
                return requested;
            }
        }
        for (const auto &record : previous->records) {
            if (next->find(record->name) == nullptr) {
                if (const SturdyResult requested = sturdy_module_request_unload(engine, record->name.c_str()); requested != STURDY_OK) {
                    return requested;
                }
            }
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_plugin_request_unload(SturdyEngine engine, SturdyPlugin handle) {
    return guarded([&]() -> SturdyResult {
        std::shared_ptr<Plugin> plugin;
        if (const SturdyResult resolved = resolve_plugin(handle, &plugin); resolved != STURDY_OK) {
            return resolved;
        }
        // Dependents first: a module that another of this plugin's modules starts after cannot be unloaded before it.
        for (auto it = plugin->records.rbegin(); it != plugin->records.rend(); ++it) {
            if (const SturdyResult requested = sturdy_module_request_unload(engine, (*it)->name.c_str()); requested != STURDY_OK) {
                return requested;
            }
        }
        return STURDY_OK;
    });
}

} // extern "C"
