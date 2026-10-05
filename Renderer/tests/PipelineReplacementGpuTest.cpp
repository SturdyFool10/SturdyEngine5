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

// Headless GPU test that the built-in graphics stack really is replaceable through the public API. One scene (a
// lit plane) is rendered through the real deferred pipeline several ways, and the pixels are compared:
//   * default pipeline: renders something (not black);
//   * `wrap`ping `gbuffer` and `hiz_build` (calling the original) is invisible in the image and the wrappers ran;
//   * `replace`ing `gbuffer` (and removing `z_prepass`) with a feature written only against public services --
//     the frame's own graph, the GBuffer* blackboard textures, and Renderer::record_draw_items with the caller's
//     own RenderItem list -- draws the plane somewhere else, so the image changes and is still not black;
//   * `set_enabled("light_indicators", false)` etc. are exercised by FramePipelineTest; here we check the stage
//     features are registered in the order the frame needs.
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
        desc.view.render_graph.frame.execution_mode = SFT::RenderSettings::ExecutionMode::WaitForCompletion;
        desc.view.render_graph.frame.bloom.enabled = false;
        desc.view.render_graph.frame.shadows.contact_shadows = false;
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


    /// A `gbuffer` replacement written only against public API: it clears and fills the five G-buffer targets and
    /// depth exactly as a base pass must (same blackboard textures, same formats), but draws its own item list --
    /// the plane, moved sideways -- through `Renderer::record_draw_items`.
    FrameFeatureFn make_custom_gbuffer(MeshHandle mesh, MaterialInstanceHandle material, int *calls) {
        auto items = std::make_shared<std::vector<RenderItem>>();
        RenderItem item{};
        item.mesh = mesh;
        item.material = material;
        item.world_transform = glm::translate(glm::mat4{1.0f}, glm::vec3{0.6f, 0.0f, 0.0f});
        item.previous_world_transform = item.world_transform;
        item.cull_mode = RHI::CullMode::None;
        item.world_bounds_center = {0.6f, 0.0f, 0.0f};
        item.world_bounds_radius = 2.0f;
        items->push_back(item);
        return [items, calls](FrameBuildContext &context) -> Core::RendererResult {
            ++*calls;
            if (!context.settings.render_scene) {
                return {};
            }
            const RenderGraphBlackboard &bb = context.resources;
            const Core::Extent2D extent = context.module.render_extent;
            const auto color = [&](RenderGraphTextureHandle texture, RHI::ClearColor clear) {
                return RenderGraphColorAttachmentDesc{
                    .texture = texture, .load_op = RHI::LoadOp::Clear, .store_op = RHI::StoreOp::Store, .clear_color = clear};
            };
            const DeferredTargetFormats formats = context.deferred_formats;
            context.graph.add_render_pass("custom gbuffer"_ustr)
                .add_color_attachment(color(bb.texture<RenderGraphSemantics::GBufferAlbedo>(), {0, 0, 0, 1}))
                .add_color_attachment(color(bb.texture<RenderGraphSemantics::GBufferNormal>(), {0.5f, 0.5f, 0, 0}))
                .add_color_attachment(color(bb.texture<RenderGraphSemantics::GBufferMaterial>(), {0, 0, 0, 0}))
                .add_color_attachment(color(bb.texture<RenderGraphSemantics::GBufferEmissive>(), {0, 0, 0, 1}))
                .add_color_attachment(color(bb.texture<RenderGraphSemantics::GBufferMotion>(), {0, 0, 0, 0}))
                .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                    .texture = bb.texture<RenderGraphSemantics::ResolvedSceneDepth>(),
                    .depth_load_op = RHI::LoadOp::Clear,
                    .depth_store_op = RHI::StoreOp::Store,
                    .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0}})
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y})
                .set_execute([&context, items, formats, extent](RenderGraphContext &pass) -> Core::RendererResult {
                    const std::array<RHI::Format, 5> targets{formats.albedo, formats.normal, formats.material,
                                                              formats.emissive, formats.motion};
                    return context.renderer.record_draw_items(
                        pass.render_pass(),
                        DrawItemPass{
                            .set = DrawItemSet::Custom,
                            .items = std::span<const RenderItem>{*items},
                            .color_formats = std::span<const RHI::Format>{targets},
                            .depth_format = formats.depth,
                            .frame_index = context.frame_index,
                            .view_projection = context.view_projection,
                            .standard_depth_test = true,
                            .allow_bundles = false,
                            .label = "custom gbuffer",
                            .viewport = RHI::Viewport{.x = 0, .y = 0, .width = static_cast<f32>(extent.x),
                                                      .height = static_cast<f32>(extent.y), .min_depth = 0, .max_depth = 1},
                            .scissor = RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y},
                        },
                        context);
                });
            return {};
        };
    }

} // namespace

