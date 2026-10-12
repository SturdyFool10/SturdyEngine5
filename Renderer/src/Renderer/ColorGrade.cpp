#include <Renderer/ColorGrade.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

#include <glm/geometric.hpp>

#include <Renderer/FrameUpload.hpp>
#include <Renderer/RendererModule.hpp>

namespace SFT::Renderer {

    namespace {

        [[nodiscard]] f32 finite_or(f32 value, f32 fallback) noexcept { return std::isfinite(value) ? value : fallback; }

        [[nodiscard]] glm::vec3 finite_vec3(const float (&values)[3], f32 fallback) noexcept {
            return glm::vec3{finite_or(values[0], fallback), finite_or(values[1], fallback), finite_or(values[2], fallback)};
        }

        [[nodiscard]] ComputeKernelDescription grade_kernel() {
            return ComputeKernelDescription{
                .shader_path = "Shaders/color_grade.slang",
                .module_name = "color_grade",
                .entry_point = "gradeMain",
                .label = "color_grade",
            };
        }

    } // namespace

    bool color_grade_active(const RenderGraphSettings &settings) noexcept {
        return settings.frame.color_grade.enabled && settings.render_scene;
    }

    glm::vec3 white_balance_gain(f32 temperature, f32 tint) noexcept {
        const f32 t = std::clamp(finite_or(temperature, 0.0f), -1.0f, 1.0f);
        const f32 g = std::clamp(finite_or(tint, 0.0f), -1.0f, 1.0f);
        // Half a stop either way at the extremes: warm lifts red and lowers blue, magenta lowers green.
        glm::vec3 gain{std::exp2(0.5f * t), std::exp2(-0.25f * g), std::exp2(-0.5f * t)};
        // Keep a neutral grey's luminance (Rec. 709 weights), so white balance shifts colour, not brightness.
        const f32 luminance = glm::dot(gain, glm::vec3{0.2126f, 0.7152f, 0.0722f});
        return gain / std::max(luminance, 1.0e-4f);
    }

    ColorGradeConstants make_color_grade_constants(const RenderSettings::ColorGradeSettings &settings, u32 width, u32 height,
                                                   u32 lut_size) noexcept {
        const glm::vec3 wb = white_balance_gain(settings.temperature, settings.tint);
        const glm::vec3 lift = finite_vec3(settings.lift, 0.0f);
        const glm::vec3 gain = glm::max(finite_vec3(settings.gain, 1.0f), glm::vec3{0.0f});
        const glm::vec3 gamma = glm::clamp(finite_vec3(settings.gamma, 1.0f), glm::vec3{0.05f}, glm::vec3{20.0f});
        const glm::vec3 shadow = glm::max(finite_vec3(settings.shadow_tint, 1.0f), glm::vec3{0.0f});
        const glm::vec3 highlight = glm::max(finite_vec3(settings.highlight_tint, 1.0f), glm::vec3{0.0f});
        return ColorGradeConstants{
            .extent = glm::uvec4{width, height, lut_size, 0u},
            .white_balance = glm::vec4{wb, std::exp2(std::clamp(finite_or(settings.exposure_ev, 0.0f), -16.0f, 16.0f))},
            .look = glm::vec4{std::clamp(finite_or(settings.contrast, 1.0f), 0.0f, 4.0f), std::clamp(finite_or(settings.saturation, 1.0f), 0.0f, 4.0f),
                              std::clamp(finite_or(settings.split_strength, 1.0f), 0.0f, 1.0f),
                              lut_size > 0 ? std::clamp(finite_or(settings.lut_intensity, 1.0f), 0.0f, 1.0f) : 0.0f},
            .lift = glm::vec4{lift, 0.0f},
            .inverse_gamma = glm::vec4{1.0f / gamma, 0.0f},
            .gain = glm::vec4{gain, 1.0f},
            .shadow_tint = glm::vec4{shadow, 1.0f},
            .highlight_tint = glm::vec4{highlight, 1.0f},
        };
    }

