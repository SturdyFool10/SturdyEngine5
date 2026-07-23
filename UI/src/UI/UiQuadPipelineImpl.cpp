#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#pragma endregion

#include "UiQuadPipeline.hpp"

#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/TileGrid.hpp>

using std::string;
using std::string_view;
using std::unexpected;

namespace SFT::UI {

    void destroy_ui_quad_frame_resources(RHI::RhiDevice &device, UiQuadFrameResources &resources) noexcept {
        for (UiQuadFrameResources::BindingCacheEntry &entry : resources.binding_cache) {
            for (const UiQuadDrawBatch::BoundGroup &group : entry.bind_groups) {
                if (group.handle) {
                    device.destroy_bind_group(group.handle);
                }
            }
        }
        if (resources.instance_buffer) {
            device.destroy_buffer(resources.instance_buffer);
        }
        resources = {};
    }

    namespace {
        namespace slang = Core::Slang;

        [[nodiscard]] usize bind_group_layout_index_for_set(span<const u32> sets, u32 set) noexcept {
            for (usize i = 0; i < sets.size(); ++i) {
                if (sets[i] == set) {
                    return i;
                }
            }
            return sets.size();
        }

        [[nodiscard]] bool same_instance(const UiQuadInstance &lhs, const UiQuadInstance &rhs) noexcept {
            return std::memcmp(&lhs, &rhs, sizeof(UiQuadInstance)) == 0;
        }

        [[nodiscard]] bool same_rect(const RHI::Rect2D &lhs, const RHI::Rect2D &rhs) noexcept {
            return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
        }

        // Matches Shaders/ui_quad.slang's `UiViewConstants` push-constant struct byte-for-byte.
        struct UiViewConstantsGpu {
            glm::vec2 viewport_size{0.0f};
            u32 instance_index_base = 0;
            u32 padding = 0;
        };

        [[nodiscard]] Core::GraphicsBackendError ui_quad_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

    } // namespace

