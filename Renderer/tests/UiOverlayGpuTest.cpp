#include <Async/Async.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/UI/UI.hpp>
#include <WindowManager/WindowManager.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Headless GPU test for the "UI in the world" path: a UI::UiSurface is laid out at an off-screen target's extent,
// drawn as an OverlayPass on an overlay-only frame, and the target's pixels are read back. The pointer is driven
// not by a window but by a ray cast at a quad in the world (UI::ui_uv_at_ray -> UiInput), which is exactly what
// an in-world panel does. Checks:
//   * the overlay reaches the target (a red panel lands where the layout put it, elsewhere is the frame's clear);
//   * a ray that hits the panel makes the panel hovered; a ray that misses does not;
//   * several frames run (prepare/draw resources are reused across the frame ring).
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
            .label = "ui overlay test readback",
        });
        if (!buffer) return false;
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "ui overlay test readback"});
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
        auto fence = device->create_fence(RHI::FenceDesc{.label = "ui overlay test readback fence"});
        if (!fence) return false;
        const RHI::CommandBufferHandle buffers[1] = {*command_buffer};
        RHI::SubmitDesc submit{
            .command_buffers = std::span<const RHI::CommandBufferHandle>{buffers, 1},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "ui overlay test readback submit",
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
        desc.view.render_graph.tone_mapping = false;
        desc.view.render_graph.bloom = false;
        desc.view.render_graph.shadows = false;
        desc.view.render_graph.ambient_occlusion = false;
        desc.view.render_graph.wait_for_completion = true;
        desc.view.render_graph.overlay_passes.push_back(ui.finish_overlay(&h.renderer));
        if (Core::RendererResult r = h.renderer.render_frame(desc); !r) {
            std::cerr << "render_frame failed: " << r.error().message << '\n';
            return false;
        }
        h.renderer.wait_idle();
        return true;
    }

    /// The panel the test UI puts at the top-left quarter of the target.
    void build_ui(UI::Context &ctx) {
        auto root = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
        });
        auto panel = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::fixed(128.0f), UI::SizingAxis::fixed(128.0f)},
            .background_color = UI::Color{1.0, 0.0, 0.0, 1.0},
            .id = UString{"panel"_ustr},
        });
    }

} // namespace

int main() {
    namespace fs = std::filesystem;
    fs::current_path(fs::path(__FILE__).parent_path().parent_path().parent_path());

    auto harness_owner = std::make_unique<Harness>();
    Harness &harness = *harness_owner;

    SFT::WindowManager::WindowManager window_manager;
    SFT::WindowManager::WindowConfig window_config{};
    window_config.title = "UiOverlayGpuTest";
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
        info.app_name = "UiOverlayGpuTest";
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
        OffscreenRenderTargetDescription{.extent = {kSize, kSize}, .label = "ui overlay test target"});
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

    // The UI hangs in the world as a 2x2 quad facing +z, top-left at (-1, 1, 0); the user is a ray from (0, 0, 5).
    const UI::UiWorldPlane plane{.origin = {-1.0f, 1.0f, 0.0f}, .u_axis = {2.0f, 0.0f, 0.0f}, .v_axis = {0.0f, -2.0f, 0.0f}};
    const glm::vec3 eye{0.0f, 0.0f, 5.0f};
    const auto point_at_pixel = [&](f32 px, f32 py) {
        const glm::vec3 world{-1.0f + 2.0f * px / kSize, 1.0f - 2.0f * py / kSize, 0.0f};
        return UI::ui_uv_at_ray(plane, eye, world - eye);
    };

    bool hovered_when_pointing_at_panel = false;
    bool hovered_when_pointing_away = true;
    for (int i = 0; i < 4; ++i) {
        const bool aim_at_panel = i < 3;
        // Pixel (64, 64) is inside the 128x128 panel; (200, 200) is outside it.
        const auto uv = aim_at_panel ? point_at_pixel(64.0f, 64.0f) : point_at_pixel(200.0f, 200.0f);
        check(uv.has_value(), "the aim ray must hit the quad");
        if (uv) {
            ui.input().move_pointer(UI::ui_pixel_from_uv(*uv, glm::vec2{static_cast<f32>(kSize)}));
        }
        UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
        build_ui(ctx);
        // Hover is resolved against the previous frame's layout, so read it once a frame has been laid out.
        if (i == 2) hovered_when_pointing_at_panel = ctx.hovered(UString{"panel"_ustr});
        if (i == 3) hovered_when_pointing_away = ctx.hovered(UString{"panel"_ustr});
        if (!render_ui_frame(harness, ui)) {
            return 1;
        }
    }
    check(hovered_when_pointing_at_panel, "a ray that hits the panel must hover it");
    check(!hovered_when_pointing_away, "a ray that lands elsewhere must not hover the panel");

    // A ray that misses the quad entirely produces no pointer position.
    check(!UI::ui_uv_at_ray(plane, eye, glm::vec3{5.0f, 0.0f, -5.0f}).has_value(), "a ray off to the side must miss the quad");

    Image image;
    if (!read_back(harness, image)) {
        std::cerr << "FAILED: readback\n";
        return 1;
    }
    const u8 *inside = image.at(64, 64);
    const u8 *outside = image.at(200, 200);
    // Rows are BGR->RGB swapped by read_back: rgb[0] is red.
    check(inside[0] > 200 && inside[1] < 60 && inside[2] < 60,
          "the panel pixel must be red (got " + std::to_string(inside[0]) + "," + std::to_string(inside[1]) + "," + std::to_string(inside[2]) + ")");
    check(!(outside[0] > 200 && outside[1] < 60 && outside[2] < 60), "outside the panel must not be red");

    ui.destroy(*harness.renderer.rhi_device());
    harness.renderer.wait_idle();
    // The renderer's swapchain must go before the window it presents to.
    harness_owner.reset();
    // Render recording uses Async::Scheduler workers; leaving them running at exit terminates the process.
    SFT::Async::Scheduler::shutdown();
    if (g_failures == 0) {
        std::cout << "UiOverlayGpuTest: ok\n";
    }
    return g_failures == 0 ? 0 : 1;
}
