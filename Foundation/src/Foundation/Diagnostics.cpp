#include <Foundation/Diagnostics.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>

#if defined(__EMSCRIPTEN__)
    #include <emscripten.h>
#elif defined(STURDY_PLATFORM_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <dbghelp.h>
    #pragma comment(lib, "dbghelp.lib")
#elif defined(__has_include)
    // glibc, Apple and the BSDs ship <execinfo.h>; musl does not, and neither do some embedded libcs. Anything without it simply
    // reports "no native backtrace" rather than failing to build.
    #if __has_include(<execinfo.h>) && !defined(__ANDROID__)
        #include <execinfo.h>
        #define STURDY_DIAG_HAVE_EXECINFO 1
    #endif
    #if __has_include(<dlfcn.h>)
        #include <dlfcn.h>
        #define STURDY_DIAG_HAVE_DLADDR 1
    #endif
    #if __has_include(<cxxabi.h>)
        #include <cxxabi.h>
        #define STURDY_DIAG_HAVE_CXXABI 1
    #endif
#endif

namespace SFT::Foundation {

    namespace {

        /// The calling thread's announced foreign site. Kept as raw owned strings so re-announcing an identical site allocates nothing.
        struct ForeignSiteSlot {
            CallSite site;
        };

        thread_local ForeignSiteSlot g_foreign_site;

        void assign_if_different(std::string &target, std::string_view value) {
            if (target.size() != value.size() || (!value.empty() && std::memcmp(target.data(), value.data(), value.size()) != 0)) {
                target.assign(value.data(), value.size());
            }
        }

    } // namespace

    bool Backtrace::supported() noexcept {
#if defined(__EMSCRIPTEN__) || defined(STURDY_PLATFORM_WINDOWS) || defined(STURDY_DIAG_HAVE_EXECINFO)
        return true;
#else
        return false;
#endif
    }

    Backtrace Backtrace::capture(usize skip_frames, usize max_frames) noexcept {
        Backtrace result;
        if (max_frames == 0) {
            return result;
        }
        // Allocation failure must not turn error reporting into a second error, so the whole body is guarded.
        try {
#if defined(__EMSCRIPTEN__)
            // WebAssembly has no addressable native stack; the host exposes the call stack as text instead. Needs -sASSERTIONS or
            // function names in the build to be informative, but never fails without them.
            char buffer[4096];
            const int written = emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_DEMANGLE | EM_LOG_NO_PATHS, buffer,
                                                         static_cast<int>(sizeof(buffer)));
            if (written > 0) {
                std::string_view all{buffer, std::strlen(buffer)};
                usize skipped = 0;
                while (!all.empty() && result.text_.size() < max_frames) {
                    const usize end = all.find('\n');
                    std::string_view line = all.substr(0, end);
                    all = end == std::string_view::npos ? std::string_view{} : all.substr(end + 1);
                    if (line.empty()) {
                        continue;
                    }
                    if (skipped < skip_frames + 1) {
                        ++skipped;
                        continue;
                    }
                    result.text_.emplace_back(line);
                }
            }
#elif defined(STURDY_PLATFORM_WINDOWS)
            void *raw[default_max_frames * 2];
            const usize cap = max_frames + skip_frames + 1 < std::size(raw) ? max_frames + skip_frames + 1 : std::size(raw);
            const USHORT count = CaptureStackBackTrace(0, static_cast<DWORD>(cap), raw, nullptr);
            for (usize i = skip_frames + 1; i < count && result.addresses_.size() < max_frames; ++i) {
                result.addresses_.push_back(static_cast<u64>(reinterpret_cast<std::uintptr_t>(raw[i])));
            }
#elif defined(STURDY_DIAG_HAVE_EXECINFO)
            void *raw[default_max_frames * 2];
            const usize cap = max_frames + skip_frames + 1 < std::size(raw) ? max_frames + skip_frames + 1 : std::size(raw);
            const int count = ::backtrace(raw, static_cast<int>(cap));
            for (int i = static_cast<int>(skip_frames) + 1; i < count && result.addresses_.size() < max_frames; ++i) {
                result.addresses_.push_back(static_cast<u64>(reinterpret_cast<std::uintptr_t>(raw[i])));
            }
#else
            (void)skip_frames;
#endif
        } catch (...) {
            result.addresses_.clear();
            result.text_.clear();
        }
        return result;
    }

    std::vector<BacktraceFrame> Backtrace::resolve() const noexcept {
        std::vector<BacktraceFrame> frames;
        try {
            frames.reserve(addresses_.size() + text_.size());
            for (const std::string &line : text_) {
                BacktraceFrame frame;
                frame.symbol = line;
                frames.push_back(std::move(frame));
            }
#if defined(STURDY_PLATFORM_WINDOWS)
            static std::mutex symbol_mutex;
            static bool symbols_ready = false;
            std::scoped_lock lock{symbol_mutex};
            const HANDLE process = GetCurrentProcess();
            if (!symbols_ready) {
                SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
                symbols_ready = SymInitialize(process, nullptr, TRUE) != FALSE;
            }
            for (const u64 address : addresses_) {
                BacktraceFrame frame;
                frame.address = address;
                HMODULE module = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(static_cast<std::uintptr_t>(address)), &module) &&
                    module != nullptr) {
                    char path[MAX_PATH] = {};
                    if (GetModuleFileNameA(module, path, MAX_PATH) != 0) {
                        frame.module = path;
                    }
                    frame.module_offset = address - static_cast<u64>(reinterpret_cast<std::uintptr_t>(module));
                }
                if (symbols_ready) {
                    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
                    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
                    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                    symbol->MaxNameLen = 255;
                    DWORD64 displacement = 0;
                    if (SymFromAddr(process, address, &displacement, symbol)) {
                        frame.symbol = symbol->Name;
                    }
                    IMAGEHLP_LINE64 line_info{};
                    line_info.SizeOfStruct = sizeof(line_info);
                    DWORD line_displacement = 0;
                    if (SymGetLineFromAddr64(process, address, &line_displacement, &line_info)) {
                        frame.file = line_info.FileName;
                        frame.line = line_info.LineNumber;
                    }
                }
                frames.push_back(std::move(frame));
            }
