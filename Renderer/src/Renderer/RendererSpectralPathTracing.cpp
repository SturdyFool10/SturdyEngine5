#include <Foundation/src/Foundation.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>

using std::array;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {
    namespace {
        constexpr array<const char *, 5> integrator_entry_points{
            "shadowOnlyMain",
            "reflectionOnlyMain",
            "ambientOcclusionOnlyMain",
            "shadowTransmissionMain",
            "fullPathTracingMain",
        };

        struct SpectralSceneInstanceGpu {
            glm::mat4 model{1.0f};
            glm::mat4 previous_model{1.0f};
            glm::mat4 normal_matrix{1.0f};
            u32 vertex_offset = 0;
            u32 index_offset = 0;
            u32 material_index = 0;
            u32 visibility_mask = ~0u;
        };
        static_assert(sizeof(GeometryVertex) == 64);
        static_assert(offsetof(GeometryVertex, position) == 0);
        static_assert(offsetof(GeometryVertex, normal) == 12);
        static_assert(offsetof(GeometryVertex, uv) == 24);
        static_assert(offsetof(GeometryVertex, color) == 32);
        static_assert(offsetof(GeometryVertex, tangent) == 48);
        static_assert(sizeof(SpectralSceneInstanceGpu) == 208);
        static_assert(offsetof(SpectralSceneInstanceGpu, model) == 0);
        static_assert(offsetof(SpectralSceneInstanceGpu, previous_model) == 64);
        static_assert(offsetof(SpectralSceneInstanceGpu, normal_matrix) == 128);
        static_assert(offsetof(SpectralSceneInstanceGpu, vertex_offset) == 192);

        struct SpectralMaterialGpu {
            glm::vec4 base_color{0.8f, 0.8f, 0.8f, 1.0f};
            glm::vec4 emissive_and_strength{0.0f, 0.0f, 0.0f, 1.0f};
            // x roughness factor, y metallic factor, z occlusion strength, w dielectric F0.
            glm::vec4 surface{0.5f, 0.0f, 1.0f, 0.04f};
            glm::vec4 transmission{0.0f, 1.4878f, 0.0042f, 0.0f};
            // base color, metallic/roughness, normal, occlusion texture heap indices.
            glm::uvec4 texture_indices0{~0u};
            // emissive texture index; remaining lanes reserved for material extensions.
            glm::uvec4 texture_indices1{~0u};
            // x alpha cutoff, y normal-map scale, zw reserved.
            glm::vec4 alpha_and_normal{0.0f, 1.0f, 0.0f, 0.0f};
        };
        static_assert(sizeof(SpectralMaterialGpu) == 112);

        struct SpectralFrameConstantsGpu {
            glm::mat4 inverse_view_projection{1.0f};
            glm::mat4 view_projection{1.0f};
            glm::mat4 previous_view_projection{1.0f};
            glm::vec4 camera_position{0.0f};
            glm::vec4 sun_direction_and_angular_radius{0.0f};
            glm::vec4 sun_radiance{0.0f};
            glm::uvec2 extent{1u};
            u32 frame_index = 0;
            u32 samples_per_pixel = 1;
            u32 max_bounces = 8;
            u32 russian_roulette_start_bounce = 3;
            u32 ao_ray_count = 1;
            u32 instance_mask = ~0u;
            f32 ao_radius = 1.0f;
            f32 wavelength_min_nm = 380.0f;
            f32 wavelength_max_nm = 780.0f;
            f32 environment_intensity = 1.0f;
            glm::vec4 caustic_gather_parameters{0.0f};
            glm::uvec4 caustic_gather_control{0u};
            glm::uvec4 accumulation_control{0u};
        };
        static_assert(sizeof(SpectralFrameConstantsGpu) == 336);

        struct alignas(16) SpectralPhotonConstantsGpu {
            glm::vec4 sun_direction_and_angular_radius{0.0f};
            glm::vec4 sun_radiance{0.0f};
            glm::vec4 emission_center_and_radius{0.0f};
            glm::vec4 launch_parameters{0.0f};
            glm::vec4 wavelength_range{0.0f};
            glm::uvec4 capacities_and_frame{0u};
            glm::uvec4 limits_and_flags{0u};
        };
        static_assert(sizeof(SpectralPhotonConstantsGpu) == 112);

        struct alignas(16) SpectralPhotonGpu {
            glm::vec4 position_and_wavelength{0.0f};
            glm::vec4 incoming_direction_and_power{0.0f};
            glm::uvec4 link_and_provenance{0u};
        };
        static_assert(sizeof(SpectralPhotonGpu) == 48);
        static_assert(offsetof(SpectralPhotonGpu, position_and_wavelength) == 0);
        static_assert(offsetof(SpectralPhotonGpu, incoming_direction_and_power) == 16);
        static_assert(offsetof(SpectralPhotonGpu, link_and_provenance) == 32);

        constexpr array<const char *, 2> photon_entry_points{
            "emitPhotonsMain",
            "hashPhotonsMain",
        };

        // VkGeometryInstanceFlagBitsKHR / D3D12_RAYTRACING_INSTANCE_FLAGS bit, matching the raw byte
        // AccelerationStructureInstance::set_shader_binding_table_offset_and_flags packs verbatim.
        // Every BLAS geometry is built Opaque (see ensure_spectral_mesh_acceleration_structures), so
        // leaving an instance's flags at zero lets ray queries take the hardware opaque auto-commit
        // fast path for it. Only alpha-cutout materials need this instance-level override to force
        // the any-hit-equivalent candidate evaluation traceSpectralClosestSurface relies on for
        // masking — see that function's RAY_FLAG_NONE comment for the other half of this contract.
        constexpr u8 kForceNoOpaqueInstanceFlag = 0x08u;

        [[nodiscard]] usize spectral_pipeline_index(SpectralRenderMode mode) noexcept {
            switch (mode) {
                case SpectralRenderMode::ShadowOnly: return 0;
                case SpectralRenderMode::ReflectionOnly: return 1;
                case SpectralRenderMode::AmbientOcclusionOnly: return 2;
                case SpectralRenderMode::ShadowAndTransmission: return 3;
                case SpectralRenderMode::FullPathTracing: return 4;
                case SpectralRenderMode::RasterDeferred: break;
            }
            return 0;
        }

        [[nodiscard]] Core::GraphicsBackendError spectral_error(
            Core::GraphicsBackendErrorCode code, string message) {
            return Core::GraphicsBackendError{code, std::move(message)};
        }

        template <typename T>
        [[nodiscard]] bool read_material_parameter(const MaterialTemplateResource &material_template,
                                                   const MaterialInstanceResource &instance,
                                                   const char *name,
                                                   T &value) noexcept {
            for (const MaterialParameter &parameter : material_template.parameters) {
                if (parameter.name != name || parameter.offset + sizeof(T) > instance.uniform_values.size()) {
                    continue;
                }
                std::memcpy(&value, instance.uniform_values.data() + parameter.offset, sizeof(T));
                return true;
            }
            return false;
        }
    } // namespace

    Core::RendererResult Renderer::ensure_spectral_path_tracing_resources(SpectralRenderMode mode) {
        if (mode == SpectralRenderMode::RasterDeferred) {
            return {};
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Cannot build spectral path-tracing resources without an RHI device."));
        }
        const RHI::FeatureSet required = RHI::features_of({
            RHI::Feature::BufferDeviceAddress,
            RHI::Feature::AccelerationStructures,
            RHI::Feature::RayQuery,
            RHI::Feature::BindlessResources,
            RHI::Feature::RuntimeDescriptorArrays,
            RHI::Feature::DescriptorBindingVariableCount,
            RHI::Feature::DescriptorBindingPartiallyBound,
            RHI::Feature::DescriptorBindingUpdateAfterBind,
            RHI::Feature::SampledImageArrayNonUniformIndexing,
        });
        if (!device->enabled_features().contains_all(required)) {
            return unexpected(spectral_error(
                Core::GraphicsBackendErrorCode::Unsupported,
                "The selected spectral ray-query mode requires ray query plus bindless sampled-image descriptor "
                "indexing to have been enabled when the Vulkan device was created. Request ray tracing through "
                "EngineConfig::features before initialization."));
        }

        auto resources = spectral_path_tracing_.lock();
        if (resources->ready) {
            return {};
        }
        resources->material_texture_capacity = std::min(
            4096u, device->feature_properties().descriptor_indexing.max_variable_descriptor_count);
        if (resources->material_texture_capacity == 0u) {
            return unexpected(spectral_error(
                Core::GraphicsBackendErrorCode::Unsupported,
                "The enabled descriptor-indexing device reports no usable variable sampled-image descriptors."));
        }

        vector<slang::ShaderEntryPointRequest> entry_requests;
        entry_requests.reserve(integrator_entry_points.size());
        for (const char *entry_point : integrator_entry_points) {
            entry_requests.push_back(slang::ShaderEntryPointRequest{
                .name = entry_point,
                .stage = slang::ShaderStage::Compute,
            });
        }
        const slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = std::move(entry_requests),
        };
        slang::ShaderCompiler compiler;
        auto shader = compiler.compile(
            slang::ShaderSource::from_file("Shaders/spectral_integrators.slang", "spectral_integrators"),
            options);
        if (!shader) {
            return unexpected(spectral_error(
                Core::GraphicsBackendErrorCode::OperationFailed,
                "Compile spectral integrators failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        }
        resources->shader = *shader;

        const slang::ShaderReflection &reflection = resources->shader.reflection();
        for (const ReflectedResource &binding : collect_resource_bindings(reflection)) {
            if (binding.set != 0) {
                destroy_spectral_path_tracing_resources_locked(*resources);
                return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                 "Spectral integrator resources must all use descriptor set zero."));
            }
            resources->resource_bindings.emplace(binding.name, binding);
        }
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(
            reflection, RHI::ShaderStage::Compute, resources->material_texture_capacity);
        if (generated.size() != 1 || generated.front().set != 0) {
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Spectral integrator reflection must produce exactly descriptor set zero."));
        }
        u32 expected_set = 0;
        for (const GeneratedBindGroupLayout &layout : generated) {
            if (layout.set != expected_set++) {
                destroy_spectral_path_tracing_resources_locked(*resources);
                return unexpected(spectral_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "Spectral integrator descriptor sets must be contiguous beginning at set zero."));
            }
            auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "spectral path tracing bind group layout",
            });
            if (!handle) {
                const Core::GraphicsBackendError error = graphics_error_from_rhi(
                    handle.error(), "create spectral path tracing bind group layout");
                destroy_spectral_path_tracing_resources_locked(*resources);
                return unexpected(error);
            }
            resources->bind_group_layouts.push_back(*handle);
            resources->bind_group_layout_sets.push_back(layout.set);
        }

        const vector<RHI::PushConstantRange> push_ranges =
            generate_push_constant_ranges(reflection, RHI::ShaderStage::Compute);
        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{
                resources->bind_group_layouts.data(), resources->bind_group_layouts.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_ranges.data(), push_ranges.size()},
            .label = "spectral path tracing pipeline layout",
        });
        if (!pipeline_layout) {
            const Core::GraphicsBackendError error = graphics_error_from_rhi(
                pipeline_layout.error(), "create spectral path tracing pipeline layout");
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(error);
        }
        resources->pipeline_layout = *pipeline_layout;

        for (usize index = 0; index < integrator_entry_points.size(); ++index) {
            const char *entry_point = integrator_entry_points[index];
            auto code = resources->shader.entry_point_code(entry_point);
            if (!code) {
                const Core::GraphicsBackendError error = spectral_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "Generate spectral integrator bytecode failed for " + string(entry_point) + ": " +
                        code.error().message);
                destroy_spectral_path_tracing_resources_locked(*resources);
                return unexpected(error);
            }
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = RHI::ShaderLanguage::SpirV,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = entry_point,
            });
            if (!module) {
                const Core::GraphicsBackendError error = graphics_error_from_rhi(
                    module.error(), "create spectral integrator shader module");
                destroy_spectral_path_tracing_resources_locked(*resources);
                return unexpected(error);
            }
            resources->modules[index] = *module;

            auto pipeline = device->create_compute_pipeline(RHI::ComputePipelineDesc{
                .layout = resources->pipeline_layout,
                .compute = RHI::ShaderEntry{
                    .module = resources->modules[index],
                    .entry_point = entry_point,
                    .stage = RHI::ShaderStage::Compute,
                },
                .label = entry_point,
            });
            if (!pipeline) {
                const Core::GraphicsBackendError error = graphics_error_from_rhi(
                    pipeline.error(), "create spectral integrator compute pipeline");
                destroy_spectral_path_tracing_resources_locked(*resources);
                return unexpected(error);
            }
            resources->pipelines[index] = *pipeline;
        }

        vector<slang::ShaderEntryPointRequest> photon_entry_requests;
        photon_entry_requests.reserve(photon_entry_points.size());
        for (const char *entry_point : photon_entry_points) {
            photon_entry_requests.push_back(slang::ShaderEntryPointRequest{
                .name = entry_point,
                .stage = slang::ShaderStage::Compute,
            });
        }
        const slang::ShaderCompileOptions photon_options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = std::move(photon_entry_requests),
        };
        auto photon_shader = compiler.compile(
            slang::ShaderSource::from_file("Shaders/spectral_photon_caustics.slang", "spectral_photon_caustics"),
            photon_options);
        if (!photon_shader) {
            const Core::GraphicsBackendError error = spectral_error(
                Core::GraphicsBackendErrorCode::OperationFailed,
                "Compile spectral photon mapper failed: " + photon_shader.error().message + "\n" +
                    photon_shader.error().diagnostics);
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(error);
        }
        resources->photon_shader = *photon_shader;
        const slang::ShaderReflection &photon_reflection = resources->photon_shader.reflection();
        for (const ReflectedResource &binding : collect_resource_bindings(photon_reflection)) {
            if (binding.set != 0) {
                destroy_spectral_path_tracing_resources_locked(*resources);
                return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                 "Spectral photon resources must all use descriptor set zero."));
            }
            resources->photon_resource_bindings.emplace(binding.name, binding);
        }
        const vector<GeneratedBindGroupLayout> photon_layouts = generate_bind_group_layouts(
            photon_reflection, RHI::ShaderStage::Compute, resources->material_texture_capacity);
        if (photon_layouts.size() != 1 || photon_layouts.front().set != 0) {
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Spectral photon reflection must produce exactly descriptor set zero."));
        }
        auto photon_bind_group_layout = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
            .entries = span<const RHI::BindGroupLayoutEntry>{
                photon_layouts.front().entries.data(), photon_layouts.front().entries.size()},
            .label = "spectral photon bind group layout",
        });
        if (!photon_bind_group_layout) {
            const Core::GraphicsBackendError error = graphics_error_from_rhi(
                photon_bind_group_layout.error(), "create spectral photon bind group layout");
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(error);
        }
        resources->photon_bind_group_layout = *photon_bind_group_layout;
        const vector<RHI::PushConstantRange> photon_push_ranges =
            generate_push_constant_ranges(photon_reflection, RHI::ShaderStage::Compute);
        auto photon_pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{
                &resources->photon_bind_group_layout, 1},
            .push_constant_ranges = span<const RHI::PushConstantRange>{
                photon_push_ranges.data(), photon_push_ranges.size()},
            .label = "spectral photon pipeline layout",
        });
        if (!photon_pipeline_layout) {
            const Core::GraphicsBackendError error = graphics_error_from_rhi(
                photon_pipeline_layout.error(), "create spectral photon pipeline layout");
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(error);
        }
        resources->photon_pipeline_layout = *photon_pipeline_layout;
        for (usize index = 0; index < photon_entry_points.size(); ++index) {
            const char *entry_point = photon_entry_points[index];
            auto code = resources->photon_shader.entry_point_code(entry_point);
            if (!code) {
                const Core::GraphicsBackendError error = spectral_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "Generate spectral photon bytecode failed for " + string(entry_point) + ": " +
                        code.error().message);
                destroy_spectral_path_tracing_resources_locked(*resources);
                return unexpected(error);
            }
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = RHI::ShaderLanguage::SpirV,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = entry_point,
            });
            if (!module) {
                const Core::GraphicsBackendError error = graphics_error_from_rhi(
                    module.error(), "create spectral photon shader module");
                destroy_spectral_path_tracing_resources_locked(*resources);
                return unexpected(error);
            }
            resources->photon_modules[index] = *module;
            auto pipeline = device->create_compute_pipeline(RHI::ComputePipelineDesc{
                .layout = resources->photon_pipeline_layout,
                .compute = RHI::ShaderEntry{
                    .module = resources->photon_modules[index],
                    .entry_point = entry_point,
                    .stage = RHI::ShaderStage::Compute,
                },
                .label = entry_point,
            });
            if (!pipeline) {
                const Core::GraphicsBackendError error = graphics_error_from_rhi(
                    pipeline.error(), "create spectral photon compute pipeline");
                destroy_spectral_path_tracing_resources_locked(*resources);
                return unexpected(error);
            }
            resources->photon_pipelines[index] = *pipeline;
        }

        const slang::ShaderCompileOptions depth_options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        auto depth_shader = compiler.compile(
            slang::ShaderSource::from_file("Shaders/spectral_depth_commit.slang", "spectral_depth_commit"),
            depth_options);
        if (!depth_shader) {
            const Core::GraphicsBackendError error = spectral_error(
                Core::GraphicsBackendErrorCode::OperationFailed,
                "Compile spectral depth commit failed: " + depth_shader.error().message + "\n" +
                    depth_shader.error().diagnostics);
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(error);
        }
        resources->depth_commit_shader = *depth_shader;
        auto depth_vertex_code = resources->depth_commit_shader.entry_point_code("vertexMain");
        auto depth_fragment_code = resources->depth_commit_shader.entry_point_code("fragmentMain");
        if (!depth_vertex_code || !depth_fragment_code) {
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Generate spectral depth-commit bytecode failed."));
        }
        auto create_depth_module = [&](const Core::Slang::ShaderBytecode &code, const char *label)
            -> Core::RendererExpected<RHI::ShaderModuleHandle> {
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = RHI::ShaderLanguage::SpirV,
                .code = span<const std::byte>{code.bytes.data(), code.bytes.size()},
                .label = label,
            });
            if (!module) return unexpected(graphics_error_from_rhi(module.error(), label));
            return *module;
        };
        auto depth_vertex_module = create_depth_module(*depth_vertex_code, "spectral depth commit vertex module");
        if (!depth_vertex_module) {
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(depth_vertex_module.error());
        }
        resources->depth_commit_vertex_module = *depth_vertex_module;
        auto depth_fragment_module = create_depth_module(*depth_fragment_code, "spectral depth commit fragment module");
        if (!depth_fragment_module) {
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(depth_fragment_module.error());
        }
        resources->depth_commit_fragment_module = *depth_fragment_module;

        const slang::ShaderReflection &depth_reflection = resources->depth_commit_shader.reflection();
        const vector<GeneratedBindGroupLayout> depth_layouts =
            generate_bind_group_layouts(depth_reflection, RHI::ShaderStage::Fragment);
        const vector<ReflectedResource> depth_bindings = collect_resource_bindings(depth_reflection);
        if (depth_layouts.size() != 1 || depth_bindings.size() != 1 ||
            depth_bindings.front().name != "primaryDepth") {
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Spectral depth-commit reflection contract is invalid."));
        }
        auto depth_bind_group_layout = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
            .entries = span<const RHI::BindGroupLayoutEntry>{
                depth_layouts.front().entries.data(), depth_layouts.front().entries.size()},
            .label = "spectral depth commit bind group layout",
        });
        if (!depth_bind_group_layout) {
            const Core::GraphicsBackendError error = graphics_error_from_rhi(
                depth_bind_group_layout.error(), "create spectral depth commit bind group layout");
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(error);
        }
        resources->depth_commit_bind_group_layout = *depth_bind_group_layout;
        resources->depth_commit_texture_binding = depth_bindings.front().binding;
        auto depth_pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{
                &resources->depth_commit_bind_group_layout, 1},
            .label = "spectral depth commit pipeline layout",
        });
        if (!depth_pipeline_layout) {
            const Core::GraphicsBackendError error = graphics_error_from_rhi(
                depth_pipeline_layout.error(), "create spectral depth commit pipeline layout");
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(error);
        }
        resources->depth_commit_pipeline_layout = *depth_pipeline_layout;
        auto depth_pipeline = device->create_render_pipeline(RHI::RenderPipelineDesc{
            .layout = resources->depth_commit_pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = resources->depth_commit_vertex_module,
                                       .entry_point = "vertexMain", .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = resources->depth_commit_fragment_module,
                                         .entry_point = "fragmentMain", .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{
                .format = RHI::Format::D32Float,
                .depth_test_enable = true,
                .depth_write_enable = true,
                .depth_compare = RHI::CompareOp::Always,
            },
            .color_targets = {},
            .label = "spectral depth commit pipeline",
        });
        if (!depth_pipeline) {
            const Core::GraphicsBackendError error = graphics_error_from_rhi(
                depth_pipeline.error(), "create spectral depth commit pipeline");
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(error);
        }
        resources->depth_commit_pipeline = *depth_pipeline;
        const RHI::SamplerDesc atmosphere_sampler_desc{
            .min_filter = RHI::Filter::Linear,
            .mag_filter = RHI::Filter::Linear,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .label = "spectral atmosphere lut sampler",
        };
        auto atmosphere_sampler = device->create_sampler(atmosphere_sampler_desc);
        if (!atmosphere_sampler) {
            destroy_spectral_path_tracing_resources_locked(*resources);
            return unexpected(graphics_error_from_rhi(atmosphere_sampler.error(), "create spectral atmosphere lut sampler"));
        }
        resources->atmosphere_sampler = *atmosphere_sampler;
        resources->shader.release_compiler_state();
        resources->photon_shader.release_compiler_state();
        resources->depth_commit_shader.release_compiler_state();
        resources->ready = true;
        return {};
    }

    Core::RendererResult Renderer::ensure_spectral_mesh_acceleration_structures(span<const RenderItem> draws) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Cannot build mesh acceleration structures without an RHI device."));
        }

        for (const RenderItem &draw : draws) {
            MeshResource *mesh_resource = mesh(draw.mesh);
            if (mesh_resource == nullptr || !mesh_resource->gpu_resident ||
                mesh_resource->bottom_level_acceleration_structure) {
                continue;
            }

            const u32 primitive_count = mesh_resource->index_count / 3u;
            // The current hit-data ABI always vertex-pulls through geometryIndices. Non-indexed meshes
            // stay raster-only until generated index data is added to their replay record.
            if (primitive_count == 0) {
                continue;
            }
            const RHI::AccelerationStructureGeometryDesc geometry{
                .type = RHI::AccelerationStructureGeometryType::Triangles,
                .flags = RHI::AccelerationStructureGeometryFlags::Opaque,
                .triangles = RHI::AccelerationStructureTrianglesDesc{
                    .vertex_buffer = vertex_arena_.buffer,
                    .vertex_offset = static_cast<u64>(mesh_resource->vertex_offset) * sizeof(GeometryVertex),
                    .vertex_format = RHI::VertexFormat::Float32x3,
                    .vertex_stride = sizeof(GeometryVertex),
                    .max_vertex = mesh_resource->vertex_count - 1u,
                    .index_buffer = index_arena_.buffer,
                    .index_offset = static_cast<u64>(mesh_resource->index_offset) * sizeof(u32),
                    .index_format = RHI::IndexFormat::Uint32,
                },
            };
            const RHI::AccelerationStructureBuildRangeInfo range{.primitive_count = primitive_count};
            RHI::AccelerationStructureBuildDesc build{
                .type = RHI::AccelerationStructureType::BottomLevel,
                .flags = RHI::AccelerationStructureBuildFlags::PreferFastTrace,
                .geometries = span<const RHI::AccelerationStructureGeometryDesc>{&geometry, 1},
                .ranges = span<const RHI::AccelerationStructureBuildRangeInfo>{&range, 1},
            };
            auto sizes = device->acceleration_structure_build_sizes(build);
            if (!sizes) {
                return unexpected(graphics_error_from_rhi(sizes.error(), "query mesh BLAS build sizes"));
            }
            auto blas = device->create_acceleration_structure(RHI::AccelerationStructureDesc{
                .type = RHI::AccelerationStructureType::BottomLevel,
                .size = sizes->acceleration_structure_size,
                .label = mesh_resource->label.empty() ? "mesh BLAS" : mesh_resource->label.c_str(),
            });
            if (!blas) {
                return unexpected(graphics_error_from_rhi(blas.error(), "create mesh BLAS"));
            }
            const u64 scratch_alignment = std::max<u64>(
                device->feature_properties().ray_tracing.min_acceleration_structure_scratch_offset_alignment, 1u);
            auto scratch = device->create_buffer(RHI::BufferDesc{
                .size = sizes->build_scratch_size + scratch_alignment - 1u,
                .usage = RHI::BufferUsage::AccelerationStructureScratch,
                .memory = RHI::MemoryLocation::DeviceLocal,
                .label = "mesh BLAS scratch",
            });
            if (!scratch) {
                device->destroy_acceleration_structure(*blas);
                return unexpected(graphics_error_from_rhi(scratch.error(), "create mesh BLAS scratch"));
            }
            auto scratch_address = device->buffer_device_address(*scratch);
            if (!scratch_address) {
                device->destroy_buffer(*scratch);
                device->destroy_acceleration_structure(*blas);
                return unexpected(graphics_error_from_rhi(scratch_address.error(), "query mesh BLAS scratch address"));
            }
            build.dst = *blas;
            build.scratch_buffer = *scratch;
            build.scratch_offset = ((*scratch_address + scratch_alignment - 1u) / scratch_alignment) *
                                   scratch_alignment - *scratch_address;

            auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "mesh BLAS build"});
            if (!encoder) {
                device->destroy_buffer(*scratch);
                device->destroy_acceleration_structure(*blas);
                return unexpected(graphics_error_from_rhi(encoder.error(), "create mesh BLAS encoder"));
            }
            (*encoder)->build_acceleration_structures(
                span<const RHI::AccelerationStructureBuildDesc>{&build, 1});
            auto command_buffer = (*encoder)->finish();
            if (!command_buffer) {
                device->destroy_buffer(*scratch);
                device->destroy_acceleration_structure(*blas);
                return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish mesh BLAS build"));
            }
            auto fence = device->create_fence(RHI::FenceDesc{.label = "mesh BLAS build fence"});
            if (!fence) {
                device->destroy_command_buffer(*command_buffer);
                device->destroy_buffer(*scratch);
                device->destroy_acceleration_structure(*blas);
                return unexpected(graphics_error_from_rhi(fence.error(), "create mesh BLAS fence"));
            }
            const array command_buffers{*command_buffer};
            const RHI::SubmitDesc submit{
                .command_buffers = span<const RHI::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
                .fence = *fence,
                .flags = RHI::SubmitFlags::OneShot,
                .label = "mesh BLAS build submit",
            };
            if (auto submitted = device->submit(submit); !submitted) {
                device->destroy_fence(*fence);
                device->destroy_command_buffer(*command_buffer);
                device->destroy_buffer(*scratch);
                device->destroy_acceleration_structure(*blas);
                return unexpected(graphics_error_from_rhi(submitted.error(), "submit mesh BLAS build"));
            }
            if (auto waited = device->wait_fences(span<const RHI::FenceHandle>{&*fence, 1}, true); !waited) {
                device->destroy_fence(*fence);
                device->destroy_command_buffer(*command_buffer);
                device->destroy_buffer(*scratch);
                device->destroy_acceleration_structure(*blas);
                return unexpected(graphics_error_from_rhi(waited.error(), "wait mesh BLAS build"));
            }
            device->destroy_fence(*fence);
            device->destroy_command_buffer(*command_buffer);
            device->destroy_buffer(*scratch);
            mesh_resource->bottom_level_acceleration_structure = *blas;
        }
        return {};
    }

    Core::RendererResult Renderer::prepare_spectral_scene_acceleration_structure(
        RHI::CommandEncoder &encoder, FrameInFlight &slot, const FrameSubmission &submission) {
        if (submission.render_graph.spectral_path_tracing.mode == SpectralRenderMode::RasterDeferred) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Cannot prepare a spectral scene without an RHI device."));
        }

        u32 material_texture_capacity = 0u;
        {
            auto resources = spectral_path_tracing_.lock();
            material_texture_capacity = resources->material_texture_capacity;
        }
        auto default_texture = ensure_default_white_texture();
        if (!default_texture) {
            return unexpected(default_texture.error());
        }

        vector<RHI::AccelerationStructureInstance> tlas_instances;
        vector<SpectralSceneInstanceGpu> scene_instances;
        vector<SpectralMaterialGpu> materials;
        tlas_instances.reserve(submission.draws.size());
        scene_instances.reserve(submission.draws.size());
        materials.reserve(submission.draws.size());
        slot.spectral_material_textures.clear();
        slot.spectral_material_textures.reserve(std::min<usize>(
            static_cast<usize>(material_texture_capacity), submission.draws.size() * 5u + 1u));
        slot.spectral_material_textures.push_back(*default_texture);
        bool material_texture_overflow = false;
        const auto append_material_texture = [&](TextureHandle handle) -> u32 {
            if (!handle || texture(handle) == nullptr) {
                return ~0u;
            }
            const auto existing = std::find(
                slot.spectral_material_textures.begin(), slot.spectral_material_textures.end(), handle);
            if (existing != slot.spectral_material_textures.end()) {
                return static_cast<u32>(std::distance(slot.spectral_material_textures.begin(), existing));
            }
            if (slot.spectral_material_textures.size() >= material_texture_capacity) {
                material_texture_overflow = true;
                return ~0u;
            }
            const u32 index = static_cast<u32>(slot.spectral_material_textures.size());
            slot.spectral_material_textures.push_back(handle);
            return index;
        };
        glm::vec3 scene_bounds_min{std::numeric_limits<f32>::max()};
        glm::vec3 scene_bounds_max{std::numeric_limits<f32>::lowest()};
        bool has_scene_bounds = false;

        for (const RenderItem &draw : submission.draws) {
            MeshResource *mesh_resource = mesh(draw.mesh);
            if (mesh_resource == nullptr || !mesh_resource->bottom_level_acceleration_structure) {
                continue;
            }
            auto blas_address = device->acceleration_structure_device_address(
                mesh_resource->bottom_level_acceleration_structure);
            if (!blas_address) {
                return unexpected(graphics_error_from_rhi(blas_address.error(), "query mesh BLAS address"));
            }

            SpectralMaterialGpu material{};
            if (MaterialInstanceResource *instance = material_instance(draw.material)) {
                auto cache_guard = spectral_material_parameter_cache_.lock();
                auto cache_it = cache_guard->find(draw.material.value);
                const bool cache_hit = cache_it != cache_guard->end() &&
                    cache_it->second.content_revision == instance->content_revision;
                if (!cache_hit) {
                    if (MaterialTemplateResource *tmpl = material_template(instance->material_template)) {
                        SpectralMaterialParameterCacheEntry entry{};
                        entry.content_revision = instance->content_revision;
                        (void)read_material_parameter(*tmpl, *instance, "base_color_factor", entry.base_color);
                        (void)read_material_parameter(*tmpl, *instance, "emissive_factor", entry.emissive_and_strength);
                        (void)read_material_parameter(*tmpl, *instance, "emissive_strength", entry.emissive_and_strength.w);
                        (void)read_material_parameter(*tmpl, *instance, "roughness_factor", entry.surface.x);
                        (void)read_material_parameter(*tmpl, *instance, "metallic_factor", entry.surface.y);
                        (void)read_material_parameter(*tmpl, *instance, "occlusion_strength", entry.surface.z);
                        (void)read_material_parameter(*tmpl, *instance, "alpha_cutoff", entry.alpha_and_normal.x);
                        (void)read_material_parameter(*tmpl, *instance, "dispersion_cauchy_b", entry.transmission.z);
                        f32 ior = 1.5f;
                        if (read_material_parameter(*tmpl, *instance, "ior", ior)) {
                            entry.transmission.y = std::max(
                                1.0f, ior - entry.transmission.z / (0.5876f * 0.5876f));
                            const f32 f0_base = (ior - 1.0f) / std::max(ior + 1.0f, 1.0e-4f);
                            const f32 f0 = f0_base * f0_base;
                            f32 specular_factor = 1.0f;
                            (void)read_material_parameter(*tmpl, *instance, "specular_factor", specular_factor);
                            entry.surface.w = f0 * std::clamp(specular_factor, 0.0f, 1.0f);
                        }
                        (void)read_material_parameter(*tmpl, *instance, "transmission_factor", entry.transmission.x);
                        (void)read_material_parameter(*tmpl, *instance, "absorption_coefficient", entry.transmission.w);

                        const auto texture_for_slot = [&](const char *name) -> TextureHandle {
                            for (const MaterialTextureSlot &slot_desc : tmpl->texture_slots) {
                                if (slot_desc.name != name) continue;
                                for (const MaterialTextureBinding &binding : instance->textures) {
                                    if (binding.binding == slot_desc.binding) return binding.texture;
                                }
                            }
                            return {};
                        };
                        entry.base_color_texture = texture_for_slot("base_color_texture");
                        entry.metallic_roughness_texture = texture_for_slot("metallic_roughness_texture");
                        entry.normal_texture = texture_for_slot("normal_texture");
                        entry.occlusion_texture = texture_for_slot("occlusion_texture");
                        entry.emissive_texture = texture_for_slot("emissive_texture");
                        cache_it = cache_guard->insert_or_assign(draw.material.value, entry).first;
                    }
                }
                if (cache_it != cache_guard->end()) {
                    const SpectralMaterialParameterCacheEntry &entry = cache_it->second;
                    material.base_color = entry.base_color;
                    material.emissive_and_strength = entry.emissive_and_strength;
                    material.surface = entry.surface;
                    material.transmission = entry.transmission;
                    material.alpha_and_normal = entry.alpha_and_normal;
                    material.texture_indices0.x = append_material_texture(entry.base_color_texture);
                    material.texture_indices0.y = append_material_texture(entry.metallic_roughness_texture);
                    material.texture_indices0.z = append_material_texture(entry.normal_texture);
                    material.texture_indices0.w = append_material_texture(entry.occlusion_texture);
                    material.texture_indices1.x = append_material_texture(entry.emissive_texture);
                }
            }
            const f32 scale_x = glm::length(glm::vec3{draw.world_transform[0]});
            const f32 scale_y = glm::length(glm::vec3{draw.world_transform[1]});
            const f32 scale_z = glm::length(glm::vec3{draw.world_transform[2]});
            const f32 world_radius = mesh_resource->bounds_radius * std::max({scale_x, scale_y, scale_z});
            const glm::vec3 world_center = glm::vec3{
                draw.world_transform * glm::vec4{mesh_resource->bounds_center, 1.0f}};
            scene_bounds_min = glm::min(scene_bounds_min, world_center - glm::vec3{world_radius});
            scene_bounds_max = glm::max(scene_bounds_max, world_center + glm::vec3{world_radius});
            has_scene_bounds = true;

            const u32 scene_index = static_cast<u32>(scene_instances.size());
            materials.push_back(material);
            scene_instances.push_back(SpectralSceneInstanceGpu{
                .model = draw.world_transform,
                .previous_model = draw.previous_world_transform,
                .normal_matrix = glm::transpose(glm::inverse(draw.world_transform)),
                .vertex_offset = mesh_resource->vertex_offset,
                .index_offset = mesh_resource->index_offset,
                .material_index = scene_index,
            });

            RHI::AccelerationStructureInstance tlas_instance{};
            for (u32 row = 0; row < 3; ++row) {
                for (u32 column = 0; column < 4; ++column) {
                    tlas_instance.transform[row * 4u + column] = draw.world_transform[column][row];
                }
            }
            tlas_instance.set_custom_index_and_mask(scene_index, 0xffu);
            const u8 instance_flags = material.alpha_and_normal.x > 0.0f ? kForceNoOpaqueInstanceFlag : 0u;
            tlas_instance.set_shader_binding_table_offset_and_flags(0u, instance_flags);
            tlas_instance.acceleration_structure_device_address = *blas_address;
            tlas_instances.push_back(tlas_instance);
        }

        if (material_texture_overflow) {
            return unexpected(spectral_error(
                Core::GraphicsBackendErrorCode::Unsupported,
                "The visible spectral scene exceeds the device's material texture descriptor capacity."));
        }

        if (has_scene_bounds) {
            const glm::vec3 bounds_center = (scene_bounds_min + scene_bounds_max) * 0.5f;
            const f32 bounds_radius = std::max(glm::length(scene_bounds_max - bounds_center), 0.001f);
            slot.spectral_scene_bounds = glm::vec4{bounds_center, bounds_radius};
        }

        if (tlas_instances.empty()) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "The selected spectral mode requires at least one traceable scene mesh."));
        }

        auto create_uploaded_buffer = [&](u64 size, RHI::BufferUsage usage, const char *label,
                                          span<const std::byte> bytes) -> Core::RendererExpected<RHI::BufferHandle> {
            auto buffer = device->create_buffer(RHI::BufferDesc{
                .size = size,
                .usage = usage,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = label,
            });
            if (!buffer) {
                return unexpected(graphics_error_from_rhi(buffer.error(), label));
            }
            if (auto written = device->write_buffer(*buffer, 0, bytes); !written) {
                device->destroy_buffer(*buffer);
                return unexpected(graphics_error_from_rhi(written.error(), label));
            }
            slot.transient_buffers.push_back(*buffer);
            return *buffer;
        };

        auto instance_buffer = create_uploaded_buffer(
            tlas_instances.size() * sizeof(RHI::AccelerationStructureInstance),
            RHI::BufferUsage::AccelerationStructureInput,
            "spectral TLAS instances",
            std::as_bytes(span<const RHI::AccelerationStructureInstance>{tlas_instances.data(), tlas_instances.size()}));
        if (!instance_buffer) return unexpected(instance_buffer.error());
        auto scene_instance_buffer = create_uploaded_buffer(
            scene_instances.size() * sizeof(SpectralSceneInstanceGpu), RHI::BufferUsage::Storage,
            "spectral scene instances",
            std::as_bytes(span<const SpectralSceneInstanceGpu>{scene_instances.data(), scene_instances.size()}));
        if (!scene_instance_buffer) return unexpected(scene_instance_buffer.error());
        auto material_buffer = create_uploaded_buffer(
            materials.size() * sizeof(SpectralMaterialGpu), RHI::BufferUsage::Storage,
            "spectral materials",
            std::as_bytes(span<const SpectralMaterialGpu>{materials.data(), materials.size()}));
        if (!material_buffer) return unexpected(material_buffer.error());

        const RHI::AccelerationStructureGeometryDesc geometry{
            .type = RHI::AccelerationStructureGeometryType::Instances,
            .instances = RHI::AccelerationStructureInstancesDesc{.buffer = *instance_buffer},
        };
        const RHI::AccelerationStructureBuildRangeInfo range{
            .primitive_count = static_cast<u32>(tlas_instances.size()),
        };
        RHI::AccelerationStructureBuildDesc build{
            .type = RHI::AccelerationStructureType::TopLevel,
            .flags = RHI::AccelerationStructureBuildFlags::PreferFastTrace,
            .geometries = span<const RHI::AccelerationStructureGeometryDesc>{&geometry, 1},
            .ranges = span<const RHI::AccelerationStructureBuildRangeInfo>{&range, 1},
        };
        auto sizes = device->acceleration_structure_build_sizes(build);
        if (!sizes) return unexpected(graphics_error_from_rhi(sizes.error(), "query spectral TLAS build sizes"));
        auto tlas = device->create_acceleration_structure(RHI::AccelerationStructureDesc{
            .type = RHI::AccelerationStructureType::TopLevel,
            .size = sizes->acceleration_structure_size,
            .label = "spectral scene TLAS",
        });
        if (!tlas) return unexpected(graphics_error_from_rhi(tlas.error(), "create spectral scene TLAS"));
        slot.transient_acceleration_structures.push_back(*tlas);
        const u64 scratch_alignment = std::max<u64>(
            device->feature_properties().ray_tracing.min_acceleration_structure_scratch_offset_alignment, 1u);
        auto scratch = device->create_buffer(RHI::BufferDesc{
            .size = sizes->build_scratch_size + scratch_alignment - 1u,
            .usage = RHI::BufferUsage::AccelerationStructureScratch,
            .memory = RHI::MemoryLocation::DeviceLocal,
            .label = "spectral TLAS scratch",
        });
        if (!scratch) return unexpected(graphics_error_from_rhi(scratch.error(), "create spectral TLAS scratch"));
        slot.transient_buffers.push_back(*scratch);
        auto scratch_address = device->buffer_device_address(*scratch);
        if (!scratch_address) {
            return unexpected(graphics_error_from_rhi(scratch_address.error(), "query spectral TLAS scratch address"));
        }
        build.dst = *tlas;
        build.scratch_buffer = *scratch;
        build.scratch_offset = ((*scratch_address + scratch_alignment - 1u) / scratch_alignment) *
                               scratch_alignment - *scratch_address;
        encoder.build_acceleration_structures(span<const RHI::AccelerationStructureBuildDesc>{&build, 1});
        const RHI::GlobalBarrier ready_for_queries{
            .src_stage = RHI::PipelineStage::AccelerationStructureBuild,
            .src_access = RHI::AccessFlags::AccelerationStructureWrite,
            .dst_stage = RHI::PipelineStage::ComputeShader,
            .dst_access = RHI::AccessFlags::AccelerationStructureRead,
        };
        encoder.barrier(span<const RHI::GlobalBarrier>{&ready_for_queries, 1}, {}, {});

        slot.spectral_tlas = *tlas;
        slot.spectral_scene_instances = *scene_instance_buffer;
        slot.spectral_materials = *material_buffer;
        return {};
    }

    Core::RendererResult Renderer::ensure_spectral_accumulation_target(
        WindowSurfaceRecord &record, Core::Extent2D extent) {
        const bool matches = record.spectral_accumulation.texture && record.spectral_accumulation.view &&
            record.spectral_accumulation.extent.width == extent.width &&
            record.spectral_accumulation.extent.height == extent.height;
        if (matches) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Cannot allocate spectral accumulation without an RHI device."));
        }
        if (record.spectral_accumulation.texture || record.spectral_accumulation.view) {
            // A history image is shared by every frame submission for this window, rather than owned by
            // one ring slot. Resizing is cold-path work and must retire all users before replacement.
            drain_frames_in_flight(record);
            destroy_spectral_accumulation_target(record);
        }
        auto texture = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = RHI::Format::RGBA32Float,
            .extent = RHI::Extent3D{.width = extent.width, .height = extent.height, .depth_or_layers = 1},
            .mip_levels = 1,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::Storage,
            .label = "spectral temporal accumulation",
        });
        if (!texture) {
            return unexpected(graphics_error_from_rhi(texture.error(), "create spectral accumulation texture"));
        }
        record.spectral_accumulation.texture = *texture;
        auto view = device->create_texture_view(RHI::TextureViewDesc{
            .texture = *texture,
            .view_type = RHI::TextureViewType::View2D,
            .label = "spectral temporal accumulation view",
        });
        if (!view) {
            const Core::GraphicsBackendError error = graphics_error_from_rhi(
                view.error(), "create spectral accumulation view");
            destroy_spectral_accumulation_target(record);
            return unexpected(error);
        }
        record.spectral_accumulation.view = *view;
        record.spectral_accumulation.extent = extent;
        record.spectral_accumulation.initialized = false;
        return {};
    }

    void Renderer::destroy_spectral_accumulation_target(WindowSurfaceRecord &record) noexcept {
        if (RHI::RhiDevice *device = rhi_device()) {
            if (record.spectral_accumulation.view) {
                device->destroy_texture_view(record.spectral_accumulation.view);
            }
            if (record.spectral_accumulation.texture) {
                device->destroy_texture(record.spectral_accumulation.texture);
            }
        }
        record.spectral_accumulation = {};
    }

    Core::RendererResult Renderer::ensure_frame_spectral_photon_targets(
        FrameInFlight &slot, u32 requested_photon_capacity) {
        const u32 photon_capacity = std::max(requested_photon_capacity, 1u);
        u32 hash_capacity = 1u;
        const u64 required_hash_entries = static_cast<u64>(photon_capacity) * 2u;
        while (hash_capacity < required_hash_entries && hash_capacity <= (1u << 30u)) {
            hash_capacity <<= 1u;
        }
        if (slot.spectral_photon_targets.photons && slot.spectral_photon_targets.valid_count &&
            slot.spectral_photon_targets.hash_heads &&
            slot.spectral_photon_targets.photon_capacity >= photon_capacity &&
            slot.spectral_photon_targets.hash_capacity >= hash_capacity) {
            return {};
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Cannot allocate photon targets without an RHI device."));
        }
        destroy_frame_spectral_photon_targets(slot);

        auto photons = device->create_buffer(RHI::BufferDesc{
            .size = static_cast<u64>(photon_capacity) * sizeof(SpectralPhotonGpu),
            .usage = RHI::BufferUsage::Storage,
            .memory = RHI::MemoryLocation::DeviceLocal,
            .label = "persistent spectral caustic photons",
        });
        if (!photons) {
            return unexpected(graphics_error_from_rhi(photons.error(), "create spectral photon buffer"));
        }
        slot.spectral_photon_targets.photons = *photons;

        auto valid_count = device->create_buffer(RHI::BufferDesc{
            .size = sizeof(u32),
            .usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferDst,
            .memory = RHI::MemoryLocation::DeviceLocal,
            .label = "persistent spectral photon count",
        });
        if (!valid_count) {
            const Core::GraphicsBackendError error = graphics_error_from_rhi(
                valid_count.error(), "create spectral photon count buffer");
            destroy_frame_spectral_photon_targets(slot);
            return unexpected(error);
        }
        slot.spectral_photon_targets.valid_count = *valid_count;

        auto hash_heads = device->create_buffer(RHI::BufferDesc{
            .size = static_cast<u64>(hash_capacity) * sizeof(u32),
            .usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferDst,
            .memory = RHI::MemoryLocation::DeviceLocal,
            .label = "persistent spectral photon hash heads",
        });
        if (!hash_heads) {
            const Core::GraphicsBackendError error = graphics_error_from_rhi(
                hash_heads.error(), "create spectral photon hash-head buffer");
            destroy_frame_spectral_photon_targets(slot);
            return unexpected(error);
        }
        slot.spectral_photon_targets.hash_heads = *hash_heads;
        slot.spectral_photon_targets.photon_capacity = photon_capacity;
        slot.spectral_photon_targets.hash_capacity = hash_capacity;
        return {};
    }

    void Renderer::destroy_frame_spectral_photon_targets(FrameInFlight &slot) noexcept {
        if (RHI::RhiDevice *device = rhi_device()) {
            if (slot.spectral_photon_targets.hash_heads) {
                device->destroy_buffer(slot.spectral_photon_targets.hash_heads);
            }
            if (slot.spectral_photon_targets.valid_count) {
                device->destroy_buffer(slot.spectral_photon_targets.valid_count);
            }
            if (slot.spectral_photon_targets.photons) {
                device->destroy_buffer(slot.spectral_photon_targets.photons);
            }
        }
        slot.spectral_photon_targets = {};
    }

    Core::RendererResult Renderer::prepare_spectral_photon_mapping(
        RHI::CommandEncoder &encoder, FrameInFlight &slot, const FrameSubmission &submission,
        bool emission_needed, u64 photon_signature) {
        const SpectralRenderMode mode = submission.render_graph.spectral_path_tracing.mode;
        if (mode == SpectralRenderMode::RasterDeferred) {
            return {};
        }
        const SpectralPathTracingSettings &settings = submission.render_graph.spectral_path_tracing;
        const bool enabled = mode == SpectralRenderMode::FullPathTracing && settings.photon_count > 0u;
        const u32 requested_capacity = enabled ? settings.photon_count : 1u;
        if (Core::RendererResult targets = ensure_frame_spectral_photon_targets(slot, requested_capacity);
            !targets.has_value()) {
            return targets;
        }
        // A fresh/resized buffer (ensure_frame_spectral_photon_targets above just recreated it) always
        // needs emission regardless of the caller's accumulation-reset signal — its contents are
        // otherwise uninitialized device memory.
        emission_needed = emission_needed || !slot.spectral_photon_targets.populated;
        if (!enabled || !emission_needed) {
            return {};
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Cannot prepare photon mapping without an RHI device."));
        }
        glm::vec3 sun_direction = submission.lighting.sun.direction;
        if (glm::dot(sun_direction, sun_direction) < 1.0e-8f) {
            sun_direction = glm::vec3{0.0f, -1.0f, 0.0f};
        } else {
            sun_direction = glm::normalize(sun_direction);
        }
        const f32 scene_radius = std::max(slot.spectral_scene_bounds.w, 0.001f);
        constexpr f32 ray_epsilon = 0.001f;
        const SpectralPhotonConstantsGpu constants{
            .sun_direction_and_angular_radius = glm::vec4{
                sun_direction, submission.lighting.sun.angular_radius_degrees * 0.01745329251994329577f},
            .sun_radiance = glm::vec4{submission.lighting.sun.radiance, 1.0f},
            .emission_center_and_radius = slot.spectral_scene_bounds,
            .launch_parameters = glm::vec4{
                scene_radius * 1.05f + ray_epsilon, ray_epsilon,
                settings.caustic_gather_radius, settings.caustic_gather_radius},
            .wavelength_range = glm::vec4{
                settings.wavelength_min_nm, settings.wavelength_max_nm, 0.0f, 0.0f},
            .capacities_and_frame = glm::uvec4{
                settings.photon_count, slot.spectral_photon_targets.hash_capacity,
                static_cast<u32>(submission.frame_index), 0xffu},
            .limits_and_flags = glm::uvec4{
                settings.max_bounces, 128u, 1u, 0u},
        };
        auto constant_buffer = device->create_buffer(RHI::BufferDesc{
            .size = sizeof(constants),
            .usage = RHI::BufferUsage::Uniform,
            .memory = RHI::MemoryLocation::HostUpload,
            .label = "spectral photon constants",
        });
        if (!constant_buffer) {
            return unexpected(graphics_error_from_rhi(
                constant_buffer.error(), "create spectral photon constants"));
        }
        if (auto written = device->write_buffer(
                *constant_buffer, 0, std::as_bytes(span{&constants, 1}));
            !written) {
            device->destroy_buffer(*constant_buffer);
            return unexpected(graphics_error_from_rhi(
                written.error(), "write spectral photon constants"));
        }
        slot.transient_buffers.push_back(*constant_buffer);
        slot.spectral_photon_constants = *constant_buffer;

        encoder.fill_buffer(slot.spectral_photon_targets.valid_count, 0, sizeof(u32), 0u);
        encoder.fill_buffer(
            slot.spectral_photon_targets.hash_heads, 0,
            static_cast<u64>(slot.spectral_photon_targets.hash_capacity) * sizeof(u32), 0xffffffffu);
        slot.spectral_photon_targets.populated = true;
        slot.spectral_photon_targets.state_signature = photon_signature;
        return {};
    }

    Core::RendererResult Renderer::record_spectral_photon_pass(
        RHI::ComputePassEncoder &pass, FrameInFlight &slot, const FrameSubmission &submission,
        usize pipeline_index, const char *label) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !slot.spectral_tlas || !slot.spectral_photon_constants ||
            pipeline_index >= photon_entry_points.size()) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Cannot dispatch spectral photons without prepared scene resources."));
        }
        auto resources = spectral_path_tracing_.lock();
        if (!resources->photon_bind_group_layout || !resources->photon_pipelines[pipeline_index]) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Spectral photon pipelines were not prepared."));
        }

        vector<RHI::BindGroupEntry> entries;
        entries.reserve(resources->photon_resource_bindings.size());
        auto append_buffer = [&](const char *name, RHI::BufferHandle buffer) -> bool {
            const auto it = resources->photon_resource_bindings.find(name);
            if (it == resources->photon_resource_bindings.end() || !buffer) return false;
            entries.push_back(RHI::BindGroupEntry{.binding = it->second.binding, .buffer = buffer});
            return true;
        };
        auto append_acceleration_structure = [&](const char *name,
                                                  RHI::AccelerationStructureHandle acceleration_structure) -> bool {
            const auto it = resources->photon_resource_bindings.find(name);
            if (it == resources->photon_resource_bindings.end() || !acceleration_structure) return false;
            entries.push_back(RHI::BindGroupEntry{
                .binding = it->second.binding,
                .acceleration_structure = acceleration_structure,
            });
            return true;
        };
        const auto append_material_texture_heap = [&]() -> bool {
            const auto it = resources->photon_resource_bindings.find("spectralMaterialTextures");
            if (it == resources->photon_resource_bindings.end() || slot.spectral_material_textures.empty()) return false;
            for (u32 index = 0; index < slot.spectral_material_textures.size(); ++index) {
                const TextureResource *material_texture = texture(slot.spectral_material_textures[index]);
                if (material_texture == nullptr || !material_texture->view || !material_texture->sampler) return false;
                entries.push_back(RHI::BindGroupEntry{
                    .binding = it->second.binding,
                    .array_element = index,
                    .texture_view = material_texture->view,
                    .sampler = material_texture->sampler,
                });
            }
            return true;
        };
        const bool complete =
            append_buffer("photonSettings", slot.spectral_photon_constants) &&
            append_acceleration_structure("sceneAccelerationStructure", slot.spectral_tlas) &&
            append_buffer("geometryVertices", vertex_arena_.buffer) &&
            append_buffer("geometryIndices", index_arena_.buffer) &&
            append_buffer("sceneInstances", slot.spectral_scene_instances) &&
            append_buffer("sceneMaterials", slot.spectral_materials) &&
            append_buffer("causticPhotons", slot.spectral_photon_targets.photons) &&
            append_buffer("causticPhotonCount", slot.spectral_photon_targets.valid_count) &&
            append_buffer("causticPhotonHashHeads", slot.spectral_photon_targets.hash_heads) &&
            append_material_texture_heap();
        if (!complete) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Spectral photon reflection/resource binding contract is incomplete."));
        }
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = resources->photon_bind_group_layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .variable_descriptor_count = static_cast<u32>(slot.spectral_material_textures.size()),
            .label = label,
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), label));
        }
        slot.transient_bind_groups.push_back(*bind_group);
        pass.set_pipeline(resources->photon_pipelines[pipeline_index]);
        pass.set_bind_group(0, *bind_group);
        pass.dispatch((submission.render_graph.spectral_path_tracing.photon_count + 255u) / 256u, 1u, 1u);
        return {};
    }

    Core::RendererResult Renderer::record_spectral_photon_emission(
        RHI::ComputePassEncoder &pass, FrameInFlight &slot, const FrameSubmission &submission) {
        return record_spectral_photon_pass(pass, slot, submission, 0u, "spectral photon emission bind group");
    }

    Core::RendererResult Renderer::record_spectral_photon_hash(
        RHI::ComputePassEncoder &pass, FrameInFlight &slot, const FrameSubmission &submission) {
        return record_spectral_photon_pass(pass, slot, submission, 1u, "spectral photon hash bind group");
    }

    Core::RendererResult Renderer::record_spectral_integrator(
        RHI::ComputePassEncoder &pass, FrameInFlight &slot, const FrameSubmission &submission,
        Core::Extent2D extent, const SpectralIntegratorViews &views, bool accumulation_reset) {
        const SpectralRenderMode mode = submission.render_graph.spectral_path_tracing.mode;
        if (mode == SpectralRenderMode::RasterDeferred) return {};
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !slot.spectral_tlas) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Cannot dispatch a spectral integrator without a prepared TLAS."));
        }
        auto resources = spectral_path_tracing_.lock();
        if (!resources->ready || resources->bind_group_layouts.empty()) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Spectral integrator pipelines were not prepared."));
        }

        const SpectralPathTracingSettings &settings = submission.render_graph.spectral_path_tracing;
        const SpectralFrameConstantsGpu constants{
            .inverse_view_projection = glm::inverse(submission.camera.projection * submission.camera.view),
            .view_projection = submission.camera.projection * submission.camera.view,
            .previous_view_projection = submission.camera.previous_view_projection,
            .camera_position = glm::vec4(submission.camera.world_position, 1.0f),
            .sun_direction_and_angular_radius = glm::vec4(
                submission.lighting.sun.direction,
                submission.lighting.sun.angular_radius_degrees * 0.01745329251994329577f),
            .sun_radiance = glm::vec4(submission.lighting.sun.radiance, 1.0f),
            .extent = glm::uvec2(extent.width, extent.height),
            .frame_index = static_cast<u32>(submission.frame_index),
            .samples_per_pixel = settings.samples_per_pixel,
            .max_bounces = settings.max_bounces,
            .russian_roulette_start_bounce = settings.russian_roulette_start_bounce,
            .ao_ray_count = std::max(1u, submission.render_graph.gtao_quality + 1u),
            .instance_mask = 0xffu,
            .ao_radius = submission.render_graph.gtao_radius,
            .wavelength_min_nm = settings.wavelength_min_nm,
            .wavelength_max_nm = settings.wavelength_max_nm,
            .environment_intensity = submission.render_graph.background_intensity,
            .caustic_gather_parameters = glm::vec4{
                settings.caustic_gather_radius, settings.caustic_gather_radius, 0.0f, 0.0f},
            .caustic_gather_control = glm::uvec4{
                slot.spectral_photon_targets.hash_capacity,
                128u,
                mode == SpectralRenderMode::FullPathTracing && settings.photon_count > 0u ? 1u : 0u,
                settings.photon_count},
            .accumulation_control = glm::uvec4{accumulation_reset ? 1u : 0u, 4096u, 0u, 0u},
        };
        auto constant_buffer = device->create_buffer(RHI::BufferDesc{
            .size = sizeof(constants),
            .usage = RHI::BufferUsage::Uniform,
            .memory = RHI::MemoryLocation::HostUpload,
            .label = "spectral frame constants",
        });
        if (!constant_buffer) {
            return unexpected(graphics_error_from_rhi(constant_buffer.error(), "create spectral frame constants"));
        }
        if (auto written = device->write_buffer(*constant_buffer, 0, std::as_bytes(span{&constants, 1})); !written) {
            device->destroy_buffer(*constant_buffer);
            return unexpected(graphics_error_from_rhi(written.error(), "write spectral frame constants"));
        }
        slot.transient_buffers.push_back(*constant_buffer);
        slot.spectral_frame_constants = *constant_buffer;

        vector<RHI::BindGroupEntry> entries;
        entries.reserve(resources->resource_bindings.size());
        auto append_buffer = [&](const char *name, RHI::BufferHandle buffer) -> bool {
            const auto it = resources->resource_bindings.find(name);
            if (it == resources->resource_bindings.end() || !buffer) return false;
            entries.push_back(RHI::BindGroupEntry{.binding = it->second.binding, .buffer = buffer});
            return true;
        };
        auto append_texture = [&](const char *name, RHI::TextureViewHandle view) -> bool {
            const auto it = resources->resource_bindings.find(name);
            if (it == resources->resource_bindings.end() || !view) return false;
            entries.push_back(RHI::BindGroupEntry{.binding = it->second.binding, .texture_view = view});
            return true;
        };
        auto append_sampler = [&](const char *name, RHI::SamplerHandle sampler) -> bool {
            const auto it = resources->resource_bindings.find(name);
            if (it == resources->resource_bindings.end() || !sampler) return false;
            entries.push_back(RHI::BindGroupEntry{.binding = it->second.binding, .sampler = sampler});
            return true;
        };
        auto append_acceleration_structure = [&](const char *name,
                                                  RHI::AccelerationStructureHandle acceleration_structure) -> bool {
            const auto it = resources->resource_bindings.find(name);
            if (it == resources->resource_bindings.end() || !acceleration_structure) return false;
            entries.push_back(RHI::BindGroupEntry{
                .binding = it->second.binding,
                .acceleration_structure = acceleration_structure,
            });
            return true;
        };
        const auto append_material_texture_heap = [&]() -> bool {
            const auto it = resources->resource_bindings.find("spectralMaterialTextures");
            if (it == resources->resource_bindings.end() || slot.spectral_material_textures.empty()) return false;
            for (u32 index = 0; index < slot.spectral_material_textures.size(); ++index) {
                const TextureResource *material_texture = texture(slot.spectral_material_textures[index]);
                if (material_texture == nullptr || !material_texture->view || !material_texture->sampler) return false;
                entries.push_back(RHI::BindGroupEntry{
                    .binding = it->second.binding,
                    .array_element = index,
                    .texture_view = material_texture->view,
                    .sampler = material_texture->sampler,
                });
            }
            return true;
        };

        const bool complete =
            append_buffer("frame", *constant_buffer) &&
            append_acceleration_structure("sceneAccelerationStructure", slot.spectral_tlas) &&
            append_buffer("geometryVertices", vertex_arena_.buffer) &&
            append_buffer("geometryIndices", index_arena_.buffer) &&
            append_buffer("sceneInstances", slot.spectral_scene_instances) &&
            append_buffer("sceneMaterials", slot.spectral_materials) &&
            append_buffer("causticPhotons", slot.spectral_photon_targets.photons) &&
            append_buffer("causticPhotonCount", slot.spectral_photon_targets.valid_count) &&
            append_buffer("causticPhotonHashHeads", slot.spectral_photon_targets.hash_heads) &&
            append_texture("rasterAlbedo", views.raster_albedo) &&
            append_texture("rasterNormal", views.raster_normal) &&
            append_texture("rasterMaterial", views.raster_material) &&
            append_texture("rasterEmissive", views.raster_emissive) &&
            append_texture("rasterMotion", views.raster_motion) &&
            append_texture("rasterDepth", views.raster_depth) &&
            append_texture("effectOutput", views.effect_output) &&
            append_texture("sceneColorOutput", views.scene_color_output) &&
            append_texture("gbufferMotionOutput", views.gbuffer_motion_output) &&
            append_texture("primaryDepthOutput", views.primary_depth_output) &&
            append_texture("accumulationOutput", views.accumulation_output) &&
            append_buffer("atmosphereData", views.atmosphere_constants) &&
            append_texture("transmittanceLut", views.transmittance_lut) &&
            append_texture("skyViewLut", views.sky_view_lut) &&
            append_sampler("atmosphereSampler", resources->atmosphere_sampler) &&
            append_material_texture_heap();
        if (!complete) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Spectral integrator reflection/resource binding contract is incomplete."));
        }
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = resources->bind_group_layouts.front(),
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .variable_descriptor_count = static_cast<u32>(slot.spectral_material_textures.size()),
            .label = "spectral integrator bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create spectral integrator bind group"));
        }
        slot.transient_bind_groups.push_back(*bind_group);

        pass.set_pipeline(resources->pipelines[spectral_pipeline_index(mode)]);
        pass.set_bind_group(0, *bind_group);
        pass.dispatch((extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1u);
        return {};
    }

    Core::RendererResult Renderer::record_spectral_depth_commit(
        RHI::RenderPassEncoder &pass, FrameInFlight &slot, RHI::TextureViewHandle primary_depth_view,
        Core::Extent2D extent) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !primary_depth_view) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Cannot commit spectral depth without a source view and RHI device."));
        }
        auto resources = spectral_path_tracing_.lock();
        if (!resources->depth_commit_pipeline || !resources->depth_commit_bind_group_layout) {
            return unexpected(spectral_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                             "Spectral depth-commit pipeline was not prepared."));
        }
        const RHI::BindGroupEntry entry{
            .binding = resources->depth_commit_texture_binding,
            .texture_view = primary_depth_view,
        };
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = resources->depth_commit_bind_group_layout,
            .entries = span<const RHI::BindGroupEntry>{&entry, 1},
            .label = "spectral depth commit bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create spectral depth commit bind group"));
        }
        slot.transient_bind_groups.push_back(*bind_group);
        pass.set_viewport(RHI::Viewport{.width = static_cast<f32>(extent.width),
                                       .height = static_cast<f32>(extent.height), .max_depth = 1.0f});
        pass.set_scissor(RHI::Rect2D{.width = extent.width, .height = extent.height});
        pass.set_pipeline(resources->depth_commit_pipeline);
        pass.set_bind_group(0, *bind_group);
        pass.draw(RHI::DrawArgs{.vertex_count = 3});
        return {};
    }

    void Renderer::destroy_spectral_path_tracing_resources_locked(
        SpectralPathTracingResources &resources) noexcept {
        if (RHI::RhiDevice *device = rhi_device()) {
            if (resources.depth_commit_pipeline) device->destroy_render_pipeline(resources.depth_commit_pipeline);
            if (resources.depth_commit_pipeline_layout) device->destroy_pipeline_layout(resources.depth_commit_pipeline_layout);
            if (resources.depth_commit_bind_group_layout) device->destroy_bind_group_layout(resources.depth_commit_bind_group_layout);
            if (resources.depth_commit_fragment_module) device->destroy_shader_module(resources.depth_commit_fragment_module);
            if (resources.depth_commit_vertex_module) device->destroy_shader_module(resources.depth_commit_vertex_module);
            for (RHI::ComputePipelineHandle pipeline : resources.photon_pipelines) {
                if (pipeline) device->destroy_compute_pipeline(pipeline);
            }
            if (resources.photon_pipeline_layout) device->destroy_pipeline_layout(resources.photon_pipeline_layout);
            if (resources.photon_bind_group_layout) device->destroy_bind_group_layout(resources.photon_bind_group_layout);
            for (RHI::ShaderModuleHandle module : resources.photon_modules) {
                if (module) device->destroy_shader_module(module);
            }
            for (RHI::ComputePipelineHandle pipeline : resources.pipelines) {
                if (pipeline) device->destroy_compute_pipeline(pipeline);
            }
            if (resources.pipeline_layout) device->destroy_pipeline_layout(resources.pipeline_layout);
            for (RHI::BindGroupLayoutHandle layout : resources.bind_group_layouts) {
                if (layout) device->destroy_bind_group_layout(layout);
            }
            for (RHI::ShaderModuleHandle module : resources.modules) {
                if (module) device->destroy_shader_module(module);
            }
            if (resources.atmosphere_sampler) device->destroy_sampler(resources.atmosphere_sampler);
        }
        resources = {};
    }

    void Renderer::destroy_spectral_path_tracing_resources() noexcept {
        auto resources = spectral_path_tracing_.lock();
        destroy_spectral_path_tracing_resources_locked(*resources);
    }

} // namespace SFT::Renderer
