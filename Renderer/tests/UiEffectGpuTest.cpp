// Headless GPU test for UI effect groups: an element with `ElementDecl::effect` is rendered into its own image by a child renderer,
// passed through the effect's fullscreen passes (here the stock tint/invert and Gaussian-blur shaders), and composited back where
// the element was. Checks real pixels: a leaf and a group with a child through a one-pass effect, a two-pass half-resolution blur that
// bleeds into its outset and stops there, and that an untouched sibling is not affected.
// It needs a real Vulkan device: otherwise it prints SKIP and succeeds.

#include <Async/Async.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/UI/UI.hpp>
#include <WindowManager/WindowManager.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>

#include <array>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Headless GPU test for the UI quad pipeline's texture handling: four image quads, each with its own texture, are drawn
// into an off-screen target and the pixels are read back. When the device binds textures as one array (bindless) the
// four quads must also share one batch and one table; either way each cell must show its own texture's colour.
// It needs a real Vulkan device: otherwise it prints SKIP and succeeds.

namespace {

    using namespace SFT;
    using namespace SFT::Renderer;

    int g_failures = 0;
    constexpr u32 kSize = 256;

    bool check(bool condition, const std::string &message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
        return condition;
    }

    struct Image {
        std::vector<u8> rgb;
        [[nodiscard]] const u8 *at(u32 x, u32 y) const { return &rgb[(static_cast<usize>(y) * kSize + x) * 3]; }
    };

    struct Harness {
        SFT::Renderer::Renderer renderer;
        Core::RenderSurfaceHandle surface{};
        OffscreenRenderTargetHandle target{};
        u64 frame = 0;
    };

