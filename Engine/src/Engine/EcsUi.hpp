#pragma once

#include <Foundation/src/Foundation.hpp>

#include "AssetManager.hpp"

#include <Async/src/Mutex.hpp>
#include <Core/Core.hpp>
#include <Ecs/src/Resource.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Renderer.hpp>
#include <UI/UI.hpp>

#include "EcsEvents.hpp"
#include "WindowRequests.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/// Engine-level ECS integration for Sturdy.UI: UI itself stays Ecs-agnostic (see UI.hpp's own doc
/// comment), so this is the seam that lets an ECS app build UI trees through ordinary systems
/// instead of hand-rolling the pointer-input plumbing and prepare()/draw() glue every consumer
/// would otherwise duplicate — see plans/clay-ui-renderer.md's Phase 2 status note.
namespace SFT::Engine {

    /// Accumulated primary-pointer state (position + primary-button-down), kept current by a
    /// built-in system Engine registers in its own constructor (folding MouseMoveEvent/
    /// MouseButtonEvent, the same streams a consumer may use for camera controls). This is the
    /// minimal slice of the still-unbuilt InputState (plans/ecs-engine-subsystem-access.md) that
    /// UI hit-testing actually needs — not a general input-state resource. Read via
    /// Ecs::ReadResource<UiPointerState>; hand `.state()` straight to UI::Context::begin_layout().
    class UiPointerState {
      public:
        void set_position(glm::vec2 position) noexcept;
        void set_down(bool down) noexcept;
        void cancel() noexcept;
        void clear_transitions() noexcept;
        /// MouseWheelEvent is a delta, not a position, so this accumulates (multiple wheel events can
        /// land in one frame) rather than overwriting — cleared by UiContext::begin_layout() below
        /// once it's been folded into Clay's scroll tracking, the same one-shot lifetime as an
        /// ordinary Events<T> read.
        void add_scroll_delta(glm::vec2 delta) noexcept;
        void clear_scroll_delta() noexcept;
        [[nodiscard]] const UI::PointerState &state() const noexcept;

        /// Whether this frame's pointer position landed on any UI element, per
        /// UI::Context::pointer_over_any() — set by UiContext::begin_layout() below once the UI tree
        /// that owns this pointer has actually hit-tested it. Gameplay systems (fly-camera look,
        /// click-to-interact, etc.) should check this before consuming raw mouse input, the same
        /// input-precedence contract plans/clay-ui-renderer.md flags as previously missing. One-frame
        /// stale like hovered()/clicked() themselves: it reflects whichever begin_layout() call ran
        /// last, which for a typical app runs once per frame after gameplay input systems already
        /// consumed this frame's raw events — see that ordering's own note in EngineImpl.cpp.
        void set_consumed(bool consumed) noexcept;
        [[nodiscard]] bool consumed() const noexcept;

      private:
        UI::PointerState state_{};
        bool consumed_ = false;
    };

    /// Accumulated keyboard/text/IME state for Sturdy.UI's text-editing widgets (UI::text_input()/
    /// UI::text_area(), built on UI::TextEdit.hpp), kept current by a built-in system Engine
    /// registers in its own constructor — the same "fold this tick's typed events into a resource so
    /// no consumer has to duplicate it" role UiPointerState fills for pointer input. Before this
    /// existed, every app wanting text-input/IME support had to hand-roll its own EditKey decoding
    /// and composition-state bridging (as Demos/UiWorkbenchGameLogic/WorkbenchUi.cpp originally did);
    /// this is that logic, written once. Single global instance, not per-window — same simplification
    /// UiPointerState already makes; a genuinely multi-window app that needs per-window text-input
    /// state still rolls its own the way WorkbenchUi.cpp's per-Surface pointer state does.
    class UiTextInputState {
      public:
        /// A committed character/string this tick — always ends whatever composition preceded it,
        /// matching Engine::InputState::apply(const TextInputEvent&)'s own rule (composition and a
        /// commit are mutually exclusive within the same instant).
        void apply(const TextInputEvent &event) noexcept;
        /// This tick's in-progress IME composition (preedit) update — an empty `event.text.utf8`
        /// means composition just ended (SDL3's own documented contract, see WindowTextEditingEvent's
        /// doc comment).
        void apply(const TextEditingEvent &event) noexcept;
        /// Decodes one raw keyboard event into UI::EditKey (Ctrl+A/C/X/V -> SelectAll/Copy/Cut/Paste,
        /// arrows/Home/End/Backspace/Delete/Enter/Escape -> themselves), tracking held Shift/Ctrl
        /// across calls. OS key-repeat naturally produces more than one Left/Backspace/etc. per
        /// second, which is why this appends rather than overwriting — UI::TextEditInput::keys is a
        /// vector for exactly this reason.
        void apply_key(const KeyboardEvent &event) noexcept;

