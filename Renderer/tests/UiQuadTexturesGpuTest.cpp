#include <Async/Async.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/UI/UI.hpp>
#include <WindowManager/WindowManager.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>

#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
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
            .label = "ui quad textures test readback",
        });
        if (!buffer) return false;
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "ui quad textures test readback"});
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
        auto fence = device->create_fence(RHI::FenceDesc{.label = "ui quad textures test readback fence"});
        if (!fence) return false;
        const RHI::CommandBufferHandle buffers[1] = {*command_buffer};
        RHI::SubmitDesc submit{
            .command_buffers = std::span<const RHI::CommandBufferHandle>{buffers, 1},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "ui quad textures test readback submit",
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

} // namespace

int main() {
    namespace fs = std::filesystem;
    fs::current_path(fs::path(__FILE__).parent_path().parent_path().parent_path());

    auto harness_owner = std::make_unique<Harness>();
    Harness &harness = *harness_owner;

    SFT::WindowManager::WindowManager window_manager;
    SFT::WindowManager::WindowConfig window_config{};
    window_config.title = "UiQuadTexturesGpuTest";
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
        info.app_name = "UiQuadTexturesGpuTest";
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
        OffscreenRenderTargetDescription{.extent = {kSize, kSize}, .label = "ui quad textures test target"});
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

    // Four 4x4 solid-colour textures.
    const std::array<std::array<u8, 4>, 4> colors{{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}}};
    std::array<TextureHandle, 4> textures{};
    for (usize i = 0; i < textures.size(); ++i) {
        std::vector<std::byte> pixels(4u * 4u * 4u);
        for (usize p = 0; p < 16; ++p) {
            for (usize c = 0; c < 4; ++c) {
                pixels[p * 4 + c] = static_cast<std::byte>(colors[i][c]);
            }
        }
        auto created = harness.renderer.create_texture(4, 4, RHI::Format::RGBA8Unorm,
                                                       std::span<const std::byte>{pixels.data(), pixels.size()}, "quad test texture");
        if (!created) {
            std::cerr << "FAILED: texture: " << created.error().message << '\n';
            return 1;
        }
        textures[i] = *created;
    }

    UI::UiRenderStats stats{};
    for (int frame = 0; frame < 4; ++frame) {
        UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
        {
            auto root = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
            });
            for (const TextureHandle texture : textures) {
                ctx.image(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(64.0f), UI::SizingAxis::fixed(64.0f)}}, texture);
            }
        }
        if (!render_ui_frame(harness, ui)) {
            return 1;
        }
        stats = ui.render_stats();
    }

    Image image;
    if (!read_back(harness, image)) {
        std::cerr << "FAILED: readback\n";
        return 1;
    }
    for (u32 i = 0; i < 4; ++i) {
        const u8 *pixel = image.at(32 + 64 * i, 32);
        const bool match = (pixel[0] > 200) == (colors[i][0] > 200) && (pixel[1] > 200) == (colors[i][1] > 200) &&
                           (pixel[2] > 200) == (colors[i][2] > 200) && (pixel[0] < 60 || pixel[0] > 200) &&
                           (pixel[1] < 60 || pixel[1] > 200) && (pixel[2] < 60 || pixel[2] > 200);
        check(match, "quad " + std::to_string(i) + " must show its own texture (got " + std::to_string(pixel[0]) + "," +
                         std::to_string(pixel[1]) + "," + std::to_string(pixel[2]) + ")");
    }
    std::cout << "bindless=" << stats.bindless_textures << " quads=" << stats.quads << " batches=" << stats.quad_batches
              << " textures=" << stats.quad_textures << " bind_groups_created(last frame)=" << stats.bind_groups_created
              << " bytes_uploaded(last frame)=" << stats.quad_bytes_uploaded << '\n';
    check(stats.quads == 4, "four quads must be drawn");
    if (stats.bindless_textures) {
        check(stats.quad_batches == 1, "with textures in one table the four quads must be one batch");
        check(stats.quad_textures == 4, "the table must hold the four textures");
    }
    check(stats.bind_groups_created == 0, "a frame identical to the last must create no bind groups");
    check(stats.quad_bytes_uploaded == 0, "a frame identical to the last must upload nothing");

    // Paint order: plain quads and a stroke. A red quad, a green stroke across the middle and a yellow quad, laid out in a
    // row; the stroke touches neither quad, so the yellow one may be drawn with the red one (a single quad batch) without
    // anything changing on screen. Then a blue quad floats over the stroke and has to stay on top of it.
    const auto run_scene = [&](bool with_overlap, UI::UiRenderStats &out_stats, Image &out_image) {
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                    .id = UString{"scene"_ustr},
                });
                {
                    auto red = ctx.element(UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(64.0f), UI::SizingAxis::fixed(64.0f)},
                        .background_color = UI::Color{1.0, 0.25, 0.0, 1.0}});
                }
                const auto spacer = [&] {
                    auto gap = ctx.element(UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(24.0f), UI::SizingAxis::fixed(64.0f)}});
                };
                spacer();
                const std::array<glm::vec2, 2> line{glm::vec2{0.0f, 32.0f}, glm::vec2{96.0f, 32.0f}};
                ctx.stroke_polyline(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(96.0f), UI::SizingAxis::fixed(64.0f)}},
                                    line, UI::StrokeStyle{.color = UI::Color{0.0, 1.0, 0.0, 1.0}, .width = 12.0f});
                spacer();
                {
                    auto yellow = ctx.element(UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(64.0f), UI::SizingAxis::fixed(64.0f)},
                        .background_color = UI::Color{1.0, 1.0, 0.0, 1.0}});
                }
                if (with_overlap) {
                    UI::ElementDecl blue{
                        .sizing = {UI::SizingAxis::fixed(40.0f), UI::SizingAxis::fixed(40.0f)},
                        .background_color = UI::Color{0.0, 0.0, 1.0, 1.0}};
                    blue.floating.attach_to = UI::FloatingAttachTo::Parent;
                    blue.floating.element_attach_point = UI::FloatingAttachPoint::LeftTop;
                    blue.floating.parent_attach_point = UI::FloatingAttachPoint::LeftTop;
                    blue.floating.offset = {100.0f, 12.0f};
                    blue.floating.z_index = 1;
                    auto floating_blue = ctx.element(blue);
                }
            }
            if (!render_ui_frame(harness, ui)) {
                return false;
            }
            out_stats = ui.render_stats();
        }
        return read_back(harness, out_image);
    };
    {
        UI::UiRenderStats scene_stats{};
        Image scene;
        if (!run_scene(false, scene_stats, scene)) {
            std::cerr << "FAILED: scene 1\n";
            return 1;
        }
        const u8 *red = scene.at(32, 32);
        const u8 *green = scene.at(136, 32); // the stroke spans x 88..184
        const u8 *yellow = scene.at(232, 32);
        // Its green channel is a quarter: half floats carry fractions, not just 0 and 1.
        check(red[0] > 200 && red[1] > 58 && red[1] < 70 && red[2] < 10,
              "scene 1: the orange quad must keep its colour through the half-float record (got " + std::to_string(red[0]) + "," +
                  std::to_string(red[1]) + "," + std::to_string(red[2]) + ")");
        check(green[1] > 200 && green[0] < 60 && green[2] < 60, "scene 1: the stroke must be green");
        check(yellow[0] > 200 && yellow[1] > 200 && yellow[2] < 60, "scene 1: the yellow quad must be yellow");
        std::cout << "scene 1: quads=" << scene_stats.quads << " quad_batches=" << scene_stats.quad_batches << '\n';
        const char *reorder = std::getenv("STURDY_UI_REORDER");
        if (reorder == nullptr || std::string{reorder} != "0") {
            check(scene_stats.quad_batches == 1, "scene 1: the two quads that the stroke does not touch must share a batch");
        }
    }
    {
        UI::UiRenderStats scene_stats{};
        Image scene;
        if (!run_scene(true, scene_stats, scene)) {
            std::cerr << "FAILED: scene 2\n";
            return 1;
        }
        const u8 *over_stroke = scene.at(120, 32);
        const u8 *stroke_only = scene.at(96, 32);
        check(over_stroke[2] > 200 && over_stroke[0] < 60 && over_stroke[1] < 60,
              "scene 2: a quad painted after the stroke and overlapping it must stay on top of it (got " +
                  std::to_string(over_stroke[0]) + "," + std::to_string(over_stroke[1]) + "," + std::to_string(over_stroke[2]) + ")");
        check(stroke_only[1] > 200 && stroke_only[0] < 60, "scene 2: the stroke must still show where nothing covers it");
    }

    // Text and a quad that overlaps it: the quad is painted after the text and has to cover it, whichever draw group each
    // ends up in. (The text uses the default font; where no font can be had the glyph count is zero and only the quad is checked.)
    {
        UI::UiRenderStats text_stats{};
        Image scene;
        for (int frame = 0; frame < 3; ++frame) {
            UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
            {
                auto root = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                    .id = UString{"text scene"_ustr},
                });
                ctx.text("MMMM"_ustr, UI::TextStyle{.font_size = 56});
                UI::ElementDecl cover{
                    .sizing = {UI::SizingAxis::fixed(60.0f), UI::SizingAxis::fixed(30.0f)},
                    .background_color = UI::Color{1.0, 0.0, 0.0, 1.0}};
                cover.floating.attach_to = UI::FloatingAttachTo::Parent;
                cover.floating.element_attach_point = UI::FloatingAttachPoint::LeftTop;
                cover.floating.parent_attach_point = UI::FloatingAttachPoint::LeftTop;
                cover.floating.offset = {20.0f, 20.0f};
                cover.floating.z_index = 1;
                auto floating_cover = ctx.element(cover);
            }
            if (!render_ui_frame(harness, ui)) {
                return 1;
            }
            text_stats = ui.render_stats();
        }
        if (!read_back(harness, scene)) {
            std::cerr << "FAILED: text scene readback\n";
            return 1;
        }
        const u8 *covered = scene.at(50, 35);
        check(covered[0] > 200 && covered[1] < 60 && covered[2] < 60,
              "text scene: the quad painted after the text must cover it (got " + std::to_string(covered[0]) + "," +
                  std::to_string(covered[1]) + "," + std::to_string(covered[2]) + ")");
        usize bright = 0;
        for (u32 y = 0; y < 64; ++y) {
            for (u32 x = 0; x < 256; ++x) {
                const u8 *pixel = scene.at(x, y);
                if (pixel[0] > 150 && pixel[1] > 150 && pixel[2] > 150) {
                    ++bright;
                }
            }
        }
        std::cout << "text scene: glyphs=" << text_stats.glyphs << " text_batches=" << text_stats.text_batches
                  << " quad_batches=" << text_stats.quad_batches << " bright_pixels=" << bright << '\n';
        if (text_stats.glyphs > 0) {
            check(bright > 100, "text scene: the text must be drawn where nothing covers it");
        }
    }

    ui.destroy(*harness.renderer.rhi_device());
    harness.renderer.wait_idle();
    harness_owner.reset();
    SFT::Async::Scheduler::shutdown();
    if (g_failures == 0) {
        std::cout << "UiQuadTexturesGpuTest: ok\n";
    }
    return g_failures == 0 ? 0 : 1;
}
