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
#include <iterator>
#include <limits>
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

    void destroy_text_frame_resources(RHI::RhiDevice &device, TextFrameResources &resources) noexcept {
        for (TextFrameResources::BindingCacheEntry &entry : resources.binding_cache) {
            for (const TextDrawBatch::BoundGroup &group : entry.bind_groups) {
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

        [[nodiscard]] bool same_glyph_instance(const GlyphInstance &lhs, const GlyphInstance &rhs) noexcept {
            return lhs.position.x == rhs.position.x && lhs.position.y == rhs.position.y &&
                   lhs.size.x == rhs.size.x && lhs.size.y == rhs.size.y &&
                   lhs.uv_min.x == rhs.uv_min.x && lhs.uv_min.y == rhs.uv_min.y &&
                   lhs.uv_max.x == rhs.uv_max.x && lhs.uv_max.y == rhs.uv_max.y &&
                   lhs.color.x == rhs.color.x && lhs.color.y == rhs.color.y &&
                   lhs.color.z == rhs.color.z && lhs.color.w == rhs.color.w &&
                   lhs.rotation == rhs.rotation && lhs.format_kind == rhs.format_kind &&
                   lhs.distance_pixel_range == rhs.distance_pixel_range &&
                   lhs.stem_darkening_px == rhs.stem_darkening_px;
        }

        // Matches Shaders/text_sdf.slang's `TextViewConstants` push-constant struct byte-for-byte.
        struct TextViewConstantsGpu {
            glm::vec2 viewport_size{0.0f};
            // Use an explicit storage-buffer base instead of relying on SV_InstanceID to include
            // DrawArgs::first_instance. Shader-language backends disagree on whether that semantic
            // is base-adjusted; the disagreement was masked while every run used one atlas image.
            u32 instance_index_base = 0;
            u32 padding = 0;
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

        // Derived from reflection rather than hand-written: `sizeof(TextViewConstantsGpu)` would
        // otherwise need to be kept in sync by hand with text_sdf.slang's `TextViewConstants` by
        // eye — see generate_push_constant_ranges' doc comment. Only vertexMain reads viewConstants
        // (the fragment stage never touches it), hence Vertex here.
        const vector<RHI::PushConstantRange> push_constant_ranges = generate_push_constant_ranges(reflection, RHI::ShaderStage::Vertex);
        if (push_constant_ranges.empty()) {
            pipeline.destroy(device);
            return unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed,
                "text_sdf shader reflection did not produce the expected viewConstants push-constant range.",
            });
        }
        auto pipeline_layout = device.create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{pipeline.bind_group_layouts_.data(), pipeline.bind_group_layouts_.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
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

    Core::RendererResult TextPipeline::prepare(RHI::RhiDevice &device, const TextAtlas &atlas,
                                               span<const GlyphInstance> instances, span<const GlyphSlot> slots,
                                               TextFrameResources &resources, vector<TextDrawBatch> &out_batches) {
        out_batches.clear();
        if (instances.size() != slots.size()) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "TextPipeline::prepare: instance and slot counts must match.");
        }
        if (instances.empty()) {
            return {};
        }

        // Only adjacent glyphs sharing an atlas tile are batched. Sorting transparent glyphs by
        // tile would change painter order when quads overlap, so correctness takes precedence over
        // merging non-contiguous batches.
        usize i = 0;
        while (i < slots.size()) {
            usize j = i + 1;
            while (j < slots.size() && slots[j].format == slots[i].format &&
                   slots[j].tile_index == slots[i].tile_index) {
                ++j;
            }
            out_batches.push_back(TextDrawBatch{
                .format = slots[i].format,
                .tile_index = slots[i].tile_index,
                .first_instance = static_cast<u32>(i),
                .instance_count = static_cast<u32>(j - i),
            });
            i = j;
        }

        const u64 required_bytes = static_cast<u64>(instances.size()) * sizeof(GlyphInstance);
        if (!resources.instance_buffer || resources.instance_capacity_bytes < required_bytes) {
            u64 new_capacity = std::max<u64>(sizeof(GlyphInstance) * 64u, resources.instance_capacity_bytes);
            while (new_capacity < required_bytes) {
                new_capacity = new_capacity > std::numeric_limits<u64>::max() / 2u ? required_bytes : new_capacity * 2u;
            }
            auto replacement = device.create_buffer(RHI::BufferDesc{
                .size = new_capacity,
                .usage = RHI::BufferUsage::Storage,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "persistent text instance buffer",
            });
            if (!replacement) {
                out_batches.clear();
                return unexpected(graphics_error_from_rhi(replacement.error(), "create persistent text instance buffer"));
            }
            // This resource slot's fence has retired before prepare() is called, so cached groups
            // and the old buffer are no longer referenced by the GPU and can be replaced now.
            destroy_text_frame_resources(device, resources);
            resources.instance_buffer = *replacement;
            resources.instance_capacity_bytes = new_capacity;
        }

        const bool instances_unchanged = resources.uploaded_instances.size() == instances.size() &&
            std::ranges::equal(resources.uploaded_instances, instances, same_glyph_instance);
        if (!instances_unchanged) {
            const auto *bytes = reinterpret_cast<const std::byte *>(instances.data());
            if (auto written = device.write_buffer(resources.instance_buffer, 0,
                                                   span<const std::byte>{bytes, static_cast<usize>(required_bytes)});
                !written) {
                out_batches.clear();
                return unexpected(graphics_error_from_rhi(written.error(), "write text instance buffer"));
            }
            resources.uploaded_instances.assign(instances.begin(), instances.end());
        }
        for (TextDrawBatch &batch : out_batches) {
            batch.instance_buffer = resources.instance_buffer;

            const RHI::TextureViewHandle atlas_view = atlas.tile_view(batch.format, batch.tile_index);
            if (!atlas_view) {
                out_batches.clear();
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Text batch references an unresident atlas image.");
            }
            auto cached = std::ranges::find_if(resources.binding_cache, [&](const auto &entry) {
                return entry.format == batch.format && entry.tile_index == batch.tile_index &&
                       entry.atlas_view == atlas_view;
            });
            if (cached == resources.binding_cache.end()) {
                // The only way the same logical tile acquires a different view is grow-only atlas
                // replacement. Retire its obsolete descriptor objects now that this frame slot is safe.
                for (auto it = resources.binding_cache.begin(); it != resources.binding_cache.end();) {
                    if (it->format == batch.format && it->tile_index == batch.tile_index) {
                        for (const TextDrawBatch::BoundGroup &group : it->bind_groups) {
                            if (group.handle) {
                                device.destroy_bind_group(group.handle);
                            }
                        }
                        it = resources.binding_cache.erase(it);
                    } else {
                        ++it;
                    }
                }

                TextFrameResources::BindingCacheEntry entry{
                    .format = batch.format,
                    .tile_index = batch.tile_index,
                    .atlas_view = atlas_view,
                };
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
                add_entry(texture_binding_, RHI::BindGroupEntry{.texture_view = atlas_view});
                add_entry(sampler_binding_, RHI::BindGroupEntry{.sampler = sampler_});
                for (const Group &group : groups) {
                    auto bind_group = device.create_bind_group(RHI::BindGroupDesc{
                        .layout = bind_group_layouts_[group.layout_index],
                        .entries = span<const RHI::BindGroupEntry>{group.entries.data(), group.entries.size()},
                        .label = "persistent text bind group",
                    });
                    if (!bind_group) {
                        for (const TextDrawBatch::BoundGroup &created : entry.bind_groups) {
                            device.destroy_bind_group(created.handle);
                        }
                        out_batches.clear();
                        return unexpected(graphics_error_from_rhi(bind_group.error(), "create persistent text bind group"));
                    }
                    entry.bind_groups.push_back(TextDrawBatch::BoundGroup{
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

    Core::RendererResult TextPipeline::draw(RHI::RenderPassEncoder &pass,
                                            span<const TextDrawBatch> batches, glm::vec2 viewport_size) {
        if (batches.empty()) {
            return {};
        }
        if (!pipeline_) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, "Text pipeline was not created.");
        }

        pass.set_pipeline(pipeline_);

        for (const TextDrawBatch &batch : batches) {
            if (!batch.instance_buffer) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Text draw batch has no prepared instance buffer.");
            }
            for (const TextDrawBatch::BoundGroup &group : batch.bind_groups) {
                if (!group.handle) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Text draw batch has an invalid persistent bind group.");
                }
                pass.set_bind_group(group.set, group.handle);
            }

            const TextViewConstantsGpu constants{
                .viewport_size = viewport_size,
                .instance_index_base = batch.first_instance,
            };
            pass.set_push_constants(
                RHI::ShaderStage::Vertex, 0,
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

    void TextPipeline::destroy(RHI::RhiDevice &device) noexcept {
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
        *this = TextPipeline{};
    }

} // namespace SFT::Renderer
