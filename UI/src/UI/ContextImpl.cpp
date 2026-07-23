#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
// Clay is a single-header library: exactly one translation unit must define
// CLAY_IMPLEMENTATION before including clay.h to emit the actual function bodies (every other
// includer just gets declarations). This is that one TU.
#define CLAY_IMPLEMENTATION
#include <clay.h>
#include <cmath>
#include <string_view>
#include <utility>
#pragma endregion

#include "Context.hpp"

using std::string_view;
using std::unordered_map;

namespace SFT::UI {

    namespace {

        [[nodiscard]] Clay_Color to_clay_color(const Color &color) noexcept {
            return Clay_Color{.r = color.r, .g = color.g, .b = color.b, .a = color.a};
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

        [[nodiscard]] Clay_ElementDeclaration to_clay_declaration(const ElementDecl &decl, void *user_data) noexcept {
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
                .childOffset = Clay_Vector2{.x = decl.clip.child_offset.x, .y = decl.clip.child_offset.y},
            };
            if (decl.id.size() != 0) {
                const string_view id_view = decl.id.cpp_string_view();
                result.id = Clay_GetElementId(
                    Clay_String{.isStaticallyAllocated = false, .length = static_cast<i32>(id_view.size()), .chars = id_view.data()});
            }
            result.userData = user_data;
            return result;
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

        [[nodiscard]] Clay_TextElementConfig to_clay_text_config(const TextStyle &style) noexcept {
            return Clay_TextElementConfig{
                .userData = nullptr,
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

    ElementScope::ElementScope(ElementScope &&other) noexcept : context_(std::exchange(other.context_, nullptr)) {}

    ElementScope &ElementScope::operator=(ElementScope &&other) noexcept {
        if (this != &other) {
            if (context_ != nullptr) {
                Clay_SetCurrentContext(context_);
                Clay__CloseElement();
            }
            context_ = std::exchange(other.context_, nullptr);
        }
        return *this;
    }

    ElementScope::~ElementScope() {
        if (context_ != nullptr) {
            Clay_SetCurrentContext(context_);
            Clay__CloseElement();
        }
    }

    Context::Context(Context &&other) noexcept
        : context_(std::exchange(other.context_, nullptr)),
          arena_memory_(std::move(other.arena_memory_)),
          text_bridge_(std::move(other.text_bridge_)),
          text_storage_(std::move(other.text_storage_)),
          image_storage_(std::move(other.image_storage_)),
          outline_cache_(std::move(other.outline_cache_)),
          pointer_down_(other.pointer_down_),
          pointer_pressed_this_frame_(other.pointer_pressed_this_frame_) {
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
            outline_cache_ = std::move(other.outline_cache_);
            pointer_down_ = other.pointer_down_;
            pointer_pressed_this_frame_ = other.pointer_pressed_this_frame_;
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

    void Context::register_font(FontId font_id, const Text::Font &font, const Text::Font *emoji_fallback) {
        text_bridge_.register_font(font_id, font, emoji_fallback);
    }

    void Context::begin_layout(glm::vec2 viewport_size, const PointerState &pointer) {
        set_current();
        text_bridge_.begin_frame();
        text_storage_.clear();
        image_storage_.clear();
        Clay_SetLayoutDimensions(Clay_Dimensions{.width = viewport_size.x, .height = viewport_size.y});
        // Must run before Clay_BeginLayout(): it hit-tests against last frame's still-committed
        // layout tree, which is exactly what makes hovered(id)/clicked(id) answerable before this
        // frame has declared anything (see their doc comments in Context.hpp).
        pointer_pressed_this_frame_ = pointer.down && !pointer_down_;
        pointer_down_ = pointer.down;
        Clay_SetPointerState(Clay_Vector2{.x = pointer.position.x, .y = pointer.position.y}, pointer.down);
        Clay_BeginLayout();
    }

    ElementScope Context::element(const ElementDecl &decl) {
        set_current();
        Clay__OpenElement();
        Clay__ConfigureOpenElement(to_clay_declaration(decl, nullptr));
        return ElementScope{context_};
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
        Clay_TextElementConfig *config = Clay__StoreTextElementConfig(to_clay_text_config(style));
        Clay__OpenTextElement(clay_string, config);
    }

    void Context::image(const ElementDecl &decl, Renderer::TextureHandle texture) {
        set_current();
        image_storage_.push_back(ImageRef{.texture = texture});
        ImageRef *stored = &image_storage_.back();
        Clay_ElementDeclaration declaration = to_clay_declaration(decl, stored);
        declaration.image = Clay_ImageElementConfig{.imageData = stored};
        Clay__OpenElement();
        Clay__ConfigureOpenElement(declaration);
        Clay__CloseElement();
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

    bool Context::clicked(const UString &id) const noexcept { return pointer_pressed_this_frame_ && hovered(id); }

    bool Context::pointer_down(const UString &id) const noexcept { return pointer_down_ && hovered(id); }

    namespace {

        [[nodiscard]] glm::vec4 clay_color_to_unit(Clay_Color color) noexcept {
            return glm::vec4{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
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

        // Builds GlyphPlacements for one shaped text run at `origin` (the Clay TEXT command's
        // bounding-box top-left) — the same pen-advance loop Renderer/RendererTextOverlay.cpp uses,
        // adapted to read from a UI::CachedShape instead of its own line cache.
        void append_glyph_placements(vector<Renderer::GlyphPlacement> &out, const CachedShape &shape, u16 font_size,
                                     glm::vec4 color, glm::vec2 origin, unordered_map<u64, Text::GlyphOutline> &outline_cache) {
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
                    const Text::GlyphOutline *outline = nullptr;
                    if (!glyph.is_color) {
                        const u64 key = (static_cast<u64>(glyph.font_id) << 32) | glyph.glyph_id;
                        auto cached = outline_cache.find(key);
                        if (cached == outline_cache.end()) {
                            if (auto extracted = Text::glyph_outline(primary_font, glyph.glyph_id)) {
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
                        .font = glyph.is_color ? shape.fonts->emoji : shape.fonts->primary,
                    });

                    cursor.x += glyph.x_advance * run_scale;
                    cursor.y -= glyph.y_advance * run_scale;
                }
                visual_run_x += run.advance_em * static_cast<f32>(font_size);
            }
        }

    } // namespace

    FrameSnapshot Context::finish_frame(glm::vec2 viewport_size) {
        set_current();
        FrameSnapshot snapshot;
        snapshot.full_viewport_scissor_ = RHI::Rect2D{
            .x = 0, .y = 0, .width = static_cast<u32>(viewport_size.x), .height = static_cast<u32>(viewport_size.y)};

        Clay_RenderCommandArray commands = Clay_EndLayout();
        vector<RHI::Rect2D> scissor_stack{snapshot.full_viewport_scissor_};

        for (i32 i = 0; i < commands.length; ++i) {
            const Clay_RenderCommand &command = *Clay_RenderCommandArray_Get(&commands, i);
            const RHI::Rect2D active_scissor = scissor_stack.back();
            switch (command.commandType) {
                case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                    const Clay_RectangleRenderData &data = command.renderData.rectangle;
                    snapshot.quads_.push_back(QuadDraw{
                        .instance = UiQuadInstance{
                            .position = {command.boundingBox.x, command.boundingBox.y},
                            .size = {command.boundingBox.width, command.boundingBox.height},
                            .corner_radius = {data.cornerRadius.topLeft, data.cornerRadius.topRight,
                                            data.cornerRadius.bottomLeft, data.cornerRadius.bottomRight},
                            .fill_color = clay_color_to_unit(data.backgroundColor),
                            .uv_min = {0.0f, 0.0f},
                            .uv_max = {1.0f, 1.0f},
                            .kind = static_cast<f32>(UiQuadKind::Rect),
                        },
                        .image_ref = nullptr,
                        .scissor = active_scissor,
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
                            .border_color = clay_color_to_unit(data.color),
                            .uv_min = {0.0f, 0.0f},
                            .uv_max = {1.0f, 1.0f},
                            .kind = static_cast<f32>(UiQuadKind::Rect),
                        },
                        .image_ref = nullptr,
                        .scissor = active_scissor,
                    });
                    break;
                }
                case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
                    const Clay_ImageRenderData &data = command.renderData.image;
                    snapshot.quads_.push_back(QuadDraw{
                        .instance = UiQuadInstance{
                            .position = {command.boundingBox.x, command.boundingBox.y},
                            .size = {command.boundingBox.width, command.boundingBox.height},
                            .corner_radius = {data.cornerRadius.topLeft, data.cornerRadius.topRight,
                                            data.cornerRadius.bottomLeft, data.cornerRadius.bottomRight},
                            .fill_color = clay_color_to_unit(data.backgroundColor),
                            .uv_min = {0.0f, 0.0f},
                            .uv_max = {1.0f, 1.0f},
                            .kind = static_cast<f32>(UiQuadKind::Image),
                        },
                        .image_ref = static_cast<const ImageRef *>(data.imageData),
                        .scissor = active_scissor,
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
                        append_glyph_placements(snapshot.glyphs_, *shape, data.fontSize, clay_color_to_unit(data.textColor),
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
                case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
                case CLAY_RENDER_COMMAND_TYPE_NONE:
                default:
                    // Phase 3 (plans/clay-ui-renderer.md): shader-driven custom elements. Not wired
                    // up yet — skipped rather than drawn incorrectly.
                    break;
            }
        }

        snapshot.image_storage_ = std::move(image_storage_);
        image_storage_.clear();
        text_storage_.clear();
        return snapshot;
    }

    void Context::destroy() noexcept {
        context_ = nullptr;
        arena_memory_.clear();
        arena_memory_.shrink_to_fit();
        text_storage_.clear();
        image_storage_.clear();
        outline_cache_.clear();
    }

} // namespace SFT::UI