namespace {

int run() {
    namespace fs = std::filesystem;
    fs::current_path(fs::path(__FILE__).parent_path().parent_path().parent_path());

    SFT::WindowManager::WindowManager window_manager;
    Harness harness;
    SFT::WindowManager::WindowConfig window_config{};
    window_config.title = "PipelineReplacementGpuTest";
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
        info.app_name = "PipelineReplacementGpuTest";
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

    // Registration order the frame relies on.
    harness.renderer.edit_frame_pipeline([](FramePipeline &pipeline) {
        const auto names = pipeline.names();
        const auto index = [&](std::string_view n) {
            return std::find(names.begin(), names.end(), std::string(n)) - names.begin();
        };
        for (const char *name : {"instance_culling", "shadow_maps", "z_prepass", "gbuffer", "hiz_build"}) {
            check(pipeline.contains(name), std::string("built-in feature registered: ") + name);
        }
        check(index("instance_culling") < index("shadow_maps") && index("shadow_maps") < index("z_prepass") &&
                  index("z_prepass") < index("gbuffer") && index("gbuffer") < index("hiz_build"),
              "scene features are registered in dependency order");
    });

    Image baseline;
    if (!render(harness, *mesh, *material, baseline)) return 1;
    check(mean_luma(baseline) > 5.0, "default pipeline renders the plane (image is not black)");

    // Wrapping is invisible.
    int wrapped_calls = 0;
    harness.renderer.edit_frame_pipeline([&](FramePipeline &pipeline) {
        for (const char *name : {"gbuffer", "hiz_build", "z_prepass"}) {
            (void)pipeline.wrap(name, [&wrapped_calls](FrameBuildContext &context, const FrameFeatureFn &inner) {
                ++wrapped_calls;
                return inner(context);
            });
        }
    });
    Image wrapped;
    if (!render(harness, *mesh, *material, wrapped)) return 1;
    const Diff wrapped_diff = diff(wrapped, baseline);
    std::cout << "wrapped vs default: mean=" << wrapped_diff.mean << " over=" << wrapped_diff.fraction_over << '\n';
    check(wrapped_calls >= 3, "the wrappers ran");
    check(wrapped_diff.mean < 0.5 && wrapped_diff.fraction_over < 0.001, "wrapping built-in features does not change the image");

    // Replacement.
    int custom_calls = 0;
    harness.renderer.edit_frame_pipeline([&](FramePipeline &pipeline) {
        (void)pipeline.remove("z_prepass");
        (void)pipeline.replace("gbuffer", make_custom_gbuffer(*mesh, *material, &custom_calls));
    });
    Image replaced;
    if (!render(harness, *mesh, *material, replaced)) return 1;
    const Diff replaced_diff = diff(replaced, baseline);
    std::cout << "replaced vs default: mean=" << replaced_diff.mean << " over=" << replaced_diff.fraction_over
              << " luma=" << mean_luma(replaced) << '\n';
    check(custom_calls >= 1, "the replacement gbuffer feature ran");
    check(mean_luma(replaced) > 5.0, "the replacement base pass still produces a lit image");
    check(replaced_diff.fraction_over > 0.02, "the replacement base pass's own geometry changed the image");

    harness.renderer.wait_idle();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "PipelineReplacementGpuTest passed\n";
    return 0;
}

} // namespace

int main() {
    const int result = run();
    SFT::Async::Scheduler::shutdown();
    return result;
}
