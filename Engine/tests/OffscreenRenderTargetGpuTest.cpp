/// Off-screen render targets are configurable: format (display-encoded SDR or linear half-float), size and sampling, set at creation and
/// changed in place by `Engine::reconfigure_offscreen_render_target` with the handle and its texture staying valid. Checks:
///   * a linear RGBA16Float target keeps values above 1.0, where an sRGB target clamps them (a real scene frame, tone mapping off);
///   * the texture a material samples reports the target's real format and size;
///   * a sampling-only change keeps the pixels and swaps the sampler; a format/size change keeps the handle and texture handle;
///   * unsupported formats and zero sizes are refused and leave the target as it was.
/// Needs a real Vulkan device; prints SKIP and succeeds without one.

#include <Async/Scheduler.hpp>
#include <Engine/EngineModule.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>
#include <WindowManager/WindowManager.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <Core/Slang/ShaderLibrary.hpp>

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
    constexpr u32 kSize = 64;

    bool check(bool condition, const std::string &message) {
        if (!condition) {
            Foundation::log_error("FAILED: {}", message);
            ++failures;
        }
        return condition;
    }

    struct Level {
        u32 width = 0;
        u32 height = 0;
        std::vector<std::array<f32, 3>> texels;
    };

    /// Mip `level` of the target as RGB per texel: 8- and 10-bit formats as stored / max (sRGB-encoded), half floats as their linear
    /// value.
    std::optional<Level> read_level(Engine::Engine &engine, Engine::RenderTargetHandle target, u32 level) {
        RHI::RhiDevice *device = engine.rhi_device();
        const Renderer::TextureResource *resource = engine.renderer()->texture(engine.offscreen_render_target_texture(target));
        if (device == nullptr || resource == nullptr || level >= resource->mip_levels) return std::nullopt;
        const u32 width = std::max(resource->width >> level, 1u), height = std::max(resource->height >> level, 1u);
        const u32 texel_bytes = resource->format == RHI::Format::RGBA16Float ? 8u : 4u;
        const u64 row_pitch = ((static_cast<u64>(width) * texel_bytes + 255u) / 256u) * 256u;
        auto buffer = device->create_buffer(RHI::BufferDesc{
            .size = row_pitch * height,
            .usage = RHI::BufferUsage::TransferDst,
            .memory = RHI::MemoryLocation::HostReadback,
            .label = "offscreen target test readback",
        });
        if (!buffer) return std::nullopt;
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "offscreen target test readback"});
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
                                               .buffer_image_height = height,
                                               .mip_level = level,
                                               .base_array_layer = 0,
                                               .array_layer_count = 1,
                                               .texture_offset = {0, 0, 0},
                                               .texture_extent = {width, height, 1},
                                           });
        RHI::TextureBarrier back = to_transfer;
        std::swap(back.src_stage, back.dst_stage);
        std::swap(back.src_access, back.dst_access);
        std::swap(back.old_layout, back.new_layout);
        (*encoder)->barrier({}, {}, std::span<const RHI::TextureBarrier>{&back, 1});
        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) return std::nullopt;
        auto fence = device->create_fence(RHI::FenceDesc{.label = "offscreen target test fence"});
        if (!fence) return std::nullopt;
        const RHI::CommandBufferHandle buffers[1] = {*command_buffer};
        if (!device->submit(RHI::SubmitDesc{.command_buffers = std::span<const RHI::CommandBufferHandle>{buffers, 1},
                                            .fence = *fence,
                                            .flags = RHI::SubmitFlags::OneShot,
                                            .label = "offscreen target test submit"})) {
            return std::nullopt;
        }
        auto waited = device->wait_fences(std::span<const RHI::FenceHandle>{&*fence, 1}, true);
        if (!waited || !*waited) return std::nullopt;
        auto mapped = device->map_buffer(*buffer);
        if (!mapped) return std::nullopt;
        Level out{.width = width, .height = height};
        out.texels.reserve(static_cast<usize>(width) * height);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const auto *texel = reinterpret_cast<const u8 *>(mapped->data()) + y * row_pitch + static_cast<u64>(x) * texel_bytes;
                std::array<f32, 3> rgb{};
                if (texel_bytes == 8) {
                    const auto *halves = reinterpret_cast<const u16 *>(texel);
                    rgb = {glm::unpackHalf1x16(halves[0]), glm::unpackHalf1x16(halves[1]), glm::unpackHalf1x16(halves[2])};
                } else if (resource->format == RHI::Format::RGB10A2Unorm) {
                    // A2B10G10R10_UNORM_PACK32: red in the low ten bits.
                    u32 packed = 0;
                    std::memcpy(&packed, texel, sizeof(packed));
                    rgb = {static_cast<f32>(packed & 0x3FFu) / 1023.0f, static_cast<f32>((packed >> 10) & 0x3FFu) / 1023.0f,
                           static_cast<f32>((packed >> 20) & 0x3FFu) / 1023.0f};
                } else if (resource->format == RHI::Format::BGRA8UnormSrgb) {
                    rgb = {texel[2] / 255.0f, texel[1] / 255.0f, texel[0] / 255.0f};
                } else {
                    rgb = {texel[0] / 255.0f, texel[1] / 255.0f, texel[2] / 255.0f};
                }
                out.texels.push_back(rgb);
            }
        }
        device->unmap_buffer(*buffer);
        device->destroy_fence(*fence);
        device->destroy_command_buffer(*command_buffer);
        device->destroy_buffer(*buffer);
        return out;
    }

    std::optional<std::array<f32, 3>> read_texel(Engine::Engine &engine, Engine::RenderTargetHandle target, u32 x, u32 y, u32 level = 0) {
        const std::optional<Level> image = read_level(engine, target, level);
        if (!image || x >= image->width || y >= image->height) return std::nullopt;
        return image->texels[static_cast<usize>(y) * image->width + x];
    }

    std::string describe(const std::optional<std::array<f32, 3>> &rgb) {
        return rgb ? std::format("{},{},{}", (*rgb)[0], (*rgb)[1], (*rgb)[2]) : std::string{"none"};
    }

    /// Renders a frame whose scene colour is a background brighter than paper white into `target`: no scene (so nothing draws over the
    /// clear), tone mapping off (the display transform still runs, with the identity operator) and bloom off. What lands in the target is
    /// that value as the target's format stores it.
    bool render_bright_background(Engine::Engine &engine, Core::RenderSurfaceHandle surface, Engine::RenderTargetHandle target, u64 index) {
        const auto description = engine.offscreen_render_target_description(target);
        if (!description) return false;
        Engine::RenderFrameParameters params{};
        params.render_graph.set_background_color(glm::vec4{4.0f, 0.5f, 0.25f, 1.0f});
        params.render_graph.description().scene.enabled = false;
        params.render_graph.description().tone_mapping.enabled = false;
        params.render_graph.description().bloom.enabled = false;
        if (!params.render_graph.set_present_target(target)) return false;
        const Core::FrameInput frame{
            .delta_seconds = 1.0 / 60.0, .frame_index = index, .framebuffer_width = description->width, .framebuffer_height = description->height};
        const Core::RendererResult result = engine.render(engine.prepare_render_frame(surface, frame, params));
        if (!result) {
            Foundation::log_error("render failed: {}", result.error().message);
            return false;
        }
        engine.wait_idle();
        return true;
    }
} // namespace

