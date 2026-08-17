#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>


#define CLAY_IMPLEMENTATION
#include <clay.h>
#include <cmath>
#include <glm/geometric.hpp>
#include <limits>
#include <string_view>
#include <utility>
#pragma endregion

#include "Context.hpp"

using std::string_view;
using std::unordered_map;

namespace SFT::UI {

    namespace {


        /// Converts the value to clay color representation.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @return Returns the value converted to clay color representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_Color to_clay_color(const Color &color) noexcept {
            return Clay_Color{
                .r = static_cast<f32>(color.r),
                .g = static_cast<f32>(color.g),
                .b = static_cast<f32>(color.b),
                .a = static_cast<f32>(color.a),
            };
        }

        /// Converts the value to clay corner radius representation.
        ///
        /// @param radius `radius` value used by the operation.
        ///
        /// @return Returns the value converted to clay corner radius representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_CornerRadius to_clay_corner_radius(const CornerRadius &radius) noexcept {
            return Clay_CornerRadius{
                .topLeft = radius.top_left,
                .topRight = radius.top_right,
                .bottomLeft = radius.bottom_left,
                .bottomRight = radius.bottom_right,
            };
        }

        /// Converts the value to clay sizing axis representation.
        ///
        /// @param axis `axis` value used by the operation.
        ///
        /// @return Returns the value converted to clay sizing axis representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_SizingAxis to_clay_sizing_axis(const SizingAxis &axis) noexcept {
            Clay_SizingAxis result{};
            switch (axis.kind) {
                case SizingKind::Fit:
                    result.type = CLAY__SIZING_TYPE_FIT;
                    result.size.minMax = Clay_SizingMinMax{.min = axis.min, .max = axis.max};
                    break;
                case SizingKind::Grow:
                    result.type = CLAY__SIZING_TYPE_GROW;
                    result.size.minMax = Clay_SizingMinMax{.min = axis.min, .max = axis.max};
                    break;
                case SizingKind::Fixed:
                    result.type = CLAY__SIZING_TYPE_FIXED;
                    result.size.minMax = Clay_SizingMinMax{.min = axis.value, .max = axis.value};
                    break;
                case SizingKind::Percent:
                    result.type = CLAY__SIZING_TYPE_PERCENT;
                    result.size.percent = axis.value;
                    break;
            }
            return result;
        }

        /// Converts the value to clay direction representation.
        ///
        /// @param direction `direction` value used by the operation.
        ///
        /// @return Returns the value converted to clay direction representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_LayoutDirection to_clay_direction(LayoutDirection direction) noexcept {
            return direction == LayoutDirection::TopToBottom ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT;
        }

        /// Converts the value to clay align x representation.
        ///
        /// @param align `align` value used by the operation.
        ///
        /// @return Returns the value converted to clay align x representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_LayoutAlignmentX to_clay_align_x(AlignX align) noexcept {
            switch (align) {
                case AlignX::Left: return CLAY_ALIGN_X_LEFT;
                case AlignX::Center: return CLAY_ALIGN_X_CENTER;
                case AlignX::Right: return CLAY_ALIGN_X_RIGHT;
            }
            return CLAY_ALIGN_X_LEFT;
        }

        /// Converts the value to clay align y representation.
        ///
        /// @param align `align` value used by the operation.
        ///
        /// @return Returns the value converted to clay align y representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_LayoutAlignmentY to_clay_align_y(AlignY align) noexcept {
            switch (align) {
                case AlignY::Top: return CLAY_ALIGN_Y_TOP;
                case AlignY::Center: return CLAY_ALIGN_Y_CENTER;
                case AlignY::Bottom: return CLAY_ALIGN_Y_BOTTOM;
            }
            return CLAY_ALIGN_Y_TOP;
        }

        /// Converts the value to clay border width representation.
        ///
        /// @param width Width of the target extent.
        ///
        /// @return Returns the value converted to clay border width representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_BorderWidth to_clay_border_width(const BorderWidth &width) noexcept {
            return Clay_BorderWidth{
                .left = width.left,
                .right = width.right,
                .top = width.top,
                .bottom = width.bottom,
                .betweenChildren = width.between_children,
            };
        }

        /// Converts the value to clay attach point representation.
        ///
        /// @param point `point` value used by the operation.
        ///
        /// @return Returns the value converted to clay attach point representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_FloatingAttachPointType to_clay_attach_point(FloatingAttachPoint point) noexcept {
            switch (point) {
                case FloatingAttachPoint::LeftTop: return CLAY_ATTACH_POINT_LEFT_TOP;
                case FloatingAttachPoint::LeftCenter: return CLAY_ATTACH_POINT_LEFT_CENTER;
                case FloatingAttachPoint::LeftBottom: return CLAY_ATTACH_POINT_LEFT_BOTTOM;
                case FloatingAttachPoint::CenterTop: return CLAY_ATTACH_POINT_CENTER_TOP;
                case FloatingAttachPoint::CenterCenter: return CLAY_ATTACH_POINT_CENTER_CENTER;
                case FloatingAttachPoint::CenterBottom: return CLAY_ATTACH_POINT_CENTER_BOTTOM;
                case FloatingAttachPoint::RightTop: return CLAY_ATTACH_POINT_RIGHT_TOP;
                case FloatingAttachPoint::RightCenter: return CLAY_ATTACH_POINT_RIGHT_CENTER;
                case FloatingAttachPoint::RightBottom: return CLAY_ATTACH_POINT_RIGHT_BOTTOM;
            }
            return CLAY_ATTACH_POINT_LEFT_TOP;
        }


        /// Converts the value to clay declaration representation.
        ///
        /// @param decl `decl` value used by the operation.
        /// @param z `z` value used by the operation.
        ///
        /// @return Returns the value converted to clay declaration representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_ElementDeclaration to_clay_declaration(const ElementDecl &decl, i32 z) noexcept {
            Clay_ElementDeclaration result{};
            result.layout.sizing = Clay_Sizing{
                .width = to_clay_sizing_axis(decl.sizing.width),
                .height = to_clay_sizing_axis(decl.sizing.height),
            };
            result.layout.padding = Clay_Padding{decl.padding.left, decl.padding.right, decl.padding.top, decl.padding.bottom};
            result.layout.childGap = decl.child_gap;
            result.layout.childAlignment = Clay_ChildAlignment{
                .x = to_clay_align_x(decl.child_alignment.x),
                .y = to_clay_align_y(decl.child_alignment.y),
            };
            result.layout.layoutDirection = to_clay_direction(decl.direction);
            result.backgroundColor = to_clay_color(decl.background_color);
            result.cornerRadius = to_clay_corner_radius(decl.corner_radius);
            result.border = Clay_BorderElementConfig{
                .color = to_clay_color(decl.border.color),
                .width = to_clay_border_width(decl.border.width),
            };
            result.clip = Clay_ClipElementConfig{
                .horizontal = decl.clip.horizontal,
                .vertical = decl.clip.vertical,


            };
            if (decl.id.size() != 0) {
                const string_view id_view = decl.id.cpp_string_view();
                result.id = Clay_GetElementId(
                    Clay_String{.isStaticallyAllocated = false, .length = static_cast<i32>(id_view.size()), .chars = id_view.data()});
            }
            if (decl.floating.attach_to != FloatingAttachTo::None) {
                Clay_FloatingAttachToElement attach_to = CLAY_ATTACH_TO_NONE;
                u32 parent_id = 0;
                switch (decl.floating.attach_to) {
                    case FloatingAttachTo::None: break;
                    case FloatingAttachTo::Parent: attach_to = CLAY_ATTACH_TO_PARENT; break;
                    case FloatingAttachTo::Root: attach_to = CLAY_ATTACH_TO_ROOT; break;
                    case FloatingAttachTo::ElementWithId: {
                        attach_to = CLAY_ATTACH_TO_ELEMENT_WITH_ID;
                        const string_view parent_id_view = decl.floating.parent_id.cpp_string_view();
                        parent_id = Clay_GetElementId(Clay_String{.isStaticallyAllocated = false,
                                                                  .length = static_cast<i32>(parent_id_view.size()),
                                                                  .chars = parent_id_view.data()})
                                       .id;
                        break;
                    }
                }
                result.floating = Clay_FloatingElementConfig{
                    .offset = Clay_Vector2{.x = decl.floating.offset.x, .y = decl.floating.offset.y},
                    .expand = Clay_Dimensions{},
                    .parentId = parent_id,
                    .zIndex = decl.floating.z_index,
                    .attachPoints = Clay_FloatingAttachPoints{
                        .element = to_clay_attach_point(decl.floating.element_attach_point),
                        .parent = to_clay_attach_point(decl.floating.parent_attach_point),
                    },
                    .pointerCaptureMode = decl.floating.capture_pointer ? CLAY_POINTER_CAPTURE_MODE_CAPTURE
                                                                        : CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                    .attachTo = attach_to,
                    .clipTo = decl.floating.clip_to == FloatingClipTo::AttachedParent
                                 ? CLAY_CLIP_TO_ATTACHED_PARENT
                                 : CLAY_CLIP_TO_NONE,
                };
            }
            result.userData = reinterpret_cast<void *>(static_cast<intptr_t>(z));
            return result;
        }


        /// Unpacks z using the supplied arguments and current state.
        ///
        /// @param user_data Data consumed or referenced by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] i32 unpack_z(void *user_data) noexcept {
            return static_cast<i32>(reinterpret_cast<intptr_t>(user_data));
        }

        /// Converts the value to clay wrap mode representation.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns the value converted to clay wrap mode representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_TextElementConfigWrapMode to_clay_wrap_mode(TextWrapMode mode) noexcept {
            switch (mode) {
                case TextWrapMode::Words: return CLAY_TEXT_WRAP_WORDS;
                case TextWrapMode::Newlines: return CLAY_TEXT_WRAP_NEWLINES;
                case TextWrapMode::None: return CLAY_TEXT_WRAP_NONE;
            }
            return CLAY_TEXT_WRAP_WORDS;
        }

        /// Converts the value to clay text align representation.
        ///
        /// @param align `align` value used by the operation.
        ///
        /// @return Returns the value converted to clay text align representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_TextAlignment to_clay_text_align(TextAlign align) noexcept {
            switch (align) {
                case TextAlign::Left: return CLAY_TEXT_ALIGN_LEFT;
                case TextAlign::Center: return CLAY_TEXT_ALIGN_CENTER;
                case TextAlign::Right: return CLAY_TEXT_ALIGN_RIGHT;
            }
            return CLAY_TEXT_ALIGN_LEFT;
        }

        /// Converts the value to clay text config representation.
        ///
        /// @param style `style` value used by the operation.
        /// @param z `z` value used by the operation.
        ///
        /// @return Returns the value converted to clay text config representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_TextElementConfig to_clay_text_config(const TextStyle &style, i32 z) noexcept {
            return Clay_TextElementConfig{
                .userData = reinterpret_cast<void *>(static_cast<intptr_t>(z)),
                .textColor = to_clay_color(style.color),
                .fontId = style.font_id,
                .fontSize = style.font_size,
                .letterSpacing = style.letter_spacing,
                .lineHeight = style.line_height,
                .wrapMode = to_clay_wrap_mode(style.wrap_mode),
                .textAlignment = to_clay_text_align(style.alignment),
            };
        }

        /// Performs the clay error handler operation for `UI` using the supplied arguments.
        ///
        /// @param error Error value describing the failure.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void clay_error_handler(Clay_ErrorData error) {
            Foundation::log_error("Clay UI layout error: {}",
                                  string_view{error.errorText.chars, static_cast<usize>(error.errorText.length)});
        }

    } // namespace

    /// Performs the element scope operation for `UI` using the supplied arguments.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @note This function does not throw exceptions.
    ElementScope::ElementScope(ElementScope &&other) noexcept
        : context_(std::exchange(other.context_, nullptr)), z_stack_(std::exchange(other.z_stack_, nullptr)) {}

    /// Assigns a new value to this `UI`.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function does not throw exceptions.
    ElementScope &ElementScope::operator=(ElementScope &&other) noexcept {
        if (this != &other) {
            if (context_ != nullptr) {
                Clay_SetCurrentContext(context_);
                Clay__CloseElement();
                if (z_stack_ != nullptr && z_stack_->size() > 1) {
                    z_stack_->pop_back();
                }
            }
            context_ = std::exchange(other.context_, nullptr);
            z_stack_ = std::exchange(other.z_stack_, nullptr);
        }
        return *this;
    }

    /// Destroys the `UI` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    ElementScope::~ElementScope() {
        if (context_ != nullptr) {
            Clay_SetCurrentContext(context_);
            Clay__CloseElement();
            if (z_stack_ != nullptr && z_stack_->size() > 1) {
                z_stack_->pop_back();
            }
        }
    }

    /// Performs the context operation for `UI` using the supplied arguments.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @note This function does not throw exceptions.
    Context::Context(Context &&other) noexcept
        : context_(std::exchange(other.context_, nullptr)),
          arena_memory_(std::move(other.arena_memory_)),
          text_bridge_(std::move(other.text_bridge_)),
          text_storage_(std::move(other.text_storage_)),
          image_storage_(std::move(other.image_storage_)),
          custom_storage_(std::move(other.custom_storage_)),
          outline_cache_(std::move(other.outline_cache_)),
          layout_extent_(other.layout_extent_),
          pointer_position_(other.pointer_position_),
          pointer_press_position_(other.pointer_press_position_),
          pointer_down_(other.pointer_down_),
          pointer_pressed_this_frame_(other.pointer_pressed_this_frame_),
          pointer_released_this_frame_(other.pointer_released_this_frame_),
          pointer_cancelled_this_frame_(other.pointer_cancelled_this_frame_),
          pointer_capture_id_(std::move(other.pointer_capture_id_)),
          focused_id_(std::move(other.focused_id_)),
          current_frame_ids_(std::move(other.current_frame_ids_)),
          last_frame_bounds_(std::move(other.last_frame_bounds_)),
          current_frame_clip_ids_(std::move(other.current_frame_clip_ids_)),
          last_frame_scroll_offsets_(std::move(other.last_frame_scroll_offsets_)),
          scroll_settings_(other.scroll_settings_),
          pending_scroll_delta_(other.pending_scroll_delta_),
          cursor_management_enabled_(other.cursor_management_enabled_),
          desired_cursor_(other.desired_cursor_),
          z_stack_(std::move(other.z_stack_)) {


        if (context_ != nullptr) {
            Clay_SetCurrentContext(context_);
            Clay_SetMeasureTextFunction(&TextBridge::measure_callback, &text_bridge_);
        }
    }

    /// Assigns a new value to this `UI`.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function does not throw exceptions.
    Context &Context::operator=(Context &&other) noexcept {
        if (this != &other) {
            destroy();
            context_ = std::exchange(other.context_, nullptr);
            arena_memory_ = std::move(other.arena_memory_);
            text_bridge_ = std::move(other.text_bridge_);
            text_storage_ = std::move(other.text_storage_);
            image_storage_ = std::move(other.image_storage_);
            custom_storage_ = std::move(other.custom_storage_);
            outline_cache_ = std::move(other.outline_cache_);
            layout_extent_ = other.layout_extent_;
            pointer_position_ = other.pointer_position_;
            pointer_press_position_ = other.pointer_press_position_;
            pointer_down_ = other.pointer_down_;
            pointer_pressed_this_frame_ = other.pointer_pressed_this_frame_;
            pointer_released_this_frame_ = other.pointer_released_this_frame_;
            pointer_cancelled_this_frame_ = other.pointer_cancelled_this_frame_;
            pointer_capture_id_ = std::move(other.pointer_capture_id_);
            focused_id_ = std::move(other.focused_id_);
            current_frame_ids_ = std::move(other.current_frame_ids_);
            last_frame_bounds_ = std::move(other.last_frame_bounds_);
            current_frame_clip_ids_ = std::move(other.current_frame_clip_ids_);
            last_frame_scroll_offsets_ = std::move(other.last_frame_scroll_offsets_);
            scroll_settings_ = other.scroll_settings_;
            pending_scroll_delta_ = other.pending_scroll_delta_;
            cursor_management_enabled_ = other.cursor_management_enabled_;
            desired_cursor_ = other.desired_cursor_;
            z_stack_ = std::move(other.z_stack_);
            if (context_ != nullptr) {
                Clay_SetCurrentContext(context_);
                Clay_SetMeasureTextFunction(&TextBridge::measure_callback, &text_bridge_);
            }
        }
        return *this;
    }

    /// Destroys the `UI` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    Context::~Context() { destroy(); }

    /// Creates a `UI` resource or value from the supplied parameters.
    ///
    /// @param config Configuration values controlling the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererExpected<Context> Context::create(const Config &config) {
        if (config.max_element_count != 0) {
            Clay_SetMaxElementCount(static_cast<i32>(config.max_element_count));
        }
        const usize required = config.arena_capacity_bytes != 0 ? config.arena_capacity_bytes
                                                                  : static_cast<usize>(Clay_MinMemorySize());

        Context result;
        result.arena_memory_.resize(required);
        Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(required, result.arena_memory_.data());
        Clay_Context *context = Clay_Initialize(
            arena, Clay_Dimensions{.width = 1.0f, .height = 1.0f},
            Clay_ErrorHandler{.errorHandlerFunction = &clay_error_handler, .userData = nullptr});
        if (context == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Clay_Initialize failed (arena too small).");
        }
        result.context_ = context;
        Clay_SetMeasureTextFunction(&TextBridge::measure_callback, &result.text_bridge_);
        return result;
    }

    /// Sets the current for this `UI`.
    ///
    /// @return Returns the current set current value.
    /// @note This function does not throw exceptions.
    void Context::set_current() const noexcept {
        if (context_ != nullptr) {
            Clay_SetCurrentContext(context_);
        }
    }

    /// Registers font using the supplied arguments and current state.
    ///
    /// @param font_id Identifier of the target object or resource.
    /// @param font `font` value used by the operation.
    /// @param emoji_fallback Fallback value used when the primary value is unavailable.
    /// @param fallbacks Fallback value used when the primary value is unavailable.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::register_font(FontId font_id, const Text::Font &font, const Text::Font *emoji_fallback,
                                std::span<const Text::Font *const> fallbacks) {
        text_bridge_.register_font(font_id, font, emoji_fallback, fallbacks);
    }

    /// Performs the begin layout operation for `UI` using the supplied arguments.
    ///
    /// @param viewport_size Requested or available size for the operation.
    /// @param pointer Pointer to the object or storage used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::begin_layout(glm::vec2 viewport_size, const PointerState &pointer, f32 delta_seconds) {
        const auto layout_dimension = [](f32 value) noexcept {
            constexpr f32 max_exact_pixel_dimension = 16777216.0f;
            if (!std::isfinite(value) || value <= 0.0f) {
                return 1u;
            }
            return static_cast<u32>(std::clamp(std::round(value), 1.0f, max_exact_pixel_dimension));
        };
        layout_extent_ = Core::Extent2D{layout_dimension(viewport_size.x), layout_dimension(viewport_size.y)};

        set_current();
        text_bridge_.begin_frame();
        text_storage_.clear();
        image_storage_.clear();
        custom_storage_.clear();
        current_frame_ids_.clear();
        current_frame_clip_ids_.clear();


        desired_cursor_ = CursorIcon::Default;


        z_stack_.assign(1, 0);
        Clay_SetLayoutDimensions(Clay_Dimensions{
            .width = static_cast<f32>(layout_extent_.x),
            .height = static_cast<f32>(layout_extent_.y),
        });


        const bool was_down = pointer_down_;
        pointer_position_ = pointer.position;
        pointer_pressed_this_frame_ = pointer.pressed || (pointer.down && !was_down);
        if (pointer_pressed_this_frame_) {
            pointer_press_position_ = pointer.press_position.value_or(pointer.position);
        }
        pointer_released_this_frame_ = pointer.released || (!pointer.down && was_down);
        pointer_cancelled_this_frame_ = pointer.cancelled;
        pointer_down_ = pointer.down;
        Clay_SetPointerState(Clay_Vector2{.x = pointer.position.x, .y = pointer.position.y}, pointer.down);


        pending_scroll_delta_ += pointer.scroll_delta * scroll_settings_.wheel_multiplier;
        glm::vec2 applied_scroll_delta = pending_scroll_delta_;
        if (scroll_settings_.smooth_scrolling && delta_seconds > 0.0f) {
            const f32 factor = std::clamp(scroll_settings_.smoothing_rate * delta_seconds, 0.0f, 1.0f);
            applied_scroll_delta = pending_scroll_delta_ * factor;
            pending_scroll_delta_ -= applied_scroll_delta;
            if (glm::length(pending_scroll_delta_) < 0.001f) {
                pending_scroll_delta_ = glm::vec2{0.0f};
            }
        } else {
            pending_scroll_delta_ = glm::vec2{0.0f};
        }
        Clay_UpdateScrollContainers(scroll_settings_.click_and_drag_scroll,
                                    Clay_Vector2{.x = applied_scroll_delta.x, .y = applied_scroll_delta.y},
                                    delta_seconds);
        Clay_BeginLayout();
    }

    /// Updates desired cursor from the supplied values.
    ///
    /// @param decl `decl` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Context::update_desired_cursor(const ElementDecl &decl) noexcept {
        if (!cursor_management_enabled_ || decl.cursor == CursorIcon::Auto || decl.id.empty()) {
            return;
        }


        if (hovered(decl.id)) {
            desired_cursor_ = decl.cursor;
        }
    }

    /// Performs the force cursor operation for `UI` using the supplied arguments.
    ///
    /// @param icon `icon` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Context::force_cursor(CursorIcon icon) noexcept {
        if (!cursor_management_enabled_) {
            return;
        }
        desired_cursor_ = icon;
    }

    /// Performs the element operation for `UI` using the supplied arguments.
    ///
    /// @param decl `decl` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ElementScope Context::element(const ElementDecl &decl) {
        set_current();
        if (!decl.id.empty()) {
            current_frame_ids_.push_back(decl.id.cpp_string());
        }
        update_desired_cursor(decl);


        const i32 effective_z = decl.z != 0 ? decl.z : z_stack_.back();
        z_stack_.push_back(effective_z);
        Clay__OpenElement();
        Clay_ElementDeclaration declaration = to_clay_declaration(decl, effective_z);
        if (decl.clip.horizontal || decl.clip.vertical) {


            if (!decl.id.empty()) {
                current_frame_clip_ids_.push_back(decl.id.cpp_string());
                const auto cached = last_frame_scroll_offsets_.find(decl.id.cpp_string());
                if (cached != last_frame_scroll_offsets_.end()) {
                    declaration.clip.childOffset = Clay_Vector2{.x = cached->second.x, .y = cached->second.y};
                }
            } else {


                declaration.clip.childOffset = Clay_GetScrollOffset();
            }
        }
        Clay__ConfigureOpenElement(declaration);
        return ElementScope{context_, &z_stack_};
    }

    /// Performs the text operation for `UI` using the supplied arguments.
    ///
    /// @param content `content` value used by the operation.
    /// @param style `style` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::text(const ustr &content, const TextStyle &style) {
        set_current();
        const UString utf8{content};
        text_storage_.push_back(string{utf8.cpp_string_view()});
        const string &stored = text_storage_.back();
        const Clay_String clay_string{
            .isStaticallyAllocated = false,
            .length = static_cast<i32>(stored.size()),
            .chars = stored.data(),
        };


        Clay_TextElementConfig *config = Clay__StoreTextElementConfig(to_clay_text_config(style, z_stack_.back()));
        Clay__OpenTextElement(clay_string, config);
    }

    /// Computes the hit test text byte offset required by the supplied values.
    ///
    /// @param style `style` value used by the operation.
    /// @param utf8_content `utf8_content` value used by the operation.
    /// @param local_x `local_x` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize Context::hit_test_text_byte_offset(const TextStyle &style, string_view utf8_content, f32 local_x) {
        if (utf8_content.empty()) {
            return 0;
        }
        const CachedShape *shape = text_bridge_.shape_and_cache(style, utf8_content);
        if (shape == nullptr) {
            return 0;
        }
        if (local_x <= 0.0f) {
            return 0;
        }
        if (local_x >= shape->width_px) {
            return utf8_content.size();
        }


        struct Boundary {
            f32 x = 0.0f;
            usize byte_offset = 0;
        };
        vector<Boundary> boundaries;
        boundaries.reserve(utf8_content.size() + 2);
        boundaries.push_back(Boundary{.x = 0.0f, .byte_offset = 0});

        const f32 font_size = static_cast<f32>(style.font_size);
        f32 visual_run_x = 0.0f;
        for (const Text::ShapedRun &run : shape->shaped.runs) {
            const f32 run_scale = font_size / static_cast<f32>(std::max(run.units_per_em, 1u));
            f32 cursor_x = visual_run_x + run.pen_origin_em * font_size;
            for (const Text::PositionedGlyph &glyph : run.glyphs) {
                if (boundaries.back().byte_offset != glyph.cluster) {
                    boundaries.push_back(Boundary{.x = cursor_x, .byte_offset = glyph.cluster});
                }
                cursor_x += glyph.x_advance * run_scale;
            }
            visual_run_x += run.advance_em * font_size;
        }
        boundaries.push_back(Boundary{.x = shape->width_px, .byte_offset = utf8_content.size()});

        usize best_offset = 0;
        f32 best_distance = std::numeric_limits<f32>::max();
        for (const Boundary &boundary : boundaries) {
            const f32 distance = std::abs(local_x - boundary.x);
            if (distance < best_distance) {
                best_distance = distance;
                best_offset = boundary.byte_offset;
            }
        }
        return best_offset;
    }

    /// Performs the image operation for `UI` using the supplied arguments.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param texture Texture used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::image(const ElementDecl &decl, Renderer::TextureHandle texture) {
        set_current();
        if (!decl.id.empty()) {
            current_frame_ids_.push_back(decl.id.cpp_string());
        }
        update_desired_cursor(decl);
        image_storage_.push_back(ImageRef{.texture = texture});
        ImageRef *stored = &image_storage_.back();
        const i32 effective_z = decl.z != 0 ? decl.z : z_stack_.back();
        Clay_ElementDeclaration declaration = to_clay_declaration(decl, effective_z);
        declaration.image = Clay_ImageElementConfig{.imageData = stored};
        Clay__OpenElement();
        Clay__ConfigureOpenElement(declaration);
        Clay__CloseElement();
    }

    /// Performs the svg operation for `UI` using the supplied arguments.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param texture Texture used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::svg(const ElementDecl &decl, Renderer::TextureHandle texture) { image(decl, texture); }

    /// Performs the custom element operation for `UI` using the supplied arguments.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param shader Shader used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ElementScope Context::custom_element(const ElementDecl &decl, const CustomShaderRef &shader) {
        set_current();
        if (!decl.id.empty()) {
            current_frame_ids_.push_back(decl.id.cpp_string());
        }
        update_desired_cursor(decl);
        custom_storage_.push_back(shader);
        CustomShaderRef *stored = &custom_storage_.back();
        const i32 effective_z = decl.z != 0 ? decl.z : z_stack_.back();
        z_stack_.push_back(effective_z);
        Clay__OpenElement();
        Clay_ElementDeclaration declaration = to_clay_declaration(decl, effective_z);
        if (decl.clip.horizontal || decl.clip.vertical) {
            declaration.clip.childOffset = Clay_GetScrollOffset();
        }
        declaration.custom = Clay_CustomElementConfig{.customData = stored};
        Clay__ConfigureOpenElement(declaration);
        return ElementScope{context_, &z_stack_};
    }

    /// Performs the hovered operation for `UI` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool Context::hovered(const UString &id) const noexcept {
        if (context_ == nullptr || id.size() == 0) {
            return false;
        }
        set_current();
        const string_view id_view = id.cpp_string_view();
        const Clay_ElementId element_id = Clay_GetElementId(
            Clay_String{.isStaticallyAllocated = false, .length = static_cast<i32>(id_view.size()), .chars = id_view.data()});
        return Clay_PointerOver(element_id);
    }

    /// Performs the clicked operation for `UI` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool Context::clicked(const UString &id) const noexcept {
        if (!pointer_pressed_this_frame_) {
            return false;
        }


        if (pointer_press_position_ == pointer_position_) {
            return hovered(id);
        }
        const std::optional<ElementBounds> bounds = element_bounds(id);
        return bounds.has_value() && pointer_press_position_.x >= bounds->position.x &&
               pointer_press_position_.y >= bounds->position.y &&
               pointer_press_position_.x <= bounds->position.x + bounds->size.x &&
               pointer_press_position_.y <= bounds->position.y + bounds->size.y;
    }

    /// Performs the pointer down operation for `UI` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool Context::pointer_down(const UString &id) const noexcept { return pointer_down_ && hovered(id); }

    /// Performs the element bounds operation for `UI` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
    std::optional<ElementBounds> Context::element_bounds(const UString &id) const noexcept {
        if (id.empty()) {
            return std::nullopt;
        }
        const auto found = last_frame_bounds_.find(id.cpp_string());
        return found != last_frame_bounds_.end() ? std::optional<ElementBounds>{found->second} : std::nullopt;
    }

    /// Attempts to capture pointer without requiring normal failure to be exceptional.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note Normal failure is reported by returning `false`.
    /// @note This function does not throw exceptions.
    bool Context::try_capture_pointer(const UString &id) noexcept {
        if (id.empty()) {
            return false;
        }
        const string_view requested = id.cpp_string_view();
        if (!pointer_capture_id_.empty() && pointer_capture_id_ != requested) {
            return false;
        }
        pointer_capture_id_.assign(requested);
        return true;
    }

    /// Reports whether this `UI` has pointer capture.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool Context::has_pointer_capture(const UString &id) const noexcept {
        return !id.empty() && pointer_capture_id_ == id.cpp_string_view();
    }

    /// Releases pointer using the supplied arguments and current state.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Context::release_pointer(const UString &id) noexcept {
        if (has_pointer_capture(id)) {
            pointer_capture_id_.clear();
        }
    }

    /// Performs the focus operation for `UI` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::focus(const UString &id) {
        if (!id.empty()) {
            focused_id_ = id.cpp_string();
        }
    }

    /// Reports whether this `UI` has focus.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool Context::has_focus(const UString &id) const noexcept {
        return !id.empty() && focused_id_ == id.cpp_string_view();
    }

    /// Clears focus.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Context::clear_focus(const UString &id) noexcept {
        if (has_focus(id)) {
            focused_id_.clear();
        }
    }

    /// Returns the current or globally available pointer over any value.
    ///
    /// @return Returns the current pointer over any value.
    /// @note This function does not throw exceptions.
    bool Context::pointer_over_any() const noexcept {


        for (const auto &[id, bounds] : last_frame_bounds_) {
            (void)bounds;
            if (hovered(UString{id})) {
                return true;
            }
        }
        return false;
    }

    /// Performs the clicked outside operation for `UI` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool Context::clicked_outside(const UString &id) const noexcept {
        return pointer_pressed_this_frame_ && !clicked(id);
    }

    namespace {
        /// Converts the value to clay element ID representation.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns the value converted to clay element ID representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Clay_ElementId to_clay_element_id(const UString &id) noexcept {
            const string_view id_view = id.cpp_string_view();
            return Clay_GetElementId(
                Clay_String{.isStaticallyAllocated = false, .length = static_cast<i32>(id_view.size()), .chars = id_view.data()});
        }
    } // namespace

    /// Scrolls into view using the supplied arguments and current state.
    ///
    /// @param container_id Identifier of the target object or resource.
    /// @param target_id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool Context::scroll_into_view(const UString &container_id, const UString &target_id) noexcept {
        if (context_ == nullptr || container_id.size() == 0 || target_id.size() == 0) {
            return false;
        }
        set_current();

        const Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(to_clay_element_id(container_id));
        if (!scroll.found || scroll.scrollPosition == nullptr) {
            return false;
        }
        const Clay_ElementData container = Clay_GetElementData(to_clay_element_id(container_id));
        const Clay_ElementData target = Clay_GetElementData(to_clay_element_id(target_id));
        if (!container.found || !target.found) {
            return false;
        }


        const auto nudge_axis = [](f32 &position, f32 target_min, f32 target_max, f32 container_min,
                                    f32 container_max, f32 content_size, f32 container_size) noexcept {
            if (target_min < container_min) {
                position += container_min - target_min;
            } else if (target_max > container_max) {
                position -= target_max - container_max;
            }
            const f32 min_position = std::min(0.0f, container_size - content_size);
            position = std::clamp(position, min_position, 0.0f);
        };

        if (scroll.config.horizontal) {
            nudge_axis(scroll.scrollPosition->x, target.boundingBox.x, target.boundingBox.x + target.boundingBox.width,
                      container.boundingBox.x, container.boundingBox.x + container.boundingBox.width,
                      scroll.contentDimensions.width, scroll.scrollContainerDimensions.width);
        }
        if (scroll.config.vertical) {
            nudge_axis(scroll.scrollPosition->y, target.boundingBox.y, target.boundingBox.y + target.boundingBox.height,
                      container.boundingBox.y, container.boundingBox.y + container.boundingBox.height,
                      scroll.contentDimensions.height, scroll.scrollContainerDimensions.height);
        }
        return true;
    }

    /// Scrolls metrics using the supplied arguments and current state.
    ///
    /// @param container_id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    Context::ScrollMetrics Context::scroll_metrics(const UString &container_id) const noexcept {
        if (context_ == nullptr || container_id.size() == 0) {
            return ScrollMetrics{};
        }
        set_current();
        const Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(to_clay_element_id(container_id));
        if (!scroll.found) {
            return ScrollMetrics{};
        }
        return ScrollMetrics{
            .found = true,
            .offset = scroll.scrollPosition != nullptr ? glm::vec2{scroll.scrollPosition->x, scroll.scrollPosition->y}
                                                        : glm::vec2{0.0f},
            .content_size = {scroll.contentDimensions.width, scroll.contentDimensions.height},
            .container_size = {scroll.scrollContainerDimensions.width, scroll.scrollContainerDimensions.height},
            .horizontal = scroll.config.horizontal,
            .vertical = scroll.config.vertical,
        };
    }

    /// Computes the set scroll offset required by the supplied values.
    ///
    /// @param container_id Identifier of the target object or resource.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool Context::set_scroll_offset(const UString &container_id, glm::vec2 offset) noexcept {
        if (context_ == nullptr || container_id.size() == 0) {
            return false;
        }
        set_current();
        const Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(to_clay_element_id(container_id));
        if (!scroll.found || scroll.scrollPosition == nullptr) {
            return false;
        }
        if (scroll.config.horizontal) {
            const f32 min_x = std::min(0.0f, scroll.scrollContainerDimensions.width - scroll.contentDimensions.width);
            scroll.scrollPosition->x = std::clamp(offset.x, min_x, 0.0f);
        }
        if (scroll.config.vertical) {
            const f32 min_y = std::min(0.0f, scroll.scrollContainerDimensions.height - scroll.contentDimensions.height);
            scroll.scrollPosition->y = std::clamp(offset.y, min_y, 0.0f);
        }


        last_frame_scroll_offsets_.insert_or_assign(
            container_id.cpp_string(), glm::vec2{scroll.scrollPosition->x, scroll.scrollPosition->y});
        return true;
    }

    namespace {


        /// Performs the clay color to linear operation for `UI` using the supplied arguments.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec4 clay_color_to_linear(Clay_Color color) noexcept {
            const Foundation::Color::Linear linear =
                Foundation::Color::Srgb{color.r, color.g, color.b, color.a}.to_linear();
            return glm::vec4{static_cast<f32>(linear.r), static_cast<f32>(linear.g), static_cast<f32>(linear.b),
                             static_cast<f32>(linear.a)};
        }

        /// Performs the intersect rect operation for `UI` using the supplied arguments.
        ///
        /// @param parent `parent` value used by the operation.
        /// @param box `box` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::Rect2D intersect_rect(const RHI::Rect2D &parent, const Clay_BoundingBox &box) noexcept {
            const i32 parent_x1 = parent.x + static_cast<i32>(parent.width);
            const i32 parent_y1 = parent.y + static_cast<i32>(parent.height);
            i32 x0 = std::max(parent.x, static_cast<i32>(std::floor(box.x)));
            i32 y0 = std::max(parent.y, static_cast<i32>(std::floor(box.y)));
            i32 x1 = std::min(parent_x1, static_cast<i32>(std::ceil(box.x + box.width)));
            i32 y1 = std::min(parent_y1, static_cast<i32>(std::ceil(box.y + box.height)));
            x1 = std::max(x0, x1);
            y1 = std::max(y0, y1);
            return RHI::Rect2D{.x = x0, .y = y0, .width = static_cast<u32>(x1 - x0), .height = static_cast<u32>(y1 - y0)};
        }


        /// Performs the font for glyph operation for `UI` using the supplied arguments.
        ///
        /// @param fonts `fonts` value used by the operation.
        /// @param font_id Identifier of the target object or resource.
        /// @param is_color `is_color` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Text::Font *font_for_glyph(const Text::FontStack &fonts, u64 font_id, bool is_color) noexcept {
            if (is_color) {
                return fonts.emoji;
            }
            if (font_id == fonts.primary_font_id) {
                return fonts.primary;
            }
            for (const Text::FallbackFont &fallback : fonts.fallbacks) {
                if (fallback.font_id == font_id) {
                    return fallback.font;
                }
            }


            return fonts.primary;
        }


        /// Appends the supplied value or range to the current contents.
        ///
        /// @param out `out` value used by the operation.
        /// @param out_scissors `out_scissors` value used by the operation.
        /// @param out_paint `out_paint` value used by the operation.
        /// @param scissor `scissor` value used by the operation.
        /// @param paint `paint` value used by the operation.
        /// @param shape `shape` value used by the operation.
        /// @param font_size Requested or available size for the operation.
        /// @param color `color` value used by the operation.
        /// @param origin `origin` value used by the operation.
        /// @param outline_cache `outline_cache` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void append_glyph_placements(vector<Renderer::GlyphPlacement> &out, vector<RHI::Rect2D> &out_scissors,
                                     vector<PaintKey> &out_paint, const RHI::Rect2D &scissor, const PaintKey &paint,
                                     const CachedShape &shape, u16 font_size, glm::vec4 color, glm::vec2 origin,
                                     unordered_map<OutlineCacheKey, Text::GlyphOutline, OutlineCacheKeyHash> &outline_cache) {
            if (shape.fonts == nullptr || shape.fonts->primary == nullptr) {
                return;
            }
            const Text::Font &primary_font = *shape.fonts->primary;
            const u32 units_per_em = primary_font.units_per_em();
            const f32 scale = units_per_em > 0 ? static_cast<f32>(font_size) / static_cast<f32>(units_per_em) : 0.0f;
            const f32 ascender_px = static_cast<f32>(primary_font.ascender()) * scale;
            const glm::vec2 pen{origin.x, origin.y + ascender_px};

            f32 visual_run_x = pen.x;
            for (const Text::ShapedRun &run : shape.shaped.runs) {
                const f32 run_scale = static_cast<f32>(font_size) / static_cast<f32>(std::max(run.units_per_em, 1u));
                glm::vec2 cursor{visual_run_x + run.pen_origin_em * static_cast<f32>(font_size), pen.y};
                for (const Text::PositionedGlyph &glyph : run.glyphs) {
                    const Text::Font *glyph_font = font_for_glyph(*shape.fonts, glyph.font_id, glyph.is_color);
                    const Text::GlyphOutline *outline = nullptr;
                    if (!glyph.is_color && glyph_font != nullptr) {
                        const OutlineCacheKey key{.font_id = glyph.font_id, .glyph_id = glyph.glyph_id};
                        auto cached = outline_cache.find(key);
                        if (cached == outline_cache.end()) {
                            if (auto extracted = Text::glyph_outline(*glyph_font, glyph.glyph_id)) {
                                cached = outline_cache.emplace(key, std::move(*extracted)).first;
                            } else {
                                cached = outline_cache.emplace(key, Text::GlyphOutline{}).first;
                            }
                        }
                        outline = &cached->second;
                    }

                    out.push_back(Renderer::GlyphPlacement{
                        .position = {cursor.x + glyph.x_offset * run_scale, cursor.y - glyph.y_offset * run_scale},
                        .size = {static_cast<f32>(font_size), static_cast<f32>(font_size)},
                        .color = color,
                        .font_id = glyph.font_id,
                        .glyph_id = glyph.glyph_id,
                        .units_per_em = run.units_per_em,
                        .pixel_size = static_cast<f32>(font_size),
                        .format = glyph.is_color ? Text::RasterFormat::Color : Text::select_raster_format(static_cast<f32>(font_size)),
                        .outline = outline,
                        .font = glyph_font,
                    });
                    out_scissors.push_back(scissor);
                    out_paint.push_back(paint);

                    cursor.x += glyph.x_advance * run_scale;
                    cursor.y -= glyph.y_advance * run_scale;
                }
                visual_run_x += run.advance_em * static_cast<f32>(font_size);
            }
        }

    } // namespace

    /// Returns the current or globally available finish frame value.
    ///
    /// @return Returns the current finish frame value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    FrameSnapshot Context::finish_frame() {
        set_current();
        FrameSnapshot snapshot;
        snapshot.full_viewport_scissor_ = RHI::Rect2D{
            .x = 0,
            .y = 0,
            .width = layout_extent_.x,
            .height = layout_extent_.y,
        };

        Clay_RenderCommandArray commands = Clay_EndLayout();

        unordered_map<string, ElementBounds> completed_bounds;
        completed_bounds.reserve(current_frame_ids_.size());
        for (const string &id : current_frame_ids_) {
            const Clay_ElementId element_id = Clay_GetElementId(Clay_String{
                .isStaticallyAllocated = false,
                .length = static_cast<i32>(id.size()),
                .chars = id.data(),
            });
            const Clay_ElementData data = Clay_GetElementData(element_id);
            if (data.found) {
                completed_bounds.insert_or_assign(id, ElementBounds{
                    .position = {data.boundingBox.x, data.boundingBox.y},
                    .size = {data.boundingBox.width, data.boundingBox.height},
                });
            }
        }


        unordered_map<string, glm::vec2> completed_scroll_offsets;
        completed_scroll_offsets.reserve(current_frame_clip_ids_.size());
        for (const string &id : current_frame_clip_ids_) {
            const Clay_ElementId element_id = Clay_GetElementId(Clay_String{
                .isStaticallyAllocated = false,
                .length = static_cast<i32>(id.size()),
                .chars = id.data(),
            });
            const Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(element_id);
            if (scroll.found && scroll.scrollPosition != nullptr) {
                completed_scroll_offsets.insert_or_assign(id, glm::vec2{scroll.scrollPosition->x, scroll.scrollPosition->y});
            }
        }
        last_frame_scroll_offsets_ = std::move(completed_scroll_offsets);
        last_frame_bounds_ = std::move(completed_bounds);
        if (!focused_id_.empty() && !last_frame_bounds_.contains(focused_id_)) {
            focused_id_.clear();
        }
        if (!pointer_capture_id_.empty() && !last_frame_bounds_.contains(pointer_capture_id_)) {
            pointer_capture_id_.clear();
        }

        vector<RHI::Rect2D> scissor_stack{snapshot.full_viewport_scissor_};

        for (i32 i = 0; i < commands.length; ++i) {
            const Clay_RenderCommand &command = *Clay_RenderCommandArray_Get(&commands, i);
            const RHI::Rect2D active_scissor = scissor_stack.back();


            const PaintKey paint{unpack_z(command.userData), static_cast<u32>(i)};
            switch (command.commandType) {
                case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                    const Clay_RectangleRenderData &data = command.renderData.rectangle;
                    snapshot.quads_.push_back(QuadDraw{
                        .instance = UiQuadInstance{
                            .position = {command.boundingBox.x, command.boundingBox.y},
                            .size = {command.boundingBox.width, command.boundingBox.height},
                            .corner_radius = {data.cornerRadius.topLeft, data.cornerRadius.topRight,
                                            data.cornerRadius.bottomLeft, data.cornerRadius.bottomRight},
                            .fill_color = clay_color_to_linear(data.backgroundColor),
                            .uv_min = {0.0f, 0.0f},
                            .uv_max = {1.0f, 1.0f},
                            .kind = static_cast<f32>(UiQuadKind::Rect),
                        },
                        .image_ref = nullptr,
                        .scissor = active_scissor,
                        .paint = paint,
                    });
                    break;
                }
                case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                    const Clay_BorderRenderData &data = command.renderData.border;
                    snapshot.quads_.push_back(QuadDraw{
                        .instance = UiQuadInstance{
                            .position = {command.boundingBox.x, command.boundingBox.y},
                            .size = {command.boundingBox.width, command.boundingBox.height},
                            .corner_radius = {data.cornerRadius.topLeft, data.cornerRadius.topRight,
                                            data.cornerRadius.bottomLeft, data.cornerRadius.bottomRight},
                            .border_width = {static_cast<f32>(data.width.left), static_cast<f32>(data.width.right),
                                            static_cast<f32>(data.width.top), static_cast<f32>(data.width.bottom)},
                            .fill_color = {0.0f, 0.0f, 0.0f, 0.0f},
                            .border_color = clay_color_to_linear(data.color),
                            .uv_min = {0.0f, 0.0f},
                            .uv_max = {1.0f, 1.0f},
                            .kind = static_cast<f32>(UiQuadKind::Rect),
                        },
                        .image_ref = nullptr,
                        .scissor = active_scissor,
                        .paint = paint,
                    });
                    break;
                }
                case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
                    const Clay_ImageRenderData &data = command.renderData.image;
                    const auto *image_ref = static_cast<const ImageRef *>(data.imageData);
                    snapshot.quads_.push_back(QuadDraw{
                        .instance = UiQuadInstance{
                            .position = {command.boundingBox.x, command.boundingBox.y},
                            .size = {command.boundingBox.width, command.boundingBox.height},
                            .corner_radius = {data.cornerRadius.topLeft, data.cornerRadius.topRight,
                                            data.cornerRadius.bottomLeft, data.cornerRadius.bottomRight},
                            .fill_color = clay_color_to_linear(data.backgroundColor),
                            .uv_min = {0.0f, 0.0f},
                            .uv_max = {1.0f, 1.0f},
                            .kind = static_cast<f32>(UiQuadKind::Image),
                        },
                        .image_ref = image_ref,
                        .scissor = active_scissor,
                        .paint = paint,
                    });
                    break;
                }
                case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                    const Clay_TextRenderData &data = command.renderData.text;
                    const CachedShape *shape = text_bridge_.shape_and_cache(
                        TextStyle{.font_id = data.fontId, .font_size = data.fontSize, .letter_spacing = data.letterSpacing,
                                 .line_height = data.lineHeight},
                        string_view{data.stringContents.chars, static_cast<usize>(data.stringContents.length)});
                    if (shape != nullptr) {
                        append_glyph_placements(snapshot.glyphs_, snapshot.glyph_scissors_, snapshot.glyph_paint_,
                                               active_scissor, paint, *shape, data.fontSize,
                                               clay_color_to_linear(data.textColor),
                                               glm::vec2{command.boundingBox.x, command.boundingBox.y}, outline_cache_);
                    }
                    break;
                }
                case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
                    scissor_stack.push_back(intersect_rect(active_scissor, command.boundingBox));
                    break;
                }
                case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: {
                    if (scissor_stack.size() > 1) {
                        scissor_stack.pop_back();
                    }
                    break;
                }
                case CLAY_RENDER_COMMAND_TYPE_CUSTOM: {
                    const Clay_CustomRenderData &data = command.renderData.custom;
                    if (data.customData != nullptr) {
                        snapshot.custom_draws_.push_back(CustomDraw{
                            .position = {command.boundingBox.x, command.boundingBox.y},
                            .size = {command.boundingBox.width, command.boundingBox.height},
                            .shader = static_cast<const CustomShaderRef *>(data.customData),
                            .scissor = active_scissor,
                            .paint = paint,
                        });
                    }
                    break;
                }
                case CLAY_RENDER_COMMAND_TYPE_NONE:
                default:
                    break;
            }
        }

        snapshot.image_storage_ = std::move(image_storage_);
        image_storage_.clear();
        snapshot.custom_storage_ = std::move(custom_storage_);
        custom_storage_.clear();
        text_storage_.clear();
        if (!pointer_down_) {
            pointer_capture_id_.clear();
        }
        return snapshot;
    }

    /// Destroys or releases the `UI` resource represented by the supplied parameters.
    ///
    /// @return Returns the current destroy value.
    /// @note This function does not throw exceptions.
    void Context::destroy() noexcept {


        if (context_ != nullptr && Clay_GetCurrentContext() == context_) {
            Clay_SetCurrentContext(nullptr);
        }
        context_ = nullptr;
        arena_memory_.clear();
        arena_memory_.shrink_to_fit();
        text_storage_.clear();
        image_storage_.clear();
        custom_storage_.clear();
        outline_cache_.clear();
        pointer_capture_id_.clear();
        focused_id_.clear();
        current_frame_ids_.clear();
        last_frame_bounds_.clear();
        current_frame_clip_ids_.clear();
        last_frame_scroll_offsets_.clear();
    }

} // namespace SFT::UI
