#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#pragma endregion

#include "UiCustomElementPipeline.hpp"

#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/ShaderTarget.hpp>
#include <Renderer/TileGrid.hpp>

using std::string;
using std::string_view;
using std::unexpected;

namespace SFT::UI {

    namespace {
        namespace slang = Core::Slang;

        [[nodiscard]] Core::GraphicsBackendError custom_element_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

    } // namespace

    UiCustomElementPipeline::CachedShader *UiCustomElementPipeline::find_shader(const CustomShaderRef &shader,
                                                                                RHI::Format color_format) noexcept {
        for (CachedShader &candidate : shaders_) {
            if (candidate.shader_path == shader.shader_path && candidate.module_name == shader.module_name &&
                candidate.fragment_entry_point == shader.fragment_entry_point && candidate.color_format == color_format) {
                return &candidate;
            }
        }
        return nullptr;
    }

    Core::RendererExpected<UiCustomElementPipeline::CachedShader *> UiCustomElementPipeline::ensure_shader(
        RHI::RhiDevice &device, RHI::Format color_format, const CustomShaderRef &shader, bool enable_shader_disk_cache) {
        if (shader.shader_path.empty() || shader.module_name.empty() || shader.fragment_entry_point.empty()) {
            return unexpected(custom_element_error(
                "UI custom element requires shader_path, module_name, and fragment_entry_point."));
        }
        if (CachedShader *cached = find_shader(shader, color_format)) {
            return cached;
        }

        CachedShader resource{
            .shader_path = shader.shader_path,
            .module_name = shader.module_name,
            .fragment_entry_point = shader.fragment_entry_point,
            .color_format = color_format,
        };
        auto cleanup = [&]() noexcept {
            if (resource.pipeline) device.destroy_render_pipeline(resource.pipeline);
            if (resource.pipeline_layout) device.destroy_pipeline_layout(resource.pipeline_layout);
            if (resource.fragment_module) device.destroy_shader_module(resource.fragment_module);
            if (resource.vertex_module) device.destroy_shader_module(resource.vertex_module);
        };

        const auto shader_target = Renderer::shader_target_for_device(device);
        if (!shader_target) {
            return unexpected(shader_target.error());
        }
        const slang::ShaderCompileOptions options{
            .targets = Renderer::shader_compile_targets_for_device(device),
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = shader.fragment_entry_point, .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderVariantCache shader_cache{
            slang::ShaderSource::from_file(shader.shader_path, shader.module_name),
            options,
            slang::ShaderCompiler{},
            enable_shader_disk_cache};
        auto compiled = shader_cache.get_or_compile_base();
        if (!compiled) {
            return unexpected(custom_element_error(
                "compile UI custom element shader failed: " + compiled.error().message + "\n" + compiled.error().diagnostics));
        }

        auto vertex_code = compiled->entry_point_code("vertexMain", shader_target->slang_target.format);
        if (!vertex_code) {
            return unexpected(custom_element_error(
                "generate UI custom element vertex bytecode failed: " + vertex_code.error().message));
        }
        auto vertex_module = device.create_shader_module(RHI::ShaderModuleDesc{
            .language = shader_target->module_language,
            .code = span<const std::byte>{vertex_code->bytes.data(), vertex_code->bytes.size()},
            .label = "ui custom element vertex module",
        });
        if (!vertex_module) {
            return unexpected(Renderer::graphics_error_from_rhi(vertex_module.error(), "create ui custom element vertex module"));
        }
        resource.vertex_module = *vertex_module;

        auto fragment_code = compiled->entry_point_code(shader.fragment_entry_point, shader_target->slang_target.format);
        if (!fragment_code) {
            cleanup();
            return unexpected(custom_element_error(
                "generate UI custom element fragment bytecode failed: " + fragment_code.error().message));
        }
        auto fragment_module = device.create_shader_module(RHI::ShaderModuleDesc{
            .language = shader_target->module_language,
            .code = span<const std::byte>{fragment_code->bytes.data(), fragment_code->bytes.size()},
            .label = "ui custom element fragment module",
        });
        if (!fragment_module) {
            cleanup();
            return unexpected(Renderer::graphics_error_from_rhi(fragment_module.error(), "create ui custom element fragment module"));
        }
        resource.fragment_module = *fragment_module;

        const slang::ShaderReflection &reflection = compiled->reflection();
        const vector<Renderer::GeneratedBindGroupLayout> generated =
            Renderer::generate_bind_group_layouts(reflection, Renderer::reflected_stage_mask(reflection));
        if (!generated.empty()) {
            cleanup();
            return unexpected(custom_element_error(
                "UI custom element shader '" + shader.shader_path +
                "' declares a texture/sampler/buffer binding — v1 custom elements support push-constant "
                "parameters only (see CustomShaderRef's own doc comment)."));
        }

        const vector<RHI::PushConstantRange> push_ranges =
            Renderer::generate_push_constant_ranges(reflection, RHI::ShaderStage::Vertex | RHI::ShaderStage::Fragment);
        if (push_ranges.size() != 1) {
            cleanup();
            return unexpected(custom_element_error(
                "UI custom element shader '" + shader.shader_path +
                "' must declare exactly one [[push_constant]] struct, starting with UiElementConstants' five fields."));
        }
        if (push_ranges.front().size < sizeof(UiElementConstants)) {
            cleanup();
            return unexpected(custom_element_error(
                "UI custom element shader '" + shader.shader_path +
                "' push-constant struct is smaller than UiElementConstants — it must start with "
                "position/size/viewportSize/two reserved floats."));
        }
        resource.push_constant_size = push_ranges.front().size;

        auto pipeline_layout = device.create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = {},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_ranges.data(), push_ranges.size()},
            .label = "ui custom element pipeline layout",
        });
        if (!pipeline_layout) {
            cleanup();
            return unexpected(Renderer::graphics_error_from_rhi(pipeline_layout.error(), "create ui custom element pipeline layout"));
        }
        resource.pipeline_layout = *pipeline_layout;

        const RHI::ColorTargetState color_target{
            .format = color_format,
            .blend_enable = true,
            .color = RHI::BlendComponent{.src_factor = RHI::BlendFactor::SrcAlpha, .dst_factor = RHI::BlendFactor::OneMinusSrcAlpha, .op = RHI::BlendOp::Add},
            .alpha = RHI::BlendComponent{.src_factor = RHI::BlendFactor::One, .dst_factor = RHI::BlendFactor::OneMinusSrcAlpha, .op = RHI::BlendOp::Add},
            .write_mask = RHI::ColorWriteMask::All,
        };
        auto pipeline = device.create_render_pipeline(RHI::RenderPipelineDesc{
            .layout = resource.pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = resource.vertex_module, .entry_point = "vertexMain", .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = resource.fragment_module, .entry_point = shader.fragment_entry_point.c_str(), .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&color_target, 1},
            .label = "ui custom element pipeline",
        });
        if (!pipeline) {
            cleanup();
            return unexpected(Renderer::graphics_error_from_rhi(pipeline.error(), "create ui custom element pipeline"));
        }
        resource.pipeline = *pipeline;

        shaders_.push_back(std::move(resource));
        return &shaders_.back();
    }

    Core::RendererResult UiCustomElementPipeline::prepare(RHI::RhiDevice &device, RHI::Format color_format,
                                                           span<const CustomDraw> draws, bool enable_shader_disk_cache) {
        for (const CustomDraw &draw : draws) {
            if (draw.shader == nullptr) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "UI custom element draw has no shader reference.");
            }
            if (auto cached = ensure_shader(device, color_format, *draw.shader, enable_shader_disk_cache); !cached) {
                return unexpected(cached.error());
            }
        }
        return {};
    }

    Core::RendererResult UiCustomElementPipeline::draw(RHI::RenderPassEncoder &pass, RHI::Format color_format,
                                                        span<const CustomDraw> draws, glm::vec2 viewport_size) {
        for (const CustomDraw &draw : draws) {
            if (draw.shader == nullptr) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "UI custom element draw has no shader reference.");
            }
            CachedShader *cached = find_shader(*draw.shader, color_format);
            if (cached == nullptr) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "UI custom element shader '" + draw.shader->shader_path +
                                                        "' was not prepared before draw().");
            }

            const UiElementConstants element_constants{
                .position = draw.position, .size = draw.size, .viewport_size = viewport_size};
            const usize total_size = sizeof(UiElementConstants) + draw.shader->push_constants.size();
            if (total_size != cached->push_constant_size) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "UI custom element shader '" + draw.shader->shader_path +
                                                        "' push_constants size does not match its reflected push-constant struct.");
            }
            vector<std::byte> bytes(total_size);
            std::memcpy(bytes.data(), &element_constants, sizeof(UiElementConstants));
            if (!draw.shader->push_constants.empty()) {
                std::memcpy(bytes.data() + sizeof(UiElementConstants), draw.shader->push_constants.data(),
                           draw.shader->push_constants.size());
            }

            pass.set_pipeline(cached->pipeline);
            pass.set_scissor(draw.scissor);
            pass.set_push_constants(RHI::ShaderStage::Vertex | RHI::ShaderStage::Fragment, 0,
                                    span<const std::byte>{bytes.data(), bytes.size()});
            pass.draw(RHI::DrawArgs{.vertex_count = 6});
        }
        return {};
    }

    void UiCustomElementPipeline::destroy(RHI::RhiDevice &device) noexcept {
        for (CachedShader &resource : shaders_) {
            if (resource.pipeline) device.destroy_render_pipeline(resource.pipeline);
            if (resource.pipeline_layout) device.destroy_pipeline_layout(resource.pipeline_layout);
            if (resource.fragment_module) device.destroy_shader_module(resource.fragment_module);
            if (resource.vertex_module) device.destroy_shader_module(resource.vertex_module);
        }
        shaders_.clear();
    }

} // namespace SFT::UI