        /// Packages this tick's accumulated state into a ready-to-use UI::TextEditInput. Clipboard
        /// callbacks are taken per-call rather than stored, since they close over a live
        /// Platform::Windowing::Window& this resource doesn't own (same reasoning
        /// WorkbenchUi.cpp's own clipboard wiring already follows) — pass nullptr for either to make
        /// Copy/Cut/Paste silent no-ops. The returned TextEditInput borrows this object's own
        /// buffers, so it must be consumed (fed into text_input()/text_area()) before the next
        /// apply()/apply_key()/clear_transitions() call.
        [[nodiscard]] UI::TextEditInput frame_input(
            std::function<UString()> get_clipboard_text = nullptr,
            std::function<void(const UString &)> set_clipboard_text = nullptr) const noexcept;

        /// Clears this tick's transient typed-text/key-press accumulation after a consumer has read
        /// it via frame_input() — composition state is intentionally NOT cleared here (it persists
        /// across frames until apply(const TextEditingEvent&) reports it changed, the same "still
        /// composing" contract UI::TextEditInput::composing documents), matching UiPointerState::
        /// clear_transitions()'s identical split between per-tick and persistent fields.
        void clear_transitions() noexcept;

      private:
        std::string typed_text_;
        std::string composition_text_;
        bool composing_ = false;
        vector<UI::EditKey> keys_;
        bool shift_down_ = false;
        bool ctrl_down_ = false;
    };

    /// Whichever text field is focused this frame, if any — a caller resolves this itself (no
    /// cross-widget focus registry exists in UI::TextEdit.hpp by design, see that file's own note on
    /// EditKey::Tab), then hands it to forward_text_input_state() below. `field_bounds` is the whole
    /// field's box (not just the caret point) — see forward_text_input_state()'s own doc comment for
    /// why. `ime_enabled` should mirror whatever TextEditStyle::features.ime_enabled the focused
    /// field itself was drawn with (UI::TextEditFeatures::ime_enabled, UI/TextEdit.hpp).
    struct TextInputFocusInfo {
        UI::ElementBounds field_bounds;
        UI::ElementBounds caret_bounds;
        bool ime_enabled = true;
    };

    /// Forwards this frame's text-input focus state to the OS: positions the IME composition
    /// candidate window near the focused field (Platform::Windowing::Window::set_text_input_area(),
    /// "native input methods may place a window with word suggestions near the cursor, without
    /// covering the text being entered") and toggles start_text_input()/stop_text_input() to match
    /// whether a field is actually focused and IME-enabled. Call this once per window per frame,
    /// after building this frame's UI, with `focus` set to whichever field (if any) ended up
    /// focused. Passing std::nullopt, or a focused field with `ime_enabled = false`, stops text
    /// input for that window — without this, IME composition mode stays globally on for the whole
    /// window regardless of focus (SDL3's SDL_StartTextInput is only ever called once, at window
    /// creation), so a field opting out of IME (a password/numeric/hotkey-capture field) had no way
    /// to actually suppress the OS candidate window before this existed.
    void forward_text_input_state(WindowRequests &requests, Platform::Windowing::WindowId window,
                                   std::optional<TextInputFocusInfo> focus) noexcept;

    /// Owns one UI::Context + UI::UiRenderer pair as an ordinary World resource
    /// (Ecs::WriteResource<UiContext>), so any system with resource access can build/query a UI
    /// tree — not just whichever code happens to own the Engine instance. Font registration stays
    /// the caller's job (font choice/discovery is app policy, not Engine policy); this only owns
    /// the two GPU-backed objects every UI consumer needs regardless of what it draws.
    class UiContext {
        struct UiRendererState {
            Async::Mutex<std::optional<UI::UiRenderer>> renderer;
        };

      public:
        /// Lazily creates the UI::Context/UI::UiRenderer pair against `device`/`color_format`.
        /// Needs a live RHI device, which isn't necessarily up before the first frame — safe to
        /// call every frame; a cheap no-op once ready() (or once creation has already failed once,
        /// to avoid retrying a hard failure every single frame).
        [[nodiscard]] bool ensure_ready(RHI::RhiDevice &device, RHI::Format color_format);

        [[nodiscard]] bool ready() const;
        [[nodiscard]] UI::Context &context() noexcept;

