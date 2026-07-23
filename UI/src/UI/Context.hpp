#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <deque>
#include <glm/vec2.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Handles.hpp>
#include <Renderer/TextInstance.hpp>
#include <Text/Text.hpp>

#include "Style.hpp"
#include "TextBridge.hpp"
#include "UiQuad.hpp"

using std::deque;
using std::string;
using std::unordered_map;
using std::vector;

// Clay's C API (clay.h) is an implementation detail of UI — forward-declared here rather than
// included, so nothing outside ContextImpl.cpp ever sees a Clay type, including UiRenderer:
// Context::finish_frame() below does all of Clay's tree-walking/text-shaping synchronously and
// returns a plain, Clay-free FrameSnapshot.
struct Clay_Context;

namespace SFT::UI {

    // Opaque image reference an image() call carries through to a Clay IMAGE render command.
    struct ImageRef {
        Renderer::TextureHandle texture{};
    };

    // This frame's pointer input, in the same pixel space as begin_layout()'s viewport_size — the
    // caller (game loop, ECS system, editor tooling) owns sourcing this from whatever input system
    // it already has; UI itself stays input-backend-agnostic (see UI.hpp's own doc comment on why
    // it doesn't depend on Sturdy.Ecs). `down` is the raw current-frame state of the primary
    // pointer button (left mouse / touch), not an edge trigger — Context derives press/click edges
    // from consecutive frames' `down` values itself.
    struct PointerState {
        glm::vec2 position{0.0f};
        bool down = false;
    };

    // RAII scope for one open Clay element: opens on construction, closes on destruction — the
    // ordinary-C++ equivalent of Clay's `CLAY(){ ... }` macro pairing (see clay.h's own comment on
    // what that macro expands to: Clay__OpenElement()/Clay__ConfigureOpenElement() then, after the
    // scope's children, Clay__CloseElement()). Move-only so exactly one close happens per open.
    class ElementScope {
      public:
        ElementScope(const ElementScope &) = delete;
        ElementScope &operator=(const ElementScope &) = delete;
        ElementScope(ElementScope &&other) noexcept;
        ElementScope &operator=(ElementScope &&other) noexcept;
        ~ElementScope();

      private:
        friend class Context;
        explicit ElementScope(Clay_Context *context) noexcept : context_(context) {}

        Clay_Context *context_ = nullptr;
    };

    // One quad render command (RECTANGLE/BORDER/IMAGE) already resolved out of Clay's output.
    // `image_ref` is non-null only for IMAGE commands — UiRenderer resolves it to an actual texture
    // view at prepare() time (that resolution is Renderer-state-dependent, not Context-state-
    // dependent, so it stays deferred rather than being folded in here).
    struct QuadDraw {
        UiQuadInstance instance;
        const ImageRef *image_ref = nullptr;
        RHI::Rect2D scissor{};
    };

    // A fully resolved, Clay-free, Context-independent snapshot of one finished layout —
    // Context::finish_frame()'s result. Building this (walking Clay's render commands, shaping any
    // new text via TextBridge) touches Context's own mutable state (its Clay arena, TextBridge's
    // shape cache, the scratch storage backing ImageRef/text pointers) and must happen on the
    // thread that called begin_layout()/element()/text(); once built, a FrameSnapshot owns
    // everything it references and is safe to hand off to another thread (e.g. Engine's dedicated
    // render thread, via Renderer::UiOverlayHooks — see plans/clay-ui-renderer.md) for the actual
    // GPU upload/draw work in UiRenderer::prepare()/draw(). Move-only, one-shot.
    class FrameSnapshot {
      public:
        FrameSnapshot() noexcept = default;
        FrameSnapshot(const FrameSnapshot &) = delete;
        FrameSnapshot &operator=(const FrameSnapshot &) = delete;
        FrameSnapshot(FrameSnapshot &&) noexcept = default;
        FrameSnapshot &operator=(FrameSnapshot &&) noexcept = default;
        ~FrameSnapshot() = default;

      private:
        friend class Context;
        friend class UiRenderer;

        vector<QuadDraw> quads_;
        vector<Renderer::GlyphPlacement> glyphs_;
        // Keeps every QuadDraw::image_ref pointer above valid for the snapshot's lifetime —
        // Context's own image_storage_ is cleared on its very next begin_layout().
        deque<ImageRef> image_storage_;
        RHI::Rect2D full_viewport_scissor_{};
    };