    Core::RendererExpected<UiQuadPipeline> UiQuadPipeline::create(RHI::RhiDevice &device, RHI::Format color_format) {
        const slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderCompiler compiler;
        auto shader = compiler.compile(slang::ShaderSource::from_file("Shaders/ui_quad.slang", "ui_quad"), options);
        if (!shader) {
            return unexpected(ui_quad_error("compile ui_quad shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        }

        UiQuadPipeline pipeline;

        auto vertex_code = shader->entry_point_code("vertexMain");
        if (!vertex_code) {
            return unexpected(ui_quad_error("generate ui_quad vertex bytecode failed: " + vertex_code.error().message));
        }
        auto vertex_module = device.create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{vertex_code->bytes.data(), vertex_code->bytes.size()},
            .label = "ui quad vertex module",
        });
        if (!vertex_module) {
            return unexpected(Renderer::graphics_error_from_rhi(vertex_module.error(), "create ui quad vertex module"));
        }
        pipeline.vertex_module_ = *vertex_module;

        auto fragment_code = shader->entry_point_code("fragmentMain");
        if (!fragment_code) {
            pipeline.destroy(device);
            return unexpected(ui_quad_error("generate ui_quad fragment bytecode failed: " + fragment_code.error().message));
        }
        auto fragment_module = device.create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{fragment_code->bytes.data(), fragment_code->bytes.size()},
            .label = "ui quad fragment module",
        });
        if (!fragment_module) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(fragment_module.error(), "create ui quad fragment module"));
        }
        pipeline.fragment_module_ = *fragment_module;

        const slang::ShaderReflection &reflection = shader->reflection();
        const RHI::ShaderStage stage_mask = Renderer::reflected_stage_mask(reflection);
        const vector<Renderer::GeneratedBindGroupLayout> generated = Renderer::generate_bind_group_layouts(reflection, stage_mask);
        if (generated.empty()) {
            pipeline.destroy(device);
            return unexpected(ui_quad_error("ui_quad shader produced no bind-group layout."));
        }
        for (const Renderer::GeneratedBindGroupLayout &layout : generated) {
            auto handle = device.create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "ui quad bind group layout",
            });
            if (!handle) {
                pipeline.destroy(device);
                return unexpected(Renderer::graphics_error_from_rhi(handle.error(), "create ui quad bind group layout"));
            }
            pipeline.bind_group_layouts_.push_back(*handle);
            pipeline.bind_group_layout_sets_.push_back(layout.set);
        }

        const vector<Renderer::ReflectedResource> resources = Renderer::collect_resource_bindings(reflection);
        auto resolve = [&](string_view name) -> ResourceBinding {
            for (const Renderer::ReflectedResource &resource : resources) {
                if (resource.name == name) {
                    const usize index = bind_group_layout_index_for_set(pipeline.bind_group_layout_sets_, resource.set);
                    if (index < pipeline.bind_group_layouts_.size()) {
                        return ResourceBinding{.layout_index = index, .binding = resource.binding, .found = true};
                    }
                }
            }
            return ResourceBinding{};
        };
        pipeline.instances_binding_ = resolve("instances");
        pipeline.texture_binding_ = resolve("quadTexture");
        pipeline.sampler_binding_ = resolve("quadSampler");
        if (!pipeline.instances_binding_.found || !pipeline.texture_binding_.found || !pipeline.sampler_binding_.found) {
            pipeline.destroy(device);
            return unexpected(ui_quad_error(
                "ui_quad shader reflection did not produce the expected instances/quadTexture/quadSampler bindings."));
        }

        const vector<RHI::PushConstantRange> push_constant_ranges = Renderer::generate_push_constant_ranges(reflection, RHI::ShaderStage::Vertex);
        if (push_constant_ranges.empty()) {
            pipeline.destroy(device);
            return unexpected(ui_quad_error("ui_quad shader reflection did not produce the expected viewConstants push-constant range."));
        }
        auto pipeline_layout = device.create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{pipeline.bind_group_layouts_.data(), pipeline.bind_group_layouts_.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
            .label = "ui quad pipeline layout",
        });
        if (!pipeline_layout) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(pipeline_layout.error(), "create ui quad pipeline layout"));
        }
        pipeline.pipeline_layout_ = *pipeline_layout;

        auto sampler = device.create_sampler(RHI::SamplerDesc{
            .min_filter = RHI::Filter::Linear,
            .mag_filter = RHI::Filter::Linear,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .label = "ui quad sampler",
        });
        if (!sampler) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(sampler.error(), "create ui quad sampler"));
        }
        pipeline.sampler_ = *sampler;

        const RHI::ColorTargetState color_target{
            .format = color_format,
            .blend_enable = true,
            .color = RHI::BlendComponent{.src_factor = RHI::BlendFactor::SrcAlpha, .dst_factor = RHI::BlendFactor::OneMinusSrcAlpha, .op = RHI::BlendOp::Add},
            .alpha = RHI::BlendComponent{.src_factor = RHI::BlendFactor::One, .dst_factor = RHI::BlendFactor::OneMinusSrcAlpha, .op = RHI::BlendOp::Add},
            .write_mask = RHI::ColorWriteMask::All,
        };
        const RHI::RenderPipelineDesc desc{
            .layout = pipeline.pipeline_layout_,
            .vertex = RHI::ShaderEntry{.module = pipeline.vertex_module_, .entry_point = "vertexMain", .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = pipeline.fragment_module_, .entry_point = "fragmentMain", .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&color_target, 1},
            .label = "ui quad pipeline",
        };
        auto rhi_pipeline = device.create_render_pipeline(desc);
        if (!rhi_pipeline) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(rhi_pipeline.error(), "create ui quad pipeline"));
        }
        pipeline.pipeline_ = *rhi_pipeline;

        return pipeline;
    }

    Core::RendererResult UiQuadPipeline::prepare(RHI::RhiDevice &device, span<const UiQuadInstance> instances,
                                                 span<const RHI::TextureViewHandle> instance_texture_views,
                                                 span<const RHI::Rect2D> instance_scissors,
                                                 UiQuadFrameResources &resources, vector<UiQuadDrawBatch> &out_batches) {
        out_batches.clear();
        if (instances.size() != instance_texture_views.size() || instances.size() != instance_scissors.size()) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "UiQuadPipeline::prepare: instance/texture-view/scissor counts must match.");
        }
        if (instances.empty()) {
            return {};
        }

        usize i = 0;
        while (i < instances.size()) {
            usize j = i + 1;
            while (j < instances.size() && instance_texture_views[j] == instance_texture_views[i] &&
                   same_rect(instance_scissors[j], instance_scissors[i])) {
                ++j;
            }
            out_batches.push_back(UiQuadDrawBatch{
                .texture_view = instance_texture_views[i],
                .scissor = instance_scissors[i],
                .first_instance = static_cast<u32>(i),
                .instance_count = static_cast<u32>(j - i),
            });
            i = j;
        }

        const u64 required_bytes = static_cast<u64>(instances.size()) * sizeof(UiQuadInstance);
        if (!resources.instance_buffer || resources.instance_capacity_bytes < required_bytes) {
            u64 new_capacity = std::max<u64>(sizeof(UiQuadInstance) * 64u, resources.instance_capacity_bytes);
            while (new_capacity < required_bytes) {
                new_capacity = new_capacity > std::numeric_limits<u64>::max() / 2u ? required_bytes : new_capacity * 2u;
            }
            auto replacement = device.create_buffer(RHI::BufferDesc{
                .size = new_capacity,
                .usage = RHI::BufferUsage::Storage,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "persistent ui quad instance buffer",
            });
            if (!replacement) {
                out_batches.clear();
                return unexpected(Renderer::graphics_error_from_rhi(replacement.error(), "create persistent ui quad instance buffer"));
            }
            destroy_ui_quad_frame_resources(device, resources);
            resources.instance_buffer = *replacement;
            resources.instance_capacity_bytes = new_capacity;
        }

        const bool instances_unchanged = resources.uploaded_instances.size() == instances.size() &&
            std::ranges::equal(resources.uploaded_instances, instances, same_instance);
        if (!instances_unchanged) {
            const auto *bytes = reinterpret_cast<const std::byte *>(instances.data());
            if (auto written = device.write_buffer(resources.instance_buffer, 0,
                                                   span<const std::byte>{bytes, static_cast<usize>(required_bytes)});
                !written) {
                out_batches.clear();
                return unexpected(Renderer::graphics_error_from_rhi(written.error(), "write ui quad instance buffer"));
            }
            resources.uploaded_instances.assign(instances.begin(), instances.end());
        }

        for (UiQuadDrawBatch &batch : out_batches) {
            batch.instance_buffer = resources.instance_buffer;
            if (!batch.texture_view) {
                out_batches.clear();
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "UI quad batch references an invalid texture view.");
            }
            auto cached = std::ranges::find_if(resources.binding_cache,
                                               [&](const auto &entry) { return entry.texture_view == batch.texture_view; });
            if (cached == resources.binding_cache.end()) {
                UiQuadFrameResources::BindingCacheEntry entry{.texture_view = batch.texture_view};
                struct Group {
                    usize layout_index = 0;
                    vector<RHI::BindGroupEntry> entries;
                };
                vector<Group> groups;
                auto add_entry = [&](const ResourceBinding &binding, RHI::BindGroupEntry value) {
                    value.binding = binding.binding;
                    auto group = std::ranges::find(groups, binding.layout_index, &Group::layout_index);
                    if (group == groups.end()) {
                        groups.push_back(Group{.layout_index = binding.layout_index});
                        group = std::prev(groups.end());
                    }
                    group->entries.push_back(value);
                };
                add_entry(instances_binding_, RHI::BindGroupEntry{.buffer = resources.instance_buffer, .offset = 0, .size = 0});
                add_entry(texture_binding_, RHI::BindGroupEntry{.texture_view = batch.texture_view});
                add_entry(sampler_binding_, RHI::BindGroupEntry{.sampler = sampler_});
                for (const Group &group : groups) {
                    auto bind_group = device.create_bind_group(RHI::BindGroupDesc{
                        .layout = bind_group_layouts_[group.layout_index],
                        .entries = span<const RHI::BindGroupEntry>{group.entries.data(), group.entries.size()},
                        .label = "persistent ui quad bind group",
                    });
                    if (!bind_group) {
                        for (const UiQuadDrawBatch::BoundGroup &created : entry.bind_groups) {
                            device.destroy_bind_group(created.handle);
                        }
                        out_batches.clear();
                        return unexpected(Renderer::graphics_error_from_rhi(bind_group.error(), "create persistent ui quad bind group"));
                    }
                    entry.bind_groups.push_back(UiQuadDrawBatch::BoundGroup{
                        .set = bind_group_layout_sets_[group.layout_index],
                        .handle = *bind_group,
                    });
                }
                resources.binding_cache.push_back(std::move(entry));
                cached = std::prev(resources.binding_cache.end());
            }
            batch.bind_groups = cached->bind_groups;
        }
        return {};
    }

    Core::RendererResult UiQuadPipeline::draw(RHI::RenderPassEncoder &pass, span<const UiQuadDrawBatch> batches, glm::vec2 viewport_size) {
        if (batches.empty()) {
            return {};
        }
        if (!pipeline_) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, "UI quad pipeline was not created.");
        }

        pass.set_pipeline(pipeline_);

        for (const UiQuadDrawBatch &batch : batches) {
            if (!batch.instance_buffer) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "UI quad draw batch has no prepared instance buffer.");
            }
            pass.set_scissor(batch.scissor);
            for (const UiQuadDrawBatch::BoundGroup &group : batch.bind_groups) {
                if (!group.handle) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "UI quad draw batch has an invalid persistent bind group.");
                }
                pass.set_bind_group(group.set, group.handle);
            }

            const UiViewConstantsGpu constants{.viewport_size = viewport_size, .instance_index_base = batch.first_instance};
            pass.set_push_constants(RHI::ShaderStage::Vertex, 0,
                                    span<const std::byte>{reinterpret_cast<const std::byte *>(&constants), sizeof(constants)});

            pass.draw(RHI::DrawArgs{
                .vertex_count = 6,
                .instance_count = batch.instance_count,
                .first_vertex = 0,
                .first_instance = 0,
            });
        }
        return {};
    }

    void UiQuadPipeline::destroy(RHI::RhiDevice &device) noexcept {
        if (pipeline_) {
            device.destroy_render_pipeline(pipeline_);
        }
        if (sampler_) {
            device.destroy_sampler(sampler_);
        }
        if (pipeline_layout_) {
            device.destroy_pipeline_layout(pipeline_layout_);
        }
        for (RHI::BindGroupLayoutHandle layout : bind_group_layouts_) {
            device.destroy_bind_group_layout(layout);
        }
        if (fragment_module_) {
            device.destroy_shader_module(fragment_module_);
        }
        if (vertex_module_) {
            device.destroy_shader_module(vertex_module_);
        }
        *this = UiQuadPipeline{};
    }

} // namespace SFT::UI
