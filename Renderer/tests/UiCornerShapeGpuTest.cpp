// Headless GPU test for UI corner shapes (CSS `corner-shape`): boxes with a 24 px radius are drawn with each profile and the pixels
// along the corner's diagonal are read back. On the diagonal the profiles' edges fall at well-separated, analytically known depths
// (measured from the box corner): square 0 px, squircle ~3.8, round ~7.0, bevel 12, scoop ~17.0, notch 24. A pixel a few px deeper than
// the edge must be filled and one a few px shallower must be empty. It needs a real Vulkan device: otherwise it prints SKIP and succeeds.

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
            .label = "ui corner shape test readback",
        });
        if (!buffer) return false;
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "ui corner shape test readback"});
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
        auto fence = device->create_fence(RHI::FenceDesc{.label = "ui corner shape test readback fence"});
        if (!fence) return false;
        const RHI::CommandBufferHandle buffers[1] = {*command_buffer};
        RHI::SubmitDesc submit{
            .command_buffers = std::span<const RHI::CommandBufferHandle>{buffers, 1},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "ui corner shape test readback submit",
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

    /// True when the pixel is clearly drawn (the box is white on a dark clear colour).
    bool filled(const Image &image, u32 x, u32 y) {
        const u8 *p = image.at(x, y);
        return p[0] > 170 && p[1] > 170 && p[2] > 170;
    }
    bool empty(const Image &image, u32 x, u32 y) {
        const u8 *p = image.at(x, y);
        return p[0] < 70 && p[1] < 70 && p[2] < 70;
    }

    struct Profile {
        const char *name;
        UI::CornerShape shape;
        f32 edge_depth; // where the edge crosses the diagonal, px from the box corner (radius 24)
    };

} // namespace

