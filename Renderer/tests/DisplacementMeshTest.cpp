#include <Async/Async.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/Displacement/DisplacementMesh.hpp>
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
#include <memory>
#include <random>
#include <optional>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

// Headless GPU test for the mesh-shader displacement geometry path (Shaders/displacement_mesh.slang, wired
// by Renderer::enable_displacement_mesh_geometry / create_displaced_material). Renders the same displaced
// plane through the real deferred pipeline twice into an off-screen target, once with the LEGACY path (CPU
// pre-tessellated mesh + displacing vertex shader) and once with the MESH path (task -> mesh, base mesh only),
// reads the pixels back and checks:
//   * the mesh material really selected the mesh-shader geometry path (and the legacy one did not);
//   * at a matched refinement (target edge tiny -> every triangle at level 8) the two images agree;
//   * with a camera-dependent, strongly varying per-triangle LOD there are no cracks along the edges between
//     different-LOD triangles (no background leaking through the surface) and the image still matches the
//     dense legacy reference closely;
//   * shadow maps (which draw displaced mesh geometry through the same path) and repeated frames run.
// It needs a real Vulkan device with mesh shaders: otherwise it prints SKIP and succeeds. Vulkan validation is
// not visible to the test; run it with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation and read stdout.
//
// STURDY_DISPLACEMENT_TEST_OUT=<dir> dumps every rendered frame as a PPM.

namespace {

    using namespace SFT;
    using namespace SFT::Renderer;
    namespace D = SFT::Renderer::Displacement;

    int g_failures = 0;
    constexpr u32 kSize = 320;

    bool check(bool condition, const std::string &message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
        return condition;
    }

    struct Image {
        std::vector<u8> rgb;
    };

