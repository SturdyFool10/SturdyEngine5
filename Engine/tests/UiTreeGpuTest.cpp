/// Two in-world "monitors": two off-screen UI trees, each laid out by `Engine::update` and rendered into its own target by
/// `Engine::render_ui_tree_targets`, then read back. Also checks that a window tree is attached to the window's prepared frame, and
/// that an `OnDirty` tree with nothing new costs no render. Needs a real Vulkan device; prints SKIP and succeeds without one.

#include <Async/Scheduler.hpp>
#include <Engine/EngineModule.hpp>
#include <Engine/UiTree.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>
#include <WindowManager/WindowManager.hpp>

#include <glm/gtc/packing.hpp>

#include <array>
#include <filesystem>
#include <format>
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

    /// A monitor's UI: a `kSize / 2` square of `color` at the top-left.
    Engine::UiTreeBuilder monitor_ui(UI::Color color, int &builds) {
        return [color, &builds](Engine::UiTreeFrame &frame) {
            ++builds;
            auto root = frame.ui.element(UI::ElementDecl{});
            auto panel = frame.ui.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(kSize / 2.0f), UI::SizingAxis::fixed(kSize / 2.0f)},
                .background_color = color,
                .id = UString{std::string_view{"panel"}},
            });
        };
    }
} // namespace

int main() {
    namespace fs = std::filesystem;
    fs::current_path(fs::path(__FILE__).parent_path().parent_path().parent_path());

    WindowManager::WindowManager window_manager;
    WindowManager::WindowConfig window_config{};
    window_config.title = "UiTreeGpuTest";
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
    // No Application here to mirror the window into the ECS; do it by hand.
    Engine::WindowSnapshot snapshot{};
    snapshot.id = surface->window_id;
    snapshot.size = {kSize, kSize};
    snapshot.framebuffer_size = {kSize, kSize};
    engine->window_state().sync({snapshot}, surface->window_id);

    std::array<Engine::RenderTargetHandle, 2> targets{};
    for (Engine::RenderTargetHandle &target : targets) {
        auto created = engine->create_offscreen_render_target(
            Engine::OffscreenRenderTargetDescription{.width = kSize, .height = kSize, .label = UString{std::string_view{"monitor"}}});
        if (!check(created.has_value(), "an off-screen target must be created")) return 1;
        target = *created;
    }

    int red_builds = 0, green_builds = 0, hud_builds = 0;
    const Ecs::Entity red = engine->spawn_ui_tree({.name = "red monitor",
                                                   .target = Engine::UiOffscreenTarget{targets[0]},
                                                   .builder = monitor_ui(UI::Color{1.0, 0.0, 0.0, 1.0}, red_builds),
                                                   .rebuild = Engine::UiRebuildPolicy::OnDirty,
                                                   .clear_color = {0.0f, 0.0f, 1.0f, 1.0f}});
    (void)engine->spawn_ui_tree({.name = "green monitor",
                                 .target = Engine::UiOffscreenTarget{targets[1]},
                                 .builder = monitor_ui(UI::Color{0.0, 1.0, 0.0, 1.0}, green_builds),
                                 .rebuild = Engine::UiRebuildPolicy::OnDirty,
                                 .clear_color = {0.0f, 0.0f, 1.0f, 1.0f}});
    (void)engine->spawn_ui_tree({.name = "hud", .builder = monitor_ui(UI::Color{1.0, 1.0, 1.0, 1.0}, hud_builds)});

    engine->update(1.0 / 60.0);
    check(engine->render_ui_tree_targets() == 2, "both monitors must be rendered into their targets after their first build");
    engine->wait_idle();
    check(engine->render_health().frames_failed == 0, std::format("rendering the monitors must not fail: {}", engine->render_health().last_error));

    const auto red_inside = read_texel(*engine, targets[0], kSize / 4, kSize / 4);
    const auto red_outside = read_texel(*engine, targets[0], kSize * 3 / 4, kSize * 3 / 4);
    const auto green_inside = read_texel(*engine, targets[1], kSize / 4, kSize / 4);
    const auto describe = [](const std::optional<std::array<u32, 3>> &rgb) {
        return rgb ? std::format("{},{},{}", (*rgb)[0], (*rgb)[1], (*rgb)[2]) : std::string{"none"};
    };
    check(red_inside && (*red_inside)[0] > 200 && (*red_inside)[1] < 60 && (*red_inside)[2] < 60,
          std::format("the red monitor's panel must be red (got {})", describe(red_inside)));
    check(green_inside && (*green_inside)[1] > 200 && (*green_inside)[0] < 60 && (*green_inside)[2] < 60,
          std::format("the green monitor's panel must be green: each tree draws only into its own target (got {})", describe(green_inside)));
    check(red_outside && (*red_outside)[2] > 200 && (*red_outside)[0] < 60,
          std::format("outside the panel a monitor shows its clear colour (got {})", describe(red_outside)));

    // Nothing changed: neither monitor is laid out or rendered again.
    engine->update(1.0 / 60.0);
    check(engine->render_ui_tree_targets() == 0, "an OnDirty monitor with nothing new must not be rendered again");
    check(red_builds == 1 && green_builds == 1, "an OnDirty monitor with nothing new must not be laid out again");
    // Poking one monitor re-renders that one only.
    engine->ecs_world().get_component<Engine::UiTree>(red)->request_rebuild();
    engine->update(1.0 / 60.0);
    check(engine->render_ui_tree_targets() == 1, "only the monitor that changed must be rendered again");

    // The window tree rides on the window's frame without the frame callback appending anything.
    const Core::FrameInput frame{.delta_seconds = 1.0 / 60.0, .frame_index = 1, .framebuffer_width = kSize, .framebuffer_height = kSize};
    const Engine::PreparedRenderFrame prepared = engine->prepare_render_frame(*surface, frame, Engine::RenderFrameParameters{});
    check(prepared.overlay_passes.size() == 1 && prepared.overlay_passes[0].name == "hud",
          "a window tree must be attached to its window's frame (and the monitors must not be)");
    const Core::RendererResult rendered = engine->render(prepared);
    check(rendered.has_value(), "a window frame with a UI tree overlay must render");
    engine->wait_idle();
    check(hud_builds >= 3, "the window tree must be laid out every update");

    engine.reset();
    Async::Scheduler::shutdown();
    if (failures != 0) {
        Foundation::log_error("{} check(s) failed", failures);
        return 1;
    }
    Foundation::log_info("UiTreeGpuTest: ok");
    return 0;
}
