/// The atmosphere module of the render graph can be turned off: pixels where nothing is drawn then take the scene's
/// background colour *and its alpha* exactly, with no sky and no aerial perspective, so a UI viewer or preview can be
/// transparent and show what its materials say. With it on (the default) the sky is painted instead.
/// Needs a real Vulkan device; prints SKIP and succeeds without one.

#include <Async/Scheduler.hpp>
#include <Engine/EngineModule.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>
#include <WindowManager/WindowManager.hpp>

#include <glm/gtc/packing.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace {
    using namespace SFT;

    int failures = 0;
    constexpr u32 kSize = 32;

    bool check(bool condition, const std::string &message) {
        if (!condition) {
            Foundation::log_error("FAILED: {}", message);
            ++failures;
        }
        return condition;
    }

    /// The target's centre texel as linear RGBA (a half-float target).
    std::optional<std::array<f32, 4>> read_center(Engine::Engine &engine, Engine::RenderTargetHandle target) {
        RHI::RhiDevice *device = engine.rhi_device();
        const Renderer::TextureResource *resource = engine.renderer()->texture(engine.offscreen_render_target_texture(target));
        if (device == nullptr || resource == nullptr || resource->format != RHI::Format::RGBA16Float) return std::nullopt;
        const u32 width = resource->width, height = resource->height;
        const u64 row_pitch = ((static_cast<u64>(width) * 8u + 255u) / 256u) * 256u;
        auto buffer = device->create_buffer(RHI::BufferDesc{.size = row_pitch * height,
                                                            .usage = RHI::BufferUsage::TransferDst,
                                                            .memory = RHI::MemoryLocation::HostReadback,
                                                            .label = "atmosphere test readback"});
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "atmosphere test readback"});
        if (!buffer || !encoder) return std::nullopt;
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
                                           RHI::BufferTextureCopy{.buffer_offset = 0,
                                                                  .buffer_row_length = static_cast<u32>(row_pitch / 8u),
                                                                  .buffer_image_height = height,
                                                                  .mip_level = 0,
                                                                  .base_array_layer = 0,
                                                                  .array_layer_count = 1,
                                                                  .texture_offset = {0, 0, 0},
                                                                  .texture_extent = {width, height, 1}});
        RHI::TextureBarrier back = to_transfer;
        std::swap(back.src_stage, back.dst_stage);
        std::swap(back.src_access, back.dst_access);
        std::swap(back.old_layout, back.new_layout);
        (*encoder)->barrier({}, {}, std::span<const RHI::TextureBarrier>{&back, 1});
        auto command_buffer = (*encoder)->finish();
        auto fence = device->create_fence(RHI::FenceDesc{.label = "atmosphere test fence"});
        if (!command_buffer || !fence) return std::nullopt;
        const RHI::CommandBufferHandle buffers[1] = {*command_buffer};
        if (!device->submit(RHI::SubmitDesc{.command_buffers = std::span<const RHI::CommandBufferHandle>{buffers, 1},
                                            .fence = *fence,
                                            .flags = RHI::SubmitFlags::OneShot,
                                            .label = "atmosphere test submit"})) {
            return std::nullopt;
        }
        auto waited = device->wait_fences(std::span<const RHI::FenceHandle>{&*fence, 1}, true);
        if (!waited || !*waited) return std::nullopt;
        auto mapped = device->map_buffer(*buffer);
        if (!mapped) return std::nullopt;
        const auto *texel = reinterpret_cast<const u16 *>(reinterpret_cast<const u8 *>(mapped->data()) + (height / 2) * row_pitch +
                                                          static_cast<u64>(width / 2) * 8u);
        std::array<f32, 4> rgba{glm::unpackHalf1x16(texel[0]), glm::unpackHalf1x16(texel[1]), glm::unpackHalf1x16(texel[2]),
                                glm::unpackHalf1x16(texel[3])};
        device->unmap_buffer(*buffer);
        device->destroy_fence(*fence);
        device->destroy_command_buffer(*command_buffer);
        device->destroy_buffer(*buffer);
        return rgba;
    }

    bool render_empty_scene(Engine::Engine &engine, Core::RenderSurfaceHandle surface, Engine::RenderTargetHandle target, u64 index,
                            bool atmosphere, glm::vec4 background) {
        Engine::RenderFrameParameters params{};
        params.render_graph.set_background_color(background);
        params.render_graph.description().atmosphere.enabled = atmosphere;
        params.render_graph.description().scene.preserve_alpha = true;
        params.render_graph.description().tone_mapping.enabled = false;
        params.render_graph.description().bloom.enabled = false;
        if (!params.render_graph.set_present_target(target)) return false;
        const Core::FrameInput frame{.delta_seconds = 1.0 / 60.0, .frame_index = index, .framebuffer_width = kSize, .framebuffer_height = kSize};
        const Core::RendererResult result = engine.render(engine.prepare_render_frame(surface, frame, params));
        if (!result) {
            Foundation::log_error("render failed: {}", result.error().message);
            return false;
        }
        engine.wait_idle();
        return true;
    }

    std::string describe(const std::optional<std::array<f32, 4>> &rgba) {
        return rgba ? std::format("{},{},{},{}", (*rgba)[0], (*rgba)[1], (*rgba)[2], (*rgba)[3]) : std::string{"none"};
    }
} // namespace