    bool read_back(Harness &h, Image &image) {
        RHI::RhiDevice *device = h.renderer.rhi_device();
        const TextureHandle sampled = h.renderer.offscreen_render_target_texture(h.target);
        const TextureResource *resource = h.renderer.texture(sampled);
        if (device == nullptr || resource == nullptr) return false;

        const u64 row_pitch = ((static_cast<u64>(kSize) * 4u + 255u) / 256u) * 256u;
        auto buffer = device->create_buffer(RHI::BufferDesc{
            .size = row_pitch * kSize,
            .usage = RHI::BufferUsage::TransferDst,
            .memory = RHI::MemoryLocation::HostReadback,
            .label = "ui effect test readback",
        });
        if (!buffer) return false;
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "ui effect test readback"});
        if (!encoder) return false;

        RHI::TextureBarrier to_transfer{
            .texture = resource->texture,
            .src_stage = RHI::PipelineStage::FragmentShader | RHI::PipelineStage::ComputeShader,
            .src_access = RHI::AccessFlags::ShaderRead,
            .dst_stage = RHI::PipelineStage::Transfer,
            .dst_access = RHI::AccessFlags::TransferRead,
            .old_layout = RHI::TextureLayout::ShaderReadOnly,
            .new_layout = RHI::TextureLayout::TransferSrc,
        };
        (*encoder)->barrier({}, {}, std::span<const RHI::TextureBarrier>{&to_transfer, 1});
        (*encoder)->copy_texture_to_buffer(resource->texture, *buffer,
                                           RHI::BufferTextureCopy{
                                               .buffer_offset = 0,
                                               .buffer_row_length = static_cast<u32>(row_pitch / 4u),
                                               .buffer_image_height = kSize,
                                               .mip_level = 0,
                                               .base_array_layer = 0,
                                               .array_layer_count = 1,
                                               .texture_offset = {0, 0, 0},
                                               .texture_extent = {kSize, kSize, 1},
                                           });
        RHI::TextureBarrier back{
            .texture = resource->texture,
            .src_stage = RHI::PipelineStage::Transfer,
            .src_access = RHI::AccessFlags::TransferRead,
            .dst_stage = RHI::PipelineStage::FragmentShader | RHI::PipelineStage::ComputeShader,
            .dst_access = RHI::AccessFlags::ShaderRead,
            .old_layout = RHI::TextureLayout::TransferSrc,
            .new_layout = RHI::TextureLayout::ShaderReadOnly,
        };
        (*encoder)->barrier({}, {}, std::span<const RHI::TextureBarrier>{&back, 1});
        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) return false;
        auto fence = device->create_fence(RHI::FenceDesc{.label = "ui effect test readback fence"});
        if (!fence) return false;
        const RHI::CommandBufferHandle buffers[1] = {*command_buffer};
        RHI::SubmitDesc submit{
            .command_buffers = std::span<const RHI::CommandBufferHandle>{buffers, 1},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "ui effect test readback submit",
        };
        if (!device->submit(submit)) return false;
        auto waited = device->wait_fences(std::span<const RHI::FenceHandle>{&*fence, 1}, true);
        if (!waited || !*waited) return false;

        auto mapped = device->map_buffer(*buffer);
        if (!mapped) return false;
        image.rgb.assign(static_cast<usize>(kSize) * kSize * 3, 0);
        for (u32 y = 0; y < kSize; ++y) {
            const auto *row = reinterpret_cast<const u8 *>(mapped->data()) + y * row_pitch;
            for (u32 x = 0; x < kSize; ++x) {
                u8 *dst = &image.rgb[(static_cast<usize>(y) * kSize + x) * 3];
                dst[0] = row[x * 4 + 2];
                dst[1] = row[x * 4 + 1];
                dst[2] = row[x * 4 + 0];
            }
        }
        device->unmap_buffer(*buffer);
        device->destroy_fence(*fence);
        device->destroy_command_buffer(*command_buffer);
        device->destroy_buffer(*buffer);
        return true;
    }


    /// Renders one overlay-only frame containing `ui`'s finished overlay into the off-screen target.
    bool render_ui_frame(Harness &h, UI::UiSurface &ui) {
        RenderFrameDesc desc{};
        desc.surface = h.surface;
        desc.offscreen_target = h.target;
        desc.frame.framebuffer_width = kSize;
        desc.frame.framebuffer_height = kSize;
        desc.frame.frame_index = h.frame++;
        desc.view.render_graph.render_scene = false;
        desc.view.render_graph.frame.tone_mapping.enabled = false;
        desc.view.render_graph.frame.bloom.enabled = false;
        desc.view.render_graph.frame.shadows.enabled = false;
        desc.view.render_graph.frame.ambient_occlusion.enabled = false;
        desc.view.render_graph.frame.execution_mode = SFT::RenderSettings::ExecutionMode::WaitForCompletion;
        desc.view.render_graph.overlay_passes.push_back(ui.finish_overlay(&h.renderer));
        if (Core::RendererResult r = h.renderer.render_frame(desc); !r) {
            std::cerr << "render_frame failed: " << r.error().message << '\n';
            return false;
        }
        h.renderer.wait_idle();
        return true;
    }

    f32 half_to_float(u16 h) {
        const int sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
        f32 value = exponent == 0 ? std::ldexp(static_cast<f32>(mantissa), -24)
                                  : std::ldexp(static_cast<f32>(mantissa + 1024), exponent - 25);
        return sign ? -value : value;
    }

    /// Reads one texel of `target` (any of the formats the test uses) as linear-or-encoded floats in RGBA order, as the format stores them.
    bool read_texel(Harness &h, OffscreenRenderTargetHandle target, RHI::Format format, u32 x, u32 y, std::array<f32, 4> &out) {
        RHI::RhiDevice *device = h.renderer.rhi_device();
        const TextureResource *resource = h.renderer.texture(h.renderer.offscreen_render_target_texture(target));
        if (device == nullptr || resource == nullptr) return false;
        auto buffer = device->create_buffer(RHI::BufferDesc{.size = 256,
                                                            .usage = RHI::BufferUsage::TransferDst,
                                                            .memory = RHI::MemoryLocation::HostReadback,
                                                            .label = "ui format readback"});
        if (!buffer) return false;
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "ui format readback"});
        if (!encoder) return false;
        RHI::TextureBarrier to_transfer{.texture = resource->texture,
                                        .src_stage = RHI::PipelineStage::FragmentShader | RHI::PipelineStage::ComputeShader,
                                        .src_access = RHI::AccessFlags::ShaderRead,
                                        .dst_stage = RHI::PipelineStage::Transfer,
                                        .dst_access = RHI::AccessFlags::TransferRead,
                                        .old_layout = RHI::TextureLayout::ShaderReadOnly,
                                        .new_layout = RHI::TextureLayout::TransferSrc};
        (*encoder)->barrier({}, {}, std::span<const RHI::TextureBarrier>{&to_transfer, 1});
        (*encoder)->copy_texture_to_buffer(resource->texture, *buffer,
                                           RHI::BufferTextureCopy{.buffer_offset = 0,
                                                                  .buffer_row_length = 1,
                                                                  .buffer_image_height = 1,
                                                                  .mip_level = 0,
                                                                  .base_array_layer = 0,
                                                                  .array_layer_count = 1,
                                                                  .texture_offset = {static_cast<i32>(x), static_cast<i32>(y), 0},
                                                                  .texture_extent = {1, 1, 1}});
        RHI::TextureBarrier back{.texture = resource->texture,
                                 .src_stage = RHI::PipelineStage::Transfer,
                                 .src_access = RHI::AccessFlags::TransferRead,
                                 .dst_stage = RHI::PipelineStage::FragmentShader | RHI::PipelineStage::ComputeShader,
                                 .dst_access = RHI::AccessFlags::ShaderRead,
                                 .old_layout = RHI::TextureLayout::TransferSrc,
                                 .new_layout = RHI::TextureLayout::ShaderReadOnly};
        (*encoder)->barrier({}, {}, std::span<const RHI::TextureBarrier>{&back, 1});
        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) return false;
        auto fence = device->create_fence(RHI::FenceDesc{.label = "ui format readback fence"});
        if (!fence) return false;
        const RHI::CommandBufferHandle buffers[1] = {*command_buffer};
        RHI::SubmitDesc submit{.command_buffers = std::span<const RHI::CommandBufferHandle>{buffers, 1},
                               .fence = *fence,
                               .flags = RHI::SubmitFlags::OneShot,
                               .label = "ui format readback submit"};
        if (!device->submit(submit)) return false;
        auto waited = device->wait_fences(std::span<const RHI::FenceHandle>{&*fence, 1}, true);
        if (!waited || !*waited) return false;
        auto mapped = device->map_buffer(*buffer);
        if (!mapped) return false;
        const u8 *bytes = reinterpret_cast<const u8 *>(mapped->data());
        switch (format) {
            case RHI::Format::RGBA8Unorm:
            case RHI::Format::RGBA8UnormSrgb:
                for (int i = 0; i < 4; ++i) out[i] = bytes[i] / 255.0f;
                break;
            case RHI::Format::BGRA8Unorm:
            case RHI::Format::BGRA8UnormSrgb:
                out = {bytes[2] / 255.0f, bytes[1] / 255.0f, bytes[0] / 255.0f, bytes[3] / 255.0f};
                break;
            case RHI::Format::RGB10A2Unorm: {
                u32 v;
                std::memcpy(&v, bytes, 4);
                out = {(v & 1023u) / 1023.0f, ((v >> 10) & 1023u) / 1023.0f, ((v >> 20) & 1023u) / 1023.0f, (v >> 30) / 3.0f};
                break;
            }
            case RHI::Format::RGBA16Float:
                for (int i = 0; i < 4; ++i) {
                    u16 half;
                    std::memcpy(&half, bytes + i * 2, 2);
                    out[i] = half_to_float(half);
                }
                break;
            case RHI::Format::RGBA32Float:
                std::memcpy(out.data(), bytes, 16);
                break;
            default:
                device->unmap_buffer(*buffer);
                return false;
        }
        device->unmap_buffer(*buffer);
        device->destroy_fence(*fence);
        device->destroy_command_buffer(*command_buffer);
        device->destroy_buffer(*buffer);
        return true;
    }

    /// Like render_ui_frame, with every layer of `ui`'s finished frame as its own overlay.
    bool render_layered_ui_frame(Harness &h, UI::UiSurface &ui) {
        RenderFrameDesc desc{};
        desc.surface = h.surface;
        desc.offscreen_target = h.target;
        desc.frame.framebuffer_width = kSize;
        desc.frame.framebuffer_height = kSize;
        desc.frame.frame_index = h.frame++;
        desc.view.render_graph.render_scene = false;
        desc.view.render_graph.frame.tone_mapping.enabled = false;
        desc.view.render_graph.frame.bloom.enabled = false;
        desc.view.render_graph.frame.shadows.enabled = false;
        desc.view.render_graph.frame.ambient_occlusion.enabled = false;
        desc.view.render_graph.frame.execution_mode = SFT::RenderSettings::ExecutionMode::WaitForCompletion;
        for (auto &overlay : ui.finish_overlays(&h.renderer)) {
            desc.view.render_graph.overlay_passes.push_back(std::move(overlay));
        }
        if (Core::RendererResult r = h.renderer.render_frame(desc); !r) {
            std::cerr << "render_frame failed: " << r.error().message << '\n';
            return false;
        }
        h.renderer.wait_idle();
        return true;
    }

    /// Like render_ui_frame, with two overlays: `back` is drawn first, `front` over it (front's effects can read what back drew).
    bool render_two_ui_frame(Harness &h, UI::UiSurface &back, UI::UiSurface &front) {
        RenderFrameDesc desc{};
        desc.surface = h.surface;
        desc.offscreen_target = h.target;
        desc.frame.framebuffer_width = kSize;
        desc.frame.framebuffer_height = kSize;
        desc.frame.frame_index = h.frame++;
        desc.view.render_graph.render_scene = false;
        desc.view.render_graph.frame.tone_mapping.enabled = false;
        desc.view.render_graph.frame.bloom.enabled = false;
        desc.view.render_graph.frame.shadows.enabled = false;
        desc.view.render_graph.frame.ambient_occlusion.enabled = false;
        desc.view.render_graph.frame.execution_mode = SFT::RenderSettings::ExecutionMode::WaitForCompletion;
        desc.view.render_graph.overlay_passes.push_back(back.finish_overlay(&h.renderer));
        desc.view.render_graph.overlay_passes.push_back(front.finish_overlay(&h.renderer));
        if (Core::RendererResult r = h.renderer.render_frame(desc); !r) {
            std::cerr << "render_frame failed: " << r.error().message << '\n';
            return false;
        }
        h.renderer.wait_idle();
        return true;
    }

    struct Pixel {
        int r, g, b;
    };
    Pixel at(const Image &image, u32 x, u32 y) {
        const u8 *p = image.at(x, y);
        return Pixel{p[0], p[1], p[2]};
    }
    bool near(const Pixel &p, int r, int g, int b, int tolerance = 24) {
        return std::abs(p.r - r) <= tolerance && std::abs(p.g - g) <= tolerance && std::abs(p.b - b) <= tolerance;
    }
    std::string str(const Pixel &p) { return std::to_string(p.r) + "," + std::to_string(p.g) + "," + std::to_string(p.b); }

    std::shared_ptr<UI::UiEffect> tint_effect(glm::vec4 tint, f32 invert) {
        auto effect = std::make_shared<UI::UiEffect>();
        effect->name = UString{"tint"};
        UI::UiEffectPass pass;
        pass.label = UString{"tint"};
        pass.shader_path = UString{"Shaders/ui_effect_tint.slang"};
        pass.module_name = UString{"ui_effect_tint"};
        pass.inputs = {UI::UiEffectInput{.kind = UI::UiEffectInputKind::GroupContent}};
        const float parameters[8] = {tint.x, tint.y, tint.z, tint.w, invert, 0.0f, 0.0f, 0.0f};
        pass.parameters.resize(sizeof(parameters));
        std::memcpy(pass.parameters.data(), parameters, sizeof(parameters));
        effect->passes.push_back(std::move(pass));
        return effect;
    }

    std::shared_ptr<UI::UiEffect> blur_effect(f32 sigma) {
        auto effect = std::make_shared<UI::UiEffect>();
        effect->name = UString{"blur"};
        effect->outset = UI::UiEffectOutset::all(24.0f);
        for (int axis = 0; axis < 2; ++axis) {
            UI::UiEffectPass pass;
            pass.label = UString{axis == 0 ? "blur horizontal" : "blur vertical"};
            pass.shader_path = UString{"Shaders/ui_effect_blur.slang"};
            pass.module_name = UString{"ui_effect_blur"};
            pass.inputs = {axis == 0 ? UI::UiEffectInput{.kind = UI::UiEffectInputKind::GroupContent}
                                     : UI::UiEffectInput{.kind = UI::UiEffectInputKind::PassOutput, .pass = 0}};
            pass.output.scale = 0.5f;
            const float parameters[4] = {axis == 0 ? 1.0f : 0.0f, axis == 0 ? 0.0f : 1.0f, sigma, 1.0f};
            pass.parameters.resize(sizeof(parameters));
            std::memcpy(pass.parameters.data(), parameters, sizeof(parameters));
            effect->passes.push_back(std::move(pass));
        }
        return effect;
    }

    UI::ElementDecl box(f32 size, UI::Color color, std::shared_ptr<const UI::UiEffect> effect = nullptr) {
        UI::ElementDecl decl{.sizing = {UI::SizingAxis::fixed(size), UI::SizingAxis::fixed(size)}, .background_color = color};
        decl.effect = std::move(effect);
        return decl;
    }

} // namespace