        /// Thin wrapper over UI::Context::begin_layout() that also keeps `pointer_state`'s consumed
        /// flag and scroll accumulator in sync — folding both into this one call (rather than leaving
        /// callers to remember them, the way the raw two-arg Context::begin_layout() would) is what
        /// makes the input-precedence contract structural instead of convention-based: any system that
        /// builds a UI frame through here automatically updates what gameplay input-precedence checks
        /// read. Callers should prefer this over calling context().begin_layout() directly whenever
        /// `pointer_state` came from Engine's own UiPointerState resource.
        void begin_layout(glm::vec2 viewport_size, UiPointerState &pointer_state, f32 delta_seconds);

        /// Packages an already-finished FrameSnapshot (UI::Context::finish_frame()'s result) into
        /// the RenderGraph's UiOverlayHooks seam (Renderer::Scene.hpp) — the exact prepare()/draw()
        /// glue every consumer of this UI package would otherwise write by hand. `snapshot` is
        /// shared (not moved) since the returned hooks' `prepare`/`draw` closures both need to
        /// outlive this call, holding it alive until the frame graph actually runs them.
        [[nodiscard]] Renderer::UiOverlayHooks build_overlay_hooks(std::shared_ptr<UI::FrameSnapshot> snapshot,
                                                                    Renderer::Renderer *texture_resolver);

        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        UI::Context context_{};
        std::shared_ptr<UiRendererState> renderer_state_ = std::make_shared<UiRendererState>();
        bool create_attempted_ = false;
    };

    /// UI-facing image cache: UI::Context::image() (UI/) needs a raw Renderer::TextureHandle,
    /// resolved fresh every frame an app declares an image element — since Sturdy.UI is immediate-
    /// mode and rebuilds its whole tree every frame, calling AssetManager::load_texture() directly
    /// from a UI-building system would decode and re-upload the same file from disk on every single
    /// frame. This cache makes a repeat resolve() call for the same (path, color_space) an O(1)
    /// lookup instead of a decode — the concrete "large UI tree" cost this exists to avoid.
    ///
    /// Synchronous: a large image's first resolve() call still blocks the calling frame on disk I/O +
    /// decode. An async/streamed path (matching plans/texture-streaming research's direction) isn't
    /// built here since no concrete need for it has come up yet.
    class UiImageCache {
      public:
        [[nodiscard]] AssetExpected<Renderer::TextureHandle> resolve(
            AssetManager &assets, const std::filesystem::path &path,
            TextureColorSpace color_space = TextureColorSpace::Srgb);

        /// Drops every cached entry without unloading the underlying AssetManager assets — callers
        /// that also own the AssetManager decide asset lifetime themselves; this only forgets the
        /// path->handle mapping (e.g. useful for a hot-reload/asset-changed-on-disk workflow).
        void clear() noexcept;

      private:
        struct Entry {
            Asset asset{};
            Renderer::TextureHandle handle{};
        };
        std::unordered_map<std::string, Entry> by_key_;
    };

    /// UI-facing SVG icon cache: loads+rasterizes a source file once per distinct (path, target_px)
    /// tuple (UI::Svg::rasterize_svg_file() — a thin wrapper over the vendored lunasvg library, see
    /// its own doc comment for why this package no longer hand-rolls SVG parsing) and uploads the
    /// result once — repeat resolve() calls are an O(1) lookup, the same "immediate-mode UI rebuilds
    /// its tree every frame" reasoning UiImageCache documents. Deliberately shaped identically to
    /// UiImageCache::resolve() (AssetManager-backed, same AssetExpected<TextureHandle> return) now
    /// that a rasterized SVG is just another RGBA8 bitmap asset, not a special SDF case.
    class UiSvgCache {
      public:
        [[nodiscard]] AssetExpected<Renderer::TextureHandle> resolve(
            AssetManager &assets, const std::filesystem::path &path, f32 target_px);

        /// Drops every cached path->handle entry — same non-destructive contract as
        /// UiImageCache::clear() (the underlying AssetManager asset isn't unloaded here).
        void clear() noexcept;

      private:
        std::unordered_map<std::string, Renderer::TextureHandle> by_key_;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::UiPointerState, "sturdy.engine.ui_pointer_state");
SFT_ECS_RESOURCE(SFT::Engine::UiTextInputState, "sturdy.engine.ui_text_input_state");
SFT_ECS_RESOURCE(SFT::Engine::UiContext, "sturdy.engine.ui_context");
SFT_ECS_RESOURCE(SFT::Engine::UiImageCache, "sturdy.engine.ui_image_cache");
SFT_ECS_RESOURCE(SFT::Engine::UiSvgCache, "sturdy.engine.ui_svg_cache");
