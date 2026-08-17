#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <deque>
#include <glm/vec2.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
using std::string_view;
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
    // pointer button (left mouse / touch). `pressed`/`released`/`cancelled` are one-frame-latched
    // transitions: backends should set them when an edge occurred since the previous UI frame,
    // even if a complete press+release happened between frames. Context still derives missing edges
    // from consecutive `down` values, so existing callers that only populate `down` keep working.
    struct PointerState {
        glm::vec2 position{0.0f};
        bool down = false;
        bool pressed = false;
        // Exact press location for a latched edge. Omit only when `pressed` is false; Context falls
        // back to `position` for compatibility with manually constructed PointerState values.
        std::optional<glm::vec2> press_position;
        bool released = false;
        bool cancelled = false;
        // This frame's accumulated mouse-wheel delta (Clay's own convention: positive scrolls
        // content down/right). One-shot like a drained event, not sticky state — the caller's own
        // accumulator (e.g. Engine::UiPointerState) should reset it after begin_layout() consumes
        // it, the same way MouseWheelEvent itself is a delta, not a position.
        glm::vec2 scroll_delta{0.0f};
    };

    // A named element's unclipped layout box in root-viewport pixel coordinates. Context caches
    // these from the immediately previous completed frame rather than exposing Clay's persistent
    // element table directly (which can retain entries older than one frame).
    struct ElementBounds {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
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

    // Identifies one glyph within one specific registered font face (primary/emoji/fallback) for
    // Context's outline_cache_ — see that member's own doc comment for why this can't be packed
    // into a single integer by shifting.
    struct OutlineCacheKey {
        u64 font_id = 0;
        u32 glyph_id = 0;
        [[nodiscard]] friend constexpr bool operator==(const OutlineCacheKey &, const OutlineCacheKey &) = default;
    };

    struct OutlineCacheKeyHash {
        [[nodiscard]] usize operator()(const OutlineCacheKey &key) const noexcept {
            u64 hashed = key.font_id;
            hashed ^= static_cast<u64>(key.glyph_id) + 0x9e3779b97f4a7c15ULL + (hashed << 6) + (hashed >> 2);
            return static_cast<usize>(hashed);
        }
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

        // Pixel extent used when this immutable layout snapshot was produced. Overlay consumers use
        // this to reject accidentally drawing a window-sized layout into a differently-sized off-screen
        // endpoint; prepare/draw viewport constants cannot retroactively reflow Clay's baked geometry.
        [[nodiscard]] Core::Extent2D viewport_extent() const noexcept {
            return Core::Extent2D{full_viewport_scissor_.width, full_viewport_scissor_.height};
        }

        // Read-only access to this frame's resolved draw list — lets a test assert on scissor/
        // position/paint-order behavior (e.g. that a floating element attached inside a clipped
        // ancestor actually got that ancestor's scissor rect, not the full viewport) without any GPU
        // involved. UiRenderer still reaches quads_ directly via friendship for the real upload path.
        [[nodiscard]] std::span<const QuadDraw> quads() const noexcept { return quads_; }

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

        // `font`/`emoji_fallback`/every font in `fallbacks` must outlive every later frame that
        // references `font_id` — see TextBridge::register_font's own doc comment for what
        // `fallbacks` is for (per-application, coverage-driven fonts for glyphs `font` lacks — CJK,
        // Arabic, or anything else, not hardcoded to one category).
        void register_font(FontId font_id, const Text::Font &font, const Text::Font *emoji_fallback = nullptr,
                           std::span<const Text::Font *const> fallbacks = {});

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

        // Governs drag-to-scroll and wheel smoothing for every scroll container in this Context —
        // see ScrollSettings' own doc comment (Style.hpp). Takes effect on the very next
        // begin_layout() call; there is no separate "apply" step. Defaults match ScrollSettings{}
        // (drag-scroll off, wheel smoothing on), so a Context that never calls this already behaves
        // as if it had.
        void set_scroll_settings(const ScrollSettings &settings) noexcept { scroll_settings_ = settings; }
        [[nodiscard]] const ScrollSettings &scroll_settings() const noexcept { return scroll_settings_; }

        // Global kill switch for the whole cursor-icon system below — "similar to how the web
        // works" per the feature request this backs: every element() call still *carries* its own
        // ElementDecl::cursor exactly like a DOM node carries a CSS `cursor` style whether or not
        // the page ever reads it, but nothing here ever calls into Platform on the caller's behalf
        // either way (see desired_cursor()'s own doc comment) — this only controls whether
        // desired_cursor() bothers computing anything beyond CursorIcon::Default, for an app that
        // wants to manage the OS cursor itself (e.g. a game's own custom crosshair) without UI
        // fighting it every frame. On by default.
        void set_cursor_management_enabled(bool enabled) noexcept { cursor_management_enabled_ = enabled; }
        [[nodiscard]] bool cursor_management_enabled() const noexcept { return cursor_management_enabled_; }

        // This frame's resolved cursor shape: whichever hovered element's ElementDecl::cursor was
        // most recently declared (later declarations win over earlier ones for the same point,
        // which is how a child naturally overrides its own ancestor's cursor — same "more specific
        // wins" feel as CSS's own cursor cascade), or CursorIcon::Default if nothing hovered set
        // one. Always CursorIcon::Default when cursor_management_enabled() is false. Purely a query
        // — Context itself never touches the OS cursor; a caller applies this to the actual window
        // (Platform::Windowing::Window::set_cursor_icon(), translating UI::CursorIcon to
        // Platform::Windowing::CursorIcon — the two are kept value-for-value identical for exactly
        // this translation) once per frame, after finish_frame().
        [[nodiscard]] CursorIcon desired_cursor() const noexcept { return desired_cursor_; }

        // Sets desired_cursor() for the remainder of this frame unconditionally, bypassing the
        // normal hover gate update_desired_cursor() applies to every ordinary element() call. For
        // an element whose active drag can carry the pointer outside its own hitbox within a
        // single frame (a resize divider under a fast/high-poll-rate mouse can jump clean over a
        // thin divider between two polled frames), the geometric per-frame hover test alone can't
        // keep the cursor pinned to it — call this once the drag's own capture state confirms it's
        // still active, instead of depending on hovered() having caught the pointer this frame.
        // Still subject to cursor_management_enabled() the same way desired_cursor() is.
        void force_cursor(CursorIcon icon) noexcept;

        [[nodiscard]] ElementScope element(const ElementDecl &decl);
        void text(const ustr &content, const TextStyle &style);
        void image(const ElementDecl &decl, Renderer::TextureHandle texture);

        // UTF-8 byte offset into `utf8_content` whose glyph-cluster boundary sits closest to
        // `local_x` pixels from the text's own left edge (0 = before the first character), were
        // `utf8_content` shaped at `style`'s font/size/spacing — the click-to-position hit test
        // TextInput.hpp/TextArea.hpp build their caret-placement-on-click behavior on (see
        // TextEditState::set_caret_to()'s own doc comment, which this replaces the "click always
        // jumps to end" v1 simplification for). Shapes through the same TextBridge cache
        // Context::text() itself uses, so calling this once per click and then rendering the exact
        // same (style, content) pair the normal way costs nothing extra on top of the render itself.
        // Returns 0 for empty content or an unregistered style.font_id, and utf8_content.size() when
        // local_x is at or past the shaped text's total width — both matching set_caret_to()'s own
        // clamp-to-buffer-bounds behavior once translated through UString::scalar_index_of_byte().
        [[nodiscard]] usize hit_test_text_byte_offset(const TextStyle &style, string_view utf8_content, f32 local_x);

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

        [[nodiscard]] glm::vec2 pointer_position() const noexcept { return pointer_position_; }
        [[nodiscard]] bool pointer_is_down() const noexcept { return pointer_down_; }
        [[nodiscard]] bool pointer_pressed_this_frame() const noexcept { return pointer_pressed_this_frame_; }
        [[nodiscard]] bool pointer_released_this_frame() const noexcept { return pointer_released_this_frame_; }
        [[nodiscard]] bool pointer_cancelled_this_frame() const noexcept { return pointer_cancelled_this_frame_; }

        // The named element's box from the immediately previous completed frame. std::nullopt means
        // that id was not declared in that frame. Geometry is deliberately floating-point so range
        // controls do not lose sub-pixel precision while mapping pointer positions to values.
        [[nodiscard]] std::optional<ElementBounds> element_bounds(const UString &id) const noexcept;

        // Exclusive primary-pointer capture shared by every widget in this Context. Capture lets a
        // drag continue after leaving its hit box while preventing overlapping controls from both
        // starting the same gesture. The owner should release on pointer-up; finish_frame() also
        // drops orphaned capture automatically when the pointer is no longer down.
        [[nodiscard]] bool try_capture_pointer(const UString &id) noexcept;
        [[nodiscard]] bool has_pointer_capture(const UString &id) const noexcept;
        [[nodiscard]] bool pointer_captured() const noexcept { return !pointer_capture_id_.empty(); }
        void release_pointer(const UString &id) noexcept;

        // One keyboard-focus owner per Context. Widgets acquire focus on pointer interaction or via
        // focus(); callers can therefore route backend-neutral key intents without multiple controls
        // responding at once.
        void focus(const UString &id);
        [[nodiscard]] bool has_focus(const UString &id) const noexcept;
        [[nodiscard]] bool focused() const noexcept { return !focused_id_.empty(); }
        void clear_focus(const UString &id) noexcept;

        // True if this frame's pointer position falls within any *named* element's bounding box
        // from the last committed layout. Named elements are the interaction surface of this API;
        // restricting this query to them also excludes Clay's generated full-viewport root, which
        // would otherwise make Engine input appear consumed everywhere.
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

        // Read-only geometry of a scroll container (ElementDecl::clip::horizontal/vertical), for
        // building an external scrollbar against it — mirrors Clay_ScrollContainerData, whose own
        // doc comment (clay.h) calls this exact use case out by name ("external functionality that
        // modifies scroll position, such as scroll bars"). `found` is false for an id nothing
        // declared with clip enabled, same one-frame-stale caveat as element_bounds().
        struct ScrollMetrics {
            bool found = false;
            // Clay's own convention: always <= 0: 0 is scrolled fully to the start, more negative is
            // further toward the end.
            glm::vec2 offset{0.0f};
            glm::vec2 content_size{0.0f};
            glm::vec2 container_size{0.0f};
            bool horizontal = false;
            bool vertical = false;
        };
        [[nodiscard]] ScrollMetrics scroll_metrics(const UString &container_id) const noexcept;

        // Directly sets a scroll container's offset (clamped to its own valid range per axis, and
        // only on the axes it actually scrolls) — the write counterpart to scroll_metrics(), for a
        // scrollbar thumb drag or track-click page-jump to apply immediately rather than only ever
        // nudging by a relative delta (scroll_into_view()'s shape). Same safe-to-call-mid-frame
        // mechanism as scroll_into_view() (Clay_GetScrollContainerData's returned scrollPosition is a
        // pointer straight at Clay's own live state, not a snapshot — see its own doc comment). A
        // no-op (returns false) for an id nothing declared with clip enabled this frame.
        bool set_scroll_offset(const UString &container_id, glm::vec2 offset) noexcept;

        // Finishes the layout tree (Clay_EndLayout()), walks the resulting render commands, shapes
        // any new text via TextBridge, and returns everything UiRenderer needs as an owned,
        // Context-independent FrameSnapshot. The snapshot records begin_layout()'s actual baked
        // extent; callers cannot accidentally relabel window-sized geometry as target-sized here.
        // Must run on the same thread that built this frame's tree; the returned snapshot itself is
        // then safe to hand to another thread.
        [[nodiscard]] FrameSnapshot finish_frame();
        // Source-compatible legacy overload. Layout dimensions are owned by begin_layout(); this
        // argument is intentionally ignored so endpoint validation still sees the true baked extent.
        [[nodiscard]] FrameSnapshot finish_frame(glm::vec2) { return finish_frame(); }

        void destroy() noexcept;

      private:
        void set_current() const noexcept;
        // Shared by element()/image()/custom_element() — see desired_cursor_'s own doc comment.
        void update_desired_cursor(const ElementDecl &decl) noexcept;

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
        // Keyed by (font_id, glyph_id) — mirrors Renderer/RendererTextOverlay.cpp's own per-glyph
        // outline cache, just keyed across every font this Context has registered instead of one.
        // `font_id` here is itself a composite key (TextBridge::register_font packs which face —
        // primary/emoji/fallback — into its own high bits), so it cannot be packed into a single
        // u64 alongside glyph_id by shifting: shifting a value that already uses its own upper 32
        // bits discards exactly the bits that distinguish one registered face from another,
        // collapsing every face of the same registration onto the same key whenever glyph_id
        // matches. OutlineCacheKey/Hash below combine the two fields properly instead.
        unordered_map<OutlineCacheKey, Text::GlyphOutline, OutlineCacheKeyHash> outline_cache_;
        Core::Extent2D layout_extent_{1, 1};

        // Current pointer sample plus latched transitions, set by begin_layout().
        glm::vec2 pointer_position_{0.0f};
        glm::vec2 pointer_press_position_{0.0f};
        bool pointer_down_ = false;
        bool pointer_pressed_this_frame_ = false;
        bool pointer_released_this_frame_ = false;
        bool pointer_cancelled_this_frame_ = false;
        string pointer_capture_id_;
        string focused_id_;

        // IDs declared during the frame being built and unclipped bounds copied from the immediately
        // previous completed frame. Clay's own element table is persistent and can answer with older
        // geometry, so interactive widgets must only read this cache through element_bounds().
        vector<string> current_frame_ids_;
        unordered_map<string, ElementBounds> last_frame_bounds_;

        // IDs of this frame's clip-enabled (scroll-container) elements, and each one's scroll
        // position as of the immediately previous completed frame — same one-frame-stale contract
        // as last_frame_bounds_/hovered()/clicked(), and for the same underlying reason: querying
        // Clay_GetScrollContainerData() for `decl.id` *while that same element is still being
        // opened* (between Clay__OpenElement() and Clay__ConfigureOpenElement() — exactly where
        // element()'s clip handling would otherwise want to read it) finds a clip config that either
        // doesn't exist yet this frame or is still the previous frame's, and Clay's own
        // Clay__FindElementConfigWithType() lookup inside that call fails as a result, silently
        // reporting `found = false` — so an element() call can never safely query its *own*
        // just-opened self this way. Querying once, safely, right after Clay_EndLayout() (see
        // finish_frame()) and feeding the result forward to next frame's element() call sidesteps it
        // entirely, the same trick hovered()/clicked() already rely on.
        vector<string> current_frame_clip_ids_;
        unordered_map<string, glm::vec2> last_frame_scroll_offsets_;

        // See set_scroll_settings()'s doc comment. pending_scroll_delta_ is begin_layout()'s
        // smoothing accumulator: wheel input not yet handed to Clay because ScrollSettings::
        // smooth_scrolling is spreading it across frames — see begin_layout()'s implementation.
        ScrollSettings scroll_settings_{};
        glm::vec2 pending_scroll_delta_{0.0f};

        // See set_cursor_management_enabled()/desired_cursor()'s own doc comments. desired_cursor_
        // is rebuilt fresh every begin_layout() (reset to Default) and updated in element() as this
        // frame's elements get declared and hover-tested — already resolved and correct for this
        // exact frame's pointer position by the time the caller reads it, no extra frame of latency
        // (unlike hovered()/clicked(), the *underlying* hit-test is one-frame-stale, but there's no
        // second stage here that adds another frame on top of that).
        bool cursor_management_enabled_ = true;
        CursorIcon desired_cursor_ = CursorIcon::Default;

        // The z (ElementDecl::z, Style.hpp) every element()/text()/image()/svg()/custom_element()
        // call currently inherits — always non-empty, back() is "whichever nonzero z was most
        // recently opened and not yet closed, or 0 at the root." element()/custom_element() push
        // their own effective z (decl.z if nonzero, else the current back()) when they open and an
        // ElementScope pops it on close; text()/image()/svg() just read back() directly since they
        // never open a scope of their own.
        vector<i32> z_stack_{0};
    };

} // namespace SFT::UI
