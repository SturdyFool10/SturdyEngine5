#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <Async/Async.hpp>
#pragma endregion

#include <Renderer/Displacement/DisplacementMesh.hpp>
#include <Renderer/Displacement/HeightfieldHierarchy.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/ShaderTarget.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {

        namespace slang = Core::Slang;

        // Whether the Renderer can *draw* the MeshShader geometry path yet. The planner would happily pick
        // it on a mesh-shader device; until the mesh pipeline + draw are wired (Shaders/displacement_mesh.slang
        // has no Renderer caller), letting the plan say MeshShader would collapse the per-pixel block to
        // NormalOnly for a geometry path that never runs, i.e. lose displacement entirely. Flip this when
        // the mesh path lands and create_displaced_material will start selecting it.
        // (Stream B: the mesh path now exists -- RendererDisplacementMesh.cpp. It is drawable exactly when
        // Renderer::displacement_mesh_geometry_supported(); the constant only remains as a kill switch.)
        constexpr bool kMeshDisplacementDrawable = true;

        [[nodiscard]] Core::GraphicsBackendError displacement_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        template <typename T>
        [[nodiscard]] span<const std::byte> bytes_of(const T &value) {
            return std::as_bytes(span<const T>{&value, 1});
        }

        [[nodiscard]] u32 pom_steps_for(u32 max_steps) noexcept { return std::max(8u, max_steps / 2u); }

    } // namespace

    Displacement::DisplacementCapabilities Renderer::displacement_capabilities() const {
        const RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Displacement::DisplacementCapabilities::baseline();
        }
        Displacement::DisplacementCapabilities caps =
            Displacement::DisplacementCapabilities::from_rhi(device->enabled_features(), device->feature_properties());
        if (!kMeshDisplacementDrawable || !displacement_mesh_geometry_supported()) {
            caps.mesh_shader = false;
            caps.task_shader = false;
        }
        // WGSL sessions bind height/hierarchy as plain sampler-less Texture2D (see sturdy_heightfield.slang), so the
        // sampler-requiring GatherRed optimization is unavailable there.
        if (auto target = shader_target_for_device(*device); target && target->module_language == RHI::ShaderLanguage::Wgsl) {
            caps.height_gather = false;
        }
        return caps;
    }

    Core::RendererExpected<DisplacedMaterial> Renderer::create_displaced_material(const DisplacedMaterialDesc &desc) {
        ZoneScopedN("Renderer::create_displaced_material");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(displacement_error("Cannot create a displaced material without an RHI device."));
        }
        if (desc.width == 0 || desc.height == 0 ||
            desc.heights.size() < static_cast<usize>(desc.width) * desc.height) {
            return unexpected(displacement_error("Displaced material heightfield is empty or smaller than width * height."));
        }

        const Displacement::DisplacementCapabilities caps = displacement_capabilities();
        DisplacedMaterial out{};
        out.plan = Displacement::plan_displacement(desc.tier, caps, desc.request);
        Displacement::DisplacementPlan &plan = out.plan;

        // ---- Textures -----------------------------------------------------------------------------------
        // The shader reads `.r` of a single-channel max hierarchy; a MinMax pack would put *min* in R. Only
        // a max-only hierarchy is consumed today (the plans/displacement-system.md section 7 rule), so force it.
        if (plan.hierarchy_channels != Displacement::HierarchyChannels::MaxOnly) {
            plan.downgrades.push_back({"hierarchy channels MinMax -> MaxOnly",
                                       "the shader's skip test reads only the max channel (R)"});
            plan.hierarchy_channels = Displacement::HierarchyChannels::MaxOnly;
        }
        if (plan.hierarchy_precision != Displacement::HierarchyPrecision::Float32) {
            plan.downgrades.push_back({"hierarchy precision Unorm16 -> Float32",
                                       "the RHI has no R16Unorm format; R32Float is exact and bit-conservative"});
            plan.hierarchy_precision = Displacement::HierarchyPrecision::Float32;
        }

        const string label = desc.label != nullptr ? string{desc.label} : string{"displaced material"};
        const string height_label = label + " height";
        auto height_texture = create_texture(desc.width, desc.height, RHI::Format::R32Float,
                                             std::as_bytes(desc.heights.first(static_cast<usize>(desc.width) * desc.height)),
                                             height_label.c_str());
        if (!height_texture) {
            return unexpected(height_texture.error());
        }
        out.height_texture = *height_texture;

        auto cleanup = [&]() noexcept {
            if (out.hierarchy_texture) destroy_texture(out.hierarchy_texture);
            if (out.height_texture) destroy_texture(out.height_texture);
            if (out.instance) destroy_material_instance(out.instance);
            if (out.material_template) destroy_material_template(out.material_template);
        };

        out.hierarchy_levels = 1;
        if (plan.needs_hierarchy()) {
            const Displacement::HeightfieldView view{desc.heights, desc.width, desc.height, desc.wrap};
            const Displacement::HeightfieldHierarchy hierarchy = Displacement::HeightfieldHierarchy::build(view);
            if (!hierarchy.valid()) {
                cleanup();
                return unexpected(displacement_error("Failed to build the heightfield hierarchy."));
            }
            const Displacement::PackedHierarchy packed = hierarchy.pack(plan.hierarchy_precision, plan.hierarchy_channels);
            out.hierarchy_levels = hierarchy.level_count();
            // Packed mip m is hierarchy level m + 1, so the chain must cover levels 1 .. level_count - 1.
            if (packed.mip_count + 1u < out.hierarchy_levels) {
                cleanup();
                return unexpected(displacement_error("Packed hierarchy has fewer mips than hierarchy levels."));
            }
            const string hierarchy_label = label + " hierarchy";
            auto hierarchy_texture = create_texture(packed.width, packed.height, RHI::Format::R32Float,
                                                    std::as_bytes(span<const u8>{packed.data.data(), packed.data.size()}),
                                                    hierarchy_label.c_str(), {}, packed.mip_count);
            if (!hierarchy_texture) {
                cleanup();
                return unexpected(hierarchy_texture.error());
            }
            out.hierarchy_texture = *hierarchy_texture;
        }

        // ---- Shader variant -----------------------------------------------------------------------------
        const bool writes_depth = plan.depth == Displacement::DepthPolicy::DisplacedDepth && caps.fragment_depth_write;
        // Conservative depth keeps early-Z, but is only *valid* for depth maps (every ray goes into the
        // volume) and needs backend support (WebGPU/WGSL has no conservative depth output).
        const bool depth_ge = writes_depth && desc.reference_height >= 0.999f &&
                              device->backend_type() != RHI::BackendType::WebGpu;
        const bool legacy_geometry = plan.geometry == Displacement::GeometryPath::PreTessellatedVertex;

        slang::ShaderCompileOptions options{};
        options.macros.push_back({"SFT_HF_ALGORITHM", std::to_string(static_cast<u32>(plan.algorithm))});
        if (writes_depth) options.macros.push_back({"SFT_DISPLACEMENT_WRITE_DEPTH", "1"});
        if (depth_ge) options.macros.push_back({"SFT_DISPLACEMENT_DEPTH_GE", "1"});
        // Explicit list: the template declares vertexMain *and* vertexMainDisplaced, and the material
        // machinery takes the first vertex entry point and fragment entries [0] = G-buffer, [1] = prepass.
        options.entry_points = {
            slang::ShaderEntryPointRequest{.name = legacy_geometry ? "vertexMainDisplaced" : "vertexMain",
                                           .stage = slang::ShaderStage::Vertex},
            slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            slang::ShaderEntryPointRequest{.name = "depthOnlyMain", .stage = slang::ShaderStage::Fragment},
        };

        auto tmpl = create_material_template_from_source(
            slang::ShaderSource::from_file(desc.shader_path, desc.module_name), options, label.c_str());
        if (!tmpl) {
            cleanup();
            return unexpected(tmpl.error());
        }
        out.material_template = *tmpl;
        if (MaterialTemplateResource *resource = material_template(out.material_template)) {
            resource->displaced_depth = writes_depth;
            resource->displaced_surface = true;
        }
        if (plan.geometry == Displacement::GeometryPath::MeshShader) {
            // Task + mesh stages replace the vertex path for this template (RendererDisplacementMesh.cpp). The
            // base mesh is drawn as-is and refined on the GPU, so no pre-tessellation is needed.
            DisplacementMeshSettings mesh_settings{};
            mesh_settings.target_edge_pixels = plan.target_edge_pixels;
            mesh_settings.max_level = std::max(1u, plan.max_subdivision_level);
            mesh_settings.macros = options.macros;
            if (Core::RendererResult enabled = enable_displacement_mesh_geometry(out.material_template, std::move(mesh_settings));
                !enabled) {
                // Automatic fallback to the legacy vertex path: re-plan with the mesh rung removed.
                Foundation::log_warn("Displaced material '{}': mesh-shader path unavailable ({}); falling back to the "
                                     "pre-tessellated vertex path.", label, enabled.error().message);
                cleanup();
                DisplacedMaterialDesc retry = desc;
                retry.request.desired_geometry = Displacement::GeometryPath::PreTessellatedVertex;
                retry.request.silhouette_critical = false;
                return create_displaced_material(retry);
            }
        }
        out.writes_displaced_depth = writes_depth;
        out.needs_pre_tessellated_mesh = legacy_geometry;

        auto instance = create_material_instance(out.material_template, label.c_str());
        if (!instance) {
            cleanup();
            return unexpected(instance.error());
        }
        out.instance = *instance;

        // ---- Uniforms + textures ------------------------------------------------------------------------
        const u32 steps = std::max(1u, plan.max_traversal_steps);
        const u32 pom_steps = pom_steps_for(steps);
        const u32 wrap = desc.wrap ? 1u : 0u;
        const glm::vec4 camera{0.0f, 0.0f, 0.0f, 0.0f};
        const f32 scale = desc.height_scale;
        const f32 reference = desc.reference_height;
        const f32 normal_mix = desc.normal_mix;
        const f32 zero = 0.0f;
        const f32 one = 1.0f;
        const f32 ior = 1.5f;
        const f32 dispersion = 0.0042f;
        const f32 metallic = desc.metallic;
        const f32 roughness = desc.roughness;

        Core::RendererResult set = {};
        auto apply = [&](const char *name, span<const std::byte> value) {
            if (set) set = set_material_parameter(out.instance, name, value);
        };
        apply("base_color_factor", bytes_of(desc.base_color));
        apply("metallic_factor", bytes_of(metallic));
        apply("roughness_factor", bytes_of(roughness));
        apply("specular_factor", bytes_of(one));
        apply("ior", bytes_of(ior));
        apply("transmission_factor", bytes_of(zero));
        apply("dispersion_cauchy_b", bytes_of(dispersion));
        apply("absorption_coefficient", bytes_of(zero));
        apply("alpha_cutoff", bytes_of(zero));
        apply("occlusion_strength", bytes_of(one));
        apply("emissive_strength", bytes_of(one));
        apply("metallic_roughness_channels_rg", bytes_of(zero));
        apply("displacement_height_scale", bytes_of(scale));
        apply("displacement_reference_height", bytes_of(reference));
        apply("displacement_tile_size", bytes_of(desc.tile_size));
        apply("displacement_camera_position", bytes_of(camera));
        apply("displacement_normal_mix", bytes_of(normal_mix));
        apply("displacement_max_steps", bytes_of(steps));
        apply("displacement_pom_steps", bytes_of(pom_steps));
        apply("displacement_hierarchy_levels", bytes_of(out.hierarchy_levels));
        apply("displacement_wrap", bytes_of(wrap));
        if (set) set = set_material_texture(out.instance, "height_texture", out.height_texture);
        if (set && out.hierarchy_texture) {
            // Materials without a hierarchy keep the default 1x1 texture the slot was initialised with.
            set = set_material_texture(out.instance, "height_hierarchy_texture", out.hierarchy_texture);
        }
        if (!set) {
            cleanup();
            return unexpected(set.error());
        }

        displaced_materials_.lock()->push_back(DisplacedMaterialRecord{.material = out, .steps = steps});

        Foundation::log_info("Displaced material '{}': algorithm={}, geometry={}, depth={}{}, steps={}, hierarchy levels={}, {} downgrade(s).",
                             label, Displacement::to_string(plan.algorithm), Displacement::to_string(plan.geometry),
                             Displacement::to_string(plan.depth), depth_ge ? " (conservative)" : "", steps,
                             out.hierarchy_levels, plan.downgrades.size());
        for (const Displacement::PlanDowngrade &downgrade : plan.downgrades) {
            Foundation::log_info("  downgrade: {} ({})", downgrade.what, downgrade.why);
        }
        return out;
    }

    void Renderer::destroy_displaced_material(MaterialInstanceHandle instance) noexcept {
        ZoneScopedN("Renderer::destroy_displaced_material");
        DisplacedMaterial material{};
        {
            auto records = displaced_materials_.lock();
            const auto found = std::ranges::find_if(
                *records, [&](const DisplacedMaterialRecord &r) { return r.material.instance == instance; });
            if (found == records->end()) {
                return;
            }
            material = found->material;
            records->erase(found);
        }
        // The material may still be referenced by in-flight frames.
        wait_idle();
        destroy_material_instance(material.instance);
        destroy_material_template(material.material_template);
        if (material.hierarchy_texture) destroy_texture(material.hierarchy_texture);
        if (material.height_texture) destroy_texture(material.height_texture);
    }

    Core::RendererExpected<MeshHandle> Renderer::create_displaced_mesh(const DisplacedMaterial &material,
                                                                       span<const GeometryVertex> vertices,
                                                                       span<const u32> indices, const char *label) {
        ZoneScopedN("Renderer::create_displaced_mesh");
        if (!material.needs_pre_tessellated_mesh) {
            return create_mesh(vertices, indices, label);
        }
        // Legacy path: vertices carry the displacement, so the mesh has to be dense enough to hold it.
        // Trade level for memory when the base mesh is already large (level L multiplies triangles by L^2).
        constexpr u64 kMaxVertices = 8u * 1024u * 1024u;
        const u64 base_triangles = indices.size() / 3u;
        u32 level = std::max(1u, material.plan.max_subdivision_level);
        while (level > 1 && Displacement::subdivision_cost(base_triangles, level).vertex_count > kMaxVertices) {
            --level;
        }
        Displacement::SubdividedMesh refined = Displacement::subdivide_uniform(vertices, indices, level);
        return create_mesh(refined.vertices, refined.indices, label);
    }

    Core::RendererResult Renderer::set_displaced_material_view_metrics(
        MaterialInstanceHandle instance, const Displacement::DisplacementViewMetrics &metrics) {
        u32 steps = 0;
        {
            auto records = displaced_materials_.lock();
            const auto found = std::ranges::find_if(
                *records, [&](const DisplacedMaterialRecord &r) { return r.material.instance == instance; });
            if (found == records->end()) {
                return unexpected(displacement_error("Unknown displaced material instance."));
            }
            steps = std::max(1u, Displacement::select_traversal_steps(found->material.plan, metrics));
            if (found->steps == steps) {
                return {};
            }
            found->steps = steps;
        }
        const u32 pom_steps = pom_steps_for(steps);
        if (Core::RendererResult set = set_material_parameter(instance, "displacement_max_steps", bytes_of(steps)); !set) {
            return set;
        }
        return set_material_parameter(instance, "displacement_pom_steps", bytes_of(pom_steps));
    }

    void Renderer::refresh_displaced_materials(const glm::vec3 &camera_world_position) {
        auto records = displaced_materials_.lock();
        for (DisplacedMaterialRecord &record : *records) {
            if (record.camera_written && record.last_camera_position == camera_world_position) {
                continue;
            }
            const glm::vec4 value{camera_world_position, 0.0f};
            if (set_material_parameter(record.material.instance, "displacement_camera_position", bytes_of(value))) {
                record.last_camera_position = camera_world_position;
                record.camera_written = true;
            }
        }
    }

} // namespace SFT::Renderer