#else
            for (const u64 address : addresses_) {
                BacktraceFrame frame;
                frame.address = address;
#if defined(STURDY_DIAG_HAVE_DLADDR)
                Dl_info info{};
                // A return address points just past the call; look up the call instruction itself so a call that is the last
                // instruction of a function does not resolve to the function that happens to follow it.
                if (::dladdr(reinterpret_cast<void *>(static_cast<std::uintptr_t>(address > 0 ? address - 1 : address)), &info) != 0) {
                    if (info.dli_fname != nullptr) {
                        frame.module = info.dli_fname;
                    }
                    if (info.dli_fbase != nullptr) {
                        frame.module_offset = address - static_cast<u64>(reinterpret_cast<std::uintptr_t>(info.dli_fbase));
                    }
                    if (info.dli_sname != nullptr) {
                        frame.symbol = info.dli_sname;
#if defined(STURDY_DIAG_HAVE_CXXABI)
                        int status = 0;
                        char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                        if (status == 0 && demangled != nullptr) {
                            frame.symbol = demangled;
                        }
                        std::free(demangled);
#endif
                    }
                }
#endif
                frames.push_back(std::move(frame));
            }
#endif
        } catch (...) {
            // Resolution is best-effort: return what was built.
        }
        return frames;
    }

    std::string format_backtrace(const std::vector<BacktraceFrame> &frames) {
        std::string out;
        for (usize i = 0; i < frames.size(); ++i) {
            const BacktraceFrame &frame = frames[i];
            if (frame.address == 0 && !frame.symbol.empty()) {
                out += std::format("#{} {}\n", i, frame.symbol);
                continue;
            }
            out += std::format("#{} 0x{:x}", i, frame.address);
            if (!frame.module.empty()) {
                out += std::format(" {}+0x{:x}", frame.module, frame.module_offset);
            }
            if (!frame.symbol.empty()) {
                out += ' ';
                out += frame.symbol;
            }
            if (!frame.file.empty()) {
                out += std::format(" ({}:{})", frame.file, frame.line);
            }
            out += '\n';
        }
        return out;
    }

    std::string CallSite::to_string() const {
        std::string out;
        if (!language.empty()) {
            out += '[';
            out += language;
            out += "] ";
        }
        out += file.empty() ? std::string{"<unknown file>"} : file;
        if (line != 0) {
            out += std::format(":{}", line);
            if (column != 0) {
                out += std::format(":{}", column);
            }
        }
        if (!function.empty()) {
            out += std::format(" ({})", function);
        }
        return out;
    }

    CallSite call_site_from(const std::source_location &location) {
        CallSite site;
        site.language = "c++";
        site.file = location.file_name() != nullptr ? location.file_name() : "";
        site.line = location.line();
        site.column = location.column();
        site.function = location.function_name() != nullptr ? location.function_name() : "";
        return site;
    }

    void set_foreign_call_site(std::string_view language, std::string_view file, u32 line, u32 column,
                               std::string_view function) noexcept {
        try {
            CallSite &site = g_foreign_site.site;
            assign_if_different(site.language, language);
            assign_if_different(site.file, file);
            assign_if_different(site.function, function);
            site.line = line;
            site.column = column;
        } catch (...) {
            g_foreign_site.site = CallSite{};
        }
    }

    void clear_foreign_call_site() noexcept {
        g_foreign_site.site.language.clear();
        g_foreign_site.site.file.clear();
        g_foreign_site.site.function.clear();
        g_foreign_site.site.line = 0;
        g_foreign_site.site.column = 0;
    }

    CallSite foreign_call_site() {
        return g_foreign_site.site;
    }

} // namespace SFT::Foundation
