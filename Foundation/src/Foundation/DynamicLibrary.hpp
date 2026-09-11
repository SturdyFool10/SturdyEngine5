#pragma once

#include <Foundation/Types.hpp>
#include <Foundation/UString.hpp>

#include <string_view>

namespace SFT::Foundation {


    /// RAII handle to a loaded shared library (`.dll`/`.so`/`.dylib`), opened through the OS
    /// loader (`LoadLibraryA`/`GetProcAddress`/`FreeLibrary` on Windows; `dlopen`/`dlsym`/
    /// `dlclose` elsewhere). Move-only — a loaded library has exactly one owner.
    ///
    /// This is the low-level primitive a hot-reloadable game-logic module is built on
    /// (`Engine::HotReloadableModule`): load, resolve exported entry points by name, unload when
    /// done — with no knowledge of what those entry points mean.
    class DynamicLibrary {
      public:
        DynamicLibrary() noexcept = default;
        ~DynamicLibrary() noexcept {
            unload();
        }

        DynamicLibrary(const DynamicLibrary &) = delete;
        DynamicLibrary &operator=(const DynamicLibrary &) = delete;

        DynamicLibrary(DynamicLibrary &&other) noexcept
            : handle_(other.handle_), last_error_(std::move(other.last_error_)) {
            other.handle_ = nullptr;
        }

        DynamicLibrary &operator=(DynamicLibrary &&other) noexcept {
            if (this != &other) {
                unload();
                handle_ = other.handle_;
                last_error_ = std::move(other.last_error_);
                other.handle_ = nullptr;
            }
            return *this;
        }

        /// Loads the shared library at `path` (a real filesystem path — callers wanting OS
        /// search-path resolution by bare module name should not rely on this; build a full path,
        /// e.g. via `platform_filename` joined with a known directory). Unloads any
        /// already-loaded library first.
        ///
        /// @return Returns `true` on success; `false` on failure — see `last_error()`.
        [[nodiscard]] bool load(std::string_view path) noexcept;

        /// Unloads the library, if loaded. Safe to call when not loaded (a no-op) and safe to
        /// call more than once.
        void unload() noexcept;

        /// Resolves `symbol_name` to a function/data address exported by the loaded library.
        ///
        /// @return Returns the resolved address, or `nullptr` when nothing is loaded or the
        /// symbol does not exist.
        /// @note This function does not throw exceptions.
        [[nodiscard]] void *symbol(const char *symbol_name) const noexcept;

        /// Resolves `symbol_name`, reinterpreting the result as `Fn` (typically a function
        /// pointer type) — the usual way to pull a typed entry point out of a loaded module.
        ///
        /// @return Returns the resolved address as `Fn`, or `nullptr` when unresolved.
        template <class Fn>
        [[nodiscard]] Fn symbol_as(const char *symbol_name) const noexcept {
            return reinterpret_cast<Fn>(symbol(symbol_name));
        }

        /// Reports whether a library is currently loaded.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_loaded() const noexcept {
            return handle_ != nullptr;
        }

        /// Same as `is_loaded`.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept {
            return is_loaded();
        }

        /// Returns the OS's description of the most recent `load` failure on this object, or an
        /// empty string when the last `load` succeeded (or none was attempted).
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] const UString &last_error() const noexcept {
            return last_error_;
        }

        /// Returns the platform-appropriate shared-library filename for `base_name` — e.g.
        /// `"MyModule"` becomes `"MyModule.dll"` on Windows, `"libMyModule.so"` on Linux/FreeBSD,
        /// `"libMyModule.dylib"` on MacOS — so callers building a reload path don't have to
        /// hand-write per-platform extensions/prefixes themselves.
        [[nodiscard]] static UString platform_filename(std::string_view base_name);

      private:
        void *handle_ = nullptr;
        UString last_error_;
    };


} // namespace SFT::Foundation
