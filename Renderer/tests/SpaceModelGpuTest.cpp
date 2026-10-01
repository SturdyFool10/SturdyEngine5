#include <Renderer/RendererModule.hpp>
#include <WindowManager/WindowManager.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

// Headless GPU test for `Renderer::set_space_model`: one lit plane is rendered through the real deferred pipeline
//   * with the default (Euclidean) space model;
//   * with a `sturdy_space` shader override that moves the whole world sideways -- the image must change, and must
//     change for a material that already existed (set_space_model recompiles live templates);
//   * with a visibility rule that rejects every item -- nothing is drawn, so the plane's lit pixels disappear;
//   * after restoring the default `SpaceModel{}` -- the baseline image comes back exactly.
// It needs a real Vulkan device: with none it prints SKIP and succeeds.

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
        std::vector<u8> rgb; // kSize * kSize * 3
    };

    /// A bumpy field with sharp features and a couple of tall narrow spikes: the shape where a linear-search
    /// block visibly differs from an exact one, and where a broken hierarchy skips real hits.
    std::vector<f32> make_heights(u32 w, u32 h) {
        std::vector<f32> out(static_cast<usize>(w) * h);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const f32 fx = static_cast<f32>(x) / static_cast<f32>(w);
                const f32 fy = static_cast<f32>(y) / static_cast<f32>(h);
                f32 v = 0.5f + 0.25f * std::sin(fx * 6.2831853f * 3.0f) * std::sin(fy * 6.2831853f * 3.0f);
                const f32 bx = std::fmod(fx * 4.0f, 1.0f) - 0.5f;
                const f32 by = std::fmod(fy * 4.0f, 1.0f) - 0.5f;
                if (std::abs(bx) < 0.12f && std::abs(by) < 0.12f) {
                    v = 0.05f; // pits
                }
                out[static_cast<usize>(y) * w + x] = std::clamp(v, 0.0f, 1.0f);
            }
        }
        return out;
    }

    /// 2x2 plane in XZ facing +Y; uv u = +X, v = -Z (matches the tangent frame (1,0,0,+1) x normal +Y).
    void make_plane(std::vector<GeometryVertex> &vertices, std::vector<u32> &indices) {
        const glm::vec3 positions[4] = {{-1, 0, 1}, {1, 0, 1}, {1, 0, -1}, {-1, 0, -1}};
        vertices.clear();
        for (const glm::vec3 &p : positions) {
            GeometryVertex v{};
            v.position = p;
            v.normal = {0, 1, 0};
            v.uv = {(p.x + 1.0f) * 0.5f, (1.0f - p.z) * 0.5f};
            v.tangent = {1, 0, 0, 1};
            vertices.push_back(v);
        }
        indices = {0, 1, 2, 0, 2, 3};
    }

    struct Harness {
        SFT::Renderer::Renderer renderer;
        Core::RenderSurfaceHandle surface{};
        OffscreenRenderTargetHandle target{};
        u64 frame = 0;
        std::string out_dir;
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
            .label = "pipeline replacement test readback",
        });
        if (!buffer) return false;
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "pipeline replacement test readback"});
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
        auto fence = device->create_fence(RHI::FenceDesc{.label = "pipeline replacement test readback fence"});
        if (!fence) return false;
        const RHI::CommandBufferHandle buffers[1] = {*command_buffer};
        RHI::SubmitDesc submit{
            .command_buffers = std::span<const RHI::CommandBufferHandle>{buffers, 1},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "pipeline replacement test readback submit",
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
                // BGRA8 target.
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


    bool render(Harness &h, MeshHandle mesh, MaterialInstanceHandle material, Image &image) {
        SceneRenderable renderable{};
        renderable.mesh = mesh;
        renderable.material = material;
        renderable.stable_id = 1;
        renderable.cull_mode = RHI::CullMode::None;
        renderable.casts_shadows = true;

        RenderFrameDesc desc{};
        desc.surface = h.surface;
        desc.offscreen_target = h.target;
        desc.frame.framebuffer_width = kSize;
        desc.frame.framebuffer_height = kSize;
        desc.view.camera.world_position = {0.0f, 1.1f, 2.0f};
        desc.view.camera.view = glm::lookAt(desc.view.camera.world_position, glm::vec3{0, 0, 0.1f}, glm::vec3{0, 1, 0});
        desc.view.camera.projection = glm::perspectiveRH_ZO(glm::radians(50.0f), 1.0f, 0.05f, 50.0f);
        desc.view.camera.previous_view_projection = desc.view.camera.projection * desc.view.camera.view;
        desc.view.lighting.sun.direction = glm::normalize(glm::vec3{-0.6f, -0.5f, -0.4f});
        desc.view.lighting.sun.radiance = {3.0f, 2.9f, 2.7f};
        desc.view.renderables = std::span<const SceneRenderable>{&renderable, 1};
        desc.view.render_graph.wait_for_completion = true;
        desc.view.render_graph.bloom = false;
        desc.view.render_graph.contact_shadows = false;
        for (int i = 0; i < 2; ++i) {
            desc.frame.frame_index = h.frame++;
            if (Core::RendererResult r = h.renderer.render_frame(desc); !r) {
                std::cerr << "render_frame failed: " << r.error().message << '\n';
                return false;
            }
        }
        h.renderer.wait_idle();
        return read_back(h, image);
    }

    struct Diff {
        f64 mean = 0.0;          // mean abs channel difference, 0..255
        f64 fraction_over = 0.0; // fraction of pixels with any channel differing by > 16
    };
    Diff diff(const Image &a, const Image &b) {
        Diff d;
        usize over = 0;
        f64 sum = 0.0;
        for (usize p = 0; p < static_cast<usize>(kSize) * kSize; ++p) {
            int worst = 0;
            for (int c = 0; c < 3; ++c) {
                const int delta = std::abs(int(a.rgb[p * 3 + c]) - int(b.rgb[p * 3 + c]));
                sum += delta;
                worst = std::max(worst, delta);
            }
            over += worst > 16 ? 1 : 0;
        }
        d.mean = sum / (static_cast<f64>(kSize) * kSize * 3.0);
        d.fraction_over = static_cast<f64>(over) / (static_cast<f64>(kSize) * kSize);
        return d;
    }

    f64 mean_luma(const Image &a) {
        f64 sum = 0.0;
        for (u8 v : a.rgb) sum += v;
        return sum / static_cast<f64>(a.rgb.size());
    }


} // namespace

