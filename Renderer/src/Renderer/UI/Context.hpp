#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <deque>
#include <memory>
#include <source_location>
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
#include <Renderer/Text/Text.hpp>

#include <Renderer/UI/CustomElement.hpp>
#include <Renderer/UI/Style.hpp>
#include <Renderer/UI/UiDiagnostics.hpp>
#include <Renderer/UI/TextBridge.hpp>
#include <Renderer/UI/UiFill.hpp>
#include <Renderer/UI/UiQuad.hpp>
#include <Renderer/UI/UiSector.hpp>
#include <Renderer/UI/UiStroke.hpp>

using std::deque;
using std::string;
using std::string_view;
using std::unordered_map;
using std::vector;


struct Clay_Context;

namespace SFT::UI {


    /// How an image element samples its texture.
    enum class ImageSampling : u8 {
        /// Bilinear (photos, scaled thumbnails).
        Linear,
        /// Nearest-neighbour: crisp pixels when scaled up (pixel art, Minecraft skins and item icons).
        Nearest,
    };

    struct ImageRef {
        Renderer::TextureHandle texture{};
        ImageSampling sampling = ImageSampling::Linear;
    };


    struct PointerState {
        glm::vec2 position{0.0f};
        bool down = false;
        bool pressed = false;


        std::optional<glm::vec2> press_position;
        bool released = false;
        bool cancelled = false;


        glm::vec2 scroll_delta{0.0f};
    };


    struct ElementBounds {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
    };


    class ElementScope {
      public:
        /// Disables this construction form for `ElementScope`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ElementScope(const ElementScope &) = delete;
        /// Assigns a new value to this `ElementScope`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ElementScope &operator=(const ElementScope &) = delete;
        /// Constructs a `ElementScope` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function does not throw exceptions.
        ElementScope(ElementScope &&other) noexcept;
        /// Assigns a new value to this `ElementScope`.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        ElementScope &operator=(ElementScope &&other) noexcept;
        /// Destroys the `ElementScope` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~ElementScope();

      private:
        friend class Context;
        /// Constructs a `ElementScope` from the supplied initialization values.
        ///
        /// @param context Context that supplies state required by the operation.
        /// @param z_stack `z_stack` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit ElementScope(Clay_Context *context, vector<i32> *z_stack) noexcept;

        Clay_Context *context_ = nullptr;


        vector<i32> *z_stack_ = nullptr;
    };