    /// Smooth bumps: both geometry paths reproduce them, so the comparison isolates the geometry paths rather
    /// than sampling-frequency differences on sharp features.
    std::vector<f32> make_heights(u32 w, u32 h) {
        std::vector<f32> out(static_cast<usize>(w) * h);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const f32 fx = static_cast<f32>(x) / static_cast<f32>(w);
                const f32 fy = static_cast<f32>(y) / static_cast<f32>(h);
                const f32 v = 0.5f + 0.3f * std::sin(fx * 6.2831853f * 2.0f) * std::sin(fy * 6.2831853f * 2.0f) +
                              0.12f * std::sin(fx * 6.2831853f * 5.0f + fy * 6.2831853f * 3.0f);
                out[static_cast<usize>(y) * w + x] = std::clamp(v, 0.0f, 1.0f);
            }
        }
        return out;
    }

    /// A `half`-radius square in XZ facing +Y, cut into quads x quads quads (2 triangles each). uv covers [0,1].
    void make_grid(f32 half, u32 quads, std::vector<GeometryVertex> &vertices, std::vector<u32> &indices) {
        vertices.clear();
        indices.clear();
        for (u32 j = 0; j <= quads; ++j) {
            for (u32 i = 0; i <= quads; ++i) {
                const f32 u = static_cast<f32>(i) / static_cast<f32>(quads);
                const f32 v = static_cast<f32>(j) / static_cast<f32>(quads);
                GeometryVertex g{};
                g.position = {(u * 2.0f - 1.0f) * half, 0.0f, (1.0f - v * 2.0f) * half};
                g.normal = {0, 1, 0};
                g.uv = {u, v};
                g.color = {1, 1, 1, 1};
                g.tangent = {1, 0, 0, 1};
                vertices.push_back(g);
            }
        }
        for (u32 j = 0; j < quads; ++j) {
            for (u32 i = 0; i < quads; ++i) {
                const u32 a = j * (quads + 1) + i;
                const u32 b = a + 1;
                const u32 c = a + (quads + 1);
                const u32 d = c + 1;
                // Counter-clockwise seen from +Y with this uv/position convention (matches A's plane).
                indices.insert(indices.end(), {a, b, d, a, d, c});
            }
        }
    }

    /// The same square as make_grid, but every triangle owns its three vertices (no sharing at all, as with
    /// uv islands / flat shading), triangles in random order and each with a random starting corner, so the
    /// two sides of any edge disagree about vertex indices and about which endpoint comes first.
    void make_grid_unwelded(f32 half, u32 quads, std::vector<GeometryVertex> &vertices, std::vector<u32> &indices) {
        std::vector<GeometryVertex> welded_v;
        std::vector<u32> welded_i;
        make_grid(half, quads, welded_v, welded_i);
        std::vector<u32> order(welded_i.size() / 3);
        for (u32 i = 0; i < order.size(); ++i) order[i] = i;
        std::mt19937 rng(1234);
        std::shuffle(order.begin(), order.end(), rng);
        vertices.clear();
        indices.clear();
        for (u32 t : order) {
            const u32 rotation = static_cast<u32>(rng() % 3);
            for (u32 c = 0; c < 3; ++c) {
                vertices.push_back(welded_v[welded_i[t * 3 + (c + rotation) % 3]]);
                indices.push_back(static_cast<u32>(vertices.size() - 1));
            }
        }
    }

    struct Harness {
        SFT::Renderer::Renderer renderer;
        Core::RenderSurfaceHandle surface{};
        OffscreenRenderTargetHandle target{};
        u64 frame = 0;
        std::string out_dir;
        // When non-empty, the single renderable handed to render() is replicated once per transform.
        std::vector<glm::mat4> instances;
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
            .label = "displacement mesh test readback",
        });
        if (!buffer) return false;
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "displacement mesh test readback"});
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
        auto fence = device->create_fence(RHI::FenceDesc{.label = "displacement mesh test readback fence"});
        if (!fence) return false;
        const RHI::CommandBufferHandle buffers[1] = {*command_buffer};
        RHI::SubmitDesc submit{
            .command_buffers = std::span<const RHI::CommandBufferHandle>{buffers, 1},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "displacement mesh test readback submit",
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

    struct Camera {
        glm::vec3 position;
        glm::vec3 target;
        f32 fov_degrees;
        glm::vec3 sun_direction{-0.6f, -0.5f, -0.4f};
    };

    bool render(Harness &h, const SceneRenderable *renderable, const Camera &camera, const char *name, Image &image) {
        RenderFrameDesc desc{};
        desc.surface = h.surface;
        desc.offscreen_target = h.target;
        desc.frame.framebuffer_width = kSize;
        desc.frame.framebuffer_height = kSize;
        desc.view.camera.world_position = camera.position;
        desc.view.camera.view = glm::lookAt(camera.position, camera.target, glm::vec3{0, 1, 0});
        desc.view.camera.projection = glm::perspectiveRH_ZO(glm::radians(camera.fov_degrees), 1.0f, 0.05f, 80.0f);
        desc.view.camera.vertical_fov_radians = glm::radians(camera.fov_degrees);
        desc.view.camera.previous_view_projection = desc.view.camera.projection * desc.view.camera.view;
        desc.view.lighting.sun.direction = glm::normalize(camera.sun_direction);
        desc.view.lighting.sun.radiance = {3.0f, 2.9f, 2.7f};
        std::vector<SceneRenderable> list;
        if (renderable != nullptr) {
            if (h.instances.empty()) {
                list.push_back(*renderable);
            } else {
                for (usize i = 0; i < h.instances.size(); ++i) {
                    SceneRenderable copy = *renderable;
                    copy.world_transform = h.instances[i];
                    copy.stable_id = i + 1;
                    list.push_back(copy);
                }
            }
        }
        desc.view.renderables = std::span<const SceneRenderable>{list.data(), list.size()};
        desc.view.render_graph.wait_for_completion = true;
        desc.view.render_graph.draw_overlay_text = false;
        desc.view.render_graph.bloom = false;
        desc.view.render_graph.contact_shadows = false;

        // Several frames: the first builds pipelines/history, later ones exercise the per-frame ring reuse.
        for (int i = 0; i < 4; ++i) {
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
        f64 mean = 0.0;
        f64 fraction_over = 0.0; // pixels with any channel differing by > 24
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
            over += worst > 24 ? 1 : 0;
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

    bool same_pixel(const Image &a, const Image &b, usize p, int tol) {
        for (int c = 0; c < 3; ++c) {
            if (std::abs(int(a.rgb[p * 3 + c]) - int(b.rgb[p * 3 + c])) > tol) return false;
        }
        return true;
    }

    /// Cracks: pixels that are background in `mesh` although `legacy` has surface there AND the surrounding
    /// window (radius 3) in `legacy` is entirely surface, i.e. holes strictly inside the surface, not
    /// silhouette differences at its border.
    usize count_cracks(const Image &mesh, const Image &legacy, const Image &background) {
        std::vector<u8> surface(static_cast<usize>(kSize) * kSize, 0);
        for (usize p = 0; p < surface.size(); ++p) {
            surface[p] = same_pixel(legacy, background, p, 6) ? 0 : 1;
        }
        usize cracks = 0;
        constexpr int r = 3;
        for (int y = r; y < static_cast<int>(kSize) - r; ++y) {
            for (int x = r; x < static_cast<int>(kSize) - r; ++x) {
                bool interior = true;
                for (int dy = -r; dy <= r && interior; ++dy) {
                    for (int dx = -r; dx <= r; ++dx) {
                        if (!surface[static_cast<usize>(y + dy) * kSize + (x + dx)]) { interior = false; break; }
                    }
                }
                if (interior && same_pixel(mesh, background, static_cast<usize>(y) * kSize + x, 6)) ++cracks;
            }
        }
        return cracks;
    }

    /// Emissive-saturated renders make "is there surface here" unambiguous: covered pixels are ~white, everything
    /// else is sky/ground. Counts pixels where `legacy` has surface but `mesh` does not (cracks/holes) and the
    /// reverse (extra coverage), ignoring a 2-pixel band along the legacy silhouette.
    struct Coverage {
        usize holes = 0;
        usize extra = 0;
        usize legacy_interior_holes = 0; // uncovered pixels inside the legacy surface (shading/geometry artefacts common to both)
    };
    bool covered(const Image &img, usize p) {
        return std::min({img.rgb[p * 3], img.rgb[p * 3 + 1], img.rgb[p * 3 + 2]}) >= 235;
    }
    Coverage compare_coverage(const Image &mesh, const Image &legacy) {
        Coverage c;
        constexpr int r = 2;
        for (int y = r; y < static_cast<int>(kSize) - r; ++y) {
            for (int x = r; x < static_cast<int>(kSize) - r; ++x) {
                const usize p = static_cast<usize>(y) * kSize + x;
                bool near_silhouette = false;
                for (int dy = -r; dy <= r && !near_silhouette; ++dy) {
                    for (int dx = -r; dx <= r; ++dx) {
                        if (covered(legacy, static_cast<usize>(y + dy) * kSize + (x + dx)) != covered(legacy, p)) {
                            near_silhouette = true;
                            break;
                        }
                    }
                }
                if (near_silhouette) continue;
                if (covered(legacy, p) && !covered(mesh, p)) ++c.holes;
                if (!covered(legacy, p) && covered(mesh, p)) ++c.extra;
            }
        }
        return c;
    }

} // namespace

int main() {
    namespace fs = std::filesystem;
    fs::current_path(fs::path(__FILE__).parent_path().parent_path().parent_path());

    auto harness_owner = std::make_unique<Harness>();
    Harness &harness = *harness_owner;
    if (const char *dir = std::getenv("STURDY_DISPLACEMENT_TEST_OUT")) {
        harness.out_dir = dir;
        fs::create_directories(harness.out_dir);
    }

    SFT::WindowManager::WindowManager window_manager;
    SFT::WindowManager::WindowConfig window_config{};
    window_config.title = "DisplacementMeshTest";
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
        info.app_name = "DisplacementMeshTest";
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
    if (!harness.renderer.displacement_mesh_geometry_supported()) {
        std::cout << "SKIP: device has no mesh/task shader support\n";
        return 0;
    }
    auto target = harness.renderer.create_offscreen_render_target(
        OffscreenRenderTargetDescription{.extent = {kSize, kSize}, .label = "displacement mesh test target"});
    if (!target) {
        std::cerr << "FAILED: off-screen target: " << target.error().message << '\n';
        return 1;
    }
    harness.target = *target;

    constexpr u32 kW = 128;
    constexpr u32 kH = 128;
    const std::vector<f32> heights = make_heights(kW, kH);

    auto make_material = [&](const char *label, D::HardwareTier tier, D::GeometryPath geometry, f32 half, f32 height_scale) {
        DisplacedMaterialDesc desc{};
        desc.heights = heights;
        desc.width = kW;
        desc.height = kH;
        desc.tier = tier;
        desc.request.desired_algorithm = D::HeightfieldAlgorithm::NormalOnly;
        desc.request.desired_geometry = geometry;
        desc.height_scale = height_scale;
        desc.reference_height = 0.5f; // displaces both up and down
        desc.tile_size = {half * 2.0f, half * 2.0f};
        desc.base_color = {0.8f, 0.7f, 0.6f, 1.0f};
        desc.label = label;
        return harness.renderer.create_displaced_material(desc);
    };

    // ---- Scene 1: matched LOD, small plane ----------------------------------------------------------------
    struct Scene {
        const char *name;
        f32 half;
        u32 base_quads;    // mesh path base grid
        u32 legacy_level;  // legacy pre-tessellation of the 2-triangle plane
        f32 target_edge;   // mesh path target edge pixels
        f32 height_scale;
        bool probe_shadows; // low sun + tall relief so the shadow atlas visibly matters
        Camera camera;
        u32 instances_per_side = 0; // > 0: an N x N grid of copies (exercises parallel render-bundle recording)
        bool unwelded = false;      // mesh-path base mesh with no shared vertices and shuffled order
    };
    const Scene scenes[] = {
        // Every triangle at the level ceiling: mesh lattice = 4 * 8 = 32 = legacy lattice.
        {"matched", 1.0f, 4, 32, 0.05f, 0.3f, false, {{0.0f, 1.1f, 2.0f}, {0.0f, 0.0f, 0.1f}, 50.0f}},
        // Camera-dependent LOD across a large plane seen at a grazing angle: neighbouring triangles land on
        // different levels, which is exactly where cracks would open.
        {"varying", 3.0f, 8, 64, 10.0f, 0.3f, false, {{0.0f, 0.7f, 3.4f}, {0.0f, 0.0f, -0.5f}, 70.0f}},
        // Every triangle at level 8 again, but tall relief under a low sun: the shadow atlas (drawn through the
        // mesh path's depth-only pipeline) now decides a large part of the image.
        {"shadowed", 1.0f, 4, 32, 0.05f, 0.9f, true, {{0.0f, 1.6f, 2.2f}, {0.0f, 0.0f, 0.0f}, 50.0f, {-0.85f, -0.22f, -0.3f}}},
        // The varying-LOD plane again, but the mesh-path base mesh is fully unwelded and shuffled: adjacent
        // triangles share positions, not vertex indices.
        {"unwelded", 3.0f, 8, 64, 10.0f, 0.3f, false, {{0.0f, 0.7f, 3.4f}, {0.0f, 0.0f, -0.5f}, 70.0f}, 0, true},
        // 15 x 15 = 225 displaced objects: enough to switch the G-buffer / shadow passes to parallel render
        // bundles (kParallelRecordThreshold = 128), so mesh draws are recorded from worker threads.
        {"many", 0.35f, 2, 16, 0.05f, 0.3f, false, {{0.0f, 7.0f, 8.0f}, {0.0f, 0.0f, 0.0f}, 50.0f}, 15, false},
    };

    std::vector<Image> mesh_images;
    std::vector<Image> legacy_images;
    bool ok = true;

    for (const Scene &scene : scenes) {
        Image bg;
        harness.instances.clear();
        if (!render(harness, nullptr, scene.camera, (std::string("background_") + scene.name).c_str(), bg)) return 1;
        if (scene.instances_per_side > 0) {
            for (u32 j = 0; j < scene.instances_per_side; ++j) {
                for (u32 i = 0; i < scene.instances_per_side; ++i) {
                    const f32 half_span = 0.8f * static_cast<f32>(scene.instances_per_side - 1) * 0.5f;
                    harness.instances.push_back(glm::translate(
                        glm::mat4{1.0f}, glm::vec3{0.8f * i - half_span, 0.0f, 0.8f * j - half_span}));
                }
            }
        }
        std::vector<GeometryVertex> base_v;
        std::vector<u32> base_i;
        if (scene.unwelded) make_grid_unwelded(scene.half, scene.base_quads, base_v, base_i);
        else make_grid(scene.half, scene.base_quads, base_v, base_i);
        std::vector<GeometryVertex> coarse_v;
        std::vector<u32> coarse_i;
        make_grid(scene.half, 1, coarse_v, coarse_i);

        auto mesh_material = make_material("mesh material", D::HardwareTier::High, D::GeometryPath::MeshShader, scene.half, scene.height_scale);
        auto legacy_material = make_material("legacy material", D::HardwareTier::Medium, D::GeometryPath::PreTessellatedVertex, scene.half, scene.height_scale);
        if (!mesh_material || !legacy_material) {
            std::cerr << "FAILED: create_displaced_material: "
                      << (!mesh_material ? mesh_material.error().message : legacy_material.error().message) << '\n';
            return 1;
        }
        check(mesh_material->plan.geometry == D::GeometryPath::MeshShader, "High tier + MeshShader request planned the mesh path");
        check(harness.renderer.displacement_mesh_geometry_active(mesh_material->material_template),
              "the mesh material's template is active on the mesh-shader path");
        check(legacy_material->plan.geometry == D::GeometryPath::PreTessellatedVertex, "Medium tier planned the legacy path");
        check(!harness.renderer.displacement_mesh_geometry_active(legacy_material->material_template),
              "the legacy material's template is NOT on the mesh path");

        // Re-enable with this scene's target edge (the automatic settings come from the tier).
        DisplacementMeshSettings settings{};
        settings.target_edge_pixels = scene.target_edge;
        settings.max_level = 8;
        // Experiment overrides (not used by the default run).
        if (const char *edge = std::getenv("DM_EDGE")) settings.target_edge_pixels = static_cast<f32>(std::atof(edge));
        if (const char *lvl = std::getenv("DM_MAXLEVEL")) settings.max_level = static_cast<u32>(std::atoi(lvl));
        if (Core::RendererResult enabled = harness.renderer.enable_displacement_mesh_geometry(
                mesh_material->material_template, settings);
            !enabled) {
            std::cerr << "FAILED: enable_displacement_mesh_geometry: " << enabled.error().message << '\n';
            return 1;
        }

        auto mesh_mesh = harness.renderer.create_displaced_mesh(*mesh_material, base_v, base_i, "mesh path base");
        D::SubdividedMesh dense = D::subdivide_uniform(coarse_v, coarse_i, scene.legacy_level);
        auto legacy_mesh = harness.renderer.create_mesh(dense.vertices, dense.indices, "legacy dense");
        if (!mesh_mesh || !legacy_mesh) {
            std::cerr << "FAILED: mesh creation\n";
            return 1;
        }
        std::cout << scene.name << ": mesh path base triangles=" << base_i.size() / 3
                  << ", legacy triangles=" << dense.indices.size() / 3 << '\n';

        // CPU mirror of the per-triangle level selection (same camera, same constants) for the record.
        {
            const f32 ppu = static_cast<f32>(kSize) / (2.0f * std::tan(glm::radians(scene.camera.fov_degrees) * 0.5f));
            u32 lo = 99;
            u32 hi = 0;
            for (usize t = 0; t + 2 < base_i.size(); t += 3) {
                D::TriangleLodInput in{};
                in.p0 = base_v[base_i[t]].position;
                in.p1 = base_v[base_i[t + 1]].position;
                in.p2 = base_v[base_i[t + 2]].position;
                in.max_displacement = scene.height_scale;
                in.pixels_per_unit_at_one = ppu;
                in.camera_position = scene.camera.position;
                in.target_edge_pixels = scene.target_edge;
                in.max_level = 8;
                const u32 level = D::select_subdivision_level(in);
                lo = std::min(lo, level);
                hi = std::max(hi, level);
            }
            std::cout << scene.name << ": CPU-mirror triangle levels span " << lo << ".." << hi << '\n';
        }

        SceneRenderable renderable{};
        renderable.stable_id = 1;
        renderable.cull_mode = RHI::CullMode::None;
        renderable.casts_shadows = true;

        Image mesh_image;
        Image legacy_image;
        renderable.mesh = *mesh_mesh;
        renderable.material = mesh_material->instance;
        const std::string mesh_name = std::string("mesh_") + scene.name;
        if (!render(harness, &renderable, scene.camera, mesh_name.c_str(), mesh_image)) { ok = false; break; }
        renderable.mesh = *legacy_mesh;
        renderable.material = legacy_material->instance;
        const std::string legacy_name = std::string("legacy_") + scene.name;
        if (!render(harness, &renderable, scene.camera, legacy_name.c_str(), legacy_image)) { ok = false; break; }

        // Coverage renders: same scene with the surface saturated to white by a huge emissive term.
        {
            const glm::vec4 glow{40.0f, 40.0f, 40.0f, 0.0f};
            const auto bytes = std::as_bytes(std::span<const glm::vec4>{&glow, 1});
            if (auto set = harness.renderer.set_material_parameter(mesh_material->instance, "emissive_factor", bytes); !set) {
                std::cerr << "FAILED: set emissive_factor (mesh): " << set.error().message << '\n';
                ++g_failures;
            }
            if (auto set = harness.renderer.set_material_parameter(legacy_material->instance, "emissive_factor", bytes); !set) {
                std::cerr << "FAILED: set emissive_factor (legacy): " << set.error().message << '\n';
                ++g_failures;
            }
            Image mesh_cov;
            Image legacy_cov;
            renderable.mesh = *mesh_mesh;
            renderable.material = mesh_material->instance;
            const std::string mesh_cov_name = std::string("meshcov_") + scene.name;
            if (!render(harness, &renderable, scene.camera, mesh_cov_name.c_str(), mesh_cov)) { ok = false; break; }
            renderable.mesh = *legacy_mesh;
            renderable.material = legacy_material->instance;
            const std::string legacy_cov_name = std::string("legacycov_") + scene.name;
            if (!render(harness, &renderable, scene.camera, legacy_cov_name.c_str(), legacy_cov)) { ok = false; break; }
            const Coverage cov = compare_coverage(mesh_cov, legacy_cov);
            usize legacy_covered = 0;
            for (usize p = 0; p < static_cast<usize>(kSize) * kSize; ++p) legacy_covered += covered(legacy_cov, p) ? 1 : 0;
            std::cout << scene.name << ": coverage (emissive-saturated): legacy surface pixels=" << legacy_covered
                      << ", holes in mesh path (legacy covered, mesh not)=" << cov.holes
                      << ", extra mesh coverage=" << cov.extra << '\n';
            check(legacy_covered > 1000, std::string(scene.name) + ": coverage render shows a surface");
            check(cov.holes == 0, std::string(scene.name) + ": no coverage holes (cracks) in the mesh path vs the dense legacy surface");
        }

        if (&scene == &scenes[0] || scene.probe_shadows) {
            // The emissive glow above was only for the coverage renders.
            const glm::vec4 no_glow{0.0f, 0.0f, 0.0f, 0.0f};
            const auto bytes = std::as_bytes(std::span<const glm::vec4>{&no_glow, 1});
            (void)harness.renderer.set_material_parameter(mesh_material->instance, "emissive_factor", bytes);
            (void)harness.renderer.set_material_parameter(legacy_material->instance, "emissive_factor", bytes);

            // Shadows: both paths draw displaced geometry into the shadow atlas. Show that the shadow term is
            // part of the picture (no-shadow legacy differs from shadowed legacy) and that the mesh path still
            // matches when shadows are on AND when they are off.
            Image legacy_noshadow;
            Image mesh_noshadow;
            renderable.casts_shadows = false;
            renderable.mesh = *legacy_mesh;
            renderable.material = legacy_material->instance;
            if (!render(harness, &renderable, scene.camera, (std::string("legacy_noshadow_") + scene.name).c_str(), legacy_noshadow)) { ok = false; break; }
            renderable.mesh = *mesh_mesh;
            renderable.material = mesh_material->instance;
            if (!render(harness, &renderable, scene.camera, (std::string("mesh_noshadow_") + scene.name).c_str(), mesh_noshadow)) { ok = false; break; }
            renderable.casts_shadows = true;
            const Diff shadow_effect = diff(legacy_image, legacy_noshadow);
            const Diff noshadow_match = diff(mesh_noshadow, legacy_noshadow);
            std::cout << scene.name << ": shadow effect on the legacy image mean=" << shadow_effect.mean
                      << " over=" << shadow_effect.fraction_over << "; mesh vs legacy without shadows mean="
                      << noshadow_match.mean << " over=" << noshadow_match.fraction_over << '\n';
            check(noshadow_match.mean < 2.0 && noshadow_match.fraction_over < 0.02, "matched LOD without shadows: mesh path matches legacy");

            if (&scene != &scenes[0]) {
                check(shadow_effect.mean > 0.5, "shadows are visible in this scene (otherwise the shadow comparison proves nothing)");
                const Diff shadowed_match = diff(mesh_image, legacy_image);
                check(shadowed_match.mean < 2.0 && shadowed_match.fraction_over < 0.02,
                      "shadowed scene: mesh path (with its own shadow-atlas geometry) matches legacy");
            }
        }
        if (&scene == &scenes[0]) {
            // Hot reload of the material template destroys/recreates its set-0 layout, which the mesh path's
            // pipeline layout embeds: the mesh path must rebuild against the new layout and draw the same image.
            if (Core::RendererResult reloaded = harness.renderer.reload_material_template(mesh_material->material_template);
                !reloaded) {
                std::cerr << "FAILED: reload_material_template: " << reloaded.error().message << '\n';
                ++g_failures;
            }
            Image mesh_reloaded;
            renderable.mesh = *mesh_mesh;
            renderable.material = mesh_material->instance;
            if (!render(harness, &renderable, scene.camera, "mesh_matched_reloaded", mesh_reloaded)) { ok = false; break; }
            const Diff reload_diff = diff(mesh_reloaded, mesh_image);
            std::cout << scene.name << ": after template hot-reload mesh image vs before mean=" << reload_diff.mean
                      << " over=" << reload_diff.fraction_over << '\n';
            check(reload_diff.mean < 0.01, "mesh path survives a material-template hot reload with an identical image");

            // Negative control: a coarse mesh-path refinement (level 1 = the base mesh) MUST differ from the
            // dense legacy surface, otherwise the comparisons above could not tell the paths apart.
            DisplacementMeshSettings coarse{};
            coarse.target_edge_pixels = 4000.0f;
            coarse.max_level = 8;
            if (Core::RendererResult enabled = harness.renderer.enable_displacement_mesh_geometry(
                    mesh_material->material_template, coarse);
                !enabled) {
                std::cerr << "FAILED: enable_displacement_mesh_geometry (coarse): " << enabled.error().message << '\n';
                ++g_failures;
            }
            Image mesh_coarse;
            if (!render(harness, &renderable, scene.camera, "mesh_matched_coarse", mesh_coarse)) { ok = false; break; }
            const Diff control = diff(mesh_coarse, legacy_image);
            std::cout << scene.name << ": negative control (level-1 mesh path vs dense legacy) mean=" << control.mean
                      << " over=" << control.fraction_over << '\n';
            check(control.fraction_over > 0.001, "negative control: a coarse mesh-path refinement differs from the dense reference");
        }

        const Diff d = diff(mesh_image, legacy_image);
        const usize cracks = count_cracks(mesh_image, legacy_image, bg);
        std::cout << scene.name << ": mesh vs legacy mean=" << d.mean << " over=" << d.fraction_over
                  << " cracks=" << cracks << " luma(mesh)=" << mean_luma(mesh_image)
                  << " luma(legacy)=" << mean_luma(legacy_image) << " luma(bg)=" << mean_luma(bg) << '\n';
        check(mean_luma(mesh_image) != mean_luma(bg), std::string(scene.name) + ": mesh path drew something");
        check(diff(mesh_image, bg).fraction_over > 0.05, std::string(scene.name) + ": mesh image differs from empty background");
        check(cracks == 0, std::string(scene.name) + ": no crack pixels inside the surface");
        if (scene.target_edge < 1.0f) {
            check(d.mean < 2.0 && d.fraction_over < 0.02,
                  std::string(scene.name) + ": matched LOD: mesh path matches the legacy path");
        } else {
            check(d.mean < 6.0 && d.fraction_over < 0.06,
                  std::string(scene.name) + ": varying LOD: mesh path stays close to the dense legacy reference");
        }

        harness.renderer.destroy_mesh(*mesh_mesh);
        harness.renderer.destroy_mesh(*legacy_mesh);
        harness.renderer.destroy_displaced_material(mesh_material->instance);
        harness.renderer.destroy_displaced_material(legacy_material->instance);
    }
    harness.renderer.wait_idle();

    // The recording paths use Async::Scheduler workers (render bundles); leaving them running at exit makes the
    // pool's destructor terminate the process.
    harness_owner.reset();
    SFT::Async::Scheduler::shutdown();
    if (g_failures != 0 || !ok) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "DisplacementMeshTest passed\n";
    return 0;
}