namespace {

int run() {
    namespace fs = std::filesystem;
    fs::current_path(fs::path(__FILE__).parent_path().parent_path().parent_path());

    SFT::WindowManager::WindowManager window_manager;
    Harness harness;
    SFT::WindowManager::WindowConfig window_config{};
    window_config.title = "SpaceModelGpuTest";
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
        info.app_name = "SpaceModelGpuTest";
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
        OffscreenRenderTargetDescription{.extent = {kSize, kSize}, .label = "pipeline replacement target"});
    if (!target) {
        std::cerr << "FAILED: off-screen target: " << target.error().message << '\n';
        return 1;
    }
    harness.target = *target;

    std::vector<GeometryVertex> vertices;
    std::vector<u32> indices;
    make_plane(vertices, indices);
    namespace D = SFT::Renderer::Displacement;
    DisplacedMaterialDesc material_desc{};
    material_desc.heights = std::vector<f32>(16 * 16, 0.5f);
    material_desc.width = 16;
    material_desc.height = 16;
    material_desc.tier = D::HardwareTier::High;
    material_desc.request.desired_algorithm = D::HeightfieldAlgorithm::NormalOnly;
    material_desc.request.desired_depth = D::DepthPolicy::BaseSurface;
    material_desc.height_scale = 0.0f;
    material_desc.reference_height = 1.0f;
    material_desc.tile_size = {2.0f, 2.0f};
    material_desc.base_color = {0.8f, 0.7f, 0.6f, 1.0f};
    material_desc.label = "test plane material";
    auto displaced = harness.renderer.create_displaced_material(material_desc);
    if (!displaced) {
        std::cerr << "FAILED: create_displaced_material: " << displaced.error().message << '\n';
        return 1;
    }
    auto mesh = harness.renderer.create_displaced_mesh(*displaced, vertices, indices, "test plane");
    if (!mesh) {
        std::cerr << "FAILED: create_displaced_mesh: " << mesh.error().message << '\n';
        return 1;
    }
    struct MaterialHolder {
        MaterialInstanceHandle value;
        const MaterialInstanceHandle &operator*() const { return value; }
    } material{displaced->instance};

    Image baseline;
    if (!render(harness, *mesh, *material, baseline)) return 1;
    check(mean_luma(baseline) > 5.0, "default space renders the plane (image is not black)");
    check(harness.renderer.space_model()->name == "euclidean", "the default space model is Euclidean");

