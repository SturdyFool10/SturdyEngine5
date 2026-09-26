#pragma once

#include <Foundation/Foundation.hpp>

#include <memory>
#include <optional>
#include <string>

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
    };

    class UiSurface {
      public:
        using OverlayOptions = UiOverlayOptions;

        /// Creates the context and renderer on first use (and again after `destroy`). `color_format` is the format
        /// of the target the overlay will draw into (`OverlayPassContext::format`). Returns false, and logs why,
        /// when the GPU objects cannot be created.
        [[nodiscard]] bool ensure_ready(RHI::RhiDevice &device, RHI::Format color_format);
        [[nodiscard]] bool ready() const;

        [[nodiscard]] Context &context() noexcept { return context_; }
        [[nodiscard]] UiInput &input() noexcept { return input_; }
        [[nodiscard]] const UiInput &input() const noexcept { return input_; }

        /// Starts a layout at `extent` pixels using the accumulated input, then clears the input's one-frame edges
        /// and records whether the UI wants the pointer (`input().pointer_consumed()`).
        Context &begin_frame(glm::vec2 extent, f32 delta_seconds);

        /// Finishes the frame and returns the overlay that draws it. `texture_resolver` is the renderer the UI's
        /// images resolve textures through (and where glow passes come from).
        [[nodiscard]] Renderer::OverlayPass finish_overlay(Renderer::Renderer *texture_resolver, OverlayOptions options = {});

        /// Like `finish_overlay`, for a snapshot you finished yourself.
        [[nodiscard]] Renderer::OverlayPass overlay_for_snapshot(std::shared_ptr<FrameSnapshot> snapshot,
                                                                 Renderer::Renderer *texture_resolver,
                                                                 OverlayOptions options = {});

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
        };

        Context context_{};
        UiInput input_{};
        std::shared_ptr<RendererState> renderer_state_ = std::make_shared<RendererState>();
        bool context_created_ = false;
        bool create_attempted_ = false;
    };

} // namespace SFT::UI
