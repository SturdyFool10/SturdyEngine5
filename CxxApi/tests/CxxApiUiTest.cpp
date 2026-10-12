// The CxxApi UI surface without a GPU: every call that needs an open UI frame reports that none is open, opening one needs a graphics device,
// and a font that cannot be read is reported. The drawing path itself needs a device; it runs live in FfiNativeHandleProbe, which goes
// through the same shared layer (`Engine/ForeignUi.hpp`).

#include <CxxApi/CxxApi.hpp>
#include <CxxApi/Error.hpp>

#include <cstdio>
#include <string>

using namespace SFT;
using namespace SFT::CxxApi;

namespace {

    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }

    /// The message of the `Error` the call throws, or empty when it does not throw.
    template <typename F> std::string error_of(F &&call) {
        try {
            call();
        } catch (const Error &e) {
            return e.what();
        }
        return {};
    }

    bool mentions(const std::string &message, const char *needle) { return message.find(needle) != std::string::npos; }

} // namespace

int main() {
    Engine::Engine engine;
    Engine::RenderFrameParameters parameters;
    FrameBuilder frame{engine, Core::RenderSurfaceHandle{}, FrameInfo{.framebuffer_width = 640, .framebuffer_height = 360}, parameters};

    const UiElement element{};
    check(mentions(error_of([&] { ui_begin_element(element, "panel"); }), "no UI frame is open"), "an element needs an open frame");
    check(mentions(error_of([&] { ui_end_element(); }), "no UI frame is open"), "ending an element needs an open frame");
    check(mentions(error_of([&] { ui_text("hello", UiTextStyle{}); }), "no UI frame is open"), "text needs an open frame");
    check(mentions(error_of([&] { (void)ui_hovered("panel"); }), "no UI frame is open"), "hover needs an open frame");
    check(mentions(error_of([&] { (void)ui_clicked("panel"); }), "no UI frame is open"), "click needs an open frame");
    check(mentions(error_of([&] { (void)ui_pointer_position(); }), "no UI frame is open"), "the pointer position needs an open frame");
    check(mentions(error_of([&] { (void)ui_pointer_down(); }), "no UI frame is open"), "the pointer state needs an open frame");
    check(mentions(error_of([&] { ui_end(frame); }), "no UI frame is open"), "ending a frame that was never begun is reported");
    check(mentions(error_of([&] { ui_end_discard(frame); }), "no UI frame is open"), "so is discarding one");

    check(mentions(error_of([&] { ui_begin(frame); }), "RHI device"), "opening a frame needs a graphics device");
    check(parameters.overlay_passes.empty(), "a failed begin attaches nothing");

    check(mentions(error_of([&] { ui_register_font(engine, "does/not/exist.ttf", 1); }), "could not read the font file"),
          "a font file that cannot be read is reported");

    // Plain-data defaults are what a binding's `Default` should produce.
    const UiTextStyle style{};
    check(style.font_size == 16 && style.color[0] == 1.0f && style.color[3] == 1.0f, "the default text style is 16 px opaque white");
    check(element.width_kind == UiSizing::Fit && element.direction == UiDirection::LeftToRight && element.background[3] == 0.0f,
          "the default element fits its content, flows left to right and draws no background");

    if (failures != 0) {
        std::fprintf(stderr, "CxxApiUiTest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("CxxApiUiTest: ok\n");
    return 0;
}
