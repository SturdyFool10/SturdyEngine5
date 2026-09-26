#include <Renderer/RendererModule.hpp>
#include <WindowManager/WindowManager.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

// Headless GPU test for the live displacement wiring (Renderer::create_displaced_material). Renders a
// displaced plane through the real deferred pipeline into an off-screen target, reads the pixels back, and
// checks:
//   * displacement visibly changes the image relative to NormalOnly (same plane, same light, same camera);
//   * the exact per-pixel blocks agree with each other, i.e. the packed hierarchy the Renderer uploaded
//     really is conservative (hierarchical == cell-exact, to float noise);
//   * DisplacedDepth (fragment depth write, no prepass) draws the same colours as BaseSurface depth when the
//     plane is alone in the scene, and does not error;
//   * the legacy geometry path (pre-tessellated mesh + vertexMainDisplaced) runs and changes the image.
// It needs a real Vulkan device: with none it prints SKIP and succeeds, so CPU-only CI stays green. Vulkan
// validation output is not visible to the test itself; run it with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
// and inspect stdout (see the report / DisplacementRenderTest notes).
//
// STURDY_DISPLACEMENT_TEST_OUT=<dir> dumps every rendered frame as a PPM for eyeballing.

namespace {

    using namespace SFT;
    using namespace SFT::Renderer;
    namespace D = SFT::Renderer::Displacement;

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
            .label = "displacement test readback",
        });
        if (!buffer) return false;
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "displacement test readback"});
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
        auto fence = device->create_fence(RHI::FenceDesc{.label = "displacement test readback fence"});
        if (!fence) return false;
        const RHI::CommandBufferHandle buffers[1] = {*command_buffer};
        RHI::SubmitDesc submit{
            .command_buffers = std::span<const RHI::CommandBufferHandle>{buffers, 1},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "displacement test readback submit",
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

    bool render(Harness &h, const DisplacedMaterial &material, MeshHandle mesh, const char *name, Image &image) {
        SceneRenderable renderable{};
        renderable.mesh = mesh;
        renderable.material = material.instance;
        renderable.stable_id = 1;
        renderable.cull_mode = RHI::CullMode::None;
        renderable.casts_shadows = true;

        RenderFrameDesc desc{};
        desc.surface = h.surface;
        desc.offscreen_target = h.target;
        desc.frame.frame_index = h.frame++;
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
        desc.view.render_graph.draw_overlay_text = false;
        desc.view.render_graph.bloom = false;
        desc.view.render_graph.contact_shadows = false;

        // Two frames: the first builds pipelines/history, the second is the steady-state image.
        for (int i = 0; i < 2; ++i) {
            desc.frame.frame_index = h.frame++;
            if (Core::RendererResult r = h.renderer.render_frame(desc); !r) {
                std::cerr << "render_frame failed for " << name << ": " << r.error().message << '\n';
                return false;
            }
        }
        h.renderer.wait_idle();
        if (!read_back(h, image)) {
            std::cerr << "readback failed for " << name << '\n';
            return false;
        }
        if (!h.out_dir.empty()) {
            std::ofstream file(std::filesystem::path(h.out_dir) / (std::string(name) + ".ppm"), std::ios::binary);
            file << "P6\n" << kSize << ' ' << kSize << "\n255\n";
            file.write(reinterpret_cast<const char *>(image.rgb.data()), static_cast<std::streamsize>(image.rgb.size()));
        }
        return true;
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
    // Shader paths are repository-relative.
    fs::current_path(fs::path(__FILE__).parent_path().parent_path().parent_path());

    // The Vulkan backend needs a window to create its primary surface even when every frame goes to an
    // off-screen target, so make a small hidden one.
    SFT::WindowManager::WindowManager window_manager;
    // Declared after the window manager so the Renderer (and its surface) is destroyed while the window still exists.
    Harness harness;
    if (const char *dir = std::getenv("STURDY_DISPLACEMENT_TEST_OUT")) {
        harness.out_dir = dir;
        fs::create_directories(harness.out_dir);
    }

    SFT::WindowManager::WindowConfig window_config{};
    window_config.title = "DisplacementRenderTest";
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
        info.app_name = "DisplacementRenderTest";
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
        OffscreenRenderTargetDescription{.extent = {kSize, kSize}, .label = "displacement test target"});
    if (!target) {
        std::cerr << "FAILED: off-screen target: " << target.error().message << '\n';
        return 1;
    }
    harness.target = *target;

    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    const std::vector<f32> heights = make_heights(kW, kH);
    std::vector<GeometryVertex> vertices;
    std::vector<u32> indices;
    make_plane(vertices, indices);

    auto make = [&](const char *label, D::HardwareTier tier, D::HeightfieldAlgorithm algorithm, D::DepthPolicy depth,
                    bool silhouette) -> std::optional<DisplacedMaterial> {
        DisplacedMaterialDesc desc{};
        desc.heights = heights;
        desc.width = kW;
        desc.height = kH;
        desc.tier = tier;
        desc.request.desired_algorithm = algorithm;
        desc.request.desired_depth = depth;
        desc.request.silhouette_critical = silhouette;
        desc.request.desired_geometry = silhouette ? D::GeometryPath::MeshShader : D::GeometryPath::None;
        desc.height_scale = 0.25f;
        desc.reference_height = 1.0f;
        desc.tile_size = {2.0f, 2.0f};
        desc.base_color = {0.8f, 0.7f, 0.6f, 1.0f};
        desc.label = label;
        auto material = harness.renderer.create_displaced_material(desc);
        if (!material) {
            std::cerr << "FAILED: create_displaced_material(" << label << "): " << material.error().message << '\n';
            ++g_failures;
            return std::nullopt;
        }
        return *material;
    };

    struct Case {
        const char *name;
        D::HardwareTier tier;
        D::HeightfieldAlgorithm algorithm;
        D::DepthPolicy depth;
        bool silhouette;
    };
    const Case cases[] = {
        {"normal_only", D::HardwareTier::High, D::HeightfieldAlgorithm::NormalOnly, D::DepthPolicy::BaseSurface, false},
        {"pom_low", D::HardwareTier::Low, D::HeightfieldAlgorithm::ParallaxOcclusion, D::DepthPolicy::BaseSurface, false},
        {"cell_exact", D::HardwareTier::High, D::HeightfieldAlgorithm::CellExact, D::DepthPolicy::BaseSurface, false},
        {"hierarchical", D::HardwareTier::High, D::HeightfieldAlgorithm::HierarchicalCellExact, D::DepthPolicy::BaseSurface, false},
        {"hierarchical_depth", D::HardwareTier::High, D::HeightfieldAlgorithm::HierarchicalCellExact, D::DepthPolicy::DisplacedDepth, false},
        {"legacy_geometry", D::HardwareTier::Medium, D::HeightfieldAlgorithm::CellExact, D::DepthPolicy::BaseSurface, true},
    };

    std::vector<Image> images(std::size(cases));
    std::vector<DisplacedMaterial> materials;
    bool ok = true;
    for (usize i = 0; i < std::size(cases); ++i) {
        const Case &c = cases[i];
        auto material = make(c.name, c.tier, c.algorithm, c.depth, c.silhouette);
        if (!material) { ok = false; break; }
        materials.push_back(*material);

        auto mesh = harness.renderer.create_displaced_mesh(*material, vertices, indices, c.name);
        if (!mesh) {
            std::cerr << "FAILED: create_displaced_mesh(" << c.name << "): " << mesh.error().message << '\n';
            ++g_failures;
            ok = false;
            break;
        }
        std::cout << c.name << ": algorithm=" << D::to_string(material->plan.algorithm)
                  << " geometry=" << D::to_string(material->plan.geometry)
                  << " depth=" << D::to_string(material->plan.depth) << " hierarchy_levels=" << material->hierarchy_levels
                  << " mesh_tris=" << (harness.renderer.mesh(*mesh) ? harness.renderer.mesh(*mesh)->index_count / 3 : 0) << '\n';
        if (!render(harness, *material, *mesh, c.name, images[i])) { ++g_failures; ok = false; break; }
        harness.renderer.destroy_mesh(*mesh);
    }

    if (ok) {
        const Image &normal_only = images[0];
        const Image &pom = images[1];
        const Image &cell = images[2];
        const Image &hier = images[3];
        const Image &hier_depth = images[4];
        const Image &legacy = images[5];

        std::cout << "mean luma: normal_only=" << mean_luma(normal_only) << " pom=" << mean_luma(pom)
                  << " cell=" << mean_luma(cell) << " hier=" << mean_luma(hier) << '\n';
        check(mean_luma(normal_only) > 5.0, "the NormalOnly plane rendered (image is not black)");

        const Diff hier_vs_normal = diff(hier, normal_only);
        const Diff pom_vs_normal = diff(pom, normal_only);
        const Diff cell_vs_hier = diff(cell, hier);
        const Diff depth_vs_hier = diff(hier_depth, hier);
        const Diff legacy_vs_normal = diff(legacy, normal_only);
        std::cout << "hier vs normal-only:   mean=" << hier_vs_normal.mean << " over=" << hier_vs_normal.fraction_over << '\n'
                  << "pom vs normal-only:    mean=" << pom_vs_normal.mean << " over=" << pom_vs_normal.fraction_over << '\n'
                  << "cell vs hier:          mean=" << cell_vs_hier.mean << " over=" << cell_vs_hier.fraction_over << '\n'
                  << "hier+depth vs hier:    mean=" << depth_vs_hier.mean << " over=" << depth_vs_hier.fraction_over << '\n'
                  << "legacy vs normal-only: mean=" << legacy_vs_normal.mean << " over=" << legacy_vs_normal.fraction_over << '\n';

        check(hier_vs_normal.fraction_over > 0.05, "hierarchical displacement visibly differs from NormalOnly");
        check(pom_vs_normal.fraction_over > 0.05, "POM displacement visibly differs from NormalOnly");
        check(cell_vs_hier.fraction_over < 0.01 && cell_vs_hier.mean < 1.0,
              "hierarchical traversal matches cell-exact (the uploaded hierarchy is conservative)");
        check(depth_vs_hier.fraction_over < 0.02 && depth_vs_hier.mean < 1.5,
              "DisplacedDepth colours match BaseSurface depth when the plane is alone");
        check(legacy_vs_normal.fraction_over > 0.02, "legacy pre-tessellated geometry path changes the image");

        check(materials[4].writes_displaced_depth, "DisplacedDepth plan produced a depth-writing variant");
        check(materials[5].needs_pre_tessellated_mesh, "Medium tier + silhouette_critical chose the legacy geometry path");
        check(materials[3].hierarchy_texture.is_valid() && materials[3].hierarchy_levels > 1,
              "hierarchical plan uploaded a hierarchy texture");
    }

    for (const DisplacedMaterial &material : materials) {
        harness.renderer.destroy_displaced_material(material.instance);
    }
    harness.renderer.wait_idle();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "DisplacementRenderTest passed\n";
    return 0;
}

} // namespace

int main() {
    const int result = run();
    // The Renderer starts Async::Scheduler workers; joinable threads at static destruction would terminate().
    SFT::Async::Scheduler::shutdown();
    return result;
}