int main() {
    namespace fs = std::filesystem;
    fs::current_path(fs::path(__FILE__).parent_path().parent_path().parent_path());

    WindowManager::WindowManager window_manager;
    WindowManager::WindowConfig window_config{};
    window_config.title = "AtmosphereModuleGpuTest";
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

    auto target = engine->create_offscreen_render_target(Engine::OffscreenRenderTargetDescription{
        .width = kSize, .height = kSize, .label = UString{std::string_view{"atmosphere"}}, .format = Engine::RenderTargetFormat::Rgba16Float});
    if (!check(target.has_value(), "a float target must be created")) return 1;

    const glm::vec4 background{0.25f, 0.5f, 0.75f, 0.5f};
    check(render_empty_scene(*engine, *surface, *target, 1, false, background), "a frame with the atmosphere off must render");
    const auto off = read_center(*engine, *target);
    check(off && std::abs((*off)[0] - 0.25f) < 0.01f && std::abs((*off)[1] - 0.5f) < 0.01f && std::abs((*off)[2] - 0.75f) < 0.01f,
          std::format("with the atmosphere off, empty pixels are exactly the background colour (got {})", describe(off)));
    check(off && std::abs((*off)[3] - 0.5f) < 0.01f,
          std::format("with the atmosphere off, empty pixels keep the background's alpha (got {})", describe(off)));

    check(render_empty_scene(*engine, *surface, *target, 2, false, glm::vec4{0.0f}), "a transparent frame must render");
    const auto clear = read_center(*engine, *target);
    check(clear && std::abs((*clear)[0]) < 0.01f && std::abs((*clear)[1]) < 0.01f && std::abs((*clear)[2]) < 0.01f &&
              std::abs((*clear)[3]) < 0.01f,
          std::format("a fully transparent background stays fully transparent (got {})", describe(clear)));

    check(render_empty_scene(*engine, *surface, *target, 3, true, background), "a frame with the atmosphere on must render");
    const auto on = read_center(*engine, *target);
    check(on && (std::abs((*on)[0] - 0.25f) > 0.02f || std::abs((*on)[1] - 0.5f) > 0.02f || std::abs((*on)[2] - 0.75f) > 0.02f),
          std::format("with the atmosphere on, the sky replaces the background colour (got {})", describe(on)));
    check(on && std::abs((*on)[3] - 1.0f) < 0.01f, std::format("the sky is opaque (got {})", describe(on)));

    if (failures != 0) {
        Foundation::log_error("{} check(s) failed", failures);
        return 1;
    }
    Foundation::log_info("AtmosphereModuleGpuTest passed");
    return 0;
}
