// Run-time binding of libwayland-client.
//
// The Wayland effect code (WaylandBackgroundEffects.cpp, WaylandColorManagement.cpp) and the wayland-scanner generated protocol sources
// call `wl_*` functions and reference the core `wl_*_interface` objects as ordinary link-time symbols. Linking libwayland-client for
// them would put it in the executable's NEEDED list, and a machine without it (X11 only, headless, minimal containers) could not start
// the program at all. Instead this file *defines* those symbols itself, hidden so nothing else sees them:
//   * each function is a two-instruction trampoline that jumps through a pointer filled in by `wayland_client_available()`. A trampoline
//     (not a C++ forwarding function) because wl_proxy_marshal_flags is variadic: a jump leaves the caller's arguments untouched;
//   * each core interface is a weak `wl_interface` object whose contents are copied from the library's own once it is loaded (unless the
//     link already holds SDL3's definition, which is complete). The scanner's
//     generated code takes their addresses in static initialisers, so the symbols have to exist at link time.
#include <WindowManager/Linux/WaylandClientLoader.hpp>

#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <wayland-util.h>

#if !defined(__linux__) || !(defined(__x86_64__) || defined(__aarch64__))
#error "WaylandClientLoader.cpp has trampolines for x86-64 and AArch64 Linux only"
#endif

#define STURDY_WL_FUNCTIONS(X)           \
    X(wl_display_create_queue)           \
    X(wl_display_dispatch_queue_pending) \
    X(wl_display_flush)                  \
    X(wl_display_roundtrip_queue)        \
    X(wl_event_queue_destroy)            \
    X(wl_proxy_add_listener)             \
    X(wl_proxy_create_wrapper)           \
    X(wl_proxy_destroy)                  \
    X(wl_proxy_get_version)              \
    X(wl_proxy_marshal_flags)            \
    X(wl_proxy_set_queue)                \
    X(wl_proxy_wrapper_destroy)

#define STURDY_WL_INTERFACES(X) \
    X(wl_compositor_interface)  \
    X(wl_output_interface)      \
    X(wl_region_interface)      \
    X(wl_registry_interface)    \
    X(wl_surface_interface)

#define STURDY_HIDDEN __attribute__((visibility("hidden")))

extern "C" {

// The slots the trampolines jump through.
#define STURDY_WL_SLOT(name) STURDY_HIDDEN void *sturdy_slot_##name = nullptr;
STURDY_WL_FUNCTIONS(STURDY_WL_SLOT)
#undef STURDY_WL_SLOT

// The core interfaces, filled from the loaded library.
// Weak: SDL3's static library carries its own definitions of these (its generated wayland-protocol.c), and a strong pair would be a
// duplicate-symbol link error. Where SDL3's are linked they win and are already filled in; otherwise these are used.
#define STURDY_WL_INTERFACE_OBJECT(name) STURDY_HIDDEN __attribute__((weak)) wl_interface name;
STURDY_WL_INTERFACES(STURDY_WL_INTERFACE_OBJECT)
#undef STURDY_WL_INTERFACE_OBJECT

} // extern "C"

#if defined(__x86_64__)
#define STURDY_WL_TRAMPOLINE(name)                                                                                    \
    __asm__(".text\n"                                                                                                 \
            ".globl " #name "\n.hidden " #name "\n.type " #name ",@function\n" #name ":\n"                            \
            "\tjmp *sturdy_slot_" #name "(%rip)\n"                                                                    \
            ".size " #name ", .-" #name "\n");
#else // __aarch64__
#define STURDY_WL_TRAMPOLINE(name)                                                                                    \
    __asm__(".text\n"                                                                                                 \
            ".globl " #name "\n.hidden " #name "\n.type " #name ",%function\n" #name ":\n"                            \
            "\tadrp x16, sturdy_slot_" #name "\n"                                                                     \
            "\tldr x16, [x16, :lo12:sturdy_slot_" #name "]\n"                                                         \
            "\tbr x16\n"                                                                                              \
            ".size " #name ", .-" #name "\n");
#endif
STURDY_WL_FUNCTIONS(STURDY_WL_TRAMPOLINE)
#undef STURDY_WL_TRAMPOLINE

namespace SFT::WindowManager::Detail {

    namespace {
        std::once_flag load_once;
        std::atomic<bool> loaded{false};

        bool bind_everything() noexcept {
            // RTLD_LOCAL: nothing else in the process should resolve against this handle. Under a Wayland window SDL3 already holds the
            // library, so this only adds a reference.
            void *library = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
            if (library == nullptr) {
                return false;
            }
            bool complete = true;
#define STURDY_WL_BIND(name)                                \
    if (void *symbol = dlsym(library, #name)) {             \
        sturdy_slot_##name = symbol;                        \
    } else {                                                \
        complete = false;                                   \
    }
            STURDY_WL_FUNCTIONS(STURDY_WL_BIND)
#undef STURDY_WL_BIND
#define STURDY_WL_COPY(symbol_name)                                                \
    if (void *symbol = dlsym(library, #symbol_name)) {                             \
        if (symbol_name.name == nullptr) { /* ours, still empty: fill; SDL3's are initialised (and read-only) */ \
            std::memcpy(static_cast<void *>(&symbol_name), symbol, sizeof(wl_interface)); \
        }                                                                   \
    } else {                                                                \
        complete = false;                                                   \
    }
            STURDY_WL_INTERFACES(STURDY_WL_COPY)
#undef STURDY_WL_COPY
            return complete;
        }
    } // namespace

    bool wayland_client_available() noexcept {
        std::call_once(load_once, [] { loaded.store(bind_everything(), std::memory_order_release); });
        return loaded.load(std::memory_order_acquire);
    }

} // namespace SFT::WindowManager::Detail
