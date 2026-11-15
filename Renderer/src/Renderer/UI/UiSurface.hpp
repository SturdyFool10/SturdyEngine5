#pragma once

#include <atomic>

#include <Foundation/Foundation.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Async/Mutex.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/UiInput.hpp>
#include <Renderer/UI/UiRenderer.hpp>

namespace SFT::UI {

    /// One UI: a layout `Context`, the `UiRenderer` that draws it, and the `UiInput` that drives it. It owns no
    /// opinion about where it is shown or who is using it.
    ///
    /// **On screen** (the default path): `finish_overlay()` returns a `Renderer::OverlayPass` you append to
    /// `RenderFrameParameters::overlay_passes`; feed `input()` from window events (`Engine::ScreenUi` does
    /// this).
    ///
    /// **In the world**: render the UI into an off-screen target instead (an overlay-only frame with this
    /// overlay, at the target's extent), sample the target's texture on a mesh, and feed `input()` from whatever
    /// the user points with (`UI::ui_uv_at_ray` turns a ray into a pointer position). Nothing else changes.
    ///
    /// Typical frame:
    /// ```cpp
    /// ui.ensure_ready(device, color_format);
    /// UI::Context &ctx = ui.begin_frame(viewport, dt);       // consumes input(), clears its one-frame edges
    /// ... build widgets with ctx ...
    /// params.overlay_passes.push_back(ui.finish_overlay(renderer));
    /// ```
    struct UiOverlayOptions {
        std::string name = "ui";
        /// HDR displays only: scales the reference white the UI is composed at.
        f32 hdr_reference_white_scale = 1.0f;
        /// HDR displays only: when above zero, the reference white in nits the UI is composed at, instead of the
        /// compositor's SDR white.
        f32 hdr_paper_white_nits = 0.0f;
    };

    /// How a `UiSurface` maps its layout units onto the framebuffer.
    enum class UiScaleMode : u8 {
        /// Layout units are logical: one unit covers `content_scale` pixels, so the UI keeps the same
        /// physical size on every display and text is rasterized at full resolution. The default.
        ContentScale,
        /// Layout units are framebuffer pixels regardless of content scale (pixel-art UI, off-screen
        /// targets whose resolution is the design size, debug overlays).
        RawPixels,
    };

    class UiSurface {
      public:
        using OverlayOptions = UiOverlayOptions;

        /// Creates the context and renderer on first use (and again after `destroy`). `color_format` is the format
        /// of the target the overlay will draw into (`OverlayPassContext::format`). Returns false, and logs why,
        /// when the GPU objects cannot be created.
        [[nodiscard]] bool ensure_ready(RHI::RhiDevice &device, RHI::Format color_format);
        [[nodiscard]] bool ready() const;
        /// The colour format the renderer was built for, or `Undefined` before there is one.
        [[nodiscard]] RHI::Format renderer_format() const noexcept { return renderer_state_->format.load(); }

        /// Creates the layout context only (no GPU objects), so a UI can be laid out before a device exists or with
        /// none at all (headless trees, tests). `ensure_ready` does this too. Returns false, and logs why, on failure.
        [[nodiscard]] bool ensure_context();

        [[nodiscard]] Context &context() noexcept { return context_; }
        [[nodiscard]] UiInput &input() noexcept { return input_; }
        [[nodiscard]] const UiInput &input() const noexcept { return input_; }

        /// Sets the display's content scale (1.0 = 100%). `Engine::ScreenUi` feeds this from the window snapshot;
        /// other owners should do the same for on-screen surfaces. Ignored in `UiScaleMode::RawPixels`.
        void set_content_scale(f32 scale) noexcept { content_scale_ = scale; }
        [[nodiscard]] f32 content_scale() const noexcept { return content_scale_; }

        /// Chooses between logical-unit (default) and raw-pixel layout.
        void set_scale_mode(UiScaleMode mode) noexcept { scale_mode_ = mode; }
        [[nodiscard]] UiScaleMode scale_mode() const noexcept { return scale_mode_; }

        /// Framebuffer pixels per layout unit for the current mode.
        [[nodiscard]] f32 pixel_scale() const noexcept {
            return scale_mode_ == UiScaleMode::RawPixels || !(content_scale_ > 0.0f) ? 1.0f : content_scale_;
        }

        /// Starts a layout at `extent` framebuffer pixels using the accumulated input (whose pointer positions are
        /// framebuffer pixels too), then clears the input's one-frame edges and records whether the UI wants the
        /// pointer (`input().pointer_consumed()`). Layout itself runs in `extent / pixel_scale()` units.
        Context &begin_frame(glm::vec2 extent, f32 delta_seconds);

        /// The renderer whose textures image masks read for `HitTestMode::Fixed` (the pixels of a texture made from data). Optional; set it once.
        void set_texture_resolver(Renderer::Renderer *resolver) noexcept { texture_resolver_ = resolver; }