    // 1. Vertex transform: the whole world shifted +0.8 in x by the sturdy_space module.
    SpaceModel shifted;
    shifted.name = "shifted";
    shifted.shader_source = R"(
float3 sturdySpaceModelToWorld(float3 localPosition, float4x4 model) { return mul(float4(localPosition, 1.0), model).xyz; }
float4 sturdySpaceWorldToClip(float3 worldPosition, float4x4 viewProjection) {
    return mul(float4(worldPosition + float3(0.8, 0.0, 0.0), 1.0), viewProjection);
}
float3 sturdySpaceTransformDirection(float3 localDirection, float4x4 model) { return normalize(mul(localDirection, (float3x3)model)); }
float4 sturdySpaceClipToWorldH(float4 clip, float4x4 inverseViewProjection) { return mul(clip, inverseViewProjection); }
float3 sturdySpaceClipToWorld(float4 clip, float4x4 inverseViewProjection) {
    float4 w = mul(clip, inverseViewProjection);
    return w.xyz / max(abs(w.w), 1.0e-6) * (w.w < 0.0 ? -1.0 : 1.0);
}
)";
    if (Core::RendererResult r = harness.renderer.set_space_model(shifted); !r) {
        std::cerr << "FAILED: set_space_model(shifted): " << r.error().message << '\n';
        return 1;
    }
    Image moved;
    if (!render(harness, *mesh, *material, moved)) return 1;
    const Diff moved_diff = diff(moved, baseline);
    std::cout << "shifted vs default: mean=" << moved_diff.mean << " over=" << moved_diff.fraction_over
              << " luma=" << mean_luma(moved) << '\n';
    check(harness.renderer.space_model()->name == "shifted", "the shifted space model is installed");
    check(mean_luma(moved) > 5.0, "the shifted world still renders");
    check(moved_diff.fraction_over > 0.05, "a sturdy_space override changes where existing geometry lands");

    // 2. Visibility: reject everything.
    int asked = 0;
    SpaceModel nothing;
    nothing.name = "nothing visible";
    nothing.item_visible = [&asked](const RenderItem &, const ItemCuller &) {
        ++asked;
        return false;
    };
    if (Core::RendererResult r = harness.renderer.set_space_model(nothing); !r) {
        std::cerr << "FAILED: set_space_model(nothing): " << r.error().message << '\n';
        return 1;
    }
    Image empty;
    if (!render(harness, *mesh, *material, empty)) return 1;
    std::cout << "nothing visible: asked=" << asked << " luma=" << mean_luma(empty) << '\n';
    check(asked > 0, "the custom visibility rule was consulted");
    const Diff empty_diff = diff(empty, baseline);
    check(empty_diff.fraction_over > 0.05, "rejecting every item removes the plane from the image");

    // 3. Restore.
    if (Core::RendererResult r = harness.renderer.set_space_model(SpaceModel{}); !r) {
        std::cerr << "FAILED: restoring the default space model: " << r.error().message << '\n';
        return 1;
    }
    Image restored;
    if (!render(harness, *mesh, *material, restored)) return 1;
    const Diff restored_diff = diff(restored, baseline);
    std::cout << "restored vs default: mean=" << restored_diff.mean << " over=" << restored_diff.fraction_over << '\n';
    check(restored_diff.mean < 0.5 && restored_diff.fraction_over < 0.001, "restoring the default space model restores the image");

    // 4. Depth reconstruction: the lighting pass follows the module too. A module whose ClipToWorld collapses to the
    //    origin must visibly change the lit image (view vectors, shadow lookups), on effect shaders that were already
    //    compiled by the frames above.
    SpaceModel broken_reconstruction;
    broken_reconstruction.name = "broken reconstruction";
    broken_reconstruction.shader_source = R"(
