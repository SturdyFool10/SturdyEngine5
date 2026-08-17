#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
// Clay is a single-header library: exactly one translation unit must define
// CLAY_IMPLEMENTATION before including clay.h to emit the actual function bodies (every other
// includer just gets declarations). This is that one TU.
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

        // Clay never interprets Clay_Color's numeric values itself (see Style.hpp's own doc comment
        // on UI::Color) — it just carries them opaquely through render commands — so this packs
        // Foundation::Color::Srgb's [0,1] channels straight in, with no *255 round-trip. The matching
        // unpack (clay_color_to_linear() below) is where the real conversion work happens.
        [[nodiscard]] Clay_Color to_clay_color(const Color &color) noexcept {
            return Clay_Color{
                .r = static_cast<f32>(color.r),
                .g = static_cast<f32>(color.g),
                .b = static_cast<f32>(color.b),
                .a = static_cast<f32>(color.a),
            };
        }

        [[nodiscard]] Clay_CornerRadius to_clay_corner_radius(const CornerRadius &radius) noexcept {
            return Clay_CornerRadius{
                .topLeft = radius.top_left,
                .topRight = radius.top_right,
                .bottomLeft = radius.bottom_left,
                .bottomRight = radius.bottom_right,
            };
        }

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

        [[nodiscard]] Clay_LayoutDirection to_clay_direction(LayoutDirection direction) noexcept {
            return direction == LayoutDirection::TopToBottom ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT;
        }

        [[nodiscard]] Clay_LayoutAlignmentX to_clay_align_x(AlignX align) noexcept {
            switch (align) {
                case AlignX::Left: return CLAY_ALIGN_X_LEFT;
                case AlignX::Center: return CLAY_ALIGN_X_CENTER;
                case AlignX::Right: return CLAY_ALIGN_X_RIGHT;
            }
            return CLAY_ALIGN_X_LEFT;
        }

        [[nodiscard]] Clay_LayoutAlignmentY to_clay_align_y(AlignY align) noexcept {
            switch (align) {
                case AlignY::Top: return CLAY_ALIGN_Y_TOP;
                case AlignY::Center: return CLAY_ALIGN_Y_CENTER;
                case AlignY::Bottom: return CLAY_ALIGN_Y_BOTTOM;
            }
            return CLAY_ALIGN_Y_TOP;
        }

        [[nodiscard]] Clay_BorderWidth to_clay_border_width(const BorderWidth &width) noexcept {
            return Clay_BorderWidth{
                .left = width.left,
                .right = width.right,
                .top = width.top,
                .bottom = width.bottom,
                .betweenChildren = width.between_children,
            };
        }

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

        // `z` is this element's *effective* depth (ElementDecl::z resolved against Context::
        // z_stack_ — see element()'s own doc comment) — packed directly into the pointer bits of
        // Clay_ElementDeclaration::userData (round-trips exactly, i32 always fits in intptr_t/
        // void* on every platform this engine targets) rather than through a heap-allocated
        // pointer, since it's the one piece of Context-owned per-element data every element needs
        // (unlike ImageRef*/CustomShaderRef*, which already have their own dedicated
        // Clay_ImageElementConfig::imageData/Clay_CustomElementConfig::customData slots and never
        // touch userData at all — see image()/svg()/custom_element()'s own comments).
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
                // Left zeroed here; Context::element() overwrites this with Clay_GetScrollOffset()
                // for any element with clip enabled, once it's open (see element()'s own comment).
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

        // Unpacks what to_clay_declaration()/text()'s Clay_TextElementConfig::userData packed —
        // the inverse of the reinterpret_cast<void*> above.
        [[nodiscard]] i32 unpack_z(void *user_data) noexcept {
            return static_cast<i32>(reinterpret_cast<intptr_t>(user_data));
        }

        [[nodiscard]] Clay_TextElementConfigWrapMode to_clay_wrap_mode(TextWrapMode mode) noexcept {
            switch (mode) {
                case TextWrapMode::Words: return CLAY_TEXT_WRAP_WORDS;
                case TextWrapMode::Newlines: return CLAY_TEXT_WRAP_NEWLINES;
                case TextWrapMode::None: return CLAY_TEXT_WRAP_NONE;
            }
            return CLAY_TEXT_WRAP_WORDS;
        }

        [[nodiscard]] Clay_TextAlignment to_clay_text_align(TextAlign align) noexcept {
            switch (align) {
                case TextAlign::Left: return CLAY_TEXT_ALIGN_LEFT;
                case TextAlign::Center: return CLAY_TEXT_ALIGN_CENTER;
                case TextAlign::Right: return CLAY_TEXT_ALIGN_RIGHT;
            }
            return CLAY_TEXT_ALIGN_LEFT;
        }

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

        void clay_error_handler(Clay_ErrorData error) {
            Foundation::log_error("Clay UI layout error: {}",
                                  string_view{error.errorText.chars, static_cast<usize>(error.errorText.length)});
        }

    } // namespace

    ElementScope::ElementScope(ElementScope &&other) noexcept
        : context_(std::exchange(other.context_, nullptr)), z_stack_(std::exchange(other.z_stack_, nullptr)) {}

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

    ElementScope::~ElementScope() {
        if (context_ != nullptr) {
            Clay_SetCurrentContext(context_);
            Clay__CloseElement();
            if (z_stack_ != nullptr && z_stack_->size() > 1) {
                z_stack_->pop_back();
            }
        }
    }

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
        // Clay_Initialize() stamped `&other.text_bridge_` into its measure-text userData; that
        // address is now stale (text_bridge_ just moved to a new home), so re-install it.
        if (context_ != nullptr) {
            Clay_SetCurrentContext(context_);
            Clay_SetMeasureTextFunction(&TextBridge::measure_callback, &text_bridge_);
        }
    }

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

    Context::~Context() { destroy(); }

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

    void Context::set_current() const noexcept {
        if (context_ != nullptr) {
            Clay_SetCurrentContext(context_);
        }
    }

    void Context::register_font(FontId font_id, const Text::Font &font, const Text::Font *emoji_fallback,
                                std::span<const Text::Font *const> fallbacks) {
        text_bridge_.register_font(font_id, font, emoji_fallback, fallbacks);
    }

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
        // Rebuilt fresh by element() below as this frame's elements get hover-tested — see
        // desired_cursor_'s own doc comment (Context.hpp).
        desired_cursor_ = CursorIcon::Default;
        // Every element()/custom_element() ElementScope pops what it pushed, so this is already
        // back to {0} by the end of a well-formed frame — reset explicitly anyway so a leaked scope
        // (e.g. a std::move()'d-away ElementScope's caller forgetting to keep it alive) can't leave
        // a later frame permanently stuck at some stale nonzero z.
        z_stack_.assign(1, 0);
        Clay_SetLayoutDimensions(Clay_Dimensions{
            .width = static_cast<f32>(layout_extent_.x),
            .height = static_cast<f32>(layout_extent_.y),
        });
        // Must run before Clay_BeginLayout(): it hit-tests against last frame's still-committed
        // layout tree, which is exactly what makes hovered(id)/clicked(id) answerable before this
        // frame has declared anything (see their doc comments in Context.hpp). Explicitly latched
        // edges preserve a complete click between UI frames; sampled-state derivation remains as a
        // compatibility fallback for callers that only populate PointerState::down.
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
        // Also needs last frame's committed scroll-container list (same reason it runs before
        // Clay_BeginLayout()); updates whichever container the pointer above is currently over, read
        // back by element() below via Clay's scroll-container query APIs.
        //
        // Clay applies whatever delta it's given this frame immediately (clay.h: `scrollPosition +=
        // scrollDelta * 10`) — there's no smoothing concept on its side. To ease a wheel tick across
        // several frames instead of snapping in one, accumulate incoming deltas into
        // pending_scroll_delta_ and only hand Clay a fraction of it each frame, keeping the remainder
        // for subsequent frames. Disabling smooth_scrolling (or a zero/negative delta_seconds, e.g. a
        // caller that hasn't wired up frame timing) just forwards the whole pending amount, matching
        // the previous always-instant behavior exactly.
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

    void Context::update_desired_cursor(const ElementDecl &decl) noexcept {
        if (!cursor_management_enabled_ || decl.cursor == CursorIcon::Auto || decl.id.empty()) {
            return;
        }
        // Later declarations win over earlier ones for the same point — see desired_cursor_'s own
        // doc comment (Context.hpp) for why that gives "more specific (a child) wins over less
        // specific (its ancestor)" for the common nested case, without needing z/paint-order
        // bookkeeping the way the actual visual stacking (QuadDraw::scissor, PaintKey) does.
        if (hovered(decl.id)) {
            desired_cursor_ = decl.cursor;
        }
    }

    void Context::force_cursor(CursorIcon icon) noexcept {
        if (!cursor_management_enabled_) {
            return;
        }
        desired_cursor_ = icon;
    }

    ElementScope Context::element(const ElementDecl &decl) {
        set_current();
        if (!decl.id.empty()) {
            current_frame_ids_.push_back(decl.id.cpp_string());
        }
        update_desired_cursor(decl);
        // 0 means "inherit" (see ElementDecl::z's own doc comment, Style.hpp) — every element
        // pushes its own *effective* z so text()/image()/svg()/nested element() calls made while
        // this one is open (i.e. before its ElementScope is destroyed) inherit it in turn.
        const i32 effective_z = decl.z != 0 ? decl.z : z_stack_.back();
        z_stack_.push_back(effective_z);
        Clay__OpenElement();
        Clay_ElementDeclaration declaration = to_clay_declaration(decl, effective_z);
        if (decl.clip.horizontal || decl.clip.vertical) {
            // Read back last frame's committed scroll position rather than querying Clay directly
            // here — see current_frame_clip_ids_/last_frame_scroll_offsets_'s own doc comment
            // (Context.hpp) for why querying *this* element's own scroll state mid-open, before
            // Clay__ConfigureOpenElement() below has (re)attached its clip config for this frame,
            // silently comes back empty every single frame rather than merely on the first one.
            if (!decl.id.empty()) {
                current_frame_clip_ids_.push_back(decl.id.cpp_string());
                const auto cached = last_frame_scroll_offsets_.find(decl.id.cpp_string());
                if (cached != last_frame_scroll_offsets_.end()) {
                    declaration.clip.childOffset = Clay_Vector2{.x = cached->second.x, .y = cached->second.y};
                }
            } else {
                // Anonymous non-floating containers retain Clay's ordinary current-element behavior
                // (matched by the currently-open element pointer, not by id, so no staleness here).
                declaration.clip.childOffset = Clay_GetScrollOffset();
            }
        }
        Clay__ConfigureOpenElement(declaration);
        return ElementScope{context_, &z_stack_};
    }

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
        // Clay__OpenTextElement only stores the pointer it's given, not a copy — it expects
        // whatever CLAY_TEXT_CONFIG()/Clay__StoreTextElementConfig() already copied into Clay's own
        // arena-backed textElementConfigs array (see clay.h). A stack-local config here would leave
        // that pointer dangling by the time Clay_EndLayout() reads it back for render commands.
        // Text has no z of its own (TextStyle carries none) — it always inherits whichever element
        // currently encloses it.
        Clay_TextElementConfig *config = Clay__StoreTextElementConfig(to_clay_text_config(style, z_stack_.back()));
        Clay__OpenTextElement(clay_string, config);
    }

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

        // One (pixel_x, byte_offset) boundary per glyph cluster, in the same pen-advance order
        // append_glyph_placements() (above) paints glyphs in — the click just needs the nearest one,
        // not the actual paint positions, so this skips outline lookup/GlyphPlacement construction
        // entirely.
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

    void Context::svg(const ElementDecl &decl, Renderer::TextureHandle texture) { image(decl, texture); }

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

    bool Context::clicked(const UString &id) const noexcept {
        if (!pointer_pressed_this_frame_) {
            return false;
        }
        // hovered()'s Clay_PointerOver() is already both scissor-aware (an element whose layout box
        // extends outside an ancestor ClipConfig's rect is excluded — clay.h's own hit-test DFS
        // checks the clip element's boundingBox too, not just the target's own) and layer-aware
        // (Clay hit-tests floating roots highest-z-first and stops at the first one whose
        // FloatingConfig::capture_pointer is true, so an overlay correctly blocks clicks reaching
        // whatever sits behind it) — see clay.h's Clay_SetPointerState. Reuse it directly whenever
        // the press happened at the position Clay already hit-tested this frame, which is the
        // overwhelming common case: every PointerState producer in this codebase sets press_position
        // equal to position at the moment of the press (see PointerState::press_position's own doc
        // comment, Context.hpp). They can only diverge for a *latched* edge — a press event and this
        // frame's begin_layout() poll separated by pointer movement — for which Clay has no hit-test
        // result to reuse (it only ever hit-tests the position it was last given), so this falls back
        // to a raw bounds check that does *not* account for layers/scissors; no backend in this
        // codebase actually produces that divergence today.
        if (pointer_press_position_ == pointer_position_) {
            return hovered(id);
        }
        const std::optional<ElementBounds> bounds = element_bounds(id);
        return bounds.has_value() && pointer_press_position_.x >= bounds->position.x &&
               pointer_press_position_.y >= bounds->position.y &&
               pointer_press_position_.x <= bounds->position.x + bounds->size.x &&
               pointer_press_position_.y <= bounds->position.y + bounds->size.y;
    }

    bool Context::pointer_down(const UString &id) const noexcept { return pointer_down_ && hovered(id); }

    std::optional<ElementBounds> Context::element_bounds(const UString &id) const noexcept {
        if (id.empty()) {
            return std::nullopt;
        }
        const auto found = last_frame_bounds_.find(id.cpp_string());
        return found != last_frame_bounds_.end() ? std::optional<ElementBounds>{found->second} : std::nullopt;
    }

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

    bool Context::has_pointer_capture(const UString &id) const noexcept {
        return !id.empty() && pointer_capture_id_ == id.cpp_string_view();
    }

    void Context::release_pointer(const UString &id) noexcept {
        if (has_pointer_capture(id)) {
            pointer_capture_id_.clear();
        }
    }

    void Context::focus(const UString &id) {
        if (!id.empty()) {
            focused_id_ = id.cpp_string();
        }
    }

    bool Context::has_focus(const UString &id) const noexcept {
        return !id.empty() && focused_id_ == id.cpp_string_view();
    }

    void Context::clear_focus(const UString &id) noexcept {
        if (has_focus(id)) {
            focused_id_.clear();
        }
    }

    bool Context::pointer_over_any() const noexcept {
        // See clicked()'s own comment: raw bounding-box containment ignores both scissor clipping
        // and layer ordering, so this used to report true for a point sitting over a *named*
        // element's layout box even when that box was entirely clipped away by an ancestor, or
        // hidden behind an occluding floating overlay. hovered() (Clay_PointerOver()) is already
        // correct on both counts and is evaluated at the current pointer_position_ — exactly what
        // this query wants (unlike clicked(), there's no separate press position involved here).
        for (const auto &[id, bounds] : last_frame_bounds_) {
            (void)bounds;
            if (hovered(UString{id})) {
                return true;
            }
        }
        return false;
    }

    bool Context::clicked_outside(const UString &id) const noexcept {
        return pointer_pressed_this_frame_ && !clicked(id);
    }

    namespace {
        [[nodiscard]] Clay_ElementId to_clay_element_id(const UString &id) noexcept {
            const string_view id_view = id.cpp_string_view();
            return Clay_GetElementId(
                Clay_String{.isStaticallyAllocated = false, .length = static_cast<i32>(id_view.size()), .chars = id_view.data()});
        }
    } // namespace

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

        // Clamps `*axis` so the container's own scroll range (never positive, never further back
        // than its content overhangs the container) is respected regardless of how far this nudge
        // pushed it — mirrors Clay__UpdateScrollContainers' own clamping (clay.h), which a manual
        // scrollPosition write like this one bypasses otherwise.
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
        // Mutating Clay's live scrollPosition alone isn't enough: element()'s clip-handling never
        // re-reads it directly (see current_frame_clip_ids_'s own doc comment, Context.hpp, for why
        // querying an element's own clip config mid-configure is unsafe) — it only ever reads
        // last_frame_scroll_offsets_, populated once per frame from finish_frame(). Without this,
        // a caller that sets the offset *between* frames (or mid-frame, before this container's own
        // finish_frame() cache refresh runs) would see it silently reverted on the very next
        // begin_layout(), since element() would still hand Clay back the stale cached childOffset.
        last_frame_scroll_offsets_.insert_or_assign(
            container_id.cpp_string(), glm::vec2{scroll.scrollPosition->x, scroll.scrollPosition->y});
        return true;
    }

    namespace {

        // Every color reaching the GPU (quad fill/border, glyph fill) goes through here — converts
        // the [0,1] sRGB channels to_clay_color() packed (see its own doc comment) to linear, since
        // the swapchain target is an *Srgb format that re-encodes on write; feeding it already-sRGB
        // numbers directly (the previous naive /255-only unpack this replaced) double-applies the
        // gamma curve, which is why UI colors used to render visibly washed out relative to their
        // authored values.
        [[nodiscard]] glm::vec4 clay_color_to_linear(Clay_Color color) noexcept {
            const Foundation::Color::Linear linear =
                Foundation::Color::Srgb{color.r, color.g, color.b, color.a}.to_linear();
            return glm::vec4{static_cast<f32>(linear.r), static_cast<f32>(linear.g), static_cast<f32>(linear.b),
                             static_cast<f32>(linear.a)};
        }

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

        // Resolves which loaded Font a shaped glyph actually belongs to — shape_with_fallback()
        // (Text::FontStack) may have shaped any given glyph against the primary font, the dedicated
        // emoji font, or any registered fallback font (CJK, ...), and stamps which one onto
        // PositionedGlyph::font_id. Outline extraction and rasterization both need the *matching*
        // font, not always the primary one, or a fallback-font glyph's index is looked up in the
        // wrong font's glyph table entirely (garbage/empty geometry, not just a wrong glyph).
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
            // Every font_id a glyph carries was stamped by shape_with_fallback() from this exact
            // FontStack, so this shouldn't happen — fall back to primary rather than risk a null
            // Font* reaching the outline/rasterization path below.
            return fonts.primary;
        }

        // Builds GlyphPlacements for one shaped text run at `origin` (the Clay TEXT command's
        // bounding-box top-left) — the same pen-advance loop Renderer/RendererTextOverlay.cpp uses,
        // adapted to read from a UI::CachedShape instead of its own line cache.
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

        // Safe now that Clay_EndLayout() has run: this frame's clip elements have their clip config
        // fully (re)attached, so Clay_GetScrollContainerData() can actually find it — see
        // current_frame_clip_ids_'s own doc comment (Context.hpp) for why the same query from
        // inside element() itself, earlier in this same frame, cannot.
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
            // `i` is this command's position in Clay's own render-command stream, which already
            // interleaves an element's background, its children (text included), and its border in
            // that exact order (see Style.hpp's PaintKey doc comment) — using it directly as the
            // paint-order tiebreak is what keeps that natural ordering intact once UiRenderer
            // re-sorts everything by z.
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

    void Context::destroy() noexcept {
        // Clay_Initialize() allocates its Clay_Context struct out of arena_memory_ itself, and
        // Clay's global Clay__currentContext (Clay_GetCurrentContext()/Clay_SetCurrentContext())
        // keeps pointing at it even after arena_memory_ below is freed — a dangling global that the
        // *next* Context::create() on this thread would otherwise inherit and immediately
        // dereference via Clay_SetMaxElementCount(). Only clear it if it's still pointing at this
        // Context specifically, so destroying one Context can't stomp on a different, still-live
        // one that happens to be current.
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