    // Owns one Clay layout context (its backing arena + Clay_Context*) plus the font registrations
    // that back Clay's text-measurement callback (TextBridge). Not thread-safe, and — since Clay
    // itself keeps one process-global "current context" pointer rather than taking an explicit
    // context argument on every call — not safe to interleave calls from two Contexts on the same
    // thread without each call going through this wrapper (every method here re-asserts itself as
    // current before touching Clay). One Context per UI surface (e.g. one per window); rebuild its
    // tree fresh every frame between begin_layout() and finish_frame(), both on the same thread.
    class Context {
      public:
        Context() noexcept = default;
        Context(const Context &) = delete;
        Context &operator=(const Context &) = delete;
        Context(Context &&other) noexcept;
        Context &operator=(Context &&other) noexcept;
        ~Context();

        struct Config {
            usize arena_capacity_bytes = 0; // 0 = Clay_MinMemorySize()
            u32 max_element_count = 8192;
        };

        // Config has no default argument here deliberately: a default member initializer used from
        // a default *function* argument needs the enclosing class (Context) already complete,
        // which it isn't yet at this point in its own definition. Pass Config{} explicitly.
        [[nodiscard]] static Core::RendererExpected<Context> create(const Config &config);

        // `font`/`emoji_fallback` must outlive every later frame that references `font_id`.
        void register_font(FontId font_id, const Text::Font &font, const Text::Font *emoji_fallback = nullptr);

        // Starts a new layout tree. Clears the previous frame's image/text scratch storage — call
        // only after any previous frame's FrameSnapshot has already been produced by
        // finish_frame(), since a snapshot takes over that storage rather than copying it.
        // `pointer` feeds Clay's own hit-testing (Clay_SetPointerState) against *last* frame's
        // already-committed layout, which is why hovered()/clicked() below are answerable
        // immediately, even before this frame has declared a single element.
        void begin_layout(glm::vec2 viewport_size, const PointerState &pointer = {});

        [[nodiscard]] ElementScope element(const ElementDecl &decl);
        void text(const ustr &content, const TextStyle &style);
        void image(const ElementDecl &decl, Renderer::TextureHandle texture);

        // True if this frame's pointer position falls within the bounding box the element
        // declared with ElementDecl::id == `id` occupied *last* frame — one-frame-stale like all
        // immediate-mode hit-testing here, and false for an id nothing declared last frame (e.g.
        // its first frame, or a typo). Safe to call before that element's own element() call this
        // frame (see begin_layout()'s doc comment) — e.g. to pick a hover background color before
        // building the ElementDecl.
        [[nodiscard]] bool hovered(const UString &id) const noexcept;

        // True on exactly the one frame the pointer transitions from up to down while hovered(id)
        // is true — a discrete click event, not "currently held" (poll pointer_down() for that).
        [[nodiscard]] bool clicked(const UString &id) const noexcept;

        // True while the pointer button is currently held down and hovered(id) is true.
        [[nodiscard]] bool pointer_down(const UString &id) const noexcept;

        // Finishes the layout tree (Clay_EndLayout()), walks the resulting render commands, shapes
        // any new text via TextBridge, and returns everything UiRenderer needs as an owned,
        // Context-independent FrameSnapshot. Must run on the same thread that built this frame's
        // tree; the returned snapshot itself is then safe to hand to another thread.
        [[nodiscard]] FrameSnapshot finish_frame(glm::vec2 viewport_size);

        void destroy() noexcept;

      private:
        void set_current() const noexcept;

        Clay_Context *context_ = nullptr;
        vector<std::byte> arena_memory_;
        TextBridge text_bridge_;
        // Frame-scoped storage for text bytes (Clay_String::chars is non-owning) and ImageRef
        // payloads handed through Clay's void* customData/imageData — stable for the frame,
        // cleared on the next begin_layout(). deque (not vector) so growth never invalidates a
        // pointer already handed to Clay this frame. image_storage_ is *moved* into
        // FrameSnapshot by finish_frame() rather than cleared, since QuadDraw::image_ref points
        // into it.
        deque<string> text_storage_;
        deque<ImageRef> image_storage_;
        // Keyed by (font_id << 32 | glyph_id) — mirrors
        // Renderer/RendererTextOverlay.cpp's own per-glyph outline cache, just keyed across every
        // font this Context has registered instead of one.
        unordered_map<u64, Text::GlyphOutline> outline_cache_;

        // This/last frame's raw pointer-button state, set by begin_layout() — clicked()'s edge
        // trigger is derived from the transition between these two, not from Clay's own internal
        // pointer state machine (which begin_layout() also drives via Clay_SetPointerState(), but
        // doesn't expose a public getter for).
        bool pointer_down_ = false;
        bool pointer_pressed_this_frame_ = false;
    };

} // namespace SFT::UI
