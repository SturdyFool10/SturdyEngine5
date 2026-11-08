/// SVG icons through the asset manager and the UI: an `Engine::UiSvgCache` icon drawn into an off-screen UI tree must show
/// its shape (not a solid block of the tint colour) at first, after its raster is made bigger, after the sizing epoch is
/// reset, and when it is loaded from memory instead of a file. Needs a real Vulkan device; prints SKIP and succeeds without one.

#include <Async/Scheduler.hpp>
#include <Engine/EngineModule.hpp>
#include <Engine/DeviceSwitch.hpp>
#include <Engine/UiTree.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>
#include <WindowManager/WindowManager.hpp>

#include <glm/gtc/packing.hpp>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {
    using namespace SFT;

    int failures = 0;
    constexpr u32 kSize = 128;

    bool check(bool condition, const std::string &message) {
        if (!condition) {
            Foundation::log_error("FAILED: {}", message);
            ++failures;
        }
        return condition;
    }

    /// Reads the target's texel at (`x`, `y`) as 0-255 RGB. Handles the 8-bit and half-float formats a target can have.
    std::optional<std::array<u32, 3>> read_texel(Engine::Engine &engine, Engine::RenderTargetHandle target, u32 x, u32 y) {
        RHI::RhiDevice *device = engine.rhi_device();
        const Renderer::TextureResource *resource = engine.renderer()->texture(engine.offscreen_render_target_texture(target));
        if (device == nullptr || resource == nullptr) return std::nullopt;
        const RHI::Format format = resource->format;
        u32 texel_bytes = 0;
        switch (format) {
            case RHI::Format::BGRA8Unorm:
            case RHI::Format::BGRA8UnormSrgb:
            case RHI::Format::RGBA8Unorm:
            case RHI::Format::RGBA8UnormSrgb: texel_bytes = 4; break;
            case RHI::Format::RGBA16Float: texel_bytes = 8; break;
            default:
                Foundation::log_error("unsupported target format {}", static_cast<int>(format));
                return std::nullopt;
        }
        const u64 row_pitch = ((static_cast<u64>(kSize) * texel_bytes + 255u) / 256u) * 256u;
        auto buffer = device->create_buffer(RHI::BufferDesc{
            .size = row_pitch * kSize,
            .usage = RHI::BufferUsage::TransferDst,
            .memory = RHI::MemoryLocation::HostReadback,
            .label = "ui tree test readback",
        });
        if (!buffer) return std::nullopt;
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "ui tree test readback"});
        if (!encoder) return std::nullopt;
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
                                               .buffer_row_length = static_cast<u32>(row_pitch / texel_bytes),
                                               .buffer_image_height = kSize,
                                               .mip_level = 0,
                                               .base_array_layer = 0,
                                               .array_layer_count = 1,
                                               .texture_offset = {0, 0, 0},
                                               .texture_extent = {kSize, kSize, 1},
                                           });
        RHI::TextureBarrier back = to_transfer;
        std::swap(back.src_stage, back.dst_stage);
        std::swap(back.src_access, back.dst_access);
        std::swap(back.old_layout, back.new_layout);
        (*encoder)->barrier({}, {}, std::span<const RHI::TextureBarrier>{&back, 1});
        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) return std::nullopt;
        auto fence = device->create_fence(RHI::FenceDesc{.label = "ui tree test readback fence"});
        if (!fence) return std::nullopt;
        const RHI::CommandBufferHandle buffers[1] = {*command_buffer};
        if (!device->submit(RHI::SubmitDesc{.command_buffers = std::span<const RHI::CommandBufferHandle>{buffers, 1},
                                            .fence = *fence,
                                            .flags = RHI::SubmitFlags::OneShot,
                                            .label = "ui tree test readback submit"})) {
            return std::nullopt;
        }
        auto waited = device->wait_fences(std::span<const RHI::FenceHandle>{&*fence, 1}, true);
        if (!waited || !*waited) return std::nullopt;
        auto mapped = device->map_buffer(*buffer);
        if (!mapped) return std::nullopt;
        const auto *texel = reinterpret_cast<const u8 *>(mapped->data()) + y * row_pitch + static_cast<u64>(x) * texel_bytes;
        std::array<u32, 3> rgb{};
        if (texel_bytes == 8) {
            const auto *halves = reinterpret_cast<const u16 *>(texel);
            for (int c = 0; c < 3; ++c) {
                rgb[c] = static_cast<u32>(std::clamp(glm::unpackHalf1x16(halves[c]), 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        } else if (format == RHI::Format::BGRA8Unorm || format == RHI::Format::BGRA8UnormSrgb) {
            rgb = {texel[2], texel[1], texel[0]};
        } else {
            rgb = {texel[0], texel[1], texel[2]};
        }
        device->unmap_buffer(*buffer);
        device->destroy_fence(*fence);
        device->destroy_command_buffer(*command_buffer);
        device->destroy_buffer(*buffer);
        return rgb;
    }


    constexpr std::string_view kIconSvg =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path stroke="none" d="M0 0h24v24H0z" fill="none"/><path d="M16 4l4 0l0 4" /><path d="M14 10l6 -6" /><path d="M8 20l-4 0l0 -4" /><path d="M4 20l6 -6" /></svg>)";

    bool is_background(const std::optional<std::array<u32, 3>> &rgb) {
        return rgb && (*rgb)[2] > 200 && (*rgb)[0] < 60 && (*rgb)[1] < 60;
    }
    bool is_icon_ink(const std::optional<std::array<u32, 3>> &rgb) { return rgb && (*rgb)[0] > 200 && (*rgb)[1] > 200; }
    std::string describe(const std::optional<std::array<u32, 3>> &rgb) {
        return rgb ? std::format("{},{},{}", (*rgb)[0], (*rgb)[1], (*rgb)[2]) : std::string{"none"};
    }
} // namespace

