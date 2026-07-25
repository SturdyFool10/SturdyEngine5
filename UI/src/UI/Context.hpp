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

#include "CustomElement.hpp"
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

    // Opaque image reference an image()/svg() call carries through to a Clay IMAGE render command —
    // both go through the same Clay command type (Clay has no separate concept) and the same
    // UiQuadKind::Image draw path: an SVG-sourced texture (Engine::UiSvgCache, backed by the
    // vendored lunasvg library) is rasterized to a plain RGBA8 bitmap ahead of time, so by the time
    // it reaches here it's indistinguishable from any other image texture.
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
        // This frame's accumulated mouse-wheel delta (Clay's own convention: positive scrolls
        // content down/right). One-shot like a drained event, not sticky state — the caller's own
        // accumulator (e.g. Engine::UiPointerState) should reset it after begin_layout() consumes
        // it, the same way MouseWheelEvent itself is a delta, not a position.
        glm::vec2 scroll_delta{0.0f};
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
        explicit ElementScope(Clay_Context *context, vector<i32> *z_stack) noexcept
            : context_(context), z_stack_(z_stack) {}

        Clay_Context *context_ = nullptr;
        // Popped on close (see ~ElementScope()) — the other half of the push element() does onto
        // Context::z_stack_ when it opens. Null for scopes that don't touch it (there are none
        // today; kept nullable so a future leaf-ish scope could opt out without an awkward dummy
        // stack).
        vector<i32> *z_stack_ = nullptr;
    };

    // One quad render command (RECTANGLE/BORDER/IMAGE) already resolved out of Clay's output.
    // `image_ref` is non-null only for IMAGE commands — UiRenderer resolves it to an actual texture
    // view at prepare() time (that resolution is Renderer-state-dependent, not Context-state-
    // dependent, so it stays deferred rather than being folded in here).
    struct QuadDraw {
        UiQuadInstance instance;
        const ImageRef *image_ref = nullptr;
        RHI::Rect2D scissor{};
        PaintKey paint{};
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
        // Parallel to glyphs_ (glyph_scissors_[i] is the clip rect glyphs_[i] was placed under) —
        // a separate array rather than a field on GlyphPlacement itself since GlyphPlacement is a
        // Renderer::-level "what glyph, where" shape shared by non-UI callers (the debug text
        // overlay, offscreen TextRenderTarget/TextCanvas) that have no notion of nested clipping;
        // mirrors QuadDraw::scissor's role for quads_ one level up.
        vector<RHI::Rect2D> glyph_scissors_;
        // Parallel to glyphs_ too — see PaintKey's own doc comment (Style.hpp) for the depth system
        // this drives. Every glyph belonging to the same original Clay TEXT command shares one
        // PaintKey (they're never reordered relative to each other, only as a unit relative to
        // other draw items), which is also what lets UiRenderer regroup them back into contiguous
        // per-command runs.
        vector<PaintKey> glyph_paint_;
        vector<CustomDraw> custom_draws_;
        // Keeps every QuadDraw::image_ref/CustomDraw::shader pointer above valid for the snapshot's
        // lifetime — Context's own image_storage_/custom_storage_ are cleared on its very next
        // begin_layout().
        deque<ImageRef> image_storage_;
        deque<CustomShaderRef> custom_storage_;
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
        // immediately, even before this frame has declared a single element. Also drives Clay's
        // scroll-container tracking (Clay_UpdateScrollContainers) from `pointer.scroll_delta`,
        // which is what any element() call with ClipConfig::horizontal/vertical set reads back via
        // Clay_GetScrollOffset() — `delta_seconds` is only used for Clay's internal drag-scroll
        // momentum easing, not layout itself, so a rough estimate is fine.
        void begin_layout(glm::vec2 viewport_size, const PointerState &pointer = {}, f32 delta_seconds = 0.0f);

        [[nodiscard]] ElementScope element(const ElementDecl &decl);
        void text(const ustr &content, const TextStyle &style);
        void image(const ElementDecl &decl, Renderer::TextureHandle texture);

        // Draws a rasterized SVG icon (Engine::UiSvgCache rasterizes the source file via the
        // vendored lunasvg library into `texture` — see UI/src/UI/Svg/SvgIcon.hpp for what's
        // supported). Purely a same-signature naming wrapper over image() — kept as its own entry
        // point for call-site clarity ("this texture came from an SVG"), since by the time a
        // texture reaches here it's an ordinary RGBA8 bitmap, drawn through the exact same path.
        void svg(const ElementDecl &decl, Renderer::TextureHandle texture);

        // Shader-driven styling (plans/clay-ui-renderer.md Phase 3): an element drawn by `shader`
        // instead of the generic rect/image pipeline — see CustomElement.hpp's own doc comments for
        // the shader-authoring contract. Returns an ElementScope like element() (not a leaf like
        // image()), so a custom-shaded panel can still have ordinary children.
        [[nodiscard]] ElementScope custom_element(const ElementDecl &decl, const CustomShaderRef &shader);

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

        // True if this frame's pointer position falls within *any* element's bounding box from
        // last frame's committed layout, regardless of whether that element declared an id — the
        // generic version of hovered(id), meant for "should a click here be treated as UI input at
        // all" precedence decisions (e.g. Engine::UiContext::begin_layout()'s consumed flag) rather
        // than answering for one specific widget.
        [[nodiscard]] bool pointer_over_any() const noexcept;

        // True on exactly the one frame the pointer transitions from up to down while *not*
        // hovering the element with ElementDecl::id == `id` — the click-outside-to-defocus signal
        // text_input()/text_area() (TextEdit.hpp) use to drop focus when the user clicks anything
        // else, including empty space. Note this is true for a click on any *other* widget too, not
        // just empty space — callers that need to distinguish those should check hovered() on the
        // specific ids they care about instead.
        [[nodiscard]] bool clicked_outside(const UString &id) const noexcept;

        // Nudges the scroll container with ElementDecl::id == `container_id` (an element with
        // ClipConfig::horizontal/vertical set) by however much is needed — on whichever axes that
        // container actually scrolls — to bring the element with ElementDecl::id == `target_id`
        // (declared somewhere inside it) fully into view, clamped to the container's own scrollable
        // range. A no-op (returns false) if either id doesn't match an element from *last* frame's
        // committed layout — same one-frame-stale convention as hovered()/clicked() — which is
        // harmless: it just means this frame doesn't adjust the scroll position, not that it snaps
        // somewhere wrong. Built for text_input()/text_area()'s scroll-to-caret behavior
        // (TextEdit.hpp) but not specific to text — anything that wants "keep this child visible"
        // can use it.
        bool scroll_into_view(const UString &container_id, const UString &target_id) noexcept;

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
        deque<CustomShaderRef> custom_storage_;
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

        // The z (ElementDecl::z, Style.hpp) every element()/text()/image()/svg()/custom_element()
        // call currently inherits — always non-empty, back() is "whichever nonzero z was most
        // recently opened and not yet closed, or 0 at the root." element()/custom_element() push
        // their own effective z (decl.z if nonzero, else the current back()) when they open and an
        // ElementScope pops it on close; text()/image()/svg() just read back() directly since they
        // never open a scope of their own.
        vector<i32> z_stack_{0};
    };

} // namespace SFT::UI
