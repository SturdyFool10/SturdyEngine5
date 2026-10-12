#pragma once

#include <Foundation/Foundation.hpp>

#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/FramePipeline.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    class Renderer;

    /// Colour grading (`Shaders/color_grade.slang`): white balance, contrast, lift / gamma / gain, split toning, saturation and an
    /// optional log-domain 3D LUT, in one compute pass on the scene-linear image just before tone mapping. Written only against
    /// public services (the compute-kernel service, the blackboard, `Renderer::texture` for the LUT), so it can be replaced or
    /// copied. See `RenderSettings::ColorGradeSettings` for the order of operations and the LUT's domain.

    /// What the shader reads (`GradeConstants` in color_grade.slang): everything that does not vary per pixel, precomputed so the
    /// shader does no setup and the maths here can be tested on the CPU.
    struct ColorGradeConstants {
        glm::uvec4 extent{};           // width, height, LUT size (0 = none), unused
        glm::vec4 white_balance{1.0f}; // gain rgb, exposure gain (2^EV)
        glm::vec4 look{1.0f};          // contrast, saturation, split strength, LUT intensity
        glm::vec4 lift{0.0f};
        glm::vec4 inverse_gamma{1.0f};
        glm::vec4 gain{1.0f};
        glm::vec4 shadow_tint{1.0f};
        glm::vec4 highlight_tint{1.0f};
    };
    static_assert(sizeof(ColorGradeConstants) == 128);

    /// Whether the grade runs this frame: enabled and a raster scene.
    [[nodiscard]] bool color_grade_active(const RenderGraphSettings &settings) noexcept;

    /// Per-channel gains for a white balance: `temperature` (-1 cool .. 1 warm) trades blue for red, `tint` (-1 green .. 1 magenta)
    /// moves green. Normalised so a neutral grey keeps its luminance; (1, 1, 1) at (0, 0).
    [[nodiscard]] glm::vec3 white_balance_gain(f32 temperature, f32 tint) noexcept;

    /// The shader constants for `settings` and a frame of `width` x `height`; `lut_size` is 0 when there is no usable LUT.
    [[nodiscard]] ColorGradeConstants make_color_grade_constants(const RenderSettings::ColorGradeSettings &settings, u32 width, u32 height,
                                                                 u32 lut_size) noexcept;

    /// Adds the pass and returns the graded scene colour. Reads `settings.frame.color_grade`.
    [[nodiscard]] Core::RendererExpected<RenderGraphTextureHandle> add_color_grade_pass(
        Renderer &renderer, RenderGraph &graph, std::vector<RHI::BindGroupHandle> &transient_bind_groups,
        std::vector<RHI::BufferHandle> &transient_buffers, RHI::RhiDevice &device, RenderGraphTextureHandle scene_color,
        Core::Extent2D extent, RHI::Extent3D output_extent, RHI::Format output_format, const RenderGraphSettings &settings);

    /// The engine's `color_grade` feature (Post stage, after `effects_after_bloom`): consumes and republishes SceneHdrColor.
    [[nodiscard]] Core::RendererResult build_color_grade_feature(FrameBuildContext &frame);

} // namespace SFT::Renderer
