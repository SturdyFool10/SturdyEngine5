#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <glm/vec2.hpp>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#pragma endregion

#include <Renderer/TextInstance.hpp>
#include <Renderer/TextAtlas.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <RHI/RHI.hpp>
#include <Core/Core.hpp>
#include <Text/Text.hpp>

using std::array;
using std::span;
using std::string;
using std::string_view;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

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

        // Matches Shaders/text_sdf.slang's `TextViewConstants` push-constant struct byte-for-byte.
        struct TextViewConstantsGpu {
            glm::vec2 viewport_size{0.0f};
        };

    } // namespace

    Core::RendererExpected<TextPipeline> TextPipeline::create(RHI::RhiDevice &device, RHI::Format color_format) {
        const slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderCompiler compiler;
        auto shader = compiler.compile(slang::ShaderSource::from_file("Shaders/text_sdf.slang", "text_sdf"), options);
        if (!shader) {
            return unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed,
                "compile text_sdf shader failed: " + shader.error().message + "\n" + shader.error().diagnostics,
            });
        }

        TextPipeline pipeline;

        auto vertex_code = shader->entry_point_code("vertexMain");
        if (!vertex_code) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "generate text_sdf vertex bytecode failed: " + vertex_code.error().message});
        }
        auto vertex_module = device.create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{vertex_code->bytes.data(), vertex_code->bytes.size()},
            .label = "text vertex module",
        });
        if (!vertex_module) {
            return unexpected(graphics_error_from_rhi(vertex_module.error(), "create text vertex module"));
        }
        pipeline.vertex_module_ = *vertex_module;

        auto fragment_code = shader->entry_point_code("fragmentMain");
        if (!fragment_code) {
            pipeline.destroy(device);
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "generate text_sdf fragment bytecode failed: " + fragment_code.error().message});
        }
        auto fragment_module = device.create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{fragment_code->bytes.data(), fragment_code->bytes.size()},
            .label = "text fragment module",
        });
        if (!fragment_module) {
            pipeline.destroy(device);
            return unexpected(graphics_error_from_rhi(fragment_module.error(), "create text fragment module"));
        }
        pipeline.fragment_module_ = *fragment_module;

        const slang::ShaderReflection &reflection = shader->reflection();
        const RHI::ShaderStage stage_mask = reflected_stage_mask(reflection);
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, stage_mask);
        if (generated.empty()) {
            pipeline.destroy(device);
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "text_sdf shader produced no bind-group layout."});
        }
        for (const GeneratedBindGroupLayout &layout : generated) {
            auto handle = device.create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "text bind group layout",
            });
            if (!handle) {
                pipeline.destroy(device);
                return unexpected(graphics_error_from_rhi(handle.error(), "create text bind group layout"));
            }
            pipeline.bind_group_layouts_.push_back(*handle);
            pipeline.bind_group_layout_sets_.push_back(layout.set);
        }

        const vector<ReflectedResource> resources = collect_resource_bindings(reflection);
        auto resolve = [&](string_view name) -> ResourceBinding {
            for (const ReflectedResource &resource : resources) {
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
        pipeline.texture_binding_ = resolve("atlasTexture");
        pipeline.sampler_binding_ = resolve("atlasSampler");
        if (!pipeline.instances_binding_.found || !pipeline.texture_binding_.found || !pipeline.sampler_binding_.found) {
            pipeline.destroy(device);
            return unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed,
                "text_sdf shader reflection did not produce the expected instances/atlasTexture/atlasSampler bindings.",
            });
        }

        const RHI::PushConstantRange push_constant_range{
            .stages = RHI::ShaderStage::Vertex,
            .offset = 0,
            .size = sizeof(TextViewConstantsGpu),
        };
        auto pipeline_layout = device.create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{pipeline.bind_group_layouts_.data(), pipeline.bind_group_layouts_.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{&push_constant_range, 1},
            .label = "text pipeline layout",
        });
        if (!pipeline_layout) {
            pipeline.destroy(device);
            return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create text pipeline layout"));
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
            .label = "text atlas sampler",
        });
        if (!sampler) {
            pipeline.destroy(device);
            return unexpected(graphics_error_from_rhi(sampler.error(), "create text atlas sampler"));
        }
        pipeline.sampler_ = *sampler;

        const RHI::ColorTargetState color_target{
            .format = color_format,
            .blend_enable = true,
            .color = RHI::BlendComponent{.src_factor = RHI::BlendFactor::SrcAlpha, .dst_factor = RHI::BlendFactor::OneMinusSrcAlpha, .op = RHI::BlendOp::Add},
            .alpha = RHI::BlendComponent{.src_factor = RHI::BlendFactor::One, .dst_factor = RHI::BlendFactor::OneMinusSrcAlpha, .op = RHI::BlendOp::Add},
            .write_mask = RHI::ColorWriteMask::All,
        };
        // No vertex buffers (glyph quads are vertex-pulled from SV_VertexID/SV_InstanceID) and no
        // depth attachment — text composites straight over whatever's already in the target.
        const RHI::RenderPipelineDesc desc{
            .layout = pipeline.pipeline_layout_,
            .vertex = RHI::ShaderEntry{.module = pipeline.vertex_module_, .entry_point = "vertexMain", .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = pipeline.fragment_module_, .entry_point = "fragmentMain", .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&color_target, 1},
            .label = "text pipeline",
        };
        auto rhi_pipeline = device.create_render_pipeline(desc);
        if (!rhi_pipeline) {
            pipeline.destroy(device);
            return unexpected(graphics_error_from_rhi(rhi_pipeline.error(), "create text pipeline"));
        }
        pipeline.pipeline_ = *rhi_pipeline;

        return pipeline;
    }

    Core::RendererResult TextPipeline::prepare(RHI::RhiDevice &device, span<const GlyphInstance> instances,
                                               span<const GlyphSlot> slots, vector<TextDrawBatch> &out_batches,
                                               vector<RHI::BufferHandle> &out_transient_buffers) {
        out_batches.clear();
        if (instances.size() != slots.size()) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "TextPipeline::prepare: instance and slot counts must match.");
        }
        if (instances.empty()) {
            return {};
        }

        // Stable group-by (format, tile), so glyphs sharing a batch keep their relative submission
        // order (matters if translucent glyph quads overlap and rely on draw order for blending).
        vector<u32> order(instances.size());
        std::iota(order.begin(), order.end(), 0u);
        std::ranges::stable_sort(order, [&](u32 a, u32 b) {
            if (slots[a].format != slots[b].format) {
                return slots[a].format < slots[b].format;
            }
            return slots[a].tile_index < slots[b].tile_index;
        });

        vector<GlyphInstance> ordered(instances.size());
        for (usize i = 0; i < order.size(); ++i) {
            ordered[i] = instances[order[i]];
        }

        usize i = 0;
        while (i < order.size()) {
            usize j = i + 1;
            while (j < order.size() && slots[order[j]].format == slots[order[i]].format &&
                   slots[order[j]].tile_index == slots[order[i]].tile_index) {
                ++j;
            }
            out_batches.push_back(TextDrawBatch{
                .format = slots[order[i]].format,
                .tile_index = slots[order[i]].tile_index,
                .first_instance = static_cast<u32>(i),
                .instance_count = static_cast<u32>(j - i),
            });
            i = j;
        }

        const u64 required_bytes = static_cast<u64>(ordered.size()) * sizeof(GlyphInstance);
        if (required_bytes > instance_buffer_capacity_) {
            if (instance_buffer_) {
                // A prior frame's bind group may still be in flight on the GPU referencing this
                // buffer (bind groups are transient and only freed once their frame's fence
                // retires — see the `transient_bind_groups` convention in RendererLifecycle.cpp).
                // Handing it to the caller's fence-gated cleanup instead of destroying it here
                // keeps this a pure CPU-side buffer swap — no stall, fire-and-forget like the rest
                // of the frame.
                out_transient_buffers.push_back(instance_buffer_);
            }
            auto buffer = device.create_buffer(RHI::BufferDesc{
                .size = required_bytes,
                .usage = RHI::BufferUsage::Storage,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "text instance buffer",
            });
            if (!buffer) {
                instance_buffer_ = {};
                instance_buffer_capacity_ = 0;
                return unexpected(graphics_error_from_rhi(buffer.error(), "create text instance buffer"));
            }
            instance_buffer_ = *buffer;
            instance_buffer_capacity_ = required_bytes;
        }

        const auto *bytes = reinterpret_cast<const std::byte *>(ordered.data());
        if (auto written = device.write_buffer(instance_buffer_, 0, span<const std::byte>{bytes, static_cast<usize>(required_bytes)}); !written) {
            return unexpected(graphics_error_from_rhi(written.error(), "write text instance buffer"));
        }
        return {};
    }

    Core::RendererResult TextPipeline::draw(RHI::RhiDevice &device, RHI::RenderPassEncoder &pass, const TextAtlas &atlas,
                                            span<const TextDrawBatch> batches, glm::vec2 viewport_size,
                                            vector<RHI::BindGroupHandle> &transient_bind_groups) {
        if (batches.empty()) {
            return {};
        }
        if (!pipeline_) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, "Text pipeline was not created.");
        }

        pass.set_pipeline(pipeline_);
        const TextViewConstantsGpu constants{.viewport_size = viewport_size};
        pass.set_push_constants(RHI::ShaderStage::Vertex, 0,
                                span<const std::byte>{reinterpret_cast<const std::byte *>(&constants), sizeof(constants)});

        for (const TextDrawBatch &batch : batches) {
            const RHI::TextureViewHandle tile_view = atlas.tile_view(batch.format, batch.tile_index);
            if (!tile_view) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Text draw batch references an unresident atlas tile.");
            }
            // Group this batch's three resource bindings by which generated bind-group layout each
            // resolved to at create() time, so a shader that places them in one set (the common
            // case) or splits them across sets both work without hardcoding either shape.
            struct Group {
                usize layout_index = 0;
                vector<RHI::BindGroupEntry> entries;
            };
            vector<Group> groups;
            auto add_entry = [&](const ResourceBinding &binding, RHI::BindGroupEntry entry) {
                if (!binding.found) {
                    return;
                }
                entry.binding = binding.binding;
                for (Group &group : groups) {
                    if (group.layout_index == binding.layout_index) {
                        group.entries.push_back(entry);
                        return;
                    }
                }
                groups.push_back(Group{.layout_index = binding.layout_index, .entries = {entry}});
            };
            add_entry(instances_binding_, RHI::BindGroupEntry{.buffer = instance_buffer_, .offset = 0, .size = 0});
            add_entry(texture_binding_, RHI::BindGroupEntry{.texture_view = tile_view});
            add_entry(sampler_binding_, RHI::BindGroupEntry{.sampler = sampler_});

            for (const Group &group : groups) {
                auto bind_group = device.create_bind_group(RHI::BindGroupDesc{
                    .layout = bind_group_layouts_[group.layout_index],
                    .entries = span<const RHI::BindGroupEntry>{group.entries.data(), group.entries.size()},
                    .label = "text bind group",
                });
                if (!bind_group) {
                    return unexpected(graphics_error_from_rhi(bind_group.error(), "create text bind group"));
                }
                transient_bind_groups.push_back(*bind_group);
                pass.set_bind_group(bind_group_layout_sets_[group.layout_index], *bind_group);
            }

            pass.draw(RHI::DrawArgs{
                .vertex_count = 6,
                .instance_count = batch.instance_count,
                .first_vertex = 0,
                .first_instance = batch.first_instance,
            });
        }
        return {};
    }

    void TextPipeline::destroy(RHI::RhiDevice &device) noexcept {
        if (pipeline_) {
            device.destroy_render_pipeline(pipeline_);
        }
        if (sampler_) {
            device.destroy_sampler(sampler_);
        }
        if (instance_buffer_) {
            device.destroy_buffer(instance_buffer_);
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
        *this = TextPipeline{};
    }

} // namespace SFT::Renderer