float3 sturdySpaceModelToWorld(float3 localPosition, float4x4 model) { return mul(float4(localPosition, 1.0), model).xyz; }
float4 sturdySpaceWorldToClip(float3 worldPosition, float4x4 viewProjection) { return mul(float4(worldPosition, 1.0), viewProjection); }
float3 sturdySpaceTransformDirection(float3 localDirection, float4x4 model) { return normalize(mul(localDirection, (float3x3)model)); }
float4 sturdySpaceClipToWorldH(float4 clip, float4x4 inverseViewProjection) { return float4(0.0, 0.0, 0.0, 1.0); }
float3 sturdySpaceClipToWorld(float4 clip, float4x4 inverseViewProjection) { return float3(0.0, 0.0, 0.0); }
)";
    if (Core::RendererResult r = harness.renderer.set_space_model(broken_reconstruction); !r) {
        std::cerr << "FAILED: set_space_model(broken_reconstruction): " << r.error().message << '\n';
        return 1;
    }
    Image reconstructed;
    if (!render(harness, *mesh, *material, reconstructed)) return 1;
    const Diff reconstructed_diff = diff(reconstructed, baseline);
    std::cout << "broken reconstruction vs default: mean=" << reconstructed_diff.mean
              << " over=" << reconstructed_diff.fraction_over << '\n';
    check(reconstructed_diff.mean > 0.5, "the lighting pass reconstructs world position through the module");

    // 5. Shadow views ask the visibility rule as well (with the light's culler).
    int shadow_asks = 0;
    int camera_asks = 0;
    SpaceModel watching;
    watching.name = "watching";
    watching.item_visible = [&](const RenderItem &item, const ItemCuller &culler) {
        (culler.shadow_view ? shadow_asks : camera_asks)++;
        return frustum_intersects_sphere(culler.frustum, item.world_bounds_center, item.world_bounds_radius);
    };
    if (Core::RendererResult r = harness.renderer.set_space_model(watching); !r) {
        std::cerr << "FAILED: set_space_model(watching): " << r.error().message << '\n';
        return 1;
    }
    Image watched;
    if (!render(harness, *mesh, *material, watched)) return 1;
    const Diff watched_diff = diff(watched, baseline);
    std::cout << "watching rule: camera_asks=" << camera_asks << " shadow_asks=" << shadow_asks
              << " diff mean=" << watched_diff.mean << '\n';
    check(camera_asks > 0, "the camera passes consulted the rule");
    check(shadow_asks > 0, "the shadow passes consulted the rule");
    check(watched_diff.mean < 1.0, "a rule equal to the frustum test reproduces the default image");

    if (Core::RendererResult r = harness.renderer.set_space_model(SpaceModel{}); !r) {
        std::cerr << "FAILED: final restore: " << r.error().message << '\n';
        return 1;
    }

    // 6. Camera payload: a hook that reproduces the ordinary matrices is invisible; one whose inverse is wrong breaks
    //    every pass that reconstructs world position, proving the inverse is taken from the hook.
    int payload_calls = 0;
    SpaceModel payload_default;
    payload_default.name = "payload default";
    payload_default.camera_payload = [&payload_calls](const CameraView &camera) {
        ++payload_calls;
        const glm::mat4 vp = camera.projection * camera.view;
        return CameraPayload{.view_projection = vp, .inverse_view_projection = glm::inverse(vp)};
    };
    if (Core::RendererResult r = harness.renderer.set_space_model(payload_default); !r) {
        std::cerr << "FAILED: set_space_model(payload_default): " << r.error().message << '\n';
        return 1;
    }
    Image payload_image;
    if (!render(harness, *mesh, *material, payload_image)) return 1;
    const Diff payload_diff = diff(payload_image, baseline);
    check(payload_calls > 0, "the camera payload hook was consulted");
    check(payload_diff.mean < 0.5 && payload_diff.fraction_over < 0.001, "an ordinary payload reproduces the default image");

    SpaceModel payload_broken;
    payload_broken.name = "payload broken inverse";
    payload_broken.camera_payload = [](const CameraView &camera) {
        return CameraPayload{.view_projection = camera.projection * camera.view, .inverse_view_projection = glm::mat4{1.0f}};
    };
    if (Core::RendererResult r = harness.renderer.set_space_model(payload_broken); !r) {
        std::cerr << "FAILED: set_space_model(payload_broken): " << r.error().message << '\n';
        return 1;
    }
    Image payload_bad;
    if (!render(harness, *mesh, *material, payload_bad)) return 1;
    const Diff payload_bad_diff = diff(payload_bad, baseline);
    std::cout << "broken inverse vs default: mean=" << payload_bad_diff.mean << " over=" << payload_bad_diff.fraction_over << '\n';
    check(payload_bad_diff.mean > 0.5, "the inverse view-projection comes from the payload hook");

    // 7. Shadow views: the hook is asked for cascades, and keeping the default (through the hook, which drops the
    //    Euclidean caster list) still renders the same image.
    int cascade_asks = 0;
    SpaceModel shadow_hook;
    shadow_hook.name = "shadow hook";
    shadow_hook.shadow_view_matrix = [&cascade_asks](const ShadowViewRequest &request) -> std::optional<glm::mat4> {
        if (request.kind == ShadowViewKind::DirectionalCascade) {
            ++cascade_asks;
            return request.default_view_projection;
        }
        return std::nullopt;
    };
    if (Core::RendererResult r = harness.renderer.set_space_model(shadow_hook); !r) {
        std::cerr << "FAILED: set_space_model(shadow_hook): " << r.error().message << '\n';
        return 1;
    }
    Image shadow_image;
    if (!render(harness, *mesh, *material, shadow_image)) return 1;
    const Diff shadow_diff = diff(shadow_image, baseline);
    std::cout << "shadow hook: cascade_asks=" << cascade_asks << " diff mean=" << shadow_diff.mean << '\n';
    check(cascade_asks > 0, "the shadow view hook was asked for directional cascades");
    check(shadow_diff.mean < 1.0, "a hook that keeps the default matrix reproduces the default image");

    if (Core::RendererResult r = harness.renderer.set_space_model(SpaceModel{}); !r) {
        std::cerr << "FAILED: restore after hooks: " << r.error().message << '\n';
        return 1;
    }

    harness.renderer.wait_idle();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "SpaceModelGpuTest passed\n";
    return 0;
}

} // namespace

int main() {
    const int result = run();
    SFT::Async::Scheduler::shutdown();
    return result;
}