    Core::RendererExpected<RenderGraphTextureHandle> add_color_grade_pass(
        Renderer &renderer, RenderGraph &graph, std::vector<RHI::BindGroupHandle> &transient_bind_groups,
        std::vector<RHI::BufferHandle> &transient_buffers, RHI::RhiDevice &device, RenderGraphTextureHandle scene_color,
        Core::Extent2D extent, RHI::Extent3D output_extent, RHI::Format output_format, const RenderGraphSettings &settings) {
        if (!scene_color || output_format == RHI::Format::Undefined) {
            return std::unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                              "Colour grading needs the scene colour and the output format."});
        }
        const auto kernel = renderer.prepare_compute_kernel(grade_kernel());
        if (!kernel) return std::unexpected(kernel.error());

        const RenderSettings::ColorGradeSettings &grade = settings.frame.color_grade;
        // The LUT is an atlas of `size` slices, `size * size` wide and `size` tall. Anything else is not a LUT.
        RHI::TextureViewHandle lut_view{};
        u32 lut_size = 0;
        if (grade.lut_texture != 0) {
            if (const TextureResource *lut = renderer.texture(TextureHandle{.value = grade.lut_texture});
                lut != nullptr && lut->view && lut->height >= 2 && lut->width == lut->height * lut->height) {
                lut_view = lut->view;
                lut_size = lut->height;
            }
        }

        const ColorGradeConstants constants = make_color_grade_constants(grade, extent.x, extent.y, lut_size);
        auto constants_buffer = upload_transient_buffer(device, transient_buffers, RHI::BufferUsage::Uniform,
                                                        std::as_bytes(std::span<const ColorGradeConstants>{&constants, 1}), "colour grade constants");
        if (!constants_buffer) return std::unexpected(constants_buffer.error());
        const RHI::BufferHandle grade_buffer = *constants_buffer;

        const RenderGraphTextureHandle destination = graph.create_texture(RenderGraphTextureDesc{
            .format = output_format,
            .extent = output_extent,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage |
                     RHI::TextureUsage::TransferSrc | RHI::TextureUsage::TransferDst,
            .label = "colour graded scene colour"});

        graph.add_compute_pass("colour grade"_ustr)
            .add_sampled_texture(scene_color)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = destination, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *kernel, grade_buffer, scene_color, destination, lut_view,
                          extent](RenderGraphComputeContext &context) -> Core::RendererResult {
                const RHI::TextureViewHandle scene_view = context.texture(scene_color).default_view;
                const ComputeBufferBinding buffers[] = {{.name = "grade", .buffer = grade_buffer, .size = sizeof(ColorGradeConstants)}};
                const ComputeBinding bindings[] = {{"sceneColor", scene_view},
                                                   // Without a LUT the scene colour stands in for it; the shader never samples it then.
                                                   {"gradeLut", lut_view ? lut_view : scene_view},
                                                   {"colorOut", context.texture(destination).default_view}};
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, {},
                                                      glm::uvec3{(extent.x + 7u) / 8u, (extent.y + 7u) / 8u, 1u}, transient_bind_groups, buffers);
            });
        return destination;
    }

    Core::RendererResult build_color_grade_feature(FrameBuildContext &frame) {
        if (frame.direct_overlay_presentation || !color_grade_active(frame.settings) || frame.transient_buffers == nullptr) {
            return {};
        }
        auto graded = add_color_grade_pass(frame.renderer, frame.graph, frame.transient_bind_groups, *frame.transient_buffers, frame.device,
                                           frame.resources.texture<RenderGraphSemantics::SceneHdrColor>(), frame.module.render_extent,
                                           frame.module.render_texture_extent(), frame.deferred_formats.scene_color, frame.settings);
        if (!graded) {
            return std::unexpected(graded.error());
        }
        frame.resources.publish_texture<RenderGraphSemantics::SceneHdrColor>(*graded);
        return {};
    }

} // namespace SFT::Renderer
