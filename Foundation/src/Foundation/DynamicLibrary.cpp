#include <Foundation/DynamicLibrary.hpp>

#include <string>

#if defined(STURDY_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace SFT::Foundation {

#if defined(STURDY_PLATFORM_WINDOWS)

    bool DynamicLibrary::load(std::string_view path) noexcept {
        unload();
        const std::string path_owned(path);
        HMODULE module = ::LoadLibraryA(path_owned.c_str());
        if (module == nullptr) {
            last_error_ = UString{"LoadLibraryA failed"};
            return false;
        }
        handle_ = static_cast<void *>(module);
        last_error_ = UString{};
        return true;
    }

    void DynamicLibrary::unload() noexcept {
        if (handle_ != nullptr) {
            ::FreeLibrary(static_cast<HMODULE>(handle_));
            handle_ = nullptr;
        }
    }

    void *DynamicLibrary::symbol(const char *symbol_name) const noexcept {
        if (handle_ == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<void *>(::GetProcAddress(static_cast<HMODULE>(handle_), symbol_name));
    }

    UString DynamicLibrary::platform_filename(std::string_view base_name) {
        return UString{std::string(base_name) + ".dll"};
    }

#else

    bool DynamicLibrary::load(std::string_view path) noexcept {
        unload();
        const std::string path_owned(path);
        (void)::dlerror(); // clear any prior error before probing
        void *handle = ::dlopen(path_owned.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            const char *error = ::dlerror();
            last_error_ = UString{error != nullptr ? std::string_view(error) : std::string_view("dlopen failed")};
            return false;
        }
        handle_ = handle;
        last_error_ = UString{};
        return true;
    }

    void DynamicLibrary::unload() noexcept {
        if (handle_ != nullptr) {
            ::dlclose(handle_);
            handle_ = nullptr;
        }
    }

    void *DynamicLibrary::symbol(const char *symbol_name) const noexcept {
        if (handle_ == nullptr) {
            return nullptr;
        }
        (void)::dlerror(); // clear any prior error before probing
        return ::dlsym(handle_, symbol_name);
    }

    UString DynamicLibrary::platform_filename(std::string_view base_name) {
#if defined(__APPLE__)
        return UString{"lib" + std::string(base_name) + ".dylib"};
#else
        return UString{"lib" + std::string(base_name) + ".so"};
#endif
    }

#endif

} // namespace SFT::Foundation
