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


struct Clay_Context;

namespace SFT::UI {


    struct ImageRef {
        Renderer::TextureHandle texture{};
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

      private:
        friend class Context;
        friend class UiRenderer;

        vector<QuadDraw> quads_;
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
            usize arena_capacity_bytes = 0;
            u32 max_element_count = 8192;
        };


        /// Creates a `Context` resource or value from the supplied parameters.
        ///
        /// @param config Configuration values controlling the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] static Core::RendererExpected<Context> create(const Config &config);


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


        /// Performs the begin layout operation for `Context` using the supplied arguments.
        ///
        /// @param viewport_size Requested or available size for the operation.
        /// @param pointer Pointer to the object or storage used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void begin_layout(glm::vec2 viewport_size, const PointerState &pointer = {}, f32 delta_seconds = 0.0f);


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
        [[nodiscard]] ElementScope element(const ElementDecl &decl);
        /// Performs the text operation for `Context` using the supplied arguments.
        ///
        /// @param content `content` value used by the operation.
        /// @param style `style` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void text(const ustr &content, const TextStyle &style);
        /// Performs the image operation for `Context` using the supplied arguments.
        ///
        /// @param decl `decl` value used by the operation.
        /// @param texture Texture used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void image(const ElementDecl &decl, Renderer::TextureHandle texture);


        /// Computes the hit test text byte offset required by the supplied values.
        ///
        /// @param style `style` value used by the operation.
        /// @param utf8_content `utf8_content` value used by the operation.
        /// @param local_x `local_x` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize hit_test_text_byte_offset(const TextStyle &style, string_view utf8_content, f32 local_x);


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
        [[nodiscard]] ElementScope custom_element(const ElementDecl &decl, const CustomShaderRef &shader);


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
        [[nodiscard]] FrameSnapshot finish_frame();


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

        Clay_Context *context_ = nullptr;
        vector<std::byte> arena_memory_;
        TextBridge text_bridge_;


        deque<string> text_storage_;
        deque<ImageRef> image_storage_;
        deque<CustomShaderRef> custom_storage_;


        unordered_map<OutlineCacheKey, Text::GlyphOutline, OutlineCacheKeyHash> outline_cache_;
        Core::Extent2D layout_extent_{1, 1};


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