    struct OutlineCacheKey {
        u64 font_id = 0;
        u32 glyph_id = 0;
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] friend constexpr bool operator==(const OutlineCacheKey &, const OutlineCacheKey &) = default;
    };

    struct OutlineCacheKeyHash {
        /// Invokes the callable behavior provided by `OutlineCacheKeyHash`.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize operator()(const OutlineCacheKey &key) const noexcept;
    };


    struct QuadDraw {
        UiQuadInstance instance;
        const ImageRef *image_ref = nullptr;
        RHI::Rect2D scissor{};
        PaintKey paint{};
    };


    // A resolved Context::stroke_paths()/stroke_polyline() call — unlike QuadDraw/CustomDraw, this
    // owns its `paths` outright (already offset into absolute pixel space) rather than pointing back
    // into a Context-owned deque, so FrameSnapshot doesn't need to keep a matching storage deque alive
    // alongside it the way image_storage_/custom_storage_ do.
    struct StrokeDraw {
        vector<StrokePath> paths;
        RHI::Rect2D scissor{};
        PaintKey paint{};
    };


    // A resolved Context::fill_quads() call — same ownership rationale as StrokeDraw (see its own
    // doc comment).
    struct FillQuadDraw {
        vector<FillQuad> quads;
        RHI::Rect2D scissor{};
        PaintKey paint{};
    };


    // A resolved Context::fill_sectors() call — same ownership rationale as StrokeDraw (see its own
    // doc comment).
    struct SectorDraw {
        vector<Sector> sectors;
        RHI::Rect2D scissor{};
        PaintKey paint{};
    };


    class FrameSnapshot {
      public:
        /// Constructs a `FrameSnapshot` in its default state.
        ///
        /// @note This function does not throw exceptions.
        FrameSnapshot() noexcept = default;
        /// Disables this construction form for `FrameSnapshot`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        FrameSnapshot(const FrameSnapshot &) = delete;
        /// Assigns a new value to this `FrameSnapshot`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        FrameSnapshot &operator=(const FrameSnapshot &) = delete;
        /// Constructs a `FrameSnapshot` from another instance.
        ///
        /// @note This function does not throw exceptions.
        FrameSnapshot(FrameSnapshot &&) noexcept = default;
        /// Assigns a new value to this `FrameSnapshot`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        FrameSnapshot &operator=(FrameSnapshot &&) noexcept = default;
        /// Destroys the `FrameSnapshot` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~FrameSnapshot() = default;


        /// Returns the current or globally available viewport extent value.
        ///
        /// @return Returns the current viewport extent value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Core::Extent2D viewport_extent() const noexcept;


        /// Returns the current or globally available quads value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::span<const QuadDraw> quads() const noexcept;


        /// Returns the current or globally available strokes value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::span<const StrokeDraw> strokes() const noexcept;


        /// Returns the current or globally available fills value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::span<const FillQuadDraw> fills() const noexcept;


        /// Returns the current or globally available sectors value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::span<const SectorDraw> sectors() const noexcept;


        /// Returns the current or globally available custom strokes value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::span<const CustomStrokeDraw> custom_strokes() const noexcept;

      private:
        friend class Context;
        friend class UiRenderer;

        vector<QuadDraw> quads_;
        vector<StrokeDraw> strokes_;
        vector<FillQuadDraw> fills_;
        vector<SectorDraw> sectors_;
        vector<CustomStrokeDraw> custom_strokes_;
        vector<Renderer::GlyphPlacement> glyphs_;


        vector<RHI::Rect2D> glyph_scissors_;


        vector<PaintKey> glyph_paint_;
        vector<CustomDraw> custom_draws_;


        deque<ImageRef> image_storage_;
        deque<CustomShaderRef> custom_storage_;
        RHI::Rect2D full_viewport_scissor_{};
    };


    class Context {
      public:
        /// Constructs a `Context` in its default state.
        ///
        /// @note This function does not throw exceptions.
        Context() noexcept = default;
        /// Disables this construction form for `Context`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Context(const Context &) = delete;
        /// Assigns a new value to this `Context`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Context &operator=(const Context &) = delete;
        /// Constructs a `Context` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function does not throw exceptions.
        Context(Context &&other) noexcept;
        /// Assigns a new value to this `Context`.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        Context &operator=(Context &&other) noexcept;
        /// Destroys the `Context` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~Context();

        struct Config {
            /// Non-zero selects a fixed-size context with exactly this many bytes, which can never grow (and truncates a layout that does not
            /// fit it). Zero (the default) selects a growable context: it sizes itself to the layout, between frames, up to `memory_limit_bytes`.
            usize arena_capacity_bytes = 0;
            /// Starting element budget. A growable context grows past it (and, with `allow_shrink`, returns to it) as the UI needs; a
            /// fixed context keeps it as a hard limit.
            u32 max_element_count = 2048;
            /// Most memory a growable context may hold at once. 0 selects 1 GiB. Past it the layout is truncated (and reported) rather than
            /// growing further.
            usize memory_limit_bytes = 0;
            /// Give memory back after the layout has needed far less than it holds for about half a minute.
            bool allow_shrink = true;
            /// When text asks for font id 0 and nothing was registered under it, load the embedded
            /// default font instead of silently drawing nothing. Turn off for layouts that must not
            /// depend on glyph metrics (e.g. tests with hard-coded pointer positions).
            bool load_default_font = true;
        };


        /// Creates a `Context` resource or value from the supplied parameters.
        ///
        /// @param config Configuration values controlling the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] static Core::RendererExpected<Context> create(const Config &config);

        // ---- debug view -----------------------------------------------------------------------------------------------------------------
        //
        // An inspector the layout engine can draw over the UI (the element tree, sizes, configs, warnings). Whether it exists at all, and
        // how a user reaches it (a key, a menu item, a command line flag, an environment variable, never), is the application's decision:
        // the engine never turns it on by itself and binds no key. Safe to switch at any time, including mid-session; the view appears from
        // the next `begin_layout`, narrowing the layout area by the panel width.

        void set_debug_view_enabled(bool enabled) noexcept;
        [[nodiscard]] bool debug_view_enabled() const noexcept;

        // ---- diagnostics ----------------------------------------------------------------------------------------------------------------

        /// Everything that went wrong in this context, as data. See `UiDiagnostics`.
        [[nodiscard]] UiDiagnostics &diagnostics() noexcept { return diagnostics_->store; }
        [[nodiscard]] const UiDiagnostics &diagnostics() const noexcept { return diagnostics_->store; }

        /// How much memory the layout engine holds and how much the last frame used of each buffer.
        struct MemoryStats {
            bool growable = false;
            /// The last finished frame left something out for lack of room (see the diagnostics for what and how much).
            bool truncated_last_frame = false;
            usize bytes_reserved = 0;
            usize bytes_limit = 0;
            u32 element_budget = 0;
            u32 grow_count = 0;
            u32 shrink_count = 0;
            /// What the last frame asked for against what each buffer holds. "Asked for" counts what was rejected too.
            u32 elements_used = 0, elements_capacity = 0;
            u32 configs_used = 0, configs_capacity = 0;
            u32 text_lines_used = 0, text_lines_capacity = 0;
            u32 draw_commands_used = 0, draw_commands_capacity = 0;
        };
        [[nodiscard]] MemoryStats memory_stats() const noexcept;


        /// Registers font using the supplied arguments and current state.
        ///
        /// @param font_id Identifier of the target object or resource.
        /// @param font `font` value used by the operation.
        /// @param emoji_fallback Fallback value used when the primary value is unavailable.
        /// @param fallbacks Fallback value used when the primary value is unavailable.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void register_font(FontId font_id, const Text::Font &font, const Text::Font *emoji_fallback = nullptr,
                           std::span<const Text::Font *const> fallbacks = {});

        /// Forgets `font_id`. Registering a different font under an id that is in use is also fine at any time: the
        /// text caches start over for that id, so a live font change re-shapes on the next frame.
        void unregister_font(FontId font_id);

        /// OpenType features for all text in `font_id` (`kern,liga,-calt,onum`); see `TextBridge::set_font_features`.
        void set_font_features(FontId font_id, string_view comma_separated_features);

        /// Forces SDF or MSDF glyph rasterisation for all non-colour UI text, or lets the size decide (`Auto`). Takes
        /// effect from the next frame; the formats have separate atlases so nothing is invalidated.
        void set_text_raster_preference(Text::RasterPreference preference) noexcept { text_raster_preference_ = preference; }
        [[nodiscard]] Text::RasterPreference text_raster_preference() const noexcept { return text_raster_preference_; }


        /// Starts a layout frame.
        ///
        /// @param viewport_size Target size in framebuffer pixels. Layout runs in
        ///        `viewport_size / pixel_scale()` logical units.
        /// @param pointer Pointer state in framebuffer pixels; converted to logical units here.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void begin_layout(glm::vec2 viewport_size, const PointerState &pointer = {}, f32 delta_seconds = 0.0f,
                          std::source_location site = std::source_location::current());


        /// Returns the current layout viewport in logical UI pixels.
        ///
        /// Widgets use this to constrain floating content such as menus to the visible context.
        [[nodiscard]] glm::vec2 viewport_size() const noexcept { return layout_size_; }


        /// Sets how many framebuffer pixels one logical layout unit covers (the display's content
        /// scale for on-screen UI; 1 for raw-pixel UI). Widget sizes, font sizes, element bounds and
        /// `pointer_position()` are all in logical units; `finish_frame()` emits pixel geometry and
        /// rasterizes text at the scaled size so it stays sharp. Takes effect at the next `begin_layout()`.
        ///
        /// @param scale Pixels per logical unit; non-finite or non-positive values are treated as 1.
        ///
        /// @note This function does not throw exceptions.
        void set_pixel_scale(f32 scale) noexcept;

        /// Returns the pixel scale set with `set_pixel_scale()`.
        ///
        /// @return Framebuffer pixels per logical unit.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 pixel_scale() const noexcept { return pixel_scale_; }


        /// Sets the scroll settings for this `Context`.
        ///
        /// @param settings Configuration values controlling the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_scroll_settings(const ScrollSettings &settings) noexcept;
        /// Scrolls settings using the supplied arguments and current state.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const ScrollSettings &scroll_settings() const noexcept;


        /// Sets the cursor management enabled for this `Context`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @note This function does not throw exceptions.
        void set_cursor_management_enabled(bool enabled) noexcept;
        /// Returns the current or globally available cursor management enabled value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool cursor_management_enabled() const noexcept;


        /// Returns the current or globally available desired cursor value.
        ///
        /// @return Returns the current desired cursor value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CursorIcon desired_cursor() const noexcept;


        /// Performs the force cursor operation for `Context` using the supplied arguments.
        ///
        /// @param icon `icon` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void force_cursor(CursorIcon icon) noexcept;

        /// Performs the element operation for `Context` using the supplied arguments.
        ///
        /// @param decl `decl` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ElementScope element(const ElementDecl &decl, std::source_location site = std::source_location::current());
        /// Performs the text operation for `Context` using the supplied arguments.
        ///
        /// @param content `content` value used by the operation.
        /// @param style `style` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void text(const ustr &content, const TextStyle &style, std::source_location site = std::source_location::current());
        /// Performs the image operation for `Context` using the supplied arguments.
        ///
        /// @param decl `decl` value used by the operation.
        /// @param texture Texture used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void image(const ElementDecl &decl, Renderer::TextureHandle texture, ImageSampling sampling = ImageSampling::Linear,
                   std::source_location site = std::source_location::current());


        /// Computes the hit test text byte offset required by the supplied values.
        ///
        /// @param style `style` value used by the operation.
        /// @param utf8_content `utf8_content` value used by the operation.
        /// @param local_x `local_x` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize hit_test_text_byte_offset(const TextStyle &style, string_view utf8_content, f32 local_x);

        /// Width and height in logical pixels of `utf8_content` laid out on one line with `style`, before any wrapping
        /// (the shaped line's natural size). Zero for empty text or an unregistered font. Usable before layout, for
        /// truncation and sizing decisions.
        [[nodiscard]] glm::vec2 measure_text(const TextStyle &style, string_view utf8_content);


        /// Performs the svg operation for `Context` using the supplied arguments.
        ///
        /// @param decl `decl` value used by the operation.
        /// @param texture Texture used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void svg(const ElementDecl &decl, Renderer::TextureHandle texture);


        /// Performs the custom element operation for `Context` using the supplied arguments.
        ///
        /// @param decl `decl` value used by the operation.
        /// @param shader Shader used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ElementScope custom_element(const ElementDecl &decl, const CustomShaderRef &shader,
                                                 std::source_location site = std::source_location::current());


        /// Draws one or more anti-aliased polylines (see Shaders/ui_stroke.slang) as a single leaf
        /// element sized/positioned by `decl`, same as image()/svg(). Each path's `points` are in the
        /// element's own local space (relative to its own top-left, e.g. [0, width] x [0, height] for
        /// a chart's plot area) — resolved to absolute pixel space once the element's own layout is
        /// known, at Context::finish_frame() time. All paths share one bounding box/scissor/paint
        /// order, which is what lets a chart widget draw axis lines, gridlines, and every data series
        /// inside the same plot rect from a single call — see StrokePolylineData's own doc comment
        /// (UI/UiStroke.hpp) for why that's a single call instead of one leaf element per line.
        ///
        /// @param decl `decl` value used by the operation.
        /// @param paths Paths used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void stroke_paths(const ElementDecl &decl, std::span<const StrokePath> paths);

        /// Convenience wrapper around stroke_paths() for a single polyline.
        ///
        /// @param decl `decl` value used by the operation.
        /// @param points Points used or affected by the operation.
        /// @param style `style` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void stroke_polyline(const ElementDecl &decl, std::span<const glm::vec2> points, const StrokeStyle &style);


        /// Draws one or more filled (optionally rounded) rects as a single leaf element sized/
        /// positioned by `decl`, same as stroke_paths() but for solid fills instead of lines — reuses
        /// UiQuadPipeline directly rather than a new pipeline (see UiRenderer.cpp's
        /// PaintEntry::Kind::Fill handling). Each quad's `position`/`size` are in the element's own
        /// local space, resolved to absolute pixel space at Context::finish_frame() time. See
        /// FillQuadListData's own doc comment (UI/UiFill.hpp) for why multiple quads share one call.
        ///
        /// @param decl `decl` value used by the operation.
        /// @param quads Quads used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void fill_quads(const ElementDecl &decl, std::span<const FillQuad> quads);


        /// Draws one or more annular sectors ("pie slices") as a single leaf element sized/positioned
        /// by `decl`, same as fill_quads()/stroke_paths() but via UiSectorPipeline instead. Each
        /// sector's `center` is in the element's own local space, resolved to absolute pixel space at
        /// Context::finish_frame() time. See SectorListData's own doc comment (UI/UiSector.hpp) for
        /// why multiple sectors share one call.
        ///
        /// @param decl `decl` value used by the operation.
        /// @param sectors Sectors used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void fill_sectors(const ElementDecl &decl, std::span<const Sector> sectors);


        /// Draws a polyline through a caller-supplied fragment shader instead of ui_stroke.slang, as
        /// a single leaf element sized/positioned by `decl` — the stroke-shaped sibling of
        /// custom_element(). See CustomStrokeElementData's own doc comment (UI/CustomElement.hpp) for
        /// the unbatched-per-segment cost model and `shader`'s vertexMain contract.
        ///
        /// @param decl `decl` value used by the operation.
        /// @param points Points used or affected by the operation.
        /// @param half_width Requested or available size for the operation.
        /// @param feather_px Requested or available size for the operation.
        /// @param shader Shader used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void stroke_custom(const ElementDecl &decl, std::span<const glm::vec2> points, f32 half_width,
                           f32 feather_px, const CustomShaderRef &shader);


        /// Performs the hovered operation for `Context` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool hovered(const UString &id) const noexcept;


        /// Performs the clicked operation for `Context` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool clicked(const UString &id) const noexcept;


        /// Performs the pointer down operation for `Context` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool pointer_down(const UString &id) const noexcept;

        /// Returns the current or globally available pointer position value.
        ///
        /// @return Returns the current pointer position value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec2 pointer_position() const noexcept;
        /// Reports whether pointer is down.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool pointer_is_down() const noexcept;
        /// Returns the current or globally available pointer pressed this frame value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool pointer_pressed_this_frame() const noexcept;
        /// Returns the current or globally available pointer released this frame value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool pointer_released_this_frame() const noexcept;
        /// Returns the current or globally available pointer cancelled this frame value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool pointer_cancelled_this_frame() const noexcept;


        /// Performs the element bounds operation for `Context` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<ElementBounds> element_bounds(const UString &id) const noexcept;


        /// Attempts to capture pointer without requiring normal failure to be exceptional.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns `true` when the operation succeeds; otherwise returns `false`.
        /// @note Normal failure is reported by returning `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool try_capture_pointer(const UString &id) noexcept;
        /// Reports whether this `Context` has pointer capture.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has_pointer_capture(const UString &id) const noexcept;
        /// Returns the current or globally available pointer captured value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool pointer_captured() const noexcept;
        /// Releases pointer using the supplied arguments and current state.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void release_pointer(const UString &id) noexcept;


        /// Performs the focus operation for `Context` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void focus(const UString &id);
        /// Reports whether this `Context` has focus.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has_focus(const UString &id) const noexcept;
        /// Returns the current or globally available focused value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool focused() const noexcept;
        /// Clears focus.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void clear_focus(const UString &id) noexcept;


        /// Returns the current or globally available pointer over any value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool pointer_over_any() const noexcept;


        /// Performs the clicked outside operation for `Context` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool clicked_outside(const UString &id) const noexcept;


        /// Scrolls into view using the supplied arguments and current state.
        ///
        /// @param container_id Identifier of the target object or resource.
        /// @param target_id Identifier of the target object or resource.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        bool scroll_into_view(const UString &container_id, const UString &target_id) noexcept;


        struct ScrollMetrics {
            bool found = false;


            glm::vec2 offset{0.0f};
            glm::vec2 content_size{0.0f};
            glm::vec2 container_size{0.0f};
            bool horizontal = false;
            bool vertical = false;
        };
        /// Scrolls metrics using the supplied arguments and current state.
        ///
        /// @param container_id Identifier of the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ScrollMetrics scroll_metrics(const UString &container_id) const noexcept;


        /// Computes the set scroll offset required by the supplied values.
        ///
        /// @param container_id Identifier of the target object or resource.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        bool set_scroll_offset(const UString &container_id, glm::vec2 offset) noexcept;


        /// Returns the current or globally available finish frame value.
        ///
        /// @return Returns the current finish frame value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] FrameSnapshot finish_frame(std::source_location site = std::source_location::current());


        /// Performs the finish frame operation for `Context` using the supplied arguments.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] FrameSnapshot finish_frame(glm::vec2);

        /// Destroys or releases the `Context` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        /// Sets the current for this `Context`.
        ///
        /// @note This function does not throw exceptions.
        void set_current() const noexcept;

        /// Updates desired cursor from the supplied values.
        ///
        /// @param decl `decl` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void update_desired_cursor(const ElementDecl &decl) noexcept;

        /// Converts a resolved snapshot's logical-unit geometry to framebuffer pixels.
        ///
        /// @param snapshot Snapshot to convert in place.
        ///
        /// @note This function does not throw exceptions.
        void scale_snapshot_to_pixels(FrameSnapshot &snapshot) const noexcept;

        Clay_Context *context_ = nullptr;
        /// Backing memory of a fixed-size context (`Config::arena_capacity_bytes != 0`); empty for a growable one, which owns its own.
        vector<std::byte> arena_memory_;
        /// Heap-allocated so the error handler registered with the layout engine keeps a stable pointer across moves of this context
        /// (`owner` is re-pointed by the move operations).
        struct DiagnosticsHub {
            UiDiagnostics store;
            Context *owner = nullptr;
        };
        std::unique_ptr<DiagnosticsHub> diagnostics_ = std::make_unique<DiagnosticsHub>();
        /// The most recent UI API call (static strings from `std::source_location`), attached to any error raised while it runs.
        struct ApiSite {
            const char *file = nullptr;
            const char *function = nullptr;
            u32 line = 0;
            u32 column = 0;
        } api_site_;

        void note_call_site(const std::source_location &site) noexcept {
            api_site_ = ApiSite{site.file_name(), site.function_name(), site.line(), site.column()};
        }

      public:
        /// A problem found by the layout engine, in the engine's own terms (no layout-engine types leak into this header). Internal: the
        /// registered error handler fills this in and hands it to `report_layout_error`.
        struct LayoutErrorInfo {
            u32 type = 0;
            u32 detail = 0;
            i64 value = 0;
            i64 limit = 0;
            const char *message = nullptr;
            usize message_length = 0;
            const char *source_file = nullptr;
            const char *source_function = nullptr;
            i32 source_line = 0;
            u32 frame = 0;
        };
        /// Turns a layout-engine problem into a `UiErrorReport` (path, call sites, native stack) and records it. Never throws.
        void report_layout_error(const LayoutErrorInfo &info) noexcept;
        /// Entry point for the registered handler: `hub` is the opaque pointer given to the layout engine when the context was created.
        static void dispatch_layout_error(void *hub, const LayoutErrorInfo &info) noexcept;

      private:
        TextBridge text_bridge_;


        deque<string> text_storage_;
        deque<ImageRef> image_storage_;
        deque<CustomShaderRef> custom_storage_;
        deque<StrokePolylineData> stroke_storage_;
        deque<FillQuadListData> fill_storage_;
        deque<SectorListData> sector_storage_;
        deque<CustomStrokeElementData> custom_stroke_storage_;


        unordered_map<OutlineCacheKey, Text::GlyphOutline, OutlineCacheKeyHash> outline_cache_;
        /// Framebuffer-pixel extent of the current frame.
        Core::Extent2D layout_extent_{1, 1};
        /// Logical-unit size Clay lays out in: `layout_extent_ / pixel_scale_`.
        glm::vec2 layout_size_{1.0f};
        f32 pixel_scale_ = 1.0f;
        Text::RasterPreference text_raster_preference_ = Text::RasterPreference::Auto;


        glm::vec2 pointer_position_{0.0f};
        glm::vec2 pointer_press_position_{0.0f};
        bool pointer_down_ = false;
        bool pointer_pressed_this_frame_ = false;
        bool pointer_released_this_frame_ = false;
        bool pointer_cancelled_this_frame_ = false;
        string pointer_capture_id_;
        string focused_id_;


        vector<string> current_frame_ids_;
        unordered_map<string, ElementBounds> last_frame_bounds_;


        vector<string> current_frame_clip_ids_;
        unordered_map<string, glm::vec2> last_frame_scroll_offsets_;


        ScrollSettings scroll_settings_{};
        glm::vec2 pending_scroll_delta_{0.0f};


        bool cursor_management_enabled_ = true;
        CursorIcon desired_cursor_ = CursorIcon::Default;


        vector<i32> z_stack_{0};
    };

} // namespace SFT::UI