int main() {
    namespace fs = std::filesystem;
    fs::current_path(fs::path(__FILE__).parent_path().parent_path().parent_path());

    WindowManager::WindowManager window_manager;
    WindowManager::WindowConfig window_config{};
    window_config.title = "OffscreenRenderTargetGpuTest";
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

    // ---- creation: format and sampling are what was asked for, and the sampled texture knows its format and size ----
    const Engine::RenderTargetSampling linear_clamp{};
    auto hdr = engine->create_offscreen_render_target(Engine::OffscreenRenderTargetDescription{
        .width = kSize, .height = kSize, .label = UString{std::string_view{"hdr"}}, .format = Engine::RenderTargetFormat::Rgba16Float});
    auto sdr = engine->create_offscreen_render_target(
        Engine::OffscreenRenderTargetDescription{.width = kSize, .height = kSize, .label = UString{std::string_view{"sdr"}}});
    if (!check(hdr.has_value() && sdr.has_value(), "float and sRGB targets must be created")) return 1;
    {
        const auto description = engine->offscreen_render_target_description(*hdr);
        check(description && description->format == Engine::RenderTargetFormat::Rgba16Float && description->sampling == linear_clamp,
              "a target's description must report the format and sampling it was created with");
        const Renderer::TextureResource *resource = engine->renderer()->texture(engine->offscreen_render_target_texture(*hdr));
        check(resource != nullptr && resource->format == RHI::Format::RGBA16Float && resource->width == kSize && resource->height == kSize,
              "the target's sampled texture must report the real format and size");
    }

    // ---- what a frame writes: linear and unclamped into the float target, clamped and display-encoded into the sRGB one ----
    check(render_bright_background(*engine, *surface, *hdr, 1), "a scene frame into the float target must render");
    check(render_bright_background(*engine, *surface, *sdr, 2), "a scene frame into the sRGB target must render");
    const auto hdr_texel = read_texel(*engine, *hdr, kSize / 2, kSize / 2);
    const auto sdr_texel = read_texel(*engine, *sdr, kSize / 2, kSize / 2);
    check(hdr_texel && (*hdr_texel)[0] > 3.0f && (*hdr_texel)[1] > 0.3f && (*hdr_texel)[1] < 0.7f,
          std::format("a float target must keep a value above paper white, linear (got {})", describe(hdr_texel)));
    check(sdr_texel && (*sdr_texel)[0] > 0.99f && (*sdr_texel)[1] > 0.6f,
          std::format("an sRGB target must clamp to 1 and store sRGB-encoded values (0.5 linear ~ 0.735) (got {})", describe(sdr_texel)));

    // ---- sampling-only change: same handle, same texture handle, same pixels, new sampler ----
    const Renderer::TextureHandle hdr_texture = engine->offscreen_render_target_texture(*hdr);
    const RHI::SamplerHandle old_sampler = engine->renderer()->texture(hdr_texture)->sampler;
    Engine::OffscreenRenderTargetDescription pixel_art = *engine->offscreen_render_target_description(*hdr);
    pixel_art.sampling = Engine::RenderTargetSampling{.filter = Engine::RenderTargetFilter::Nearest, .address = Engine::RenderTargetAddressMode::Repeat};
    check(engine->reconfigure_offscreen_render_target(*hdr, pixel_art).has_value(), "a sampling change must succeed");
    {
        const Renderer::TextureResource *resource = engine->renderer()->texture(hdr_texture);
        check(engine->offscreen_render_target_texture(*hdr) == hdr_texture, "a sampling change must keep the texture handle");
        check(resource != nullptr && resource->sampler != old_sampler && resource->nearest_sampling,
              "a sampling change must give the texture the new sampler");
        const auto kept = read_texel(*engine, *hdr, kSize / 2, kSize / 2);
        check(kept && hdr_texel && (*kept)[0] == (*hdr_texel)[0], std::format("a sampling change must keep the pixels (got {})", describe(kept)));
        const auto description = engine->offscreen_render_target_description(*hdr);
        check(description && description->sampling == pixel_art.sampling, "the description must report the new sampling");
    }

    // ---- format and size change: same handles, new image ----
    Engine::OffscreenRenderTargetDescription smaller = pixel_art;
    smaller.width = 32;
    smaller.height = 48;
    smaller.format = Engine::RenderTargetFormat::Rgba8Srgb;
    check(engine->reconfigure_offscreen_render_target(*hdr, smaller).has_value(), "a format and size change must succeed");
    {
        check(engine->offscreen_render_target_texture(*hdr) == hdr_texture, "a format change must keep the texture handle");
        const Renderer::TextureResource *resource = engine->renderer()->texture(hdr_texture);
        check(resource != nullptr && resource->format == RHI::Format::RGBA8UnormSrgb && resource->width == 32 && resource->height == 48,
              "after a format and size change the texture must report the new format and size");
        check(render_bright_background(*engine, *surface, *hdr, 3), "a frame into the reconfigured target must render");
        const auto clamped = read_texel(*engine, *hdr, 16, 24);
        check(clamped && (*clamped)[0] > 0.99f && (*clamped)[0] <= 1.0f, std::format("the reconfigured 8-bit target must clamp (got {})", describe(clamped)));
        // Resize keeps the format and sampling it was given.
        check(engine->resize_offscreen_render_target(*hdr, 40, 40).has_value(), "a resize must succeed");
        const auto resized = engine->offscreen_render_target_description(*hdr);
        check(resized && resized->width == 40 && resized->format == Engine::RenderTargetFormat::Rgba8Srgb &&
                  resized->sampling == pixel_art.sampling,
              "a resize must keep the target's format and sampling");
    }

    // ---- refusals leave the target alone ----
    {
        const auto before = engine->renderer()->offscreen_render_target_description(Renderer::OffscreenRenderTargetHandle{.value = hdr->value});
        Renderer::OffscreenRenderTargetDescription bad = *before;
        bad.format = RHI::Format::R8Unorm;
        check(!engine->renderer()->reconfigure_offscreen_render_target(Renderer::OffscreenRenderTargetHandle{.value = hdr->value}, bad),
              "an unsupported format must be refused");
        bad = *before;
        bad.extent = Core::Extent2D{0u, 0u};
        check(!engine->renderer()->reconfigure_offscreen_render_target(Renderer::OffscreenRenderTargetHandle{.value = hdr->value}, bad),
              "a zero size must be refused");
        const auto after = engine->renderer()->offscreen_render_target_description(Renderer::OffscreenRenderTargetHandle{.value = hdr->value});
        check(after && after->format == before->format && after->extent == before->extent, "a refused change must leave the target as it was");
        Renderer::OffscreenRenderTargetDescription bad_create{.extent = {8u, 8u}, .format = RHI::Format::RGBA32Float};
        check(!engine->renderer()->create_offscreen_render_target(bad_create), "creating a target in an unsupported format must fail");
    }

    // ---- 10-bit: drawn as half-float, then sRGB-encoded by a shader into the target ----
    {
        auto ten_bit = engine->create_offscreen_render_target(
            Engine::OffscreenRenderTargetDescription{.width = 32, .height = 32, .format = Engine::RenderTargetFormat::Rgb10A2});
        if (check(ten_bit.has_value(), "a 10-bit target must be created")) {
            check(render_bright_background(*engine, *surface, *ten_bit, 10), "a frame into the 10-bit target must render");
            const auto texel = read_texel(*engine, *ten_bit, 16, 16);
            // (4, 0.5, 0.25) linear -> clamped and sRGB-encoded: (1, 0.7354, 0.5371).
            check(texel && (*texel)[0] > 0.995f && std::abs((*texel)[1] - 0.7354f) < 0.004f && std::abs((*texel)[2] - 0.5371f) < 0.004f,
                  std::format("a 10-bit target must hold the clamped, sRGB-encoded value (got {})", describe(texel)));
            engine->destroy_offscreen_render_target(*ten_bit);
        }
    }

    // ---- compute writes and mip chains: a kernel writes level 0 of a storage target, the chain is filtered down from it ----
    {
        check(!engine->create_offscreen_render_target(
                  Engine::OffscreenRenderTargetDescription{.width = 8, .height = 8, .storage = Engine::RenderTargetFeature::Required}),
              "an sRGB target must refuse allow_storage");
        // Left half 4.0, right half 0.0 in red; 0.25 in green everywhere.
        Core::Slang::override_shader_module("offscreen_target_test_pattern", R"(
[format("rgba16f")] RWTexture2D<float4> target;
[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    uint width;
    uint height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) {
        return;
    }
    target[id.xy] = float4(id.x < width / 2u ? 4.0 : 0.0, 0.25, 0.0, 1.0);
}
)");
        auto kernel = engine->renderer()->prepare_compute_kernel(Renderer::ComputeKernelDescription{
            .shader_path = "Shaders/offscreen_target_test_pattern.slang",
            .module_name = "offscreen_target_test_pattern",
            .label = "offscreen target test pattern",
        });
        check(kernel.has_value(), std::format("the test kernel must compile: {}", kernel ? std::string{} : kernel.error().message));
        auto storage = engine->create_offscreen_render_target(Engine::OffscreenRenderTargetDescription{
            .width = kSize, .height = kSize, .format = Engine::RenderTargetFormat::Rgba16Float, .mip_levels = 0, .storage = Engine::RenderTargetFeature::Required});
        if (kernel && check(storage.has_value(), "a float storage target with a full mip chain must be created")) {
            const Renderer::TextureResource *resource = engine->renderer()->texture(engine->offscreen_render_target_texture(*storage));
            check(resource != nullptr && resource->mip_levels == 7, "mip_levels = 0 must give a 64x64 target the full chain of 7");
            const auto dispatched = engine->dispatch_into_offscreen_render_target(*storage, *kernel, "target", glm::uvec3{kSize / 8, kSize / 8, 1});
            check(dispatched.has_value(),
                  std::format("a compute write into a storage target must succeed: {}", dispatched ? std::string{} : dispatched.error().message));
            const auto left = read_texel(*engine, *storage, 8, 32, 0);
            const auto right = read_texel(*engine, *storage, 56, 32, 0);
            check(left && right && (*left)[0] == 4.0f && (*right)[0] == 0.0f, std::format("level 0 must hold what the kernel wrote (got {} / {})", describe(left), describe(right)));
            const auto level1 = read_texel(*engine, *storage, 4, 16, 1);
            check(level1 && (*level1)[0] == 4.0f && (*level1)[1] == 0.25f, std::format("level 1 must be filtered from level 0 (got {})", describe(level1)));
            const auto smallest = read_texel(*engine, *storage, 0, 0, 6);
            check(smallest && std::abs((*smallest)[0] - 2.0f) < 0.01f && std::abs((*smallest)[1] - 0.25f) < 0.001f,
                  std::format("the 1x1 level must be the mean of level 0 (got {})", describe(smallest)));

            // A frame drawn into a mipped target rebuilds its chain too.
            check(render_bright_background(*engine, *surface, *storage, 11), "a frame into a mipped target must render");
            const auto framed = read_texel(*engine, *storage, 0, 0, 6);
            check(framed && std::abs((*framed)[0] - 4.0f) < 0.01f && std::abs((*framed)[1] - 0.5f) < 0.01f,
                  std::format("a frame must rebuild the mip chain (got {})", describe(framed)));
            engine->destroy_offscreen_render_target(*storage);
        }
        // A frame into a mipped 8-bit target: filtering happens in linear light (the format decodes and encodes).
        auto mipped = engine->create_offscreen_render_target(
            Engine::OffscreenRenderTargetDescription{.width = 16, .height = 16, .mip_levels = 0});
        if (check(mipped.has_value(), "an 8-bit target with a mip chain must be created")) {
            check(render_bright_background(*engine, *surface, *mipped, 12), "a frame into a mipped 8-bit target must render");
            const auto top = read_texel(*engine, *mipped, 8, 8, 0);
            const auto bottom = read_texel(*engine, *mipped, 0, 0, 4);
            check(top && bottom && std::abs((*top)[1] - (*bottom)[1]) < 0.01f,
                  std::format("a uniform frame must give every level the same value (got {} / {})", describe(top), describe(bottom)));
            engine->destroy_offscreen_render_target(*mipped);
        }
        auto plain = engine->create_offscreen_render_target(
            Engine::OffscreenRenderTargetDescription{.width = 8, .height = 8, .format = Engine::RenderTargetFormat::Rgba16Float});
        if (kernel && plain) {
            check(!engine->dispatch_into_offscreen_render_target(*plain, *kernel, "target", glm::uvec3{1, 1, 1}),
                  "a compute write into a target without allow_storage must be refused");
            engine->destroy_offscreen_render_target(*plain);
        }
    }

    // ---- MSAA is the frame's: the same scene at 1x and 4x into two targets differs exactly along edges ----
    {
        auto shader = engine->assets().load_shader("Shaders/gbuffer_geometry.slang", UString{std::string_view{"msaa test shader"}});
        auto model = shader ? engine->assets().create_model(Renderer::Mesh::cube(), *shader, glm::vec4{0.9f, 0.3f, 0.2f, 1.0f})
                            : Engine::AssetExpected<Engine::Asset>{std::unexpected(shader.error())};
        if (check(model.has_value(), "the MSAA test cube must be created")) {
            // Turned 30 degrees about the view axis so its silhouette has slanted edges.
            const glm::mat4 transform = glm::rotate(glm::mat4{1.0f}, glm::radians(30.0f), glm::vec3{0.0f, 0.0f, 1.0f}) *
                                        glm::rotate(glm::mat4{1.0f}, glm::radians(20.0f), glm::vec3{1.0f, 0.0f, 0.0f});
            (void)engine->ecs_world().spawn(Engine::WorldTransform{transform}, Engine::ModelRenderer{.model = *model});
            std::array<Engine::RenderTargetHandle, 2> views{};
            std::array<u32, 2> samples{1u, 4u};
            bool rendered = true;
            for (usize i = 0; i < 2; ++i) {
                auto target = engine->create_offscreen_render_target(Engine::OffscreenRenderTargetDescription{
                    .width = kSize, .height = kSize, .format = Engine::RenderTargetFormat::Rgba16Float});
                if (!target) {
                    rendered = false;
                    break;
                }
                views[i] = *target;
                Engine::RenderFrameParameters params{};
                params.camera = Engine::Camera::perspective(50.0f, 1.0f, 0.1f, 100.0f);
                params.camera.set_position(glm::vec3{0.0f, 0.0f, 3.0f});
                params.camera.look_at(glm::vec3{0.0f});
                params.render_graph.description().anti_aliasing.msaa_samples = samples[i];
                params.render_graph.description().anti_aliasing.post_process = RenderSettings::PostProcessAntiAliasing::None;
                params.render_graph.description().bloom.enabled = false;
                params.render_graph.description().temporal_upscaler.enabled = false;
                rendered = rendered && params.render_graph.set_present_target(*target);
                const Core::FrameInput frame{.delta_seconds = 1.0 / 60.0, .frame_index = 20 + i, .framebuffer_width = kSize, .framebuffer_height = kSize};
                const Core::RendererResult result = engine->render(engine->prepare_render_frame(*surface, frame, params));
                rendered = rendered && result.has_value();
                engine->wait_idle();
            }
            if (check(rendered, "the 1x and 4x frames must render")) {
                u32 differing = 0;
                const std::optional<Level> a = read_level(*engine, views[0], 0);
                const std::optional<Level> b = read_level(*engine, views[1], 0);
                check(a && b, "both MSAA test targets must read back");
                for (usize i = 0; a && b && i < a->texels.size(); ++i) {
                    if (std::abs(a->texels[i][0] - b->texels[i][0]) > 0.02f || std::abs(a->texels[i][1] - b->texels[i][1]) > 0.02f) {
                        ++differing;
                    }
                }
                Foundation::log_info("MSAA 1x vs 4x: {} of {} pixels differ", differing, kSize * kSize);
                // Silhouette pixels are a few hundred at 64x64; the whole image would be 4096.
                check(differing > 8 && differing < kSize * kSize / 4,
                      std::format("4x MSAA must change the edge pixels of an off-screen frame, and only those ({} differ)", differing));
            }
            for (Engine::RenderTargetHandle view : views) {
                if (view) engine->destroy_offscreen_render_target(view);
            }
        }
    }

    engine->destroy_offscreen_render_target(*hdr);
    engine->destroy_offscreen_render_target(*sdr);
    engine.reset();
    Async::Scheduler::shutdown();
    if (failures != 0) {
        Foundation::log_error("{} check(s) failed", failures);
        return 1;
    }
    Foundation::log_info("OffscreenRenderTargetGpuTest: ok");
    return 0;
}