        /// Finishes the frame and returns the overlay that draws it. `texture_resolver` is the renderer the UI's
        /// images resolve textures through (and where glow passes come from).
        [[nodiscard]] Renderer::OverlayPass finish_overlay(Renderer::Renderer *texture_resolver, OverlayOptions options = {});

        /// Finishes the frame and returns one overlay per UI layer, lowest first (`UiEffect::layer`, `Effects::layer`). Append them all
        /// to the frame's overlay passes in order: a layer's effects can read (`Backdrop`) everything the lower layers drew. With no
        /// layered element this is the one overlay `finish_overlay` returns. Layers above the first build their renderers on first use.
        [[nodiscard]] std::vector<Renderer::OverlayPass> finish_overlays(Renderer::Renderer *texture_resolver, OverlayOptions options = {});

        /// Like `finish_overlay`, for a snapshot you finished yourself.
        [[nodiscard]] Renderer::OverlayPass overlay_for_snapshot(std::shared_ptr<FrameSnapshot> snapshot,
                                                                 Renderer::Renderer *texture_resolver,
                                                                 OverlayOptions options = {});

        /// The overlay that draws `snapshot`, with no effect on `input()`. Unlike `overlay_for_snapshot` this does not end the
        /// input's text frame, so the same snapshot can be drawn on several frames (a UI that was not rebuilt) or attached
        /// later than it was finished. The caller ends the text frame once, when it finishes the layout.
        [[nodiscard]] Renderer::OverlayPass make_overlay(std::shared_ptr<FrameSnapshot> snapshot, Renderer::Renderer *texture_resolver,
                                                         OverlayOptions options = {}) const;

        /// A side overlay that draws `snapshot` into an off-screen render target of its own, clearing it to `clear_color` first (straight alpha),
        /// as passes the frame's graph schedules like any others: several of these in one frame are independent of each other, so the graph
        /// records them in parallel. Works for single-mip targets in a format the UI renderer was built for (`ensure_ready` with the
        /// target's format); anything else draws nothing and the caller renders that target the ordinary way. The overlay is a
        /// `prepare_only` overlay: it adds nothing to the frame's own target.
        [[nodiscard]] Renderer::OverlayPass make_target_overlay(std::shared_ptr<FrameSnapshot> snapshot, Renderer::Renderer *texture_resolver,
                                                                Renderer::OffscreenRenderTargetHandle target, glm::vec4 clear_color,
                                                                OverlayOptions options = {}) const;

        /// The overlays for the pieces `FrameSnapshot::split_layers` made, lowest first, without ending the input's text frame.
        [[nodiscard]] std::vector<Renderer::OverlayPass> make_layer_overlays(std::vector<std::shared_ptr<FrameSnapshot>> pieces,
                                                                             Renderer::Renderer *texture_resolver,
                                                                             OverlayOptions options = {});

        /// What the renderer built for the most recent frame (draws, textures, bytes uploaded), or zeros before there is one.
        [[nodiscard]] UiRenderStats render_stats() const;

        /// Releases the GPU renderer only (a graphics reconstruction, an HDR toggle). The layout `Context` - fonts,
        /// text caches, widget state - is kept, so nothing has to be re-registered, and snapshots already handed to
        /// the render thread stay valid (they refer into the context). An overlay whose renderer has gone draws
        /// nothing. `ensure_ready` recreates the renderer.
        void release_renderer(RHI::RhiDevice &device) noexcept;

        /// Tears everything down, context included; call before the device goes away for good, once no frame that
        /// used this surface is still in flight. `ensure_ready` starts over.
        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        struct RendererState {
            Async::Mutex<std::optional<UiRenderer>> renderer;
            /// Colour format the renderer's pipelines were built for; the overlay's pass must draw into the same one.
            std::atomic<RHI::Format> format{RHI::Format::Undefined};
            /// `UiQuadPipeline::bindless_preferred()` when the renderer was built; a change rebuilds it.
            std::atomic<bool> bindless_preferred{true};
        };

        Context context_{};
        UiInput input_{};
        std::shared_ptr<RendererState> renderer_state_ = std::make_shared<RendererState>();
        /// Renderers of the layers above the first (index 0 is layer piece 1), built when their overlay is first prepared.
        std::vector<std::shared_ptr<RendererState>> layer_states_;

        [[nodiscard]] Renderer::OverlayPass make_overlay_for(std::shared_ptr<RendererState> state, bool create_on_demand,
                                                             std::shared_ptr<FrameSnapshot> snapshot,
                                                             Renderer::Renderer *texture_resolver, OverlayOptions options) const;
        Renderer::Renderer *texture_resolver_ = nullptr;
        f32 content_scale_ = 1.0f;
        UiScaleMode scale_mode_ = UiScaleMode::ContentScale;
        bool context_created_ = false;
        bool create_attempted_ = false;
    };

} // namespace SFT::UI
