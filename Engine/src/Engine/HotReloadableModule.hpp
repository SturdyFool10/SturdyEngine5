#pragma once

#include <Foundation/Foundation.hpp>
#include <Reflection/Reflection.hpp>

#include <expected>
#include <string_view>

/// Marks a function as exported across the shared-library boundary a hot-reloadable module is
/// loaded through — `__declspec(dllexport)` on Windows, default ELF/Mach-O visibility elsewhere
/// (needed when the rest of the build uses hidden visibility by default).
#if defined(_WIN32)
#define STURDY_HOT_RELOAD_EXPORT extern "C" __declspec(dllexport)
#else
#define STURDY_HOT_RELOAD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace SFT::Engine {


    /// Function-pointer contract a hot-reloadable game-logic module exports (see
    /// `SFT_HOT_RELOAD_MODULE_EXPORTS` for the boilerplate that declares them from a module's own
    /// register/unregister/version functions):
    ///
    /// - `sturdy_module_register_types(TypeRegistry*)` — called once right after load. Registers
    ///   whatever `SFT_REFLECT_TYPE`s the module wants moddable/introspectable directly into the
    ///   registry pointer it's handed.
    /// - `sturdy_module_unregister_types(TypeRegistry*)` — called once right before unload (or
    ///   before a reload replaces it). Must `unregister_type` everything `register_types` added.
    /// - `sturdy_module_version()` — a module-defined integer, purely informational
    ///   (logging/diagnostics); never interpreted by `HotReloadableModule` itself.
    using ModuleRegisterTypesFn = void (*)(Reflection::TypeRegistry *registry);
    using ModuleUnregisterTypesFn = void (*)(Reflection::TypeRegistry *registry);
    using ModuleVersionFn = i32 (*)();

    enum class HotReloadErrorCode : u32 {
        /// `Foundation::DynamicLibrary::load` failed — see the error message for the OS's reason.
        LibraryLoadFailed,
        /// The library loaded, but is missing one of the three required exported entry points.
        MissingEntryPoint,
    };

    struct HotReloadError {
        HotReloadErrorCode code = HotReloadErrorCode::LibraryLoadFailed;
        UString message;
    };

    template <class Value>
    using HotReloadExpected = std::expected<Value, HotReloadError>;

    /// Owns a loaded, hot-reloadable shared library exposing the three entry points above, wired
    /// directly into a `Reflection::TypeRegistry` the caller supplies.
    ///
    /// **Why the registry is passed in explicitly, not resolved as a shared singleton inside the
    /// module**: a loaded module and the host executable are two separate binary images. Whether
    /// `Reflection::TypeRegistry::instance()`'s function-local `static` resolves to the *same*
    /// storage in both depends on whether `Reflection` itself was built as a shared library both
    /// link against dynamically (`-DSTURDY_BUILD_SHARED_LIBS=ON`). Under the default static build,
    /// each binary image gets its *own*, unconnected copy of the singleton — a module registering
    /// into "its own" `TypeRegistry::instance()` would be invisible to the host's. Handing the
    /// module `&TypeRegistry::instance()` explicitly from the host side sidesteps this: the module
    /// operates on the concrete object it was given, not on whatever its own copy of the singleton
    /// resolves to, so this is correct regardless of how `Reflection` was built. It does assume
    /// the host and the module were built against the same `TypeRegistry`/`TypeInfo`/etc. layout
    /// (the same headers, compiler, and ABI) — true for a same-repo rebuild-and-reload workflow,
    /// which is what this is for; it is not a general cross-language/cross-compiler FFI mechanism
    /// (that's `FFI/`'s job).
    class HotReloadableModule {
      public:
        HotReloadableModule() = default;
        ~HotReloadableModule() {
            unload();
        }

        HotReloadableModule(const HotReloadableModule &) = delete;
        HotReloadableModule &operator=(const HotReloadableModule &) = delete;
        HotReloadableModule(HotReloadableModule &&) = default;
        HotReloadableModule &operator=(HotReloadableModule &&) = default;

        /// Loads the library at `path`, resolves its three entry points, and calls
        /// `sturdy_module_register_types(&registry)` — load and initial registration happen
        /// together, so this type is never left in a "loaded but never registered" state (on
        /// failure partway through, the library is unloaded again before returning).
        ///
        /// @return Returns the value alternative on success; the error alternative describes why.
        [[nodiscard]] HotReloadExpected<void> load(std::string_view path, Reflection::TypeRegistry &registry);

        /// Calls `sturdy_module_unregister_types` against whichever registry `load`/`reload` was
        /// last called with (if a module is loaded), then unloads the library. Safe to call when
        /// nothing is loaded (a no-op).
        void unload();

        /// The actual hot-reload operation: unloads the currently-loaded module (unregistering
        /// its types from `registry` first), then loads `new_path` in its place and registers it
        /// against `registry`. The registry is never left with both the old and new module's
        /// types simultaneously, nor with neither — old unregisters before new registers, and if
        /// loading the new module fails, the old one stays unloaded rather than the reload
        /// silently keeping stale code active.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why
        /// the new module failed to load.
        [[nodiscard]] HotReloadExpected<void> reload(std::string_view new_path, Reflection::TypeRegistry &registry);

        /// Reports whether a module is currently loaded.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_loaded() const noexcept {
            return library_.is_loaded();
        }

        /// Returns the loaded module's self-reported version, or `-1` when nothing is loaded.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] i32 version() const noexcept {
            return (is_loaded() && version_fn_ != nullptr) ? version_fn_() : -1;
        }

      private:
        Foundation::DynamicLibrary library_;
        ModuleRegisterTypesFn register_types_fn_ = nullptr;
        ModuleUnregisterTypesFn unregister_types_fn_ = nullptr;
        ModuleVersionFn version_fn_ = nullptr;
        Reflection::TypeRegistry *registered_against_ = nullptr;
    };


} // namespace SFT::Engine

/// Declares the three `extern "C"` entry points `HotReloadableModule` looks for, forwarding to
/// `REGISTER_FN(SFT::Reflection::TypeRegistry &)`/`UNREGISTER_FN(SFT::Reflection::TypeRegistry &)`
/// (ordinary C++ functions the module author writes) and a fixed `VERSION` integer. Use once per
/// hot-reloadable module, at namespace scope in exactly one translation unit.
#define SFT_HOT_RELOAD_MODULE_EXPORTS(REGISTER_FN, UNREGISTER_FN, VERSION)                        \
    STURDY_HOT_RELOAD_EXPORT void sturdy_module_register_types(SFT::Reflection::TypeRegistry *registry) { \
        REGISTER_FN(*registry);                                                                    \
    }                                                                                               \
    STURDY_HOT_RELOAD_EXPORT void sturdy_module_unregister_types(SFT::Reflection::TypeRegistry *registry) { \
        UNREGISTER_FN(*registry);                                                                   \
    }                                                                                                \
    STURDY_HOT_RELOAD_EXPORT SFT::i32 sturdy_module_version() {                                     \
        return (VERSION);                                                                            \
    }
