#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <Async/Async.hpp>
#pragma endregion

#include <Renderer/RendererModule.hpp>
#include <Renderer/Scene.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

using std::array;
using std::chrono::steady_clock;
using std::optional;
using std::span;
using std::string;
using std::string_view;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {

        namespace slang = Core::Slang;

        [[nodiscard]] Core::GraphicsBackendError material_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        // A GraphicsBackendError carrying a Slang compile failure's message + diagnostics (the Slang error
        // dump is where the actual cause lives, so it must be surfaced, not just the summary line).
        [[nodiscard]] Core::GraphicsBackendError material_shader_error(const slang::ShaderError &error, const char *operation) {
            string message = string(operation) + " failed: " + error.message;
            if (!error.diagnostics.empty()) {
                message += "\n";
                message += error.diagnostics;
            }
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        // Whether two templates expose the same binding/parameter *layout* — the test for a hot-reload
        // that only tweaked shader logic (fast path: swap modules, keep instance UBOs/bind groups' shape)
        // versus one that changed the interface (slow path: rebuild instances). Compares CPU-side
        // descriptors only, never GPU handles (which always differ across a rebuild).
        [[nodiscard]] bool material_template_layout_compatible(const MaterialTemplateResource &a, const MaterialTemplateResource &b) {
            if (a.has_uniform_block != b.has_uniform_block || a.uniform_block_size != b.uniform_block_size ||
                a.uniform_set != b.uniform_set || a.uniform_binding != b.uniform_binding ||
                a.bind_group_layout_sets != b.bind_group_layout_sets ||
                a.parameters.size() != b.parameters.size() || a.texture_slots.size() != b.texture_slots.size()) {
                return false;
            }
            for (usize i = 0; i < a.parameters.size(); ++i) {
                if (a.parameters[i].name != b.parameters[i].name || a.parameters[i].offset != b.parameters[i].offset ||
                    a.parameters[i].size != b.parameters[i].size) {
                    return false;
                }
            }
            for (usize i = 0; i < a.texture_slots.size(); ++i) {
                if (a.texture_slots[i].name != b.texture_slots[i].name || a.texture_slots[i].set != b.texture_slots[i].set ||
                    a.texture_slots[i].binding != b.texture_slots[i].binding) {
                    return false;
                }
            }
            return true;
        }

        // Whether two paths point at the same file on disk. Uses std::filesystem::equivalent (resolves
        // `./`, symlinks, and differing-but-equivalent spellings); on any error (a path that doesn't
        // exist) falls back to comparing filenames, which is enough to match a watched `Shaders/x.slang`
        // against a template's stored source path.
        [[nodiscard]] bool same_shader_file(std::string_view a, std::string_view b) {
            namespace fs = std::filesystem;
            std::error_code ec;
            if (!a.empty() && !b.empty() && fs::equivalent(fs::path{a}, fs::path{b}, ec) && !ec) {
                return true;
            }
            return !a.empty() && fs::path{a}.filename() == fs::path{b}.filename();
        }

        // Find the first entry point matching `stage`; returns its reflected name or empty.
        [[nodiscard]] optional<string> entry_point_name(const slang::ShaderReflection &reflection, slang::ShaderStage stage) {
            for (const slang::ShaderEntryPointReflection &entry : reflection.entry_points) {
                if (entry.stage == stage) {
                    return entry.name;
                }
            }
            return std::nullopt;
        }

        // The set/binding of the shader's default (global) constant buffer, if it has one — scanned from
        // the descriptor sets so it matches what generate_bind_group_layouts() emitted.
        struct UniformLocation {
            bool present = false;
            u32 set = 0;
            u32 binding = 0;
        };
        [[nodiscard]] UniformLocation find_uniform_location(const slang::ShaderReflection &reflection) {
            if (reflection.global_constant_buffer_size == 0 ||
                reflection.global_constant_buffer_size == slang::shader_unbounded_size ||
                reflection.global_constant_buffer_size == slang::shader_unknown_size) {
                return {};
            }
            for (const slang::ShaderDescriptorSetReflection &set : reflection.descriptor_sets) {
                for (const slang::ShaderDescriptorRangeReflection &range : set.ranges) {
                    if (range.type == slang::ShaderBindingType::ConstantBuffer) {
                        return UniformLocation{.present = true, .set = set.space, .binding = range.binding};
                    }
                }
            }
            return UniformLocation{.present = true, .set = 0, .binding = reflection.global_constant_buffer_binding};
        }

        // The GeometryVertex input layout every material pipeline binds (position/normal/uv/color).
        constexpr array<RHI::VertexAttribute, 4> geometry_vertex_attributes() {
            return {
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x3, .offset = offsetof(GeometryVertex, position), .shader_location = 0},
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x3, .offset = offsetof(GeometryVertex, normal), .shader_location = 1},
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x2, .offset = offsetof(GeometryVertex, uv), .shader_location = 2},
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x4, .offset = offsetof(GeometryVertex, color), .shader_location = 3},
            };
        }

    } // namespace

    Core::RendererResult Renderer::build_material_template_gpu(MaterialTemplateResource &resource,
                                                               const Core::Slang::Shader &shader) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(material_error("Cannot build a material template without an RHI device."));
        }
        if (!shader) {
            return unexpected(material_error("Cannot build a material template from an empty shader."));
        }

        const slang::ShaderReflection &reflection = shader.reflection();

        // — Shader modules (vertex required, fragment optional) —
        const optional<string> vertex_name = entry_point_name(reflection, slang::ShaderStage::Vertex);
        if (!vertex_name) {
            return unexpected(material_error("Material shader has no vertex entry point."));
        }
        resource.vertex_entry_point = *vertex_name;
        auto vertex_code = shader.entry_point_code(resource.vertex_entry_point);
        if (!vertex_code) {
            return unexpected(material_error("Failed to generate material vertex bytecode: " + vertex_code.error().message));
        }
        auto vertex_module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{vertex_code->bytes.data(), vertex_code->bytes.size()},
            .label = "material vertex module",
        });
        if (!vertex_module) {
            return unexpected(graphics_error_from_rhi(vertex_module.error(), "create material vertex module"));
        }
        resource.vertex_module = *vertex_module;

        if (const optional<string> fragment_name = entry_point_name(reflection, slang::ShaderStage::Fragment)) {
            resource.fragment_entry_point = *fragment_name;
            auto fragment_code = shader.entry_point_code(resource.fragment_entry_point);
            if (!fragment_code) {
                destroy_material_template_gpu(resource);
                return unexpected(material_error("Failed to generate material fragment bytecode: " + fragment_code.error().message));
            }
            auto fragment_module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = RHI::ShaderLanguage::SpirV,
                .code = span<const std::byte>{fragment_code->bytes.data(), fragment_code->bytes.size()},
                .label = "material fragment module",
            });
            if (!fragment_module) {
                destroy_material_template_gpu(resource);
                return unexpected(graphics_error_from_rhi(fragment_module.error(), "create material fragment module"));
            }
            resource.fragment_module = *fragment_module;
            resource.has_fragment = true;
        }

        // — Reflection-derived bind-group + pipeline layouts —
        const RHI::ShaderStage visibility = reflected_stage_mask(reflection);
        vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, visibility);
        for (const GeneratedBindGroupLayout &layout : generated) {
            auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "material bind group layout",
            });
            if (!handle) {
                destroy_material_template_gpu(resource);
                return unexpected(graphics_error_from_rhi(handle.error(), "create material bind group layout"));
            }
            resource.bind_group_layouts.push_back(*handle);
            resource.bind_group_layout_sets.push_back(layout.set);
        }

        // Derived from reflection rather than hand-written `sizeof(SceneDrawConstants)` — see
        // generate_push_constant_ranges' doc comment. Only the vertex stage reads scene_draw
        // (RendererLifecycle.cpp's draw call pushes it with ShaderStage::Vertex), hence Vertex here.
        const vector<RHI::PushConstantRange> scene_draw_constants = generate_push_constant_ranges(reflection, RHI::ShaderStage::Vertex);
        if (scene_draw_constants.empty()) {
            destroy_material_template_gpu(resource);
            return unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed,
                "material shader reflection did not produce the expected scene_draw push-constant range.",
            });
        }
        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{resource.bind_group_layouts.data(), resource.bind_group_layouts.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{scene_draw_constants.data(), scene_draw_constants.size()},
            .label = "material pipeline layout",
        });
        if (!pipeline_layout) {
            destroy_material_template_gpu(resource);
            return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create material pipeline layout"));
        }
        resource.pipeline_layout = *pipeline_layout;

        // — Uniform block + parameters —
        const UniformLocation uniform = find_uniform_location(reflection);
        if (uniform.present) {
            resource.has_uniform_block = true;
            resource.uniform_set = uniform.set;
            resource.uniform_binding = uniform.binding;
            resource.uniform_block_size = static_cast<u32>(reflection.global_constant_buffer_size);
        }
        for (const ReflectedUniform &field : collect_uniform_fields(reflection)) {
            MaterialParameter parameter{
                .name = field.name,
                .offset = static_cast<u32>(field.offset),
                .size = static_cast<u32>(field.size),
                .type = material_parameter_type_of(field),
            };
            parameter.default_bytes.assign(static_cast<usize>(field.size), std::byte{0});
            resource.parameters.push_back(std::move(parameter));
        }

        // — Texture slots —
        for (const ReflectedResource &binding : collect_resource_bindings(reflection)) {
            const bool is_texture = binding.type == RHI::BindingType::SampledTexture ||
                                    binding.type == RHI::BindingType::CombinedImageSampler ||
                                    binding.type == RHI::BindingType::StorageTexture;
            if (!is_texture) {
                continue;
            }
            resource.texture_slots.push_back(MaterialTextureSlot{
                .name = binding.name,
                .set = binding.set,
                .binding = binding.binding,
                .type = binding.type,
            });
        }

        return {};
    }

    Core::RendererExpected<MaterialTemplateHandle> Renderer::create_material_template(
        const Core::Slang::Shader &shader, const char *label) {
        MaterialTemplateResource resource{};
        resource.handle = MaterialTemplateHandle{static_cast<u64>(material_templates_.size() + 1)};
        resource.label = label ? label : "";
        resource.shader = shader;

        if (Core::RendererResult built = build_material_template_gpu(resource, shader); !built) {
            destroy_material_template_gpu(resource);
            return unexpected(built.error());
        }

        resource.alive = true;
        Foundation::log_info("Material template '{}': {} set(s), {} parameter(s), {} texture slot(s), uniform block {} bytes.",
                             resource.label, resource.bind_group_layouts.size(), resource.parameters.size(),
                             resource.texture_slots.size(), resource.uniform_block_size);
        material_templates_.push_back(std::move(resource));
        return material_templates_.back().handle;
    }

    Core::RendererExpected<MaterialTemplateHandle> Renderer::create_material_template_from_source(
        const Core::Slang::ShaderSource &source, const Core::Slang::ShaderCompileOptions &options, const char *label) {
        if (rhi_device() == nullptr) {
            return unexpected(material_error("Cannot create a material template without an RHI device."));
        }

        // The variant cache owns the source + base options and compiles the base (define-less) permutation
        // now; SKINNED/ALPHA_TEST/... permutations compile lazily on later requests, and a hot-reload
        // re-drives the same cache against the edited file.
        slang::ShaderVariantCache variant_cache{source, options};
        auto base = variant_cache.get_or_compile_base();
        if (!base) {
            return unexpected(material_shader_error(base.error(), "compile material template source"));
        }

        MaterialTemplateResource resource{};
        resource.handle = MaterialTemplateHandle{static_cast<u64>(material_templates_.size() + 1)};
        resource.label = label ? label : "";
        resource.shader = *base;
        resource.hot_reloadable = !source.path.empty();

        if (Core::RendererResult built = build_material_template_gpu(resource, resource.shader); !built) {
            destroy_material_template_gpu(resource);
            return unexpected(built.error());
        }
        // Move the cache in only after a successful build so a compile-but-fails-to-build template doesn't
        // leave a half-live cache behind.
        resource.variant_cache = std::move(variant_cache);

        resource.alive = true;
        Foundation::log_info("Material template '{}' (from '{}'): {} set(s), {} parameter(s), {} texture slot(s), uniform block {} bytes.",
                             resource.label, source.path.empty() ? source.module_name : source.path,
                             resource.bind_group_layouts.size(), resource.parameters.size(),
                             resource.texture_slots.size(), resource.uniform_block_size);
        material_templates_.push_back(std::move(resource));
        return material_templates_.back().handle;
    }

    Core::RendererResult Renderer::reload_material_template(MaterialTemplateHandle handle) {
        MaterialTemplateResource *tmpl = material_template(handle);
        if (tmpl == nullptr) {
            return unexpected(material_error("Cannot reload an unknown material template."));
        }
        if (!tmpl->hot_reloadable) {
            return {}; // Not source-backed — nothing on disk to reload from.
        }
        if (rhi_device() == nullptr) {
            return unexpected(material_error("Cannot reload a material template without an RHI device."));
        }

        // Recompile the base permutation from the (possibly edited) source. `invalidate()` drops every
        // stale compiled permutation so the whole variant set rebuilds from the new code on next request.
        tmpl->variant_cache.invalidate();
        auto recompiled = tmpl->variant_cache.get_or_compile_base();
        if (!recompiled) {
            // Keep the current template intact on a compile error — a broken save shouldn't blank the
            // screen; the last good pipeline keeps rendering until the file compiles again.
            return unexpected(material_shader_error(recompiled.error(), "recompile material template on hot-reload"));
        }

        // Build the new GPU objects into a scratch resource first, so a build failure leaves the live
        // template untouched (the rebuild is all-or-nothing).
        MaterialTemplateResource next{};
        next.shader = *recompiled;
        if (Core::RendererResult built = build_material_template_gpu(next, next.shader); !built) {
            destroy_material_template_gpu(next);
            return unexpected(built.error());
        }

        // The heavy hammer: make sure no in-flight frame still references the objects we're about to
        // destroy before we swap them. Sanctioned here because a hot-reload is a resource reload, not the
        // per-frame path — see plans/async-submission-model.md.
        wait_idle();

        const bool compatible = material_template_layout_compatible(*tmpl, next);

        // Swap the freshly built GPU objects + reflection-derived descriptors into the live template,
        // tearing down the old ones. Identity/ownership fields (handle/label/variant_cache/hot_reloadable)
        // stay put.
        destroy_material_template_gpu(*tmpl);
        tmpl->shader = std::move(next.shader);
        tmpl->vertex_module = next.vertex_module;
        tmpl->fragment_module = next.fragment_module;
        tmpl->vertex_entry_point = std::move(next.vertex_entry_point);
        tmpl->fragment_entry_point = std::move(next.fragment_entry_point);
        tmpl->has_fragment = next.has_fragment;
        tmpl->bind_group_layouts = std::move(next.bind_group_layouts);
        tmpl->bind_group_layout_sets = std::move(next.bind_group_layout_sets);
        tmpl->pipeline_layout = next.pipeline_layout;
        tmpl->uniform_block_size = next.uniform_block_size;
        tmpl->uniform_set = next.uniform_set;
        tmpl->uniform_binding = next.uniform_binding;
        tmpl->has_uniform_block = next.has_uniform_block;
        tmpl->parameters = std::move(next.parameters);
        tmpl->texture_slots = std::move(next.texture_slots);
        tmpl->pipeline_variants.clear(); // rebuilt lazily by material_pipeline_for against the new layout

        // Fix up every instance of this template. Bind groups always rebuild (they were allocated against
        // the now-destroyed layouts); on a compatible reload the UBOs keep their size and only need a
        // re-upload, but on a layout change the instance's whole CPU/GPU state is rebuilt from scratch.
        const MaterialTemplateHandle template_handle = tmpl->handle;
        for (MaterialInstanceResource &instance : material_instances_) {
            if (!instance.alive || instance.material_template != template_handle) {
                continue;
            }
            MaterialTemplateResource *live = material_template(template_handle);
            if (compatible) {
                for (MaterialInstanceFrame &frame : instance.frames) {
                    frame.uniform_dirty = true;
                    frame.bind_groups_dirty = true;
                }
            } else {
                destroy_material_instance_gpu(instance);
                instance.uniform_values.clear();
                instance.textures.clear();
                instance.frames.clear();
                if (Core::RendererResult reseeded = initialize_material_instance_state(instance, *live); !reseeded) {
                    return reseeded;
                }
            }
        }

        Foundation::log_info("Hot-reloaded material template '{}' ({} reload).", tmpl->label,
                             compatible ? "compatible" : "layout-changed");
        return {};
    }

    usize Renderer::poll_shader_hot_reload() {
        using namespace std::chrono_literals;

        usize reloaded = 0;
        if (shader_hot_reload_poll_ && shader_hot_reload_poll_->is_done()) {
            ShaderHotReloadPollResult poll_result = shader_hot_reload_poll_->wait();
            shader_hot_reload_poll_.reset();
            shader_watcher_ = std::move(poll_result.watcher);

            for (const slang::ShaderChange &change : poll_result.changes) {
                for (MaterialTemplateResource &tmpl : material_templates_) {
                    if (!tmpl.alive || !tmpl.hot_reloadable) {
                        continue;
                    }
                    if (!same_shader_file(tmpl.variant_cache.source().path, change.path)) {
                        continue;
                    }
                    if (Core::RendererResult result = reload_material_template(tmpl.handle); result) {
                        ++reloaded;
                    } else {
                        Foundation::log_error("Shader hot-reload of '{}' failed: {}", change.path, result.error().message);
                    }
                }
            }
        }

        const steady_clock::time_point now = steady_clock::now();
        if (!shader_hot_reload_poll_ &&
            (next_shader_hot_reload_poll_ == steady_clock::time_point{} || now >= next_shader_hot_reload_poll_)) {
            next_shader_hot_reload_poll_ = now + 250ms;
            auto watcher = shader_watcher_;
            shader_hot_reload_poll_ = Async::Scheduler::spawn([watcher = std::move(watcher)]() mutable {
                ShaderHotReloadPollResult result{};
                result.watcher = std::move(watcher);
                if (!result.watcher) {
                    // Primed: the first real poll reports only edits made after the engine started.
                    result.watcher = std::make_shared<slang::ShaderWatcher>(std::filesystem::path{"Shaders"});
                }
                result.changes = result.watcher->poll();
                return result;
            });
        }

        return reloaded;
    }

    Core::RendererExpected<RHI::RenderPipelineHandle> Renderer::material_pipeline_for(
        MaterialTemplateResource &material_template, span<const RHI::Format> color_formats, RHI::Format depth_format) {
        if (color_formats.empty()) {
            return unexpected(material_error("Cannot build a material pipeline without at least one color target."));
        }
        // See material_pipeline_mutex_'s doc comment (RendererModule.cppm) for why this is a plain mutex
        // rather than an Async::Mutex<T> like the other lazy caches: pipeline_variants lives inside a
        // MaterialTemplateResource stored by value in vector<MaterialTemplateResource>.
        std::lock_guard<std::mutex> lock(material_pipeline_mutex_);
        for (const MaterialPipelineVariant &variant : material_template.pipeline_variants) {
            if (variant.depth_format == depth_format && variant.color_formats.size() == color_formats.size() &&
                std::equal(variant.color_formats.begin(), variant.color_formats.end(), color_formats.begin())) {
                return variant.pipeline;
            }
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(material_error("Cannot build a material pipeline without an RHI device."));
        }

        const array<RHI::VertexAttribute, 4> attributes = geometry_vertex_attributes();
        const RHI::VertexBufferLayout vertex_layout{
            .stride = sizeof(GeometryVertex),
            .step_mode = RHI::VertexStepMode::Vertex,
            .attributes = span<const RHI::VertexAttribute>{attributes.data(), attributes.size()},
        };
        vector<RHI::ColorTargetState> color_targets;
        color_targets.reserve(color_formats.size());
        for (RHI::Format color_format : color_formats) {
            color_targets.push_back(RHI::ColorTargetState{.format = color_format, .blend_enable = false, .write_mask = RHI::ColorWriteMask::All});
        }
        RHI::DepthStencilState depth_stencil{};
        if (depth_format != RHI::Format::Undefined) {
            depth_stencil = RHI::DepthStencilState{
                .format = depth_format,
                .depth_test_enable = true,
                .depth_write_enable = true,
                .depth_compare = RHI::CompareOp::Less,
            };
        }

        RHI::RenderPipelineDesc desc{
            .layout = material_template.pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = material_template.vertex_module, .entry_point = material_template.vertex_entry_point.c_str(), .stage = RHI::ShaderStage::Vertex},
            .fragment = material_template.has_fragment
                            ? RHI::ShaderEntry{.module = material_template.fragment_module, .entry_point = material_template.fragment_entry_point.c_str(), .stage = RHI::ShaderStage::Fragment}
                            : RHI::ShaderEntry{},
            .vertex_buffers = span<const RHI::VertexBufferLayout>{&vertex_layout, 1},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = depth_stencil,
            .color_targets = span<const RHI::ColorTargetState>{color_targets.data(), color_targets.size()},
            .label = "material pipeline",
        };
        auto pipeline = device->create_render_pipeline(desc);
        if (!pipeline) {
            return unexpected(graphics_error_from_rhi(pipeline.error(), "create material pipeline"));
        }
        material_template.pipeline_variants.push_back(MaterialPipelineVariant{
            .color_formats = vector<RHI::Format>{color_formats.begin(), color_formats.end()},
            .depth_format = depth_format,
            .pipeline = *pipeline,
        });
        return *pipeline;
    }

    Core::RendererExpected<MaterialInstanceHandle> Renderer::create_material_instance(
        MaterialTemplateHandle material_template_handle, const char *label) {
        MaterialTemplateResource *tmpl = material_template(material_template_handle);
        if (tmpl == nullptr) {
            return unexpected(material_error("Cannot create a material instance from an unknown template."));
        }
        if (rhi_device() == nullptr) {
            return unexpected(material_error("Cannot create a material instance without an RHI device."));
        }

        MaterialInstanceResource instance{};
        instance.handle = MaterialInstanceHandle{static_cast<u64>(material_instances_.size() + 1)};
        instance.material_template = material_template_handle;
        instance.label = label ? label : "";

        if (Core::RendererResult seeded = initialize_material_instance_state(instance, *tmpl); !seeded) {
            destroy_material_instance_gpu(instance);
            return unexpected(seeded.error());
        }

        instance.alive = true;
        material_instances_.push_back(std::move(instance));
        return material_instances_.back().handle;
    }

    Core::RendererResult Renderer::initialize_material_instance_state(MaterialInstanceResource &instance,
                                                                      MaterialTemplateResource &tmpl) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(material_error("Cannot initialize a material instance without an RHI device."));
        }
        auto white = ensure_default_white_texture();
        if (!white) {
            return unexpected(white.error());
        }

        instance.uniform_values.assign(static_cast<usize>(tmpl.uniform_block_size), std::byte{0});
        for (const MaterialParameter &parameter : tmpl.parameters) {
            if (!parameter.default_bytes.empty() &&
                static_cast<u64>(parameter.offset) + parameter.default_bytes.size() <= instance.uniform_values.size()) {
                std::memcpy(instance.uniform_values.data() + parameter.offset,
                            parameter.default_bytes.data(), parameter.default_bytes.size());
            }
        }
        instance.textures.clear();
        for (const MaterialTextureSlot &slot : tmpl.texture_slots) {
            instance.textures.push_back(MaterialTextureBinding{.binding = slot.binding, .texture = *white});
        }

        const u32 frame_count = capabilities_.max_frames_in_flight == 0 ? 1u : capabilities_.max_frames_in_flight;
        instance.frames.assign(frame_count, MaterialInstanceFrame{});
        for (MaterialInstanceFrame &frame : instance.frames) {
            if (tmpl.has_uniform_block && tmpl.uniform_block_size > 0) {
                auto ubo = device->create_buffer(RHI::BufferDesc{
                    .size = tmpl.uniform_block_size,
                    .usage = RHI::BufferUsage::Uniform,
                    .memory = RHI::MemoryLocation::HostUpload,
                    .label = "material instance UBO",
                });
                if (!ubo) {
                    return unexpected(graphics_error_from_rhi(ubo.error(), "create material instance UBO"));
                }
                frame.uniform_buffer = *ubo;
            }
            frame.uniform_dirty = true;
            frame.bind_groups_dirty = true;
        }
        return {};
    }

    Core::RendererExpected<span<const RHI::BindGroupHandle>> Renderer::prepare_material_frame(
        MaterialInstanceResource &instance, u32 frame_slot) {
        MaterialTemplateResource *tmpl = material_template(instance.material_template);
        if (tmpl == nullptr) {
            return unexpected(material_error("Material instance references an unknown template."));
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(material_error("Cannot prepare a material frame without an RHI device."));
        }
        if (frame_slot >= instance.frames.size()) {
            return unexpected(material_error("Material frame slot out of range."));
        }
        MaterialInstanceFrame &frame = instance.frames[frame_slot];

        // Re-upload this slot's UBO if the CPU value block changed since it was last written.
        if (frame.uniform_dirty && frame.uniform_buffer && !instance.uniform_values.empty()) {
            if (auto written = device->write_buffer(frame.uniform_buffer, 0,
                                                    span<const std::byte>{instance.uniform_values.data(), instance.uniform_values.size()});
                !written) {
                return unexpected(graphics_error_from_rhi(written.error(), "upload material instance UBO"));
            }
            frame.uniform_dirty = false;
        }

        // (Re)build one bind group per template set when textures/bindings changed.
        if (frame.bind_groups_dirty) {
            for (RHI::BindGroupHandle group : frame.bind_groups) {
                if (group) {
                    device->destroy_bind_group(group);
                }
            }
            frame.bind_groups.clear();

            for (usize set_index = 0; set_index < tmpl->bind_group_layouts.size(); ++set_index) {
                const u32 set = tmpl->bind_group_layout_sets[set_index];
                vector<RHI::BindGroupEntry> entries;

                if (tmpl->has_uniform_block && tmpl->uniform_set == set && frame.uniform_buffer) {
                    entries.push_back(RHI::BindGroupEntry{.binding = tmpl->uniform_binding, .buffer = frame.uniform_buffer});
                }
                for (const MaterialTextureSlot &slot : tmpl->texture_slots) {
                    if (slot.set != set) {
                        continue;
                    }
                    TextureHandle bound{};
                    for (const MaterialTextureBinding &binding : instance.textures) {
                        if (binding.binding == slot.binding) {
                            bound = binding.texture;
                            break;
                        }
                    }
                    const TextureResource *tex = texture(bound);
                    if (tex == nullptr) {
                        return unexpected(material_error("Material texture slot bound to a missing texture."));
                    }
                    entries.push_back(RHI::BindGroupEntry{
                        .binding = slot.binding,
                        .texture_view = tex->view,
                        .sampler = tex->sampler,
                    });
                }

                auto group = device->create_bind_group(RHI::BindGroupDesc{
                    .layout = tmpl->bind_group_layouts[set_index],
                    .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
                    .label = "material bind group",
                });
                if (!group) {
                    return unexpected(graphics_error_from_rhi(group.error(), "create material bind group"));
                }
                frame.bind_groups.push_back(*group);
            }
            frame.bind_groups_dirty = false;
        }

        return span<const RHI::BindGroupHandle>{frame.bind_groups.data(), frame.bind_groups.size()};
    }

    Core::RendererResult Renderer::set_material_parameter(MaterialInstanceHandle handle, string_view name,
                                                          span<const std::byte> value) {
        MaterialInstanceResource *instance = material_instance(handle);
        if (instance == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, "Unknown material instance.");
        }
        MaterialTemplateResource *tmpl = material_template(instance->material_template);
        if (tmpl == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, "Material instance references an unknown template.");
        }
        for (const MaterialParameter &parameter : tmpl->parameters) {
            if (parameter.name != name) {
                continue;
            }
            if (value.size() != parameter.size) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Material parameter value size does not match the reflected parameter.");
            }
            if (static_cast<u64>(parameter.offset) + value.size() > instance->uniform_values.size()) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Material parameter write is out of the uniform block bounds.");
            }
            std::memcpy(instance->uniform_values.data() + parameter.offset, value.data(), value.size());
            for (MaterialInstanceFrame &frame : instance->frames) {
                frame.uniform_dirty = true;
            }
            return {};
        }
        return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                            string("Material has no parameter named '") + string(name) + "'.");
    }

    Core::RendererResult Renderer::set_material_float(MaterialInstanceHandle handle, string_view name, f32 value) {
        return set_material_parameter(handle, name, std::as_bytes(span<const f32>{&value, 1}));
    }

    Core::RendererResult Renderer::set_material_vec4(MaterialInstanceHandle handle, string_view name,
                                                     f32 x, f32 y, f32 z, f32 w) {
        const array<f32, 4> v{x, y, z, w};
        return set_material_parameter(handle, name, std::as_bytes(span<const f32>{v.data(), v.size()}));
    }

    Core::RendererResult Renderer::set_material_texture(MaterialInstanceHandle handle, string_view slot,
                                                        TextureHandle texture_handle) {
        MaterialInstanceResource *instance = material_instance(handle);
        if (instance == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, "Unknown material instance.");
        }
        MaterialTemplateResource *tmpl = material_template(instance->material_template);
        if (tmpl == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, "Material instance references an unknown template.");
        }
        if (texture(texture_handle) == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, "Material texture bind target does not exist.");
        }
        for (const MaterialTextureSlot &texture_slot : tmpl->texture_slots) {
            if (texture_slot.name != slot) {
                continue;
            }
            for (MaterialTextureBinding &binding : instance->textures) {
                if (binding.binding == texture_slot.binding) {
                    binding.texture = texture_handle;
                    for (MaterialInstanceFrame &frame : instance->frames) {
                        frame.bind_groups_dirty = true;
                    }
                    return {};
                }
            }
        }
        return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                            string("Material has no texture slot named '") + string(slot) + "'.");
    }

    MaterialTemplateResource *Renderer::material_template(MaterialTemplateHandle handle) noexcept {
        if (!handle || handle.value > material_templates_.size()) {
            return nullptr;
        }
        MaterialTemplateResource &resource = material_templates_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    const MaterialTemplateResource *Renderer::material_template(MaterialTemplateHandle handle) const noexcept {
        if (!handle || handle.value > material_templates_.size()) {
            return nullptr;
        }
        const MaterialTemplateResource &resource = material_templates_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    MaterialInstanceResource *Renderer::material_instance(MaterialInstanceHandle handle) noexcept {
        if (!handle || handle.value > material_instances_.size()) {
            return nullptr;
        }
        MaterialInstanceResource &resource = material_instances_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    const MaterialInstanceResource *Renderer::material_instance(MaterialInstanceHandle handle) const noexcept {
        if (!handle || handle.value > material_instances_.size()) {
            return nullptr;
        }
        const MaterialInstanceResource &resource = material_instances_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    void Renderer::destroy_material_template_gpu(MaterialTemplateResource &resource) noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return;
        }
        for (const MaterialPipelineVariant &variant : resource.pipeline_variants) {
            if (variant.pipeline) {
                device->destroy_render_pipeline(variant.pipeline);
            }
        }
        if (resource.pipeline_layout) {
            device->destroy_pipeline_layout(resource.pipeline_layout);
        }
        for (RHI::BindGroupLayoutHandle layout : resource.bind_group_layouts) {
            device->destroy_bind_group_layout(layout);
        }
        if (resource.fragment_module) {
            device->destroy_shader_module(resource.fragment_module);
        }
        if (resource.vertex_module) {
            device->destroy_shader_module(resource.vertex_module);
        }
    }

    void Renderer::destroy_material_instance_gpu(MaterialInstanceResource &resource) noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return;
        }
        for (MaterialInstanceFrame &frame : resource.frames) {
            for (RHI::BindGroupHandle group : frame.bind_groups) {
                if (group) {
                    device->destroy_bind_group(group);
                }
            }
            if (frame.uniform_buffer) {
                device->destroy_buffer(frame.uniform_buffer);
            }
        }
    }

    void Renderer::destroy_material_template(MaterialTemplateHandle handle) noexcept {
        MaterialTemplateResource *resource = material_template(handle);
        if (resource == nullptr) {
            return;
        }
        destroy_material_template_gpu(*resource);
        *resource = {};
    }

    void Renderer::destroy_material_instance(MaterialInstanceHandle handle) noexcept {
        MaterialInstanceResource *resource = material_instance(handle);
        if (resource == nullptr) {
            return;
        }
        destroy_material_instance_gpu(*resource);
        *resource = {};
    }

} // namespace SFT::Renderer
