// Image fit (CSS `object-fit` / `object-position`), CPU side: the quad an image becomes has the position, size and uv range its fit mode
// calls for. No GPU is involved.

#include <Renderer/UI/UI.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#undef assert
#define assert(condition)               \
    ((condition) ? static_cast<void>(0) \
                 : (std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition), std::abort()))

namespace {

    using namespace SFT;
    using SFT::UI::Context;
    using SFT::UI::ImageFit;

    bool near(f32 a, f32 b) { return std::abs(a - b) < 0.01f; }

    /// Lays out one 100x50 box holding an image of `image_size` with `fit`, and returns the image's quad.
    UI::UiQuadInstance quad_for(Context &ui, ImageFit fit, glm::vec2 image_size, glm::vec2 position = {0.5f, 0.5f}) {
        ui.begin_layout(glm::vec2{400.0f, 300.0f}, UI::PointerState{}, 1.0f / 60.0f);
        {
            auto root = ui.element(UI::ElementDecl{});
            ui.image(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(100.0f), UI::SizingAxis::fixed(50.0f)},
                                      .background_color = UI::Color{1.0, 1.0, 1.0, 1.0}},
                     Renderer::TextureHandle{.value = 1},
                     UI::ImageFitOptions{.fit = fit, .position = position, .intrinsic_size = image_size});
        }
        const UI::FrameSnapshot snapshot = ui.finish_frame();
        assert(!snapshot.quads().empty());
        return snapshot.quads().back().instance;
    }

} // namespace

int main() {
    Context::Config config{};
    config.load_default_font = false;
    auto created = Context::create(config);
    assert(created.has_value());
    Context &ui = *created;

    // Fill stretches to the box, whatever the image.
    auto quad = quad_for(ui, ImageFit::Fill, {40.0f, 40.0f});
    assert(near(quad.size.x, 100.0f) && near(quad.size.y, 50.0f) && near(quad.uv_min.x, 0.0f) && near(quad.uv_max.x, 1.0f));

    // Contain: a square 40x40 image in a 100x50 box is 50x50, centred, uv untouched.
    quad = quad_for(ui, ImageFit::Contain, {40.0f, 40.0f});
    assert(near(quad.size.x, 50.0f) && near(quad.size.y, 50.0f) && near(quad.position.x, 25.0f) && near(quad.position.y, 0.0f));
    assert(near(quad.uv_min.x, 0.0f) && near(quad.uv_max.x, 1.0f) && near(quad.uv_min.y, 0.0f) && near(quad.uv_max.y, 1.0f));

    // Contain at the left edge (object-position: left).
    quad = quad_for(ui, ImageFit::Contain, {40.0f, 40.0f}, {0.0f, 0.5f});
    assert(near(quad.position.x, 0.0f) && near(quad.size.x, 50.0f));

    // Cover: the same image fills the box and is cropped top and bottom: 100x100 scaled, showing the middle half vertically.
    quad = quad_for(ui, ImageFit::Cover, {40.0f, 40.0f});
    assert(near(quad.size.x, 100.0f) && near(quad.size.y, 50.0f) && near(quad.uv_min.x, 0.0f) && near(quad.uv_max.x, 1.0f));
    assert(near(quad.uv_min.y, 0.25f) && near(quad.uv_max.y, 0.75f));

    // None: a 40x20 image keeps its size and is centred; a 200x100 one is cropped to the box.
    quad = quad_for(ui, ImageFit::None, {40.0f, 20.0f});
    assert(near(quad.size.x, 40.0f) && near(quad.size.y, 20.0f) && near(quad.position.x, 30.0f) && near(quad.position.y, 15.0f));
    quad = quad_for(ui, ImageFit::None, {200.0f, 100.0f});
    assert(near(quad.size.x, 100.0f) && near(quad.size.y, 50.0f) && near(quad.uv_min.x, 0.25f) && near(quad.uv_max.x, 0.75f));

    // ScaleDown: small images are left alone, big ones shrink to fit.
    quad = quad_for(ui, ImageFit::ScaleDown, {20.0f, 10.0f});
    assert(near(quad.size.x, 20.0f) && near(quad.size.y, 10.0f));
    quad = quad_for(ui, ImageFit::ScaleDown, {200.0f, 100.0f});
    assert(near(quad.size.x, 100.0f) && near(quad.size.y, 50.0f));

    // An unknown image size falls back to Fill.
    quad = quad_for(ui, ImageFit::Contain, {0.0f, 0.0f});
    assert(near(quad.size.x, 100.0f) && near(quad.size.y, 50.0f));

    ui.destroy();
    SFT::Foundation::log_info("UiImageFitTest: ok");
    return 0;
}