int main() {
    namespace fs = std::filesystem;
    fs::current_path(fs::path(__FILE__).parent_path().parent_path().parent_path());

    WindowManager::WindowManager window_manager;
    WindowManager::WindowConfig window_config{};
    window_config.title = "UiSvgGpuTest";
    window_config.extent = {kSize, kSize};
    window_config.visible = false;
    window_config.graphics_api = WindowManager::WindowGraphicsApi::Vulkan;
    auto window_id = window_manager.spawn_window<WindowManager::SDL3::SDL3Window>(window_config);
    if (!window_id) {
        Foundation::log_info("SKIP: could not create a window ({})", window_id.error().message);
        return 0;
    }
    auto engine = std::make_unique<Engine::Engine>();
    std::optional<Core::RenderSurfaceHandle> surface;
    std::string skip_reason;
    (void)window_manager.with_window(*window_id, [&](WindowManager::Window &window) {
        auto initialized = engine->initialize(window);
        if (!initialized) {
            skip_reason = initialized.error().message;
            return false;
        }
        surface = *initialized;
        return true;
    });
    if (!surface) {
        Foundation::log_info("SKIP: no Vulkan device available ({})", skip_reason);
        return 0;
    }
    Engine::WindowSnapshot snapshot{};
    snapshot.id = surface->window_id;
    snapshot.size = {kSize, kSize};
    snapshot.framebuffer_size = {kSize, kSize};
    engine->window_state().sync({snapshot}, surface->window_id);

    const fs::path directory = fs::temp_directory_path() / "sturdy_ui_svg_gpu_test";
    fs::create_directories(directory);
    const fs::path icon = directory / "arrows.svg";
    {
        std::ofstream out(icon);
        out << kIconSvg;
    }

    auto target = engine->create_offscreen_render_target(
        Engine::OffscreenRenderTargetDescription{.width = kSize, .height = kSize, .label = UString{std::string_view{"svg"}}});
    if (!check(target.has_value(), "an off-screen target must be created")) return 1;

    // What the UI draws: the icon, 64 px square at the top-left, tinted white, on a blue clear colour. `request_px` is the
    // raster size asked of the cache each frame, and `by_memory` loads the SVG from its bytes instead of the file.
    f32 request_px = 16.0f;
    bool by_memory = false;
    const Engine::UiTreeBuilder builder = [&](Engine::UiTreeFrame &frame) {
        auto handle = by_memory ? engine->ui_svg_cache().resolve_data(engine->assets(), "memory-icon", kIconSvg, request_px)
                                : engine->ui_svg_cache().resolve(engine->assets(), icon, request_px);
        auto root = frame.ui.element(UI::ElementDecl{});
        if (handle) {
            frame.ui.svg(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(64.0f), UI::SizingAxis::fixed(64.0f)},
                                         .background_color = UI::Color{1.0, 1.0, 1.0, 1.0}},
                         *handle);
        }
    };
    const Ecs::Entity tree = engine->spawn_ui_tree({.name = "svg",
                                                    .target = Engine::UiOffscreenTarget{*target},
                                                    .builder = builder,
                                                    .rebuild = Engine::UiRebuildPolicy::OnDirty,
                                                    .clear_color = {0.0f, 0.0f, 1.0f, 1.0f}});

    // Draws the tree again and reports (corner, ink) texels: the icon's empty corner and a texel on its diagonal stroke.
    const auto draw = [&](const char *what) {
        engine->ecs_world().get_component<Engine::UiTree>(tree)->request_rebuild();
        // A few frames: the first may only start loading, and growth happens between frames.
        for (int i = 0; i < 3; ++i) {
            engine->ecs_world().get_component<Engine::UiTree>(tree)->request_rebuild();
            engine->update(1.0 / 60.0);
            (void)engine->render_ui_tree_targets();
            const Core::FrameInput frame{.delta_seconds = 1.0 / 60.0, .frame_index = static_cast<u64>(i), .framebuffer_width = kSize, .framebuffer_height = kSize};
            (void)engine->render(engine->prepare_render_frame(*surface, frame, Engine::RenderFrameParameters{}));
            engine->wait_idle();
        }
        const auto corner = read_texel(*engine, *target, 3, 3);
        const auto ink = read_texel(*engine, *target, 40, 24); // on the diagonal (14,10)-(20,4) of the 24-unit icon, scaled by 64/24
        check(is_background(corner), std::format("{}: the icon's empty corner must show the background, not a block of the tint (got {})", what, describe(corner)));
        check(is_icon_ink(ink), std::format("{}: the icon's stroke must be drawn (got {})", what, describe(ink)));
    };

    draw("first draw from a file");
    request_px = 128.0f;
    draw("after the raster grows");
    engine->ui_svg_cache().clear_and_unload(engine->assets());
    request_px = 16.0f;
    draw("after the sizing epoch is reset");
    // The launcher applies its saved graphics device on the first frames: every texture is rebuilt on the new device, and a
    // moment later the sizing epoch is reset once more.
    request_px = 32.0f;
    draw("before the device is rebuilt");
    const u64 generation = engine->device_generation();
    const Engine::DeviceSwitchResult switched = engine->switch_device(Engine::DeviceRequest{.force_recreate = true});
    if (switched.outcome == Engine::DeviceSwitchOutcome::Failed) {
        Foundation::log_info("SKIP the device-rebuild part: {}", switched.reason);
    } else {
        check(engine->device_generation() != generation, "the device generation must move on a rebuild");
        draw("right after the device is rebuilt");
        engine->ui_svg_cache().clear_and_unload(engine->assets());
        draw("after the rebuild and a sizing reset");
        request_px = 64.0f;
        draw("after the rebuild and growing");
    }
    by_memory = true;
    request_px = 32.0f;
    draw("loaded from memory");
    request_px = 96.0f;
    draw("loaded from memory, after growing");

    fs::remove_all(directory);
    engine.reset();
    Async::Scheduler::shutdown();
    if (failures != 0) {
        Foundation::log_error("{} check(s) failed", failures);
        return 1;
    }
    Foundation::log_info("UiSvgGpuTest: ok");
    return 0;
}