int main() {
    namespace fs = std::filesystem;
    fs::current_path(fs::path(__FILE__).parent_path().parent_path().parent_path());

    auto harness_owner = std::make_unique<Harness>();
    Harness &harness = *harness_owner;

    SFT::WindowManager::WindowManager window_manager;
    SFT::WindowManager::WindowConfig window_config{};
    window_config.title = "UiEffectGpuTest";
    window_config.extent = {kSize, kSize};
    window_config.visible = false;
    window_config.graphics_api = SFT::WindowManager::WindowGraphicsApi::Vulkan;
    auto window_id = window_manager.spawn_window<SFT::WindowManager::SDL3::SDL3Window>(window_config);
    if (!window_id) {
        std::cout << "SKIP: could not create a window (" << window_id.error().message << ")\n";
        return 0;
    }
    std::string skip_reason;
    auto initialized = window_manager.with_window(*window_id, [&](SFT::WindowManager::Window &window) {
        Core::RendererCreateInfo info{};
        info.backend = RHI::BackendType::Vulkan;
        info.app_name = "UiEffectGpuTest";
        info.window = &window;
        auto surface = harness.renderer.initialize(info);
        if (!surface) {
            skip_reason = surface.error().message;
            return false;
        }
        harness.surface = *surface;
        return true;
    });
    if (!initialized || !*initialized) {
        std::cout << "SKIP: no Vulkan device available (" << skip_reason << ")\n";
        return 0;
    }
    auto target = harness.renderer.create_offscreen_render_target(
        OffscreenRenderTargetDescription{.extent = {kSize, kSize}, .label = "ui effect test target"});
    if (!target) {
        std::cerr << "FAILED: off-screen target: " << target.error().message << '\n';
        return 1;
    }
    harness.target = *target;

    UI::UiSurface ui;
    if (!ui.ensure_ready(*harness.renderer.rhi_device(), RHI::Format::BGRA8UnormSrgb)) {
        std::cerr << "FAILED: UI surface could not be created\n";
        return 1;
    }

    const auto invert = tint_effect({1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);
    const auto tint_red = tint_effect({1.0f, 0.0f, 0.0f, 1.0f}, 0.0f);
    const auto invert_group = tint_effect({1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);

    // Scene 1: a row of four 64 px boxes. Control red; red inverted (cyan); white tinted red; a white box holding a blue square,
    // inverted as a whole (black with a yellow square).
    for (int frame = 0; frame < 3; ++frame) {
        UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
        {
            auto root = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))}});
            { auto control = ctx.element(box(64.0f, UI::Color{1.0, 0.0, 0.0, 1.0})); }
            { auto inverted = ctx.element(box(64.0f, UI::Color{1.0, 0.0, 0.0, 1.0}, invert)); }
            { auto tinted = ctx.element(box(64.0f, UI::Color{1.0, 1.0, 1.0, 1.0}, tint_red)); }
            {
                UI::ElementDecl holder = box(64.0f, UI::Color{1.0, 1.0, 1.0, 1.0}, invert_group);
                holder.padding = UI::Padding::all(16);
                auto group = ctx.element(holder);
                auto child = ctx.element(box(32.0f, UI::Color{0.0, 0.0, 1.0, 1.0}));
            }
        }
        if (!render_ui_frame(harness, ui)) {
            return 1;
        }
    }
    {
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 1\n";
            return 1;
        }
        const Pixel control = at(image, 32, 32);
        const Pixel inverted = at(image, 96, 32);
        const Pixel tinted = at(image, 160, 32);
        const Pixel group_corner = at(image, 196, 4);
        const Pixel group_child = at(image, 224, 32);
        const Pixel outside = at(image, 32, 100);
        check(near(control, 255, 0, 0), "the untouched control stays red (got " + str(control) + ")");
        check(near(inverted, 0, 255, 255), "the inverted red box becomes cyan (got " + str(inverted) + ")");
        check(near(tinted, 255, 0, 0), "the white box tinted red becomes red (got " + str(tinted) + ")");
        check(near(group_corner, 0, 0, 0), "an inverted white group background becomes black (got " + str(group_corner) + ")");
        check(near(group_child, 255, 255, 0), "the blue child of an inverted group becomes yellow (got " + str(group_child) + ")");
        check(near(outside, 0, 0, 0), "nothing is drawn outside the boxes (got " + str(outside) + ")");
    }

    // Scene 2: a white 64 px box blurred by a two-pass, half-resolution Gaussian with a 24 px outset.
    const auto blur = blur_effect(2.5f);
    for (int frame = 0; frame < 3; ++frame) {
        UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
        {
            auto root = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                .padding = UI::Padding{96, 0, 96, 0}});
            auto blurred = ctx.element(box(64.0f, UI::Color{1.0, 1.0, 1.0, 1.0}, blur));
        }
        if (!render_ui_frame(harness, ui)) {
            return 1;
        }
    }
    {
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 2\n";
            return 1;
        }
        const Pixel center = at(image, 128, 128);
        const Pixel just_outside = at(image, 96 - 4, 128);
        const Pixel beyond_outset = at(image, 96 - 40, 128);
        check(center.r > 200 && center.g > 200 && center.b > 200, "the blur keeps the middle of the box bright (got " + str(center) + ")");
        check(just_outside.r > 12 && just_outside.r < 235, "the blur bleeds past the box edge, partly bright (got " + str(just_outside) + ")");
        check(beyond_outset.r < 8 && beyond_outset.g < 8 && beyond_outset.b < 8,
              "nothing is drawn beyond the outset (got " + str(beyond_outset) + ")");
    }

    // Scene 3: the built-in effects. A red-to-blue gradient across a bevel-cornered box, a green box turned greyscale, and a white box
    // with a green drop shadow offset down and to the right.
    {
        const std::array<UI::Effects::GradientStop, 2> stops{{{UI::Color{1.0, 0.0, 0.0, 1.0}, 0.0f}, {UI::Color{0.0, 0.0, 1.0, 1.0}, 1.0f}}};
        const auto gradient = UI::Effects::linear_gradient(0.0f, stops);
        UI::Effects::ColorAdjust grey_adjust;
        grey_adjust.grayscale = 1.0f;
        const auto grey = UI::Effects::color_adjust(grey_adjust);
        const auto shadow = UI::Effects::drop_shadow({10.0f, 10.0f}, 3.0f, UI::Color{0.0, 1.0, 0.0, 1.0});
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))}});
                {
                    UI::ElementDecl shaped = box(64.0f, UI::Color{1.0, 1.0, 1.0, 1.0}, gradient);
                    shaped.corner_radius = UI::CornerRadius::all(16.0f);
                    shaped.corner_shapes = UI::CornerShapes::all(UI::CornerShape::bevel());
                    auto element = ctx.element(shaped);
                }
                { auto element = ctx.element(box(64.0f, UI::Color{0.0, 1.0, 0.0, 1.0}, grey)); }
                {
                    UI::ElementDecl holder{.sizing = {UI::SizingAxis::fixed(64.0f), UI::SizingAxis::fixed(64.0f)},
                                           .padding = UI::Padding::all(8)};
                    auto slot = ctx.element(holder);
                    auto element = ctx.element(box(40.0f, UI::Color{1.0, 1.0, 1.0, 1.0}, shadow));
                }
            }
            if (!render_ui_frame(harness, ui)) {
                return 1;
            }
        }
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 3\n";
            return 1;
        }
        const Pixel gradient_left = at(image, 8, 32);
        const Pixel gradient_right = at(image, 56, 32);
        const Pixel masked_corner = at(image, 1, 1);
        const Pixel greyed = at(image, 96, 32);
        const Pixel shadow_pixel = at(image, 180, 52);
        const Pixel shadowed_box = at(image, 148, 28);
        check(gradient_left.r > 150 && gradient_left.b < 110, "the gradient starts red on the left (got " + str(gradient_left) + ")");
        check(gradient_right.b > 150 && gradient_right.r < 110, "the gradient ends blue on the right (got " + str(gradient_right) + ")");
        check(near(masked_corner, 0, 0, 0), "the gradient keeps the element's bevelled corner (got " + str(masked_corner) + ")");
        check(std::abs(greyed.r - greyed.g) < 12 && std::abs(greyed.g - greyed.b) < 12 && greyed.g > 150,
              "grayscale turns green into a light grey (got " + str(greyed) + ")");
        check(shadow_pixel.g > 100 && shadow_pixel.r < 70 && shadow_pixel.b < 70,
              "the drop shadow shows green beside the box (got " + str(shadow_pixel) + ")");
        check(near(shadowed_box, 255, 255, 255, 30), "the box itself is drawn over its shadow (got " + str(shadowed_box) + ")");
    }

    // Scene 4: gradient colour interpolation on the GPU against the CPU reference (UI/UiColor.hpp). Four 64 px boxes, each a red-to-blue
    // gradient mixed differently: Oklab (the default), Oklch by the shorter hue arc, HSL by the longer arc, and stops written in Display P3.
    {
        using UI::ColorInterpolation;
        using UI::ColorSpace;
        using UI::HueInterpolation;
        const glm::vec4 red{1.0f, 0.0f, 0.0f, 1.0f};
        const glm::vec4 blue{0.0f, 0.0f, 1.0f, 1.0f};
        const std::array<UI::Effects::GradientStop, 2> srgb_stops{{{red, 0.0f}, {blue, 1.0f}}};
        const std::array<UI::Effects::GradientStop, 2> p3_stops{{{red, 0.0f, ColorSpace::DisplayP3}, {blue, 1.0f, ColorSpace::DisplayP3}}};
        const ColorInterpolation oklch{ColorSpace::Oklch, HueInterpolation::Shorter};
        const ColorInterpolation hsl_long{ColorSpace::Hsl, HueInterpolation::Longer};
        const std::array<std::shared_ptr<UI::UiEffect>, 4> gradients{
            UI::Effects::linear_gradient(0.0f, srgb_stops),
            UI::Effects::linear_gradient(0.0f, srgb_stops, oklch),
            UI::Effects::linear_gradient(0.0f, srgb_stops, hsl_long),
            UI::Effects::linear_gradient(0.0f, p3_stops),
        };
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))}});
                for (const auto &gradient : gradients) {
                    auto element = ctx.element(box(64.0f, UI::Color{1.0, 1.0, 1.0, 1.0}, gradient));
                }
            }
            if (!render_ui_frame(harness, ui)) {
                return 1;
            }
        }
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 4\n";
            return 1;
        }
        const std::array<ColorInterpolation, 4> methods{ColorInterpolation{}, oklch, hsl_long, ColorInterpolation{}};
        const std::array<ColorSpace, 4> sources{ColorSpace::Srgb, ColorSpace::Srgb, ColorSpace::Srgb, ColorSpace::DisplayP3};
        for (u32 i = 0; i < 4; ++i) {
            for (const u32 local_x : {10u, 32u, 54u}) {
                const f32 t = (static_cast<f32>(local_x) + 0.5f) / 64.0f;
                const glm::vec4 expected = UI::mix(red, blue, t, methods[i], sources[i], ColorSpace::Srgb);
                const Pixel got = at(image, i * 64 + local_x, 32);
                const auto byte = [](f32 value) { return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
                check(near(got, byte(expected.x), byte(expected.y), byte(expected.z), 8),
                      "gradient " + std::to_string(i) + " at x=" + std::to_string(local_x) + " must match the CPU mix (expected " +
                          std::to_string(byte(expected.x)) + "," + std::to_string(byte(expected.y)) + "," + std::to_string(byte(expected.z)) +
                          ", got " + str(got) + ")");
            }
        }
        // The spaces really differ: the same two colours, the same position, visibly different middles.
        const Pixel oklab_middle = at(image, 32, 32);
        const Pixel oklch_middle = at(image, 64 + 32, 32);
        const Pixel hsl_middle = at(image, 128 + 32, 32);
        check(std::abs(oklab_middle.r - oklch_middle.r) + std::abs(oklab_middle.g - oklch_middle.g) + std::abs(oklab_middle.b - oklch_middle.b) > 40,
              "oklab and oklch middles differ");
        check(std::abs(oklch_middle.g - hsl_middle.g) + std::abs(oklch_middle.r - hsl_middle.r) > 40, "oklch and hsl (longer arc) middles differ");
    }

    // Scene 5: mip chains. (a) `fast_blur` reads the content's generated chain; (b) a hand-built effect whose first pass writes a
    // half-resolution output *with its own mip chain* and whose second pass blurs by reading level 2 of it. A white 64 px box on black.
    {
        const auto fast = UI::Effects::fast_blur(8.0f);
        auto chained = std::make_shared<UI::UiEffect>();
        chained->name = UString{"chained mips"};
        chained->outset = UI::UiEffectOutset::all(30.0f);
        {
            UI::UiEffectPass copy;
            copy.label = UString{"copy with mips"};
            copy.shader_path = UString{"Shaders/ui_effect_tint.slang"};
            copy.module_name = UString{"ui_effect_tint"};
            copy.inputs = {UI::UiEffectInput{.kind = UI::UiEffectInputKind::GroupContent}};
            const float parameters[8] = {1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            copy.parameters.resize(sizeof(parameters));
            std::memcpy(copy.parameters.data(), parameters, sizeof(parameters));
            copy.output.scale = 0.5f;
            copy.output.mip_levels = 0;
            chained->passes.push_back(std::move(copy));
            UI::UiEffectPass blur;
            blur.label = UString{"read level 2"};
            blur.shader_path = UString{"Shaders/ui_effect_mipblur.slang"};
            blur.module_name = UString{"ui_effect_mipblur"};
            blur.inputs = {UI::UiEffectInput{.kind = UI::UiEffectInputKind::PassOutput, .pass = 0}};
            const float level[4] = {2.0f, 0.5f, 0.0f, 0.0f};
            blur.parameters.resize(sizeof(level));
            std::memcpy(blur.parameters.data(), level, sizeof(level));
            chained->passes.push_back(std::move(blur));
        }
        // A restricted view: read only level 3 of the content's chain, as if it were a whole 1/8-size image.
        auto restricted = std::make_shared<UI::UiEffect>();
        restricted->name = UString{"one level"};
        restricted->content.mip_levels = 0;
        restricted->outset = UI::UiEffectOutset::all(30.0f);
        {
            UI::UiEffectPass pass;
            pass.label = UString{"level 3 as an image"};
            pass.shader_path = UString{"Shaders/ui_effect_tint.slang"};
            pass.module_name = UString{"ui_effect_tint"};
            pass.inputs = {UI::UiEffectInput{.kind = UI::UiEffectInputKind::GroupContent, .base_mip = 3, .mip_count = 1}};
            const float parameters[8] = {1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            pass.parameters.resize(sizeof(parameters));
            std::memcpy(pass.parameters.data(), parameters, sizeof(parameters));
            restricted->passes.push_back(std::move(pass));
        }
        const std::array<std::shared_ptr<UI::UiEffect>, 3> effects{fast, chained, restricted};
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                    .padding = UI::Padding{32, 0, 96, 0},
                    .child_gap = 40});
                for (const auto &effect : effects) {
                    auto element = ctx.element(box(40.0f, UI::Color{1.0, 1.0, 1.0, 1.0}, effect));
                }
            }
            if (!render_ui_frame(harness, ui)) {
                return 1;
            }
        }
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 5\n";
            return 1;
        }
        // Boxes are 40 px wide starting at x = 32, 112, 192 (gap 40), top at y = 96; the outside samples are 6 and 28 px above the top edge.
        const std::array<u32, 3> left{32, 112, 192};
        const std::array<const char *, 3> names{"fast_blur", "chained pass mips", "restricted content level"};
        for (usize i = 0; i < 3; ++i) {
            const Pixel centre = at(image, left[i] + 20, 96 + 20);
            // Sampled above the box, in line with its middle: its neighbours are a box width and a gap away sideways, out of reach.
            const Pixel outside = at(image, left[i] + 20, 96 - 6);
            const Pixel far_outside = at(image, left[i] + 20, 96 - 28);
            check(centre.r > 60, std::string{names[i]} + ": a blurred box keeps a bright middle (got " + str(centre) + ")");
            // The restricted case shows level 3 as a blocky 1/8-size image, so right next to the box it may be exactly dark.
            const bool soft_edge = i != 2;
            check(outside.r < centre.r && (!soft_edge || outside.r > 6), std::string{names[i]} + ": the blur reaches outside the box but is dimmer (got " + str(outside) + ")");
            check(far_outside.r <= outside.r, std::string{names[i]} + ": the blur fades with distance (got " + str(far_outside) + ")");
        }
    }

    // Scene 6: composite blend modes over a mid-grey backdrop (sRGB 0.5 = linear 0.214). Four 64 px boxes with an identity tint effect:
    //   additive, opaque red       adds linear (1,0,0): red clips to 255, green and blue stay at the backdrop's 127
    //   multiply, opaque red       multiplies the backdrop by (1,0,0): red keeps 127, green and blue go to 0
    //   replace, half-transparent red   overwrites the backdrop with straight red: 255,0,0
    //   normal, half-transparent red    mixes half of it into the backdrop: neither the backdrop nor pure red
    {
        const auto make = [&](UI::UiEffectBlend blend) {
            auto effect = UI::Effects::tint(glm::vec4{1.0f});
            effect->composite = blend;
            return effect;
        };
        const auto additive = make(UI::UiEffectBlend::Additive);
        const auto multiply = make(UI::UiEffectBlend::Multiply);
        const auto replace = make(UI::UiEffectBlend::Replace);
        const auto normal = make(UI::UiEffectBlend::Normal);
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                    .background_color = UI::Color{0.5, 0.5, 0.5, 1.0}});
                { auto element = ctx.element(box(64.0f, UI::Color{1.0, 0.0, 0.0, 1.0}, additive)); }
                { auto element = ctx.element(box(64.0f, UI::Color{1.0, 0.0, 0.0, 1.0}, multiply)); }
                { auto element = ctx.element(box(64.0f, UI::Color{1.0, 0.0, 0.0, 0.5}, replace)); }
                { auto element = ctx.element(box(64.0f, UI::Color{1.0, 0.0, 0.0, 0.5}, normal)); }
            }
            if (!render_ui_frame(harness, ui)) {
                return 1;
            }
        }
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 6\n";
            return 1;
        }
        const Pixel backdrop = at(image, 32, 128);
        const Pixel added = at(image, 32, 32);
        const Pixel multiplied = at(image, 96, 32);
        const Pixel replaced = at(image, 160, 32);
        const Pixel mixed = at(image, 224, 32);
        check(near(backdrop, 127, 127, 127, 6), "the backdrop is mid grey (got " + str(backdrop) + ")");
        check(near(added, 255, 127, 127, 20), "additive: red added to the backdrop (got " + str(added) + ")");
        check(near(multiplied, 127, 0, 0, 20), "multiply: the backdrop multiplied by red (got " + str(multiplied) + ")");
        check(near(replaced, 255, 0, 0, 20), "replace: the backdrop is overwritten by straight red (got " + str(replaced) + ")");
        check(!near(mixed, 255, 0, 0, 40) && !near(mixed, 127, 127, 127, 40) && mixed.r > 127, "normal: half-transparent red mixes with the backdrop (got " + str(mixed) + ")");
    }

    // Scene 7: an outline (with a 6 px gap) around a bevelled box, and an inset shadow inside a white box.
    {
        const auto ring = UI::Effects::outline(4.0f, glm::vec4{0.0f, 1.0f, 0.0f, 1.0f}, 6.0f);
        const auto inset = UI::Effects::inset_shadow({0.0f, 0.0f}, 4.0f, glm::vec4{0.0f, 0.0f, 0.0f, 1.0f}, 1.5f);
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                    .padding = UI::Padding{32, 0, 64, 0},
                    .child_gap = 64});
                {
                    UI::ElementDecl shaped = box(64.0f, UI::Color{1.0, 1.0, 1.0, 1.0}, ring);
                    shaped.corner_radius = UI::CornerRadius::all(12.0f);
                    auto element = ctx.element(shaped);
                }
                { auto element = ctx.element(box(64.0f, UI::Color{1.0, 1.0, 1.0, 1.0}, inset)); }
            }
            if (!render_ui_frame(harness, ui)) {
                return 1;
            }
        }
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 7\n";
            return 1;
        }
        // First box: x 32..96, y 64..128. The ring starts 6 px out and is 4 px wide, so 8 px out is inside the ring, 3 px out is the gap,
        // and 14 px out is beyond it.
        const Pixel inside_box = at(image, 64, 96);
        const Pixel gap = at(image, 32 - 3, 96);
        const Pixel in_ring = at(image, 32 - 8, 96);
        const Pixel beyond = at(image, 32 - 14, 96);
        check(near(inside_box, 255, 255, 255, 12), "the outlined box itself is untouched (got " + str(inside_box) + ")");
        check(near(gap, 0, 0, 0, 14), "the gap between a box and its outline is empty (got " + str(gap) + ")");
        check(in_ring.g > 150 && in_ring.r < 90 && in_ring.b < 90, "the outline ring is green (got " + str(in_ring) + ")");
        check(near(beyond, 0, 0, 0, 14), "nothing is drawn beyond the outline (got " + str(beyond) + ")");
        // Second box: x 160..224. Its middle stays bright and its edge is shadowed (2 px in, a sigma-4 blur leaves the shadow about 46% dense).
        const Pixel inset_middle = at(image, 192, 96);
        const Pixel inset_edge = at(image, 160 + 2, 96);
        check(inset_middle.r > 200, "the middle of an inset-shadowed box stays bright (got " + str(inset_middle) + ")");
        check(inset_edge.r < inset_middle.r - 40,  "an inset shadow darkens the edge (got " + str(inset_edge) + " against " + str(inset_middle) + ")");
    }

    // Scene 8: backdrop. The first overlay paints the left half red and the right half blue; the second overlay holds a frosted-glass box
    // (96..160) across the seam at x = 128. Inside the glass the seam is blurred: just left of it is no longer pure red.
    {
        UI::UiSurface front;
        if (!front.ensure_ready(*harness.renderer.rhi_device(), RHI::Format::BGRA8UnormSrgb)) {
            std::cerr << "FAILED: second UI surface could not be created\n";
            return 1;
        }
        const auto glass = UI::Effects::frosted_glass(16.0f, glm::vec3{1.0f}, 0.0f);
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &back_ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = back_ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))}});
                { auto left = back_ctx.element(box(128.0f, UI::Color{1.0, 0.0, 0.0, 1.0})); }
                { auto right = back_ctx.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(128.0f), UI::SizingAxis::fixed(128.0f)},
                                                                  .background_color = UI::Color{0.0, 0.0, 1.0, 1.0}}); }
            }
            UI::Context &front_ctx = front.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = front_ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                    .padding = UI::Padding{96, 0, 0, 0}});
                UI::ElementDecl pane = box(64.0f, UI::Color{1.0, 1.0, 1.0, 0.02}, glass);
                pane.background_color = UI::Color{1.0, 1.0, 1.0, 0.02};
                auto element = front_ctx.element(pane);
            }
            if (!render_two_ui_frame(harness, ui, front)) {
                return 1;
            }
        }
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 8\n";
            return 1;
        }
        const Pixel outside_seam = at(image, 124, 100);
        const Pixel glass_seam = at(image, 124, 20);
        check(near(outside_seam, 255, 0, 0, 20), "outside the glass the seam is untouched (got " + str(outside_seam) + ")");
        check(glass_seam.r < 235 && glass_seam.r > 60 && glass_seam.b > 40,
              "inside the glass the red/blue seam is blurred (got " + str(glass_seam) + ")");
        front.destroy(*harness.renderer.rhi_device());
    }

    // Scene 9: the same UI rendered into off-screen targets of several hardware formats. A pure-red box over a half-grey (sRGB 0.5)
    // backdrop; each format must hold red where the box is (channel order right) and the format's own encoding of the grey elsewhere.
    {
        struct FormatCase {
            RHI::Format format;
            const char *name;
            bool srgb_encoded; // the stored value is the sRGB-encoded 0.5 (127/255), or else linear 0.214
        };
        const FormatCase cases[] = {
            {RHI::Format::RGBA8Unorm, "RGBA8Unorm", true},
            {RHI::Format::BGRA8Unorm, "BGRA8Unorm", true},
            {RHI::Format::BGRA8UnormSrgb, "BGRA8UnormSrgb", true},
            {RHI::Format::RGBA8UnormSrgb, "RGBA8UnormSrgb", true},
            {RHI::Format::RGB10A2Unorm, "RGB10A2Unorm", true},
            {RHI::Format::RGBA16Float, "RGBA16Float", false},
            {RHI::Format::RGBA32Float, "RGBA32Float", false},
        };
        for (const FormatCase &c : cases) {
            auto format_target = harness.renderer.create_offscreen_render_target(
                OffscreenRenderTargetDescription{.extent = {kSize, kSize}, .format = c.format, .label = "ui format target"});
            if (!format_target) {
                std::cout << "SKIP format " << c.name << ": " << format_target.error().message << '\n';
                continue;
            }
            const OffscreenRenderTargetHandle previous = harness.target;
            harness.target = *format_target;
            UI::UiSurface surface;
            if (!surface.ensure_ready(*harness.renderer.rhi_device(), c.format)) {
                check(false, std::string{"UI surface for "} + c.name);
                harness.target = previous;
                harness.renderer.destroy_offscreen_render_target(*format_target);
                continue;
            }
            bool rendered = true;
            for (int frame = 0; frame < 3 && rendered; ++frame) {
                UI::Context &ctx = surface.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
                {
                    auto root = ctx.element(UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                        .background_color = UI::Color{0.5, 0.5, 0.5, 1.0}});
                    auto element = ctx.element(box(64.0f, UI::Color{1.0, 0.0, 0.0, 1.0}));
                }
                rendered = render_ui_frame(harness, surface);
            }
            check(rendered, std::string{"rendering UI into "} + c.name);
            std::array<f32, 4> red{}, grey{};
            if (rendered && read_texel(harness, *format_target, c.format, 32, 32, red) &&
                read_texel(harness, *format_target, c.format, 200, 200, grey)) {
                const f32 grey_expected = c.srgb_encoded ? 0.5f : 0.2140f;
                const std::string where = std::string{c.name} + ": red " + std::to_string(red[0]) + "," + std::to_string(red[1]) + "," +
                                          std::to_string(red[2]) + " grey " + std::to_string(grey[0]);
                check(red[0] > 0.95f && red[1] < 0.05f && red[2] < 0.05f, "pure red box in " + where);
                check(std::abs(grey[0] - grey_expected) < 0.03f && std::abs(grey[2] - grey_expected) < 0.03f, "grey backdrop in " + where);
            } else if (rendered) {
                check(false, std::string{"reading back "} + c.name);
            }
            surface.destroy(*harness.renderer.rhi_device());
            harness.target = previous;
            harness.renderer.destroy_offscreen_render_target(*format_target);
        }
    }

    // Scene 10: UI layers. One UI: a red/blue backdrop, then an element in layer 1 with frosted glass (the layer is on the effect itself).
    // The glass element sits in the *middle* of the tree, with a plain sibling after it: that sibling must still draw in the lowest layer.
    {
        auto glass = UI::Effects::frosted_glass(16.0f, glm::vec3{1.0f}, 0.0f);
        glass->layer = 1;
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))}});
                {
                    // The red half holds the glass pane (layer 1) at x 96..160; the blue half is a plain sibling drawn after it in the tree.
                    UI::ElementDecl red = box(128.0f, UI::Color{1.0, 0.0, 0.0, 1.0});
                    red.padding = UI::Padding{96, 0, 0, 0};
                    auto left = ctx.element(red);
                    auto pane = ctx.element(box(64.0f, UI::Color{1.0, 1.0, 1.0, 0.02}, glass));
                }
                { auto right = ctx.element(box(128.0f, UI::Color{0.0, 0.0, 1.0, 1.0})); }
            }
            if (!render_layered_ui_frame(harness, ui)) {
                return 1;
            }
        }
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 10\n";
            return 1;
        }
        const Pixel seam_outside = at(image, 124, 100);
        const Pixel seam_inside = at(image, 124, 50);
        check(near(seam_outside, 255, 0, 0, 20), "layers: outside the glass the seam is untouched (got " + str(seam_outside) + ")");
        check(seam_inside.r < 235 && seam_inside.r > 60 && seam_inside.b > 40,
              "layers: inside the glass the seam is blurred (got " + str(seam_inside) + ")");
    }

    // Scene 11: dependency-aware parallelism. A base layer, and on top of it two frosted-glass widgets that each read the base layer but
    // not each other. The render graph must find the widgets' passes independent (same execution level, so they are recorded in
    // parallel), and must still order each widget's own chain (content and backdrop, then its effect) and the base layer before them.
    {
        auto glass_a = UI::Effects::frosted_glass(12.0f, glm::vec3{1.0f}, 0.0f);
        glass_a->layer = 1;
        glass_a->name = UString{"glass A"};
        auto glass_b = UI::Effects::frosted_glass(12.0f, glm::vec3{1.0f}, 0.0f);
        glass_b->layer = 1;
        glass_b->name = UString{"glass B"};
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))}});
                {
                    UI::ElementDecl red = box(128.0f, UI::Color{1.0, 0.0, 0.0, 1.0});
                    red.padding = UI::Padding{32, 0, 0, 0};
                    auto left = ctx.element(red);
                    auto pane = ctx.element(box(64.0f, UI::Color{1.0, 1.0, 1.0, 0.02}, glass_a));
                }
                {
                    UI::ElementDecl blue = box(128.0f, UI::Color{0.0, 0.0, 1.0, 1.0});
                    blue.padding = UI::Padding{32, 0, 0, 0};
                    auto right = ctx.element(blue);
                    auto pane = ctx.element(box(64.0f, UI::Color{1.0, 1.0, 1.0, 0.02}, glass_b));
                }
            }
            if (!render_layered_ui_frame(harness, ui)) {
                return 1;
            }
        }
        const auto plan = harness.renderer.last_frame_execution_plan(harness.surface);
        const auto level_of = [&](const std::string &needle) -> std::optional<u32> {
            for (const auto &pass : plan) {
                if (pass.label.cpp_string().find(needle) != std::string::npos) return pass.level;
            }
            return std::nullopt;
        };
        for (const auto &pass : plan) {
            std::cout << "  level " << pass.level << ": " << pass.label.cpp_string() << '\n';
        }
        for (const char *kind : {"ui effect content: ", "ui effect backdrop: "}) {
            const auto a = level_of(std::string{kind} + "glass A");
            const auto b = level_of(std::string{kind} + "glass B");
            check(a && b && *a == *b, std::string{"the two widgets' '"} + kind + "' passes land on the same level (they are independent)");
        }
        const auto capture_a = level_of("ui effect backdrop: glass A");
        const auto effect_a = level_of("ui effect: effect 'glass A' pass 0");
        const auto effect_b = level_of("ui effect: effect 'glass B' pass 0");
        check(effect_a && effect_b && *effect_a == *effect_b, "the two widgets' effect passes land on the same level");
        check(capture_a && effect_a && *capture_a < *effect_a, "a widget's effect pass waits for its own backdrop capture");
        check(capture_a && *capture_a > 0, "the backdrop capture waits for the base layer");
        {
            // Both widgets' content was prepared on workers, each recording its uploads on an encoder of its own.
            u32 parallel = 0;
            parallel = ui.render_stats().parallel_group_prepares;
            std::cout << "  parallel group prepares: " << parallel << "\n";
            check(parallel == 2 || std::thread::hardware_concurrency() <= 1, "the two widgets' content is prepared in parallel");
        }
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 11\n";
            return 1;
        }
        const Pixel pane_a = at(image, 64, 32);
        const Pixel pane_b = at(image, 192, 32);
        check(near(pane_a, 255, 0, 0, 45), "widget A shows the red base layer through its glass (got " + str(pane_a) + ")");
        check(near(pane_b, 0, 0, 255, 45), "widget B shows the blue base layer through its glass (got " + str(pane_b) + ")");
    }

    // Scene 12: per-edge borders. A white 96 px box with an 8 px border: left red, top green, right dashed blue, bottom absent.
    {
        auto edges = std::make_shared<UI::BorderEdges>();
        edges->left = UI::BorderEdge{UI::Color{1.0, 0.0, 0.0, 1.0}, UI::BorderLineStyle::Solid};
        edges->top = UI::BorderEdge{UI::Color{0.0, 1.0, 0.0, 1.0}, UI::BorderLineStyle::Solid};
        edges->right = UI::BorderEdge{UI::Color{0.0, 0.0, 1.0, 1.0}, UI::BorderLineStyle::Dashed};
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                    .padding = UI::Padding{64, 0, 64, 0}});
                UI::ElementDecl framed = box(96.0f, UI::Color{1.0, 1.0, 1.0, 1.0});
                framed.border = UI::BorderStyle{.color = UI::Color{1.0, 1.0, 1.0, 1.0}, .width = UI::BorderWidth{8, 8, 8, 8}};
                framed.border_edges = edges;
                auto element = ctx.element(framed);
            }
            if (!render_ui_frame(harness, ui)) {
                return 1;
            }
        }
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 12\n";
            return 1;
        }
        const Pixel left_edge = at(image, 64 + 3, 112);
        const Pixel top_edge = at(image, 112, 64 + 3);
        const Pixel bottom_edge = at(image, 112, 64 + 96 - 3);
        const Pixel inside = at(image, 112, 112);
        check(near(left_edge, 255, 0, 0, 30), "the left edge is red (got " + str(left_edge) + ")");
        check(near(top_edge, 0, 255, 0, 30), "the top edge is green (got " + str(top_edge) + ")");
        check(near(bottom_edge, 255, 255, 255, 30), "an edge with no colour shows the element's own background (got " + str(bottom_edge) + ")");
        check(near(inside, 255, 255, 255, 30), "the inside is untouched (got " + str(inside) + ")");
        // The right edge is dashed: along it there are blue dashes and gaps showing the background.
        int blue = 0;
        int white = 0;
        for (u32 y = 64 + 8; y < 64 + 96 - 8; ++y) {
            const Pixel p = at(image, 64 + 96 - 4, y);
            blue += near(p, 0, 0, 255, 40);
            white += near(p, 255, 255, 255, 40);
        }
        check(blue > 10 && white > 10, "the right edge alternates blue dashes and gaps (blue " + std::to_string(blue) + ", gaps " + std::to_string(white) + ")");
    }

    // Scene 13: 2D transforms. A 64x32 box turned a quarter turn about its centre (it becomes 32 wide and 64 tall, centred where it was), and
    // another moved 100 px down and scaled to half height.
    {
        const auto rotate = UI::Effects::transform({0.0f, 0.0f}, 1.57079632679f, glm::vec2{1.0f}, glm::vec2{0.5f}, glm::vec2{64.0f, 32.0f});
        const auto move = UI::Effects::transform({0.0f, 100.0f}, 0.0f, glm::vec2{1.0f, 0.5f}, glm::vec2{0.5f}, glm::vec2{64.0f, 32.0f});
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                    .padding = UI::Padding{16, 0, 16, 0},
                    .child_gap = 48});
                {
                    UI::ElementDecl a = box(64.0f, UI::Color{1.0, 0.0, 0.0, 1.0}, rotate);
                    a.sizing = {UI::SizingAxis::fixed(64.0f), UI::SizingAxis::fixed(32.0f)};
                    auto element = ctx.element(a);
                }
                {
                    UI::ElementDecl b = box(64.0f, UI::Color{0.0, 0.0, 1.0, 1.0}, move);
                    b.sizing = {UI::SizingAxis::fixed(64.0f), UI::SizingAxis::fixed(32.0f)};
                    auto element = ctx.element(b);
                }
            }
            if (!render_ui_frame(harness, ui)) {
                return 1;
            }
        }
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 13\n";
            return 1;
        }
        // A: centre (48, 32). Rotated it covers x 32..64, y 0..64. The original left end (20, 32) is now empty, and above the old box (48, 6) is red.
        check(near(at(image, 48, 6), 255, 0, 0, 30), "the rotated box reaches above where it was (got " + str(at(image, 48, 6)) + ")");
        check(near(at(image, 20, 32), 0, 0, 0, 14), "the rotated box no longer covers its old left end (got " + str(at(image, 20, 32)) + ")");
        check(near(at(image, 48, 32), 255, 0, 0, 30), "the rotated box still covers its centre");
        // B: at x 128..192, y 16..48, scaled to half height about the centre (y 24..40) and moved down 100: y 124..140.
        check(near(at(image, 160, 130), 0, 0, 255, 30), "the moved box is where it was sent (got " + str(at(image, 160, 130)) + ")");
        check(near(at(image, 160, 32), 0, 0, 0, 14), "and gone from where it was (got " + str(at(image, 160, 32)) + ")");
        check(near(at(image, 160, 120), 0, 0, 0, 14), "the half-height box does not reach above its new top (got " + str(at(image, 160, 120)) + ")");
    }

    // Scene 14: clip-path and masks. Six 64 px white boxes in a row-and-a-half: a circle, a triangle, an inset with rounded corners, a fade-out
    // mask along x, an image mask (left half hidden) and the same inverted.
    {
        const glm::vec2 triangle[3] = {{0.5f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        const auto circle = UI::Effects::clip_circle(24.0f);
        const auto tri = UI::Effects::clip_polygon(triangle);
        const auto inset = UI::Effects::clip_inset(16.0f, 16.0f, 16.0f, 16.0f, 8.0f);
        const auto fade = UI::Effects::mask_linear(0.0f, 1.0f, 0.0f);
        const std::byte mask_pixels[8] = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
        auto mask_texture = harness.renderer.create_texture(2, 1, RHI::Format::RGBA8Unorm, mask_pixels, "ui mask test", {}, 1, true);
        if (!mask_texture) {
            std::cerr << "FAILED: mask texture\n";
            return 1;
        }
        const auto image_mask = UI::Effects::mask_image(*mask_texture);
        const auto image_mask_inverted = UI::Effects::mask_image(*mask_texture, false, true);
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                    .padding = UI::Padding{8, 0, 8, 0},
                    .child_gap = 36,
                    .direction = UI::LayoutDirection::TopToBottom});
                {
                    auto row = ctx.element(UI::ElementDecl{.child_gap = 8});
                    const std::shared_ptr<const UI::UiEffect> effects[3] = {circle, tri, inset};
                    for (const auto &effect : effects) {
                        auto element = ctx.element(box(64.0f, UI::Color{1.0, 1.0, 1.0, 1.0}, effect));
                    }
                }
                {
                    auto row = ctx.element(UI::ElementDecl{.child_gap = 8});
                    const std::shared_ptr<const UI::UiEffect> effects[3] = {fade, image_mask, image_mask_inverted};
                    for (const auto &effect : effects) {
                        auto element = ctx.element(box(64.0f, UI::Color{1.0, 1.0, 1.0, 1.0}, effect));
                    }
                }
            }
            if (!render_ui_frame(harness, ui)) {
                return 1;
            }
        }
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 14\n";
            return 1;
        }
        // Row one: boxes at x 8..72, 80..144, 152..216; y 8..72.
        check(near(at(image, 40, 40), 255, 255, 255, 12), "the circle's middle is drawn (got " + str(at(image, 40, 40)) + ")");
        check(near(at(image, 12, 12), 0, 0, 0, 14), "the circle's corner is cut away (got " + str(at(image, 12, 12)) + ")");
        check(near(at(image, 112, 60), 255, 255, 255, 12), "the triangle's base is drawn (got " + str(at(image, 112, 60)) + ")");
        check(near(at(image, 84, 14), 0, 0, 0, 14), "the triangle's upper left is cut away (got " + str(at(image, 84, 14)) + ")");
        check(near(at(image, 184, 40), 255, 255, 255, 12), "the inset keeps its middle (got " + str(at(image, 184, 40)) + ")");
        check(near(at(image, 156, 40), 0, 0, 0, 14), "the inset cuts 16 px off the left (got " + str(at(image, 156, 40)) + ")");
        // Row two (y 108..172: 8 + 64 + 36): the fade is bright on the left and gone on the right; the image mask hides the left half, inverted the right.
        const Pixel fade_left = at(image, 12, 140);
        const Pixel fade_right = at(image, 68, 140);
        check(fade_left.r > 200 && fade_right.r < 100, "the linear mask fades from left to right (left " + str(fade_left) + ", right " + str(fade_right) + ")");
        check(near(at(image, 84, 140), 0, 0, 0, 14) && near(at(image, 140, 140), 255, 255, 255, 14),
              "the image mask hides its transparent half (got " + str(at(image, 84, 140)) + " / " + str(at(image, 140, 140)) + ")");
        check(near(at(image, 156, 140), 255, 255, 255, 14) && near(at(image, 212, 140), 0, 0, 0, 14), "inverted, it hides the other half");
        harness.renderer.destroy_texture(*mask_texture);
    }

    // Scene 15: clipping to shapes. Three 96 px boxes holding a 96 px red child that overflows them: one clipped to its own rounded corners
    // (radius 40), one to notched corners (radius 32: the corner squares are cut out), one to a circle path.
    {
        const auto circle_path = UI::Effects::clip_circle(40.0f);
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                    .padding = UI::Padding{8, 0, 8, 0},
                    .child_gap = 8});
                const auto holder = [&](UI::ElementDecl decl) {
                    decl.sizing = {UI::SizingAxis::fixed(72.0f), UI::SizingAxis::fixed(72.0f)};
                    auto outer = ctx.element(decl);
                    auto child = ctx.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(72.0f), UI::SizingAxis::fixed(72.0f)},
                                                              .background_color = UI::Color{1.0, 0.0, 0.0, 1.0}});
                };
                UI::ElementDecl rounded;
                rounded.corner_radius = UI::CornerRadius::all(30.0f);
                rounded.clip.follow_corners = true;
                holder(rounded);
                UI::ElementDecl notched;
                notched.corner_radius = UI::CornerRadius::all(24.0f);
                notched.corner_shapes = UI::CornerShapes::all(UI::CornerShape::notch());
                notched.clip.follow_corners = true;
                holder(notched);
                UI::ElementDecl pathed;
                pathed.clip.path = circle_path;
                holder(pathed);
            }
            if (!render_ui_frame(harness, ui)) {
                return 1;
            }
        }
        Image image;
        if (!read_back(harness, image)) {
            std::cerr << "FAILED: readback 15\n";
            return 1;
        }
        // Boxes at x 8..80, 88..160, 168..240; y 8..80.
        check(near(at(image, 44, 44), 255, 0, 0, 12), "rounded: the middle shows the child (got " + str(at(image, 44, 44)) + ")");
        check(near(at(image, 10, 10), 0, 0, 0, 14), "rounded: the child is cut at the corner (got " + str(at(image, 10, 10)) + ")");
        check(near(at(image, 44, 10), 255, 0, 0, 12), "rounded: the straight edge is kept (got " + str(at(image, 44, 10)) + ")");
        check(near(at(image, 90, 10), 0, 0, 0, 14), "notched: the corner square is cut out (got " + str(at(image, 90, 10)) + ")");
        check(near(at(image, 124, 10), 255, 0, 0, 12), "notched: between the corners is kept (got " + str(at(image, 124, 10)) + ")");
        check(near(at(image, 204, 44), 255, 0, 0, 12), "path: the circle's middle is kept (got " + str(at(image, 204, 44)) + ")");
        check(near(at(image, 170, 10), 0, 0, 0, 14), "path: outside the circle is cut (got " + str(at(image, 170, 10)) + ")");
    }

    ui.destroy(*harness.renderer.rhi_device());
    harness.renderer.wait_idle();
    harness_owner.reset();
    SFT::Async::Scheduler::shutdown();
    if (g_failures == 0) {
        std::cout << "UiEffectGpuTest: ok\n";
    }
    return g_failures == 0 ? 0 : 1;
}
