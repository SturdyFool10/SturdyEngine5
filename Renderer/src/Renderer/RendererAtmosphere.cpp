// Hillaire/Bruneton-style precomputed atmospheric scattering: per-frame constants buffer plus the
// three LUT-bake compute passes (transmittance/multi-scattering/sky-view — see
// Shaders/sturdy_atmosphere.slang and Shaders/sky_*_lut.slang for the shared math and bake shaders).
// Consumed by Shaders/deferred_shadow_lighting.slang for the sky background and aerial perspective.
//
// Phase 1: atmosphere physics parameters are hardcoded Earth defaults (Bruneton's commonly used
// reference values), not yet user-configurable — exposing them through Engine's public settings API
// is deferred to a follow-up (see agent_collaboration.md) so this doesn't have to touch the render
// graph description types being actively refactored elsewhere in this tree. The buffer/shader
// plumbing here does not change when that follow-up lands; only prepare_atmosphere_frame's literals
// get replaced with real submitted values.

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <array>
#include <cstddef>
#include <span>
#include <vector>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        [[nodiscard]] Core::GraphicsBackendError atmosphere_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        [[nodiscard]] glm::vec3 safe_normalize(glm::vec3 value, glm::vec3 fallback) noexcept {
            const f32 length_squared = glm::dot(value, value);
            return std::isfinite(length_squared) && length_squared > 1.0e-12f
                       ? value * glm::inversesqrt(length_squared)
                       : fallback;
        }

        // Earth reference constants (Bruneton's commonly used values) — identical to
        // Shaders/sturdy_atmosphere.slang's defaultEarthAtmosphere(), which documents each field.
        // Phase 2 replaces this with real user-submitted values; nothing else about the buffer layout
        // or the shaders that read it changes when that lands.
        struct EarthAtmosphereDefaults {
            static constexpr glm::vec3 kRayleighScattering{5.802e-6f, 13.558e-6f, 33.1e-6f};
            static constexpr f32 kRayleighScaleHeight = 8000.0f;
            static constexpr glm::vec3 kMieScattering{3.996e-6f, 3.996e-6f, 3.996e-6f};
            static constexpr glm::vec3 kMieExtinction{4.440e-6f, 4.440e-6f, 4.440e-6f};
            static constexpr f32 kMieScaleHeight = 1200.0f;
            static constexpr f32 kMiePhaseG = 0.8f;
            static constexpr glm::vec3 kOzoneAbsorption{0.650e-6f, 1.881e-6f, 0.085e-6f};
            static constexpr f32 kOzoneLayerCenterAltitude = 25000.0f;
            static constexpr f32 kOzoneLayerWidth = 15000.0f;
            static constexpr glm::vec3 kGroundAlbedo{0.3f, 0.3f, 0.3f};
            static constexpr f32 kPlanetRadiusMeters = 6360000.0f;
            static constexpr f32 kAtmosphereRadiusMeters = 6460000.0f;
        };

        constexpr RHI::Extent3D kTransmittanceLutExtent{.width = 256, .height = 64, .depth_or_layers = 1};
        constexpr RHI::Extent3D kMultiScatteringLutExtent{.width = 32, .height = 32, .depth_or_layers = 1};
        constexpr RHI::Extent3D kSkyViewLutExtent{.width = 192, .height = 108, .depth_or_layers = 1};
    } // namespace

    Core::RendererResult Renderer::ensure_frame_atmosphere_targets(FrameInFlight &slot) {
        ZoneScopedN("Renderer::ensure_frame_atmosphere_targets");
        if (slot.atmosphere_targets.constants_buffer) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(atmosphere_error("Cannot allocate atmosphere targets without an RHI device."));
        }
        auto buffer = device->create_buffer(RHI::BufferDesc{
            .size = sizeof(AtmosphereGpuData),
            .usage = RHI::BufferUsage::Uniform,
            .memory = RHI::MemoryLocation::HostUpload,
            .label = "atmosphere constants",
        });
        if (!buffer) {
            return unexpected(graphics_error_from_rhi(buffer.error(), "create atmosphere constants buffer"));
        }
        slot.atmosphere_targets.constants_buffer = *buffer;
        return {};
    }

    void Renderer::destroy_frame_atmosphere_targets(FrameInFlight &slot) noexcept {
        ZoneScopedN("Renderer::destroy_frame_atmosphere_targets");
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr && slot.atmosphere_targets.constants_buffer) {
            device->destroy_buffer(slot.atmosphere_targets.constants_buffer);
        }
        slot.atmosphere_targets = {};
    }

    Core::RendererResult Renderer::prepare_atmosphere_frame(const FrameSubmission &submission,
                                                             RHI::BufferHandle constants_buffer) {
        ZoneScopedN("Renderer::prepare_atmosphere_frame");
        static_assert(sizeof(AtmosphereGpuData) == 160);
        static_assert(offsetof(AtmosphereGpuData, planet_center_world) == 96);
        static_assert(offsetof(AtmosphereGpuData, sun_illuminance) == 144);
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !constants_buffer) {
            return unexpected(atmosphere_error("Cannot prepare atmosphere frame without its per-frame constant buffer."));
        }

        AtmosphereGpuData gpu{};
        gpu.rayleigh_scattering_exp_scale = glm::vec4{
            EarthAtmosphereDefaults::kRayleighScattering, -1.0f / EarthAtmosphereDefaults::kRayleighScaleHeight};
        gpu.mie_scattering_exp_scale = glm::vec4{
            EarthAtmosphereDefaults::kMieScattering, -1.0f / EarthAtmosphereDefaults::kMieScaleHeight};
        gpu.mie_extinction_phase_g = glm::vec4{EarthAtmosphereDefaults::kMieExtinction, EarthAtmosphereDefaults::kMiePhaseG};
        gpu.ozone_absorption_center_altitude = glm::vec4{
            EarthAtmosphereDefaults::kOzoneAbsorption, EarthAtmosphereDefaults::kOzoneLayerCenterAltitude};
        gpu.ozone_width_planet_atmosphere_radius = glm::vec4{
            EarthAtmosphereDefaults::kOzoneLayerWidth, EarthAtmosphereDefaults::kPlanetRadiusMeters,
            EarthAtmosphereDefaults::kAtmosphereRadiusMeters, 0.0f};
        gpu.ground_albedo = glm::vec4{EarthAtmosphereDefaults::kGroundAlbedo, 0.0f};

        // The planet is re-centered directly beneath the camera every frame (world "up" is always
        // +Y in this engine's atmosphere model) — standard large-radius precision trick, the same
        // idea CSM's per-frame texel snapping already uses elsewhere in this codebase. Camera drift
        // horizontally relative to the ~6360km planet radius is astronomically small, so this is not
        // an approximation that trades away accuracy, just numerical range.
        const glm::vec3 planet_center_world{
            submission.camera.world_position.x,
            -EarthAtmosphereDefaults::kPlanetRadiusMeters,
            submission.camera.world_position.z,
        };
        gpu.planet_center_world = glm::vec4{planet_center_world, 0.0f};
        gpu.camera_position_planet_space = glm::vec4{submission.camera.world_position - planet_center_world, 0.0f};

        const DirectionalLight &sun = submission.lighting.sun;
        const glm::vec3 sun_direction_toward_scene = safe_normalize(sun.direction, glm::vec3{0.0f, -1.0f, 0.0f});
        // Pre-negated here (toward the sun, not toward the scene) — see AtmosphereGpuData::
        // sun_direction_angular_radius's own doc comment in sturdy_atmosphere.slang for why every
        // atmosphere shader wants this convention instead of DirectionalLightGpuData's.
        gpu.sun_direction_angular_radius = glm::vec4{
            -sun_direction_toward_scene,
            glm::radians(std::clamp(std::isfinite(sun.angular_radius_degrees) ? sun.angular_radius_degrees : 0.27f, 0.0f, 10.0f)),
        };
        gpu.sun_illuminance = glm::vec4{glm::max(sun.radiance, glm::vec3{0.0f}), 0.0f};

        const span<const AtmosphereGpuData> data{&gpu, 1};
        auto written = device->write_buffer(constants_buffer, 0, std::as_bytes(data));
        if (!written) {
            return unexpected(graphics_error_from_rhi(written.error(), "write atmosphere constants"));
        }
        return {};
    }

    Core::RendererResult Renderer::ensure_atmosphere_lut_resources() {
        ZoneScopedN("Renderer::ensure_atmosphere_lut_resources");
        auto guard = atmosphere_lut_.lock();
        if (guard->ready) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(atmosphere_error("Cannot build atmosphere LUT resources without an RHI device."));
        }

        // Same reflection-driven compute-pipeline build as Renderer::ensure_instance_cull_resources
        // (RendererGpuCulling.cpp), repeated once per bake shader — no push constants in any of the
        // three, so pipeline_layout's range list is always empty (matching
        // ensure_shadow_lighting_resources' pipeline layout, not instance-cull's).
        auto build_lut_pipeline = [&](const char *shader_path, const char *module_name, const char *label,
                                       AtmosphereLutBakePipeline &out) -> Core::RendererResult {
            const slang::ShaderCompileOptions options{
                .targets = {slang::ShaderTarget{}},
                .entry_points = {slang::ShaderEntryPointRequest{.name = "mainCS", .stage = slang::ShaderStage::Compute}},
            };
            slang::ShaderVariantCache shader_cache{
                slang::ShaderSource::from_file(shader_path, module_name),
                options,
                slang::ShaderCompiler{},
                recovery_create_info_.enable_shader_disk_cache};
            auto shader = shader_cache.get_or_compile_base();
            if (!shader) {
                return unexpected(atmosphere_error(string{"compile "} + label + " failed: " + shader.error().message + "\n" + shader.error().diagnostics));
            }
            out.shader = *shader;

            auto code = out.shader.entry_point_code("mainCS");
            if (!code) {
                return unexpected(atmosphere_error(string{"generate "} + label + " bytecode failed: " + code.error().message));
            }
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = RHI::ShaderLanguage::SpirV,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = label,
            });
            if (!module) {
                return unexpected(graphics_error_from_rhi(module.error(), label));
            }
            out.module = *module;

            const slang::ShaderReflection &reflection = out.shader.reflection();
            const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, RHI::ShaderStage::Compute);
            if (generated.empty()) {
                return unexpected(atmosphere_error(string{label} + " reflection produced no bind-group layout."));
            }
            auto layout = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{generated.front().entries.data(), generated.front().entries.size()},
                .label = label,
            });
            if (!layout) {
                return unexpected(graphics_error_from_rhi(layout.error(), label));
            }
            out.bind_group_layout = *layout;

            const array<RHI::BindGroupLayoutHandle, 1> layouts{out.bind_group_layout};
            auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
                .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{layouts.data(), layouts.size()},
                .push_constant_ranges = {},
                .label = label,
            });
            if (!pipeline_layout) {
                return unexpected(graphics_error_from_rhi(pipeline_layout.error(), label));
            }
            out.pipeline_layout = *pipeline_layout;

            auto pipeline = device->create_compute_pipeline(RHI::ComputePipelineDesc{
                .layout = out.pipeline_layout,
                .compute = RHI::ShaderEntry{.module = out.module, .entry_point = "mainCS", .stage = RHI::ShaderStage::Compute},
                .label = label,
            });
            if (!pipeline) {
                return unexpected(graphics_error_from_rhi(pipeline.error(), label));
            }
            out.pipeline = *pipeline;
            out.shader.release_compiler_state();
            return {};
        };

        if (Core::RendererResult built = build_lut_pipeline("Shaders/sky_transmittance_lut.slang", "sky_transmittance_lut",
                                                             "sky transmittance lut pipeline", guard->transmittance);
            !built.has_value()) {
            destroy_atmosphere_lut_resources();
            return built;
        }
        if (Core::RendererResult built = build_lut_pipeline("Shaders/sky_multi_scattering_lut.slang", "sky_multi_scattering_lut",
                                                             "sky multi-scattering lut pipeline", guard->multi_scattering);
            !built.has_value()) {
            destroy_atmosphere_lut_resources();
            return built;
        }
        if (Core::RendererResult built = build_lut_pipeline("Shaders/sky_view_lut.slang", "sky_view_lut",
                                                             "sky view lut pipeline", guard->sky_view);
            !built.has_value()) {
            destroy_atmosphere_lut_resources();
            return built;
        }

        const RHI::SamplerDesc sampler_desc{
            .min_filter = RHI::Filter::Linear,
            .mag_filter = RHI::Filter::Linear,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .label = "atmosphere lut bake sampler",
        };
        auto lut_sampler = device->create_sampler(sampler_desc);
        if (!lut_sampler) {
            destroy_atmosphere_lut_resources();
            return unexpected(graphics_error_from_rhi(lut_sampler.error(), "create atmosphere lut bake sampler"));
        }
        guard->lut_sampler = *lut_sampler;
        guard->ready = true;
        return {};
    }

    void Renderer::destroy_atmosphere_lut_resources() noexcept {
        ZoneScopedN("Renderer::destroy_atmosphere_lut_resources");
        RHI::RhiDevice *device = rhi_device();
        auto guard = atmosphere_lut_.lock();
        if (device != nullptr) {
            if (guard->lut_sampler) {
                device->destroy_sampler(guard->lut_sampler);
            }
            for (AtmosphereLutBakePipeline *pipeline : {&guard->transmittance, &guard->multi_scattering, &guard->sky_view}) {
                if (pipeline->pipeline) {
                    device->destroy_compute_pipeline(pipeline->pipeline);
                }
                if (pipeline->pipeline_layout) {
                    device->destroy_pipeline_layout(pipeline->pipeline_layout);
                }
                if (pipeline->bind_group_layout) {
                    device->destroy_bind_group_layout(pipeline->bind_group_layout);
                }
                if (pipeline->module) {
                    device->destroy_shader_module(pipeline->module);
                }
            }
        }
        *guard = {};
    }

    Core::RendererResult Renderer::record_atmosphere_lut_bakes(
        RenderGraph &graph, RHI::BufferHandle atmosphere_buffer,
        RenderGraphTextureHandle &out_transmittance_lut, RenderGraphTextureHandle &out_multi_scattering_lut,
        RenderGraphTextureHandle &out_sky_view_lut, vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_atmosphere_lut_bakes");
        if (Core::RendererResult ready = ensure_atmosphere_lut_resources(); !ready.has_value()) {
            return ready;
        }
        if (!atmosphere_buffer) {
            return unexpected(atmosphere_error("Cannot record atmosphere LUT bakes without a constants buffer."));
        }

        const RenderGraphTextureHandle transmittance_lut = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RGBA16Float,
            .extent = kTransmittanceLutExtent,
            .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
            .label = "sky transmittance lut",
        });
        const RenderGraphTextureHandle multi_scattering_lut = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RGBA16Float,
            .extent = kMultiScatteringLutExtent,
            .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
            .label = "sky multi-scattering lut",
        });
        const RenderGraphTextureHandle sky_view_lut = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RGBA16Float,
            .extent = kSkyViewLutExtent,
            .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
            .label = "sky view lut",
        });

        graph.add_compute_pass("sky transmittance lut"_ustr)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = transmittance_lut, .read = false, .write = true})
            .set_execute([this, atmosphere_buffer, transmittance_lut, &transient_bind_groups](
                             RenderGraphComputeContext &context) -> Core::RendererResult {
                RHI::RhiDevice *device = rhi_device();
                if (device == nullptr) {
                    return unexpected(atmosphere_error("Cannot record atmosphere LUT bakes without an RHI device."));
                }
                vector<ReflectedResource> resources;
                RHI::BindGroupLayoutHandle bind_group_layout{};
                RHI::ComputePipelineHandle pipeline{};
                {
                    auto guard = atmosphere_lut_.lock();
                    resources = collect_resource_bindings(guard->transmittance.shader.reflection());
                    bind_group_layout = guard->transmittance.bind_group_layout;
                    pipeline = guard->transmittance.pipeline;
                }
                vector<RHI::BindGroupEntry> entries;
                entries.reserve(resources.size());
                for (const ReflectedResource &resource : resources) {
                    RHI::BindGroupEntry entry{.binding = resource.binding};
                    if (resource.name == "atmosphereData") {
                        entry.buffer = atmosphere_buffer;
                        entry.size = sizeof(AtmosphereGpuData);
                    } else if (resource.name == "outputLut") {
                        entry.texture_view = context.texture(transmittance_lut).default_view;
                    } else {
                        return unexpected(atmosphere_error("Sky transmittance lut reflection contains an unknown resource: " + resource.name));
                    }
                    entries.push_back(entry);
                }
                auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
                    .layout = bind_group_layout,
                    .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
                    .label = "sky transmittance lut bind group",
                });
                if (!bind_group) {
                    return unexpected(graphics_error_from_rhi(bind_group.error(), "create sky transmittance lut bind group"));
                }
                { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

                RHI::ComputePassEncoder &pass = context.compute_pass();
                pass.set_pipeline(pipeline);
                pass.set_bind_group(0, *bind_group);
                pass.dispatch((kTransmittanceLutExtent.width + 7) / 8, (kTransmittanceLutExtent.height + 7) / 8, 1);
                return {};
            });

        graph.add_compute_pass("sky multi-scattering lut"_ustr)
            .add_sampled_texture(transmittance_lut)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = multi_scattering_lut, .read = false, .write = true})
            .set_execute([this, atmosphere_buffer, transmittance_lut, multi_scattering_lut, &transient_bind_groups](
                             RenderGraphComputeContext &context) -> Core::RendererResult {
                RHI::RhiDevice *device = rhi_device();
                if (device == nullptr) {
                    return unexpected(atmosphere_error("Cannot record atmosphere LUT bakes without an RHI device."));
                }
                vector<ReflectedResource> resources;
                RHI::BindGroupLayoutHandle bind_group_layout{};
                RHI::ComputePipelineHandle pipeline{};
                RHI::SamplerHandle lut_sampler{};
                {
                    auto guard = atmosphere_lut_.lock();
                    resources = collect_resource_bindings(guard->multi_scattering.shader.reflection());
                    bind_group_layout = guard->multi_scattering.bind_group_layout;
                    pipeline = guard->multi_scattering.pipeline;
                    lut_sampler = guard->lut_sampler;
                }
                vector<RHI::BindGroupEntry> entries;
                entries.reserve(resources.size());
                for (const ReflectedResource &resource : resources) {
                    RHI::BindGroupEntry entry{.binding = resource.binding};
                    if (resource.name == "atmosphereData") {
                        entry.buffer = atmosphere_buffer;
                        entry.size = sizeof(AtmosphereGpuData);
                    } else if (resource.name == "transmittanceLut") {
                        entry.texture_view = context.texture(transmittance_lut).default_view;
                    } else if (resource.name == "atmosphereSampler") {
                        entry.sampler = lut_sampler;
                    } else if (resource.name == "outputLut") {
                        entry.texture_view = context.texture(multi_scattering_lut).default_view;
                    } else {
                        return unexpected(atmosphere_error("Sky multi-scattering lut reflection contains an unknown resource: " + resource.name));
                    }
                    entries.push_back(entry);
                }
                auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
                    .layout = bind_group_layout,
                    .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
                    .label = "sky multi-scattering lut bind group",
                });
                if (!bind_group) {
                    return unexpected(graphics_error_from_rhi(bind_group.error(), "create sky multi-scattering lut bind group"));
                }
                { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

                RHI::ComputePassEncoder &pass = context.compute_pass();
                pass.set_pipeline(pipeline);
                pass.set_bind_group(0, *bind_group);
                pass.dispatch((kMultiScatteringLutExtent.width + 7) / 8, (kMultiScatteringLutExtent.height + 7) / 8, 1);
                return {};
            });

        graph.add_compute_pass("sky view lut"_ustr)
            .add_sampled_texture(transmittance_lut)
            .add_sampled_texture(multi_scattering_lut)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = sky_view_lut, .read = false, .write = true})
            .set_execute([this, atmosphere_buffer, transmittance_lut, multi_scattering_lut, sky_view_lut, &transient_bind_groups](
                             RenderGraphComputeContext &context) -> Core::RendererResult {
                RHI::RhiDevice *device = rhi_device();
                if (device == nullptr) {
                    return unexpected(atmosphere_error("Cannot record atmosphere LUT bakes without an RHI device."));
                }
                vector<ReflectedResource> resources;
                RHI::BindGroupLayoutHandle bind_group_layout{};
                RHI::ComputePipelineHandle pipeline{};
                RHI::SamplerHandle lut_sampler{};
                {
                    auto guard = atmosphere_lut_.lock();
                    resources = collect_resource_bindings(guard->sky_view.shader.reflection());
                    bind_group_layout = guard->sky_view.bind_group_layout;
                    pipeline = guard->sky_view.pipeline;
                    lut_sampler = guard->lut_sampler;
                }
                vector<RHI::BindGroupEntry> entries;
                entries.reserve(resources.size());
                for (const ReflectedResource &resource : resources) {
                    RHI::BindGroupEntry entry{.binding = resource.binding};
                    if (resource.name == "atmosphereData") {
                        entry.buffer = atmosphere_buffer;
                        entry.size = sizeof(AtmosphereGpuData);
                    } else if (resource.name == "transmittanceLut") {
                        entry.texture_view = context.texture(transmittance_lut).default_view;
                    } else if (resource.name == "multiScatteringLut") {
                        entry.texture_view = context.texture(multi_scattering_lut).default_view;
                    } else if (resource.name == "atmosphereSampler") {
                        entry.sampler = lut_sampler;
                    } else if (resource.name == "outputLut") {
                        entry.texture_view = context.texture(sky_view_lut).default_view;
                    } else {
                        return unexpected(atmosphere_error("Sky view lut reflection contains an unknown resource: " + resource.name));
                    }
                    entries.push_back(entry);
                }
                auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
                    .layout = bind_group_layout,
                    .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
                    .label = "sky view lut bind group",
                });
                if (!bind_group) {
                    return unexpected(graphics_error_from_rhi(bind_group.error(), "create sky view lut bind group"));
                }
                { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

                RHI::ComputePassEncoder &pass = context.compute_pass();
                pass.set_pipeline(pipeline);
                pass.set_bind_group(0, *bind_group);
                pass.dispatch((kSkyViewLutExtent.width + 7) / 8, (kSkyViewLutExtent.height + 7) / 8, 1);
                return {};
            });

        out_transmittance_lut = transmittance_lut;
        out_multi_scattering_lut = multi_scattering_lut;
        out_sky_view_lut = sky_view_lut;
        return {};
    }

} // namespace SFT::Renderer
