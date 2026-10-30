#include <WindowManager/WindowEffects.hpp>

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>

// Explicit checks rather than assert(): asserts compile to nothing under NDEBUG and would make this a no-op.
#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                                        \
        }                                                                                      \
    } while (false)

// Drives the X11 CornerRadius route against a real X server (run under Xvfb) and reads the window's bounding shape
// back from the server. Libraries are loaded at run time, as the engine does, so the test needs no X11 link
// dependency; without a display or the libraries it reports a skip and passes.

using namespace SFT::WindowManager;

namespace {

    struct XRect {
        short x;
        short y;
        unsigned short width;
        unsigned short height;
    };

    template <typename Fn>
    Fn symbol(void *library, const char *name) {
        return reinterpret_cast<Fn>(dlsym(library, name));
    }

    std::set<std::pair<int, int>> covered(const XRect *rects, int count) {
        std::set<std::pair<int, int>> pixels;
        for (int i = 0; i < count; ++i) {
            for (int y = rects[i].y; y < rects[i].y + rects[i].height; ++y) {
                for (int x = rects[i].x; x < rects[i].x + rects[i].width; ++x) {
                    pixels.insert({x, y});
                }
            }
        }
        return pixels;
    }

} // namespace

int main() {
    int failures = 0;

    void *x11 = dlopen("libX11.so.6", RTLD_LAZY);
    void *xext = dlopen("libXext.so.6", RTLD_LAZY);
    if (x11 == nullptr || xext == nullptr || std::getenv("DISPLAY") == nullptr) {
        std::printf("WindowEffectX11CornerTest: skipped (needs libX11, libXext and a DISPLAY).\n");
        return 0;
    }
    auto open_display = symbol<void *(*)(const char *)>(x11, "XOpenDisplay");
    auto root_window = symbol<unsigned long (*)(void *)>(x11, "XDefaultRootWindow");
    auto create_window = symbol<unsigned long (*)(void *, unsigned long, int, int, unsigned, unsigned, unsigned, unsigned long,
                                                  unsigned long)>(x11, "XCreateSimpleWindow");
    auto sync = symbol<int (*)(void *, int)>(x11, "XSync");
    auto free_memory = symbol<int (*)(void *)>(x11, "XFree");
    auto get_rectangles = symbol<XRect *(*)(void *, unsigned long, int, int *, int *)>(xext, "XShapeGetRectangles");
    void *display = open_display != nullptr ? open_display(nullptr) : nullptr;
    if (display == nullptr || get_rectangles == nullptr) {
        std::printf("WindowEffectX11CornerTest: skipped (could not open the X display).\n");
        return 0;
    }

    constexpr unsigned width = 100;
    constexpr unsigned height = 60;
    const unsigned long window = create_window(display, root_window(display), 0, 0, width, height, 0, 0, 0);
    const NativeWindowHandle handle{NativeWindowSystem::X11, display, reinterpret_cast<void *>(window)};

    // A freshly created window is rectangular: one rectangle covering all of it.
    {
        int count = 0;
        int ordering = 0;
        XRect *rects = get_rectangles(display, window, 0, &count, &ordering);
        CHECK(rects != nullptr);
        CHECK(covered(rects, count).size() == width * height);
        free_memory(rects);
    }

    WindowEffect effect{WindowEffectKind::CornerRadius, true, 0, LinuxBlurProtocol::Automatic};
    effect.options.window_corner_radius = 10.0f;
    effect.options.window_width = width;
    effect.options.window_height = height;
    effect.options.pixel_scale = 1.0f;
    const WindowEffectResult applied = enable_native_window_effect(handle, effect);
    CHECK(applied.succeeded());
    sync(display, 0);

    {
        int count = 0;
        int ordering = 0;
        XRect *rects = get_rectangles(display, window, 0, &count, &ordering);
        CHECK(rects != nullptr);
        const auto pixels = covered(rects, count);
        CHECK(pixels.count({0, 0}) == 0);                  // the corner pixel is cut away
        CHECK(pixels.count({width - 1, height - 1}) == 0); // so is the opposite one
        CHECK(pixels.count({width / 2, 0}) == 1);          // straight edges are kept
        CHECK(pixels.count({0, height / 2}) == 1);
        CHECK(pixels.count({width / 2, height / 2}) == 1);
        CHECK(pixels.size() < width * height);
        free_memory(rects);
    }

    // Twice the pixel density: the same logical radius covers twice the device pixels.
    effect.options.window_width = width / 2;
    effect.options.window_height = height / 2;
    effect.options.pixel_scale = 2.0f;
    CHECK(enable_native_window_effect(handle, effect).succeeded());
    sync(display, 0);
    {
        int count = 0;
        int ordering = 0;
        XRect *rects = get_rectangles(display, window, 0, &count, &ordering);
        CHECK(rects != nullptr);
        const auto pixels = covered(rects, count);
        CHECK(pixels.count({0, 0}) == 0);
        CHECK(pixels.count({15, 0}) == 0); // a 20-device-pixel radius still cuts here
        CHECK(pixels.count({width / 2, 0}) == 1);
        free_memory(rects);
    }

    // Disabling restores the rectangle.
    effect.enabled = false;
    CHECK(enable_native_window_effect(handle, effect).succeeded());
    sync(display, 0);
    {
        int count = 0;
        int ordering = 0;
        XRect *rects = get_rectangles(display, window, 0, &count, &ordering);
        CHECK(rects != nullptr);
        CHECK(covered(rects, count).size() == width * height);
        free_memory(rects);
    }

    // The support query names the route.
    {
        const WindowEffectSupport support = query_native_window_effect_support(handle, WindowEffectKind::CornerRadius);
        CHECK(support.availability == EffectAvailability::Available);
    }

    if (failures == 0) {
        std::printf("WindowEffectX11CornerTest: passed.\n");
    }
    return failures == 0 ? 0 : 1;
}
