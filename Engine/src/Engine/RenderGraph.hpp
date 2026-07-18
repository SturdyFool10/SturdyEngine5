#pragma once

#include <Foundation/Foundation.hpp>

#include <expected>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include <glm/vec4.hpp>

namespace SFT::Engine {

    // High-level, built-in operations rather than GPU pass kinds. Engine consumers choose what the
    // frame should do; Renderer still chooses attachments, formats, barriers, queues and allocation.
    enum class RenderFeature : u8 {
        Scene,
        DeferredLighting,
        ToneMapping,
        DebugOverlay,
    };

    enum class ToneMappingOperator : u8 {
        None,
        Reinhard,
        Aces,
        Exponential,
    };

    struct SceneRenderSettings {
        bool enabled = true;
        // Empty inherits Camera::clear_color(). This is display-independent linear color.
        std::optional<glm::vec4> background_color;
    };

    struct LightingRenderSettings {
        bool enabled = true;
        // Multiplies the background before it enters the HDR scene target.
        f32 background_intensity = 1.0f;
    };

    struct ToneMappingSettings {
        bool enabled = true;
        ToneMappingOperator operation = ToneMappingOperator::Aces;
        f32 exposure = 1.0f;
        f32 white_point = 1.0f;
        f32 saturation = 1.0f;
    };

    struct DebugOverlayRenderSettings {
        bool enabled = false;
    };

    struct RenderGraphDescription {
        SceneRenderSettings scene{};
        LightingRenderSettings lighting{};
        ToneMappingSettings tone_mapping{};
        DebugOverlayRenderSettings debug_overlay{};

        // Scales scene targets, not the presentation surface or UI. The final presentation pass
        // automatically samples at output resolution, so dynamic resolution requires no resource work
        // from the consumer. Valid range is [0.1, 2.0].
        f32 resolution_scale = 1.0f;
    };

    enum class RenderGraphErrorCode : u8 {
        InvalidResolutionScale,
        InvalidBackgroundColor,
        InvalidLightingSettings,
        InvalidToneMappingSettings,
        InvalidFeatureCombination,
    };

    struct RenderGraphError {
        RenderGraphErrorCode code = RenderGraphErrorCode::InvalidFeatureCombination;
        UString message;
    };

    using RenderGraphResult = std::expected<void, RenderGraphError>;

    // A reusable frame recipe. It is deliberately a small value object: keep one on a game camera,
    // copy it into a frame request, or reconstruct it through an eventual C/FFI description API.
    class RenderGraph {
      public:
        RenderGraph() noexcept = default;
        explicit RenderGraph(RenderGraphDescription description) noexcept;

        [[nodiscard]] static RenderGraph standard() noexcept;
        [[nodiscard]] static RenderGraph overlay_only() noexcept;

        [[nodiscard]] const RenderGraphDescription &description() const noexcept;
        [[nodiscard]] RenderGraphDescription &description() noexcept;

        [[nodiscard]] const SceneRenderSettings &scene() const noexcept;
        [[nodiscard]] SceneRenderSettings &scene() noexcept;
        [[nodiscard]] const LightingRenderSettings &lighting() const noexcept;
        [[nodiscard]] LightingRenderSettings &lighting() noexcept;
        [[nodiscard]] const ToneMappingSettings &tone_mapping() const noexcept;
        [[nodiscard]] ToneMappingSettings &tone_mapping() noexcept;
        [[nodiscard]] const DebugOverlayRenderSettings &debug_overlay() const noexcept;
        [[nodiscard]] DebugOverlayRenderSettings &debug_overlay() noexcept;

        [[nodiscard]] bool enabled(RenderFeature feature) const noexcept;
        RenderGraph &set_enabled(RenderFeature feature, bool enabled) noexcept;
        RenderGraph &enable(RenderFeature feature) noexcept;
        RenderGraph &disable(RenderFeature feature) noexcept;
        RenderGraph &set_resolution_scale(f32 scale) noexcept;
        RenderGraph &set_background_color(glm::vec4 color) noexcept;
        RenderGraph &inherit_camera_background() noexcept;
        RenderGraph &set_tone_mapping(ToneMappingOperator operation,
                                      f32 exposure = 1.0f,
                                      f32 white_point = 1.0f,
                                      f32 saturation = 1.0f) noexcept;

        // Native consumers can configure a typed section with a concise lambda while the ordinary
        // struct accessors above remain straightforward to expose through C.
        template <typename Configure>
            requires std::invocable<Configure, SceneRenderSettings &>
        RenderGraph &configure_scene(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.scene);
            return *this;
        }

        template <typename Configure>
            requires std::invocable<Configure, LightingRenderSettings &>
        RenderGraph &configure_lighting(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.lighting);
            return *this;
        }

        template <typename Configure>
            requires std::invocable<Configure, ToneMappingSettings &>
        RenderGraph &configure_tone_mapping(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.tone_mapping);
            return *this;
        }

        [[nodiscard]] RenderGraphResult validate() const noexcept;

        // Produces a safe executable recipe from untrusted/serialized settings. Engine uses this at
        // frame extraction after logging validate()'s diagnostic, so malformed settings never leak to
        // target allocation or shader constants.
        [[nodiscard]] RenderGraph normalized() const noexcept;

      private:
        RenderGraphDescription description_{};
    };

} // namespace SFT::Engine
