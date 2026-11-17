// The Wayland window-effect code does not link libwayland-client; Linux/WaylandClientLoader.cpp defines the wl_* symbols it calls and
// binds them to the library at run time. This test drives that binding end to end without a compositor: it opens a Wayland "connection"
// over a socketpair, issues a request through the trampolines, and reads the wire bytes off the other end.
#include <cstdio>

#if defined(__linux__)

#include <WindowManager/Linux/WaylandClientLoader.hpp>

#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>

// Explicit checks rather than assert(): asserts compile to nothing under NDEBUG and would make this a no-op.
#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                                        \
        }                                                                                      \
    } while (false)

int main() {
    int failures = 0;

    if (!SFT::WindowManager::Detail::wayland_client_available()) {
        // A machine without libwayland-client: the effect code reports "unavailable" instead of failing to start. Nothing more to check.
        std::fprintf(stderr, "libwayland-client is not installed; loader correctly reports it unavailable\n");
        return 0;
    }

    // The core interfaces were copied from the library, not left zeroed.
    CHECK(wl_surface_interface.name != nullptr && std::strcmp(wl_surface_interface.name, "wl_surface") == 0);
    CHECK(wl_registry_interface.name != nullptr && std::strcmp(wl_registry_interface.name, "wl_registry") == 0);
    CHECK(wl_compositor_interface.method_count > 0);
    CHECK(wl_region_interface.version > 0);
    CHECK(wl_output_interface.event_count > 0);

    // A connection that talks to us instead of a compositor. wl_display_connect_to_fd is not one of the bound functions; take it from the
    // library directly.
    void *library = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    CHECK(library != nullptr);
    using ConnectToFd = wl_display *(*)(int);
    using Disconnect = void (*)(wl_display *);
    auto connect_to_fd = reinterpret_cast<ConnectToFd>(library != nullptr ? dlsym(library, "wl_display_connect_to_fd") : nullptr);
    auto disconnect = reinterpret_cast<Disconnect>(library != nullptr ? dlsym(library, "wl_display_disconnect") : nullptr);
    CHECK(connect_to_fd != nullptr && disconnect != nullptr);

    int sockets[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    if (connect_to_fd != nullptr && disconnect != nullptr && sockets[0] >= 0) {
        wl_display *display = connect_to_fd(sockets[0]);
        CHECK(display != nullptr);
        if (display != nullptr) {
            // Through the trampolines: queue create/destroy, a variadic request (get_registry is wl_proxy_marshal_flags with a new_id
            // argument), flush, listener and destroy.
            wl_event_queue *queue = wl_display_create_queue(display);
            CHECK(queue != nullptr);
            wl_registry *registry = wl_display_get_registry(display);
            CHECK(registry != nullptr);
            // wl_display.get_registry creates the registry at the display's own version, which is 0; the call going through
            // the trampoline and returning the proxy's real version is the point.
            CHECK(wl_proxy_get_version(reinterpret_cast<wl_proxy *>(registry)) == 0);
            CHECK(wl_display_flush(display) >= 0);

            // wl_display.get_registry on object 1: opcode 1, one new_id argument, 12 bytes (object, size<<16|opcode, new id 2).
            std::uint32_t message[3] = {};
            const ssize_t got = read(sockets[1], message, sizeof(message));
            CHECK(got == static_cast<ssize_t>(sizeof(message)));
            CHECK(message[0] == 1u);
            CHECK(message[1] == ((12u << 16) | 1u));
            CHECK(message[2] == 2u);

            wl_registry_destroy(registry);
            if (queue != nullptr) {
                wl_event_queue_destroy(queue);
            }
            disconnect(display);
        }
    }
    if (sockets[1] >= 0) {
        close(sockets[1]);
    }

    if (failures == 0) {
        std::fprintf(stderr, "WaylandClientLoaderTest passed\n");
    }
    return failures == 0 ? 0 : 1;
}

#else

int main() { return 0; }

#endif
