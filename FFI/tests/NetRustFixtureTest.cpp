/// Loads the `net_fixture` Rust library (Rust/examples/net_fixture: a self-test of the `sturdy-net` crate) and runs it against the real
/// engine. The library leaves the C ABI undefined, like a plugin, so this executable must export it: it calls two entry points itself so
/// the static archive pulls them in, and the build exports the executable's symbols.

#include <cstdio>
#include <string>

#include <dlfcn.h>

#include <FFI/Sturdy.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        (void)std::fprintf(stderr, "usage: NetRustFixtureTest <path to libnet_fixture>\n");
        return 2;
    }
    // Referenced so a static build links the whole async/net ABI unit (and the error slot) into this executable.
    uint32_t features = 0;
    if (sturdy_async_features(&features) != STURDY_OK || sturdy_last_error_message() == nullptr) return 3;
    // One function from each of the other translation units of the ABI (serving, file system / streams, synchronisation), for the same reason.
    static const void *volatile anchors[] = {reinterpret_cast<const void *>(&sturdy_http_server_create), reinterpret_cast<const void *>(&sturdy_fs_exists),
                                             reinterpret_cast<const void *>(&sturdy_channel_create), reinterpret_cast<const void *>(&sturdy_sync_mutex_create)};
    if (anchors[0] == nullptr || anchors[1] == nullptr || anchors[2] == nullptr || anchors[3] == nullptr) return 3;

    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        (void)std::fprintf(stderr, "NetRustFixtureTest: cannot load %s: %s\n", argv[1], dlerror());
        return 4;
    }
    using SelfTest = int (*)(char *, size_t);
    auto selftest = reinterpret_cast<SelfTest>(dlsym(library, "sturdy_net_selftest"));
    if (selftest == nullptr) {
        (void)std::fprintf(stderr, "NetRustFixtureTest: the library has no sturdy_net_selftest\n");
        return 5;
    }
    std::string message(4096, '\0');
    const int failures = selftest(message.data(), message.size());
    if (failures != 0) (void)std::fprintf(stderr, "NetRustFixtureTest: %d check(s) failed: %s\n", failures, message.c_str());
    return failures == 0 ? 0 : 1;
}
