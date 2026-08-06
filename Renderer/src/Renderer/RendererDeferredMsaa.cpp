#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>
#pragma endregion

#include <Renderer/RendererModule.hpp>
#include <Renderer/ReflectionBinding.hpp>
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

        struct DeferredMsaaConstants {
            u32 width = 1;
            u32 height = 1;
            u32 sample_count = 1;
            f32 near_plane = 0.01f;
            f32 far_plane = 1000.0f;
            f32 depth_falloff = 128.0f;
            f32 spatial_falloff = 2.0f;
            f32 same_surface_threshold = 0.01f;
        };
        static_assert(sizeof(DeferredMsaaConstants) == 32);

        [[nodiscard]] Core::GraphicsBackendError deferred_msaa_error(string message) {
            return Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed,
                std::move(message),
            };
        }
    } // namespace

    Core::RendererResult Renderer::ensure_deferred_msaa_resources() {
        ZoneScopedN("Renderer::ensure_deferred_msaa_resources");
        auto guard = deferred_msaa_.lock();
        if (guard->ready) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(deferred_msaa_error("Cannot build deferred MSAA resources without an RHI device."));
        }

        const slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderVariantCache shader_cache{
            slang::ShaderSource::from_file("Shaders/deferred_msaa_reconstruction.slang", "deferred_msaa_reconstruction"),
            options,
            slang::ShaderCompiler{},
            recovery_create_info_.enable_shader_disk_cache};
        auto shader = shader_cache.get_or_compile_base();
        if (!shader) {
            return unexpected(deferred_msaa_error(
                "compile deferred MSAA reconstruction shader failed: " + shader.error().message +
                "\n" + shader.error().diagnostics));
        }
        guard->shader = *shader;
        guard->vertex_entry_point = "vertexMain";
        guard->fragment_entry_point = "fragmentMain";

        auto create_module = [&](const string &entry_point, const char *label)
            -> Core::RendererExpected<RHI::ShaderModuleHandle> {
            auto code = guard->shader.entry_point_code(entry_point);
            if (!code) {
                return unexpected(deferred_msaa_error(
                    "generate deferred MSAA shader bytecode failed: " + code.error().message));
            }
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = RHI::ShaderLanguage::SpirV,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = label,
            });
            if (!module) {
                return unexpected(graphics_error_from_rhi(module.error(), label));
            }
            return *module;
        };

        auto vertex_module = create_module(guard->vertex_entry_point, "deferred MSAA reconstruction vertex module");
        if (!vertex_module) {
            return unexpected(vertex_module.error());
        }
        guard->vertex_module = *vertex_module;
        auto fragment_module = create_module(guard->fragment_entry_point, "deferred MSAA reconstruction fragment module");
        if (!fragment_module) {
            destroy_deferred_msaa_resources_locked(*guard);
            return unexpected(fragment_module.error());
        }
        guard->fragment_module = *fragment_module;

        const slang::ShaderReflection &reflection = guard->shader.reflection();
        const vector<GeneratedBindGroupLayout> generated =
            generate_bind_group_layouts(reflection, reflected_stage_mask(reflection));
        for (const GeneratedBindGroupLayout &layout : generated) {
            auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "deferred MSAA reconstruction bind group layout",
            });
            if (!handle) {
                destroy_deferred_msaa_resources_locked(*guard);
                return unexpected(graphics_error_from_rhi(
                    handle.error(), "create deferred MSAA reconstruction bind group layout"));
            }
            guard->bind_group_layouts.push_back(*handle);
            guard->bind_group_layout_sets.push_back(layout.set);

            array<u32, 3> sampled_bindings{};
            usize sampled_count = 0;
            for (const RHI::BindGroupLayoutEntry &entry : layout.entries) {
                if (entry.type == RHI::BindingType::SampledTexture && sampled_count < sampled_bindings.size()) {
                    sampled_bindings[sampled_count++] = entry.binding;
                }
            }
            if (!guard->sampled_layout && sampled_count == sampled_bindings.size()) {
                guard->sampled_layout = *handle;
                guard->sampled_set = layout.set;
                guard->color_binding = sampled_bindings[0];
                guard->depth_binding = sampled_bindings[1];
                guard->geometry_depth_binding = sampled_bindings[2];
            }
        }
        if (!guard->sampled_layout) {
            destroy_deferred_msaa_resources_locked(*guard);
            return unexpected(deferred_msaa_error(
                "deferred MSAA shader reflection did not produce three sampled textures in one bind group."));
        }

        const vector<RHI::PushConstantRange> push_constant_ranges =
            generate_push_constant_ranges(reflection, RHI::ShaderStage::Fragment);
        if (push_constant_ranges.empty()) {
            destroy_deferred_msaa_resources_locked(*guard);
            return unexpected(deferred_msaa_error(
                "deferred MSAA shader produced no fragment push-constant range."));
        }
        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{
                guard->bind_group_layouts.data(), guard->bind_group_layouts.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{
                push_constant_ranges.data(), push_constant_ranges.size()},
            .label = "deferred MSAA reconstruction pipeline layout",
        });
        if (!pipeline_layout) {
            destroy_deferred_msaa_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(
                pipeline_layout.error(), "create deferred MSAA reconstruction pipeline layout"));
        }
        guard->pipeline_layout = *pipeline_layout;
        guard->shader.release_compiler_state();
        guard->ready = true;
        return {};
    }

    Core::RendererExpected<RHI::RenderPipelineHandle> Renderer::deferred_msaa_pipeline_for(
        RHI::Format color_format) {
        ZoneScopedN("Renderer::deferred_msaa_pipeline_for");
        if (Core::RendererResult ready = ensure_deferred_msaa_resources(); !ready) {
            return unexpected(ready.error());
        }

        auto guard = deferred_msaa_.lock();
        for (const DeferredMsaaPipelineVariant &variant : guard->pipeline_variants) {
            if (variant.color_format == color_format) {
                return variant.pipeline;
            }
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(deferred_msaa_error(
                "Cannot build a deferred MSAA reconstruction pipeline without an RHI device."));
        }
        const RHI::ColorTargetState color_target{
            .format = color_format,
            .blend_enable = false,
            .write_mask = RHI::ColorWriteMask::All,
        };
        auto pipeline = device->create_render_pipeline(RHI::RenderPipelineDesc{
            .layout = guard->pipeline_layout,
            .vertex = RHI::ShaderEntry{
                .module = guard->vertex_module,
                .entry_point = guard->vertex_entry_point.c_str(),
                .stage = RHI::ShaderStage::Vertex,
            },
            .fragment = RHI::ShaderEntry{
                .module = guard->fragment_module,
                .entry_point = guard->fragment_entry_point.c_str(),
                .stage = RHI::ShaderStage::Fragment,
            },
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&color_target, 1},
            .label = "deferred MSAA reconstruction pipeline",
        });
        if (!pipeline) {
            return unexpected(graphics_error_from_rhi(
                pipeline.error(), "create deferred MSAA reconstruction pipeline"));
        }
        guard->pipeline_variants.push_back(DeferredMsaaPipelineVariant{
            .color_format = color_format,
            .pipeline = *pipeline,
        });
        return *pipeline;
    }

    Core::RendererResult Renderer::record_deferred_msaa_reconstruction(
        RHI::RenderPassEncoder &pass,
        RHI::TextureViewHandle color_view,
        RHI::TextureViewHandle depth_view,
        RHI::TextureViewHandle geometry_depth_view,
        RHI::Format color_format,
        Core::Extent2D extent,
        RHI::SampleCount samples,
        f32 near_plane,
        f32 far_plane,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_deferred_msaa_reconstruction");
        auto pipeline = deferred_msaa_pipeline_for(color_format);
        if (!pipeline) {
            return unexpected(pipeline.error());
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !color_view || !depth_view || !geometry_depth_view) {
            return unexpected(deferred_msaa_error(
                "Cannot record deferred MSAA reconstruction without all three source textures."));
        }

        u32 color_binding = 0;
        u32 depth_binding = 0;
        u32 geometry_depth_binding = 0;
        RHI::BindGroupLayoutHandle sampled_layout{};
        u32 sampled_set = 0;
        {
            auto guard = deferred_msaa_.lock();
            color_binding = guard->color_binding;
            depth_binding = guard->depth_binding;
            geometry_depth_binding = guard->geometry_depth_binding;
            sampled_layout = guard->sampled_layout;
            sampled_set = guard->sampled_set;
        }
        const array<RHI::BindGroupEntry, 3> entries{
            RHI::BindGroupEntry{.binding = color_binding, .texture_view = color_view},
            RHI::BindGroupEntry{.binding = depth_binding, .texture_view = depth_view},
            RHI::BindGroupEntry{.binding = geometry_depth_binding, .texture_view = geometry_depth_view},
        };
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = sampled_layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .label = "deferred MSAA reconstruction bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(
                bind_group.error(), "create deferred MSAA reconstruction bind group"));
        }
        {
            auto transient_guard = transient_bind_groups_lock_.lock();
            transient_bind_groups.push_back(*bind_group);
        }

        const f32 safe_near = std::max(near_plane, 1.0e-4f);
        const DeferredMsaaConstants constants{
            .width = std::max(extent.width, 1u),
            .height = std::max(extent.height, 1u),
            .sample_count = static_cast<u32>(samples),
            .near_plane = safe_near,
            .far_plane = std::max(far_plane, safe_near + 1.0e-3f),
        };
        pass.set_pipeline(*pipeline);
        pass.set_bind_group(sampled_set, *bind_group);
        pass.set_push_constants(
            RHI::ShaderStage::Fragment, 0,
            std::as_bytes(span<const DeferredMsaaConstants>{&constants, 1}));
        pass.draw(RHI::DrawArgs{.vertex_count = 3});
        return {};
    }

    void Renderer::destroy_deferred_msaa_resources() noexcept {
        ZoneScopedN("Renderer::destroy_deferred_msaa_resources");
        auto guard = deferred_msaa_.lock();
        destroy_deferred_msaa_resources_locked(*guard);
    }

    void Renderer::destroy_deferred_msaa_resources_locked(DeferredMsaaResources &resources) noexcept {
        ZoneScopedN("Renderer::destroy_deferred_msaa_resources_locked");
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            for (const DeferredMsaaPipelineVariant &variant : resources.pipeline_variants) {
                if (variant.pipeline) {
                    device->destroy_render_pipeline(variant.pipeline);
                }
            }
            if (resources.pipeline_layout) {
                device->destroy_pipeline_layout(resources.pipeline_layout);
            }
            for (RHI::BindGroupLayoutHandle layout : resources.bind_group_layouts) {
                device->destroy_bind_group_layout(layout);
            }
            if (resources.fragment_module) {
                device->destroy_shader_module(resources.fragment_module);
            }
            if (resources.vertex_module) {
                device->destroy_shader_module(resources.vertex_module);
            }
        }
        resources = {};
    }

} // namespace SFT::Renderer