int main() {
    namespace fs = std::filesystem;
    fs::current_path(fs::path(__FILE__).parent_path().parent_path().parent_path());

    auto harness_owner = std::make_unique<Harness>();
    Harness &harness = *harness_owner;

    SFT::WindowManager::WindowManager window_manager;
    SFT::WindowManager::WindowConfig window_config{};
    window_config.title = "UiCornerShapeGpuTest";
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
        info.app_name = "UiCornerShapeGpuTest";
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
        OffscreenRenderTargetDescription{.extent = {kSize, kSize}, .label = "ui corner shape test target"});
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

    // The encoding must round-trip on the CPU before it is trusted on the GPU.
    check(UI::encode_corner_shape(UI::CornerShape::round()) == 0, "round encodes as the default byte 0");
    check(UI::encode_corner_shape(UI::CornerShape::square()) == 253, "square encodes as 253");
    check(UI::encode_corner_shape(UI::CornerShape::notch()) == 1, "notch encodes as 1");
    check(UI::encode_corner_shape(UI::CornerShape::bevel()) == 127, "bevel encodes as 127");
    check(std::abs(UI::decode_corner_shape_k(UI::encode_corner_shape(UI::CornerShape::scoop())) + 1.0f) < 0.02f, "scoop survives the encoding");
    check(std::abs(UI::decode_corner_shape_k(UI::encode_corner_shape(UI::CornerShape::squircle())) - 2.0f) < 0.05f, "squircle survives the encoding");

    constexpr f32 kBox = 64.0f;
    constexpr f32 kRadius = 24.0f;
    const std::array<Profile, 6> profiles{{
        {"square", UI::CornerShape::square(), 0.0f},
        {"squircle", UI::CornerShape::squircle(), 3.8f},
        {"round", UI::CornerShape::round(), 7.0f},
        {"bevel", UI::CornerShape::bevel(), 12.0f},
        {"scoop", UI::CornerShape::scoop(), 17.0f},
        {"notch", UI::CornerShape::notch(), 24.0f},
    }};
    const UI::CornerShapes mixed{UI::CornerShape::notch(), UI::CornerShape::square(), UI::CornerShape::scoop(), UI::CornerShape::bevel()};

    for (int frame = 0; frame < 3; ++frame) {
        UI::Context &ctx = ui.begin_frame({static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0f / 60.0f);
        {
            auto root = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(static_cast<f32>(kSize)), UI::SizingAxis::fixed(static_cast<f32>(kSize))},
                .direction = UI::LayoutDirection::TopToBottom,
            });
            const auto box = [&](const UI::CornerShapes &shapes) {
                auto element = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(kBox), UI::SizingAxis::fixed(kBox)},
                    .background_color = UI::Color{1.0, 1.0, 1.0, 1.0},
                    .corner_radius = UI::CornerRadius::all(kRadius),
                    .corner_shapes = shapes,
                });
            };
            for (int row = 0; row < 3; ++row) {
                auto line = ctx.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fixed(kBox)}});
                if (row < 2) {
                    for (int column = 0; column < 3; ++column) {
                        box(UI::CornerShapes::all(profiles[static_cast<usize>(row * 3 + column)].shape));
                    }
                } else {
                    box(mixed);
                }
            }
        }
        if (!render_ui_frame(harness, ui)) {
            return 1;
        }
    }

    Image image;
    if (!read_back(harness, image)) {
        std::cerr << "FAILED: readback\n";
        return 1;
    }

    // Uniform boxes: probe the top-left corner (diagonal inwards from the box corner) and the opposite bottom-right corner.
    for (usize i = 0; i < profiles.size(); ++i) {
        const u32 x0 = static_cast<u32>((i % 3) * 64);
        const u32 y0 = static_cast<u32>((i / 3) * 64);
        const Profile &profile = profiles[i];
        const auto probe = [&](f32 depth, bool top_left, bool expect_filled) {
            const i32 d = static_cast<i32>(std::lround(depth));
            const u32 x = top_left ? x0 + static_cast<u32>(d) : x0 + 63u - static_cast<u32>(d);
            const u32 y = top_left ? y0 + static_cast<u32>(d) : y0 + 63u - static_cast<u32>(d);
            const bool ok = expect_filled ? filled(image, x, y) : empty(image, x, y);
            const u8 *p = image.at(x, y);
            check(ok, std::string{profile.name} + " " + (top_left ? "top-left" : "bottom-right") + ": the pixel " + std::to_string(d) +
                          " px along the diagonal must be " + (expect_filled ? "filled" : "empty") + " (got " + std::to_string(p[0]) +
                          "," + std::to_string(p[1]) + "," + std::to_string(p[2]) + ")");
        };
        for (const bool top_left : {true, false}) {
            probe(profile.edge_depth + 3.0f, top_left, true);
            if (profile.edge_depth > 3.5f) {
                probe(profile.edge_depth - 3.0f, top_left, false);
            }
        }
    }

    // The mixed box (third row, first column) gives each corner its own profile.
    {
        const u32 x0 = 0;
        const u32 y0 = 128;
        check(empty(image, x0 + 6, y0 + 6), "mixed: the notched top-left corner must be empty 6 px in");
        check(filled(image, x0 + 63 - 1, y0 + 1), "mixed: the square top-right corner must be filled 1 px in");
        check(empty(image, x0 + 6, y0 + 63 - 6), "mixed: the scooped bottom-left corner must be empty 6 px in");
        check(empty(image, x0 + 63 - 6, y0 + 63 - 6), "mixed: the bevelled bottom-right corner must be empty 6 px in");
        check(filled(image, x0 + 63 - 15, y0 + 63 - 15), "mixed: the bevelled bottom-right corner must be filled 15 px in");
    }

    ui.destroy(*harness.renderer.rhi_device());
    harness.renderer.wait_idle();
    harness_owner.reset();
    SFT::Async::Scheduler::shutdown();
    if (g_failures == 0) {
        std::cout << "UiCornerShapeGpuTest: ok\n";
    }
    return g_failures == 0 ? 0 : 1;
}
