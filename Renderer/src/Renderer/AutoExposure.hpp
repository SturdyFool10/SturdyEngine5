#pragma once

#include <Foundation/Foundation.hpp>

#include <chrono>
#include <unordered_map>
#include <vector>

#include <Async/Async.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/FramePipeline.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    class Renderer;

    /// Persistent 1x1 exposure textures, one per key (the engine keys them by window), that carry the adapted
    /// exposure from frame to frame. Nothing about it is private to the renderer: an application can create
    /// its own instance for its own metering feature, or share the renderer's (`Renderer::auto_exposure_history`).
    class AutoExposureHistory {
      public:
        struct Lease {
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            /// False on the first frame after the texture was (re)created: the metering pass should jump straight
            /// to the target exposure instead of adapting from garbage.
            bool has_history = false;
            f32 delta_seconds = 1.0f / 60.0f;
        };

        /// Finds or creates the texture for `key` on `device` and marks it as written this frame.
        [[nodiscard]] Core::RendererExpected<Lease> acquire(RHI::RhiDevice &device, u64 key);
        /// Destroys every texture (on `device`, if given). Call when the graphics resources are torn down.
        void release(RHI::RhiDevice *device) noexcept;

      private:
        struct Slot {
            const RHI::RhiDevice *device = nullptr;
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            bool initialized = false;
            std::chrono::steady_clock::time_point last_use{};
        };
        Async::Mutex<std::unordered_map<u64, Slot>> slots_;
    };

    /// The engine's histogram auto-exposure: a per-tile luminance histogram, a reduction that meters the
    /// clipped histogram and adapts the exposure over time, and a pass that applies it to the scene colour
    /// (three passes, `Shaders/auto_exposure_*.slang`). Written only against `Renderer::prepare_compute_kernel`,
    /// `record_compute_kernel`, `prepare_fullscreen_effect` and graph textures, so an application can use it as
    /// is, feed it its own colour, or copy it.
    struct AutoExposureDescription {
        /// Scene-linear colour to meter and expose.
        RenderGraphTextureHandle source{};
        Core::Extent2D extent{};
        RHI::Extent3D output_extent{};
        RHI::Format output_format = RHI::Format::Undefined;
        /// Which persistent exposure state to adapt (the engine passes the window id).
        u64 history_key = 0;
    };

    struct AutoExposureResult {
        /// `source` multiplied by the exposure.
        RenderGraphTextureHandle exposed{};
        /// 1x1 R32Float texture holding the exposure multiplier that was applied.
        RenderGraphTextureHandle exposure{};
    };

    /// Number of 32x32-pixel metering tiles per axis.
    [[nodiscard]] glm::uvec2 auto_exposure_tile_extent(Core::Extent2D extent) noexcept;

    /// Adds the passes. Reads `settings.auto_exposure`.
    [[nodiscard]] Core::RendererExpected<AutoExposureResult> add_auto_exposure_passes(
        Renderer &renderer, RHI::RhiDevice &device, AutoExposureHistory &history, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, const AutoExposureDescription &description,
        const RenderGraphSettings &settings);

    /// The engine's `auto_exposure` frame feature: meters `SceneHdrColor` and republishes the exposed colour.
    /// Register it (or your own) with `FramePipeline::add` / `replace`.
    [[nodiscard]] Core::RendererResult build_auto_exposure_feature(FrameBuildContext &frame);

} // namespace SFT::Renderer
