#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cstdlib>


#define SECLAY_IMPLEMENTATION
#include <Renderer/UI/SEClay/SEClay.h>
#include <cmath>
#include <glm/geometric.hpp>
#include <limits>
#include <string_view>
#include <utility>
#pragma endregion

#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/UiEffectLibrary.hpp>

using std::string_view;
using std::unordered_map;

namespace SFT::UI {
    /// One horizontal text-decoration line of `thickness` starting at (`x`, `top`): solid, double (two thirds-thick lines), dashed or dotted.
    inline void append_text_decoration_line(vector<QuadDraw> &out, f32 x, f32 top, f32 length, f32 thickness, BorderLineStyle style,
                                            const glm::vec4 &color, const RHI::Rect2D &scissor, const PaintKey &paint) {
        if (!(length > 0.0f) || !(thickness > 0.0f) || !(color.a > 0.0f)) {
            return;
        }
        const auto rect = [&](f32 at_x, f32 at_y, f32 w, f32 h, f32 round) {
            out.push_back(QuadDraw{.instance = UiQuadInstance{.position = {at_x, at_y},
                                                              .size = {w, h},
                                                              .corner_radius = glm::vec4{round},
                                                              .fill_color = color,
                                                              .kind = static_cast<f32>(UiQuadKind::Rect)},
                                   .image_ref = nullptr,
                                   .scissor = scissor,
                                   .paint = paint});
        };
        switch (style) {
            case BorderLineStyle::Solid: rect(x, top, length, thickness, 0.0f); break;
            case BorderLineStyle::Double:
                if (thickness < 3.0f) {
                    rect(x, top, length, thickness, 0.0f);
                } else {
                    const f32 each = thickness / 3.0f;
                    rect(x, top, length, each, 0.0f);
                    rect(x, top + thickness - each, length, each, 0.0f);
                }
                break;
            case BorderLineStyle::Dashed:
                for (f32 at = 0.0f; at < length; at += 5.0f * thickness) {
                    rect(x + at, top, std::min(3.0f * thickness, length - at), thickness, 0.0f);
                }
                break;
            case BorderLineStyle::Dotted:
                for (f32 at = 0.0f; at + thickness <= length + 0.001f; at += 2.0f * thickness) {
                    rect(x + at, top, thickness, thickness, thickness * 0.5f);
                }
                break;
        }
    }

    /// The quads of a border whose edges differ. A solid edge is the element's own border shader with only that edge's width set, so it
    /// follows rounded and shaped corners; dashed, dotted and double edges are straight segments between the corners.
    inline void append_edge_borders(vector<QuadDraw> &out, const BorderEdges &edges, glm::vec2 position, glm::vec2 size,
                                    const SEClay_BorderRenderData &data, const RHI::Rect2D &scissor, const PaintKey &paint) {
        const glm::vec4 radius{data.cornerRadius.topLeft, data.cornerRadius.topRight, data.cornerRadius.bottomLeft, data.cornerRadius.bottomRight};
        const auto to_linear = [](const Color &color) {
            const Foundation::Color::Linear linear = Foundation::Color::Srgb{color.r, color.g, color.b, color.a}.to_linear();
            return glm::vec4{static_cast<f32>(linear.r), static_cast<f32>(linear.g), static_cast<f32>(linear.b), static_cast<f32>(linear.a)};
        };
        const auto rect = [&](glm::vec2 at, glm::vec2 extent, const glm::vec4 &color, f32 round) {
            if (!(extent.x > 0.0f) || !(extent.y > 0.0f) || !(color.a > 0.0f)) {
                return;
            }
            out.push_back(QuadDraw{.instance = UiQuadInstance{.position = at,
                                                              .size = extent,
                                                              .corner_radius = glm::vec4{round},
                                                              .fill_color = color,
                                                              .kind = static_cast<f32>(UiQuadKind::Rect)},
                                   .image_ref = nullptr,
                                   .scissor = scissor,
                                   .paint = paint});
        };
        // One edge. `index` 0..3 = left, right, top, bottom.
        const auto edge = [&](const BorderEdge &style, usize index, f32 width) {
            if (!(width > 0.0f) || !(style.color.a > 0.0)) {
                return;
            }
            const glm::vec4 color = to_linear(style.color);
            const bool horizontal = index >= 2;
            if (style.style == BorderLineStyle::Solid || (style.style == BorderLineStyle::Double && width < 3.0f)) {
                glm::vec4 widths{0.0f};
                widths[static_cast<int>(index)] = width;
                out.push_back(QuadDraw{.instance = UiQuadInstance{.position = position,
                                                                  .size = size,
                                                                  .corner_radius = radius,
                                                                  .corner_shape_codes = data.cornerRadius.shapeCodes,
                                                                  .border_width = widths,
                                                                  .fill_color = glm::vec4{0.0f},
                                                                  .border_color = color,
                                                                  .kind = static_cast<f32>(UiQuadKind::Rect)},
                                       .image_ref = nullptr,
                                       .scissor = scissor,
                                       .paint = paint});
                return;
            }
            // A straight segment between the corners: `along` from the start of the edge, `across` from its outside.
            const f32 inset_start = horizontal ? (index == 2 ? radius.x : radius.z) : (index == 0 ? radius.x : radius.y);
            const f32 inset_end = horizontal ? (index == 2 ? radius.y : radius.w) : (index == 0 ? radius.z : radius.w);
            const f32 span = (horizontal ? size.x : size.y) - inset_start - inset_end;
            if (!(span > 0.0f)) {
                return;
            }
            const auto band_origin = [&](f32 across, f32 along) {
                switch (index) {
                    case 0: return position + glm::vec2{across, inset_start + along};
                    case 1: return position + glm::vec2{size.x - width + across, inset_start + along};
                    case 2: return position + glm::vec2{inset_start + along, across};
                    default: return position + glm::vec2{inset_start + along, size.y - width + across};
                }
            };
            const auto band_size = [&](f32 across, f32 length) { return horizontal ? glm::vec2{length, across} : glm::vec2{across, length}; };
            if (style.style == BorderLineStyle::Double) {
                const f32 line = width / 3.0f;
                rect(band_origin(0.0f, 0.0f), band_size(line, span), color, 0.0f);
                rect(band_origin(width - line, 0.0f), band_size(line, span), color, 0.0f);
            } else if (style.style == BorderLineStyle::Dashed) {
                const f32 dash = 3.0f * width;
                const f32 gap = 2.0f * width;
                for (f32 at = 0.0f; at < span; at += dash + gap) {
                    rect(band_origin(0.0f, at), band_size(width, std::min(dash, span - at)), color, 0.0f);
                }
            } else { // Dotted
                const f32 step = 2.0f * width;
                for (f32 at = 0.0f; at + width <= span + 0.001f; at += step) {
                    rect(band_origin(0.0f, at), band_size(width, width), color, width * 0.5f);
                }
            }
        };
        edge(edges.left, 0, static_cast<f32>(data.width.left));
        edge(edges.right, 1, static_cast<f32>(data.width.right));
        edge(edges.top, 2, static_cast<f32>(data.width.top));
        edge(edges.bottom, 3, static_cast<f32>(data.width.bottom));
    }


    /// Where an image goes inside its box: the visible rectangle (offset and size from the box's top-left, in box units) and the part of
    /// the image that shows there (uv). See `ImageFit`.
    struct ImagePlacement {
        glm::vec2 offset{0.0f};
        glm::vec2 size{0.0f};
        glm::vec2 uv_min{0.0f};
        glm::vec2 uv_max{1.0f};
        /// The image covers the whole box (so the box's rounded corners apply to it).
        bool full_box = true;
    };

    [[nodiscard]] inline ImagePlacement place_image(const ImageFitOptions &options, glm::vec2 box) noexcept {
        ImagePlacement out;
        out.size = box;
        if (options.fit == ImageFit::Fill || !(options.intrinsic_size.x > 0.0f) || !(options.intrinsic_size.y > 0.0f) || !(box.x > 0.0f) ||
            !(box.y > 0.0f)) {
            return out;
        }
        const glm::vec2 image = options.intrinsic_size;
        const f32 contain = std::min(box.x / image.x, box.y / image.y);
        const f32 cover = std::max(box.x / image.x, box.y / image.y);
        f32 scale = 1.0f;
        switch (options.fit) {
            case ImageFit::Contain: scale = contain; break;
            case ImageFit::Cover: scale = cover; break;
            case ImageFit::ScaleDown: scale = std::min(1.0f, contain); break;
            case ImageFit::None:
            case ImageFit::Fill: scale = 1.0f; break;
        }
        const glm::vec2 shown = image * scale;
        const glm::vec2 anchor = glm::clamp(options.position, glm::vec2{0.0f}, glm::vec2{1.0f});
        const glm::vec2 origin = (box - shown) * anchor; // where the image's top-left lands, relative to the box (negative when cropped)
        const glm::vec2 visible_min = glm::max(origin, glm::vec2{0.0f});
        const glm::vec2 visible_max = glm::min(origin + shown, box);
        out.offset = visible_min;
        out.size = glm::max(visible_max - visible_min, glm::vec2{0.0f});
        out.uv_min = (visible_min - origin) / shown;
        out.uv_max = (visible_max - origin) / shown;
        out.full_box = out.size.x >= box.x - 0.001f && out.size.y >= box.y - 0.001f;
        return out;
    }


    namespace {


        /// Converts the value to clay color representation.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @return Returns the value converted to clay color representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SEClay_Color to_clay_color(const Color &color) noexcept {
            return SEClay_Color{
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
        [[nodiscard]] SEClay_CornerRadius to_clay_corner_radius(const CornerRadius &radius, const CornerShapes &shapes) noexcept {
            return SEClay_CornerRadius{
                .topLeft = radius.top_left,
                .topRight = radius.top_right,
                .bottomLeft = radius.bottom_left,
                .bottomRight = radius.bottom_right,
                .shapeCodes = pack_corner_shapes(shapes),
            };
        }

        /// Converts the value to clay sizing axis representation.
        ///
        /// @param axis `axis` value used by the operation.
        ///
        /// @return Returns the value converted to clay sizing axis representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SEClay_SizingAxis to_clay_sizing_axis(const SizingAxis &axis) noexcept {
            SEClay_SizingAxis result{};
            switch (axis.kind) {
                case SizingKind::Fit:
                    result.type = SEClay__SizingType::Fit;
                    result.size.minMax = SEClay_SizingMinMax{.min = axis.min, .max = axis.max};
                    break;
                case SizingKind::Grow:
                    result.type = SEClay__SizingType::Grow;
                    result.size.minMax = SEClay_SizingMinMax{.min = axis.min, .max = axis.max};
                    break;
                case SizingKind::Fixed:
                    result.type = SEClay__SizingType::Fixed;
                    result.size.minMax = SEClay_SizingMinMax{.min = axis.value, .max = axis.value};
                    break;
                case SizingKind::Percent:
                    result.type = SEClay__SizingType::Percent;
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
        [[nodiscard]] SEClay_LayoutDirection to_clay_direction(LayoutDirection direction) noexcept {
            return direction == LayoutDirection::TopToBottom ? SEClay_LayoutDirection::TopToBottom : SEClay_LayoutDirection::LeftToRight;
        }

        /// Converts the value to clay align x representation.
        ///
        /// @param align `align` value used by the operation.
        ///
        /// @return Returns the value converted to clay align x representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SEClay_LayoutAlignmentX to_clay_align_x(AlignX align) noexcept {
            switch (align) {
                case AlignX::Left: return SEClay_LayoutAlignmentX::Left;
                case AlignX::Center: return SEClay_LayoutAlignmentX::Center;
                case AlignX::Right: return SEClay_LayoutAlignmentX::Right;
            }
            return SEClay_LayoutAlignmentX::Left;
        }

        /// Converts the value to clay align y representation.
        ///
        /// @param align `align` value used by the operation.
        ///
        /// @return Returns the value converted to clay align y representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SEClay_LayoutAlignmentY to_clay_align_y(AlignY align) noexcept {
            switch (align) {
                case AlignY::Top: return SEClay_LayoutAlignmentY::Top;
                case AlignY::Center: return SEClay_LayoutAlignmentY::Center;
                case AlignY::Bottom: return SEClay_LayoutAlignmentY::Bottom;
            }
            return SEClay_LayoutAlignmentY::Top;
        }

        /// Converts the value to clay border width representation.
        ///
        /// @param width Width of the target extent.
        ///
        /// @return Returns the value converted to clay border width representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SEClay_BorderWidth to_clay_border_width(const BorderWidth &width) noexcept {
            return SEClay_BorderWidth{
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
        [[nodiscard]] SEClay_FloatingAttachPointType to_clay_attach_point(FloatingAttachPoint point) noexcept {
            switch (point) {
                case FloatingAttachPoint::LeftTop: return SEClay_FloatingAttachPointType::LeftTop;
                case FloatingAttachPoint::LeftCenter: return SEClay_FloatingAttachPointType::LeftCenter;
                case FloatingAttachPoint::LeftBottom: return SEClay_FloatingAttachPointType::LeftBottom;
                case FloatingAttachPoint::CenterTop: return SEClay_FloatingAttachPointType::CenterTop;
                case FloatingAttachPoint::CenterCenter: return SEClay_FloatingAttachPointType::CenterCenter;
                case FloatingAttachPoint::CenterBottom: return SEClay_FloatingAttachPointType::CenterBottom;
                case FloatingAttachPoint::RightTop: return SEClay_FloatingAttachPointType::RightTop;
                case FloatingAttachPoint::RightCenter: return SEClay_FloatingAttachPointType::RightCenter;
                case FloatingAttachPoint::RightBottom: return SEClay_FloatingAttachPointType::RightBottom;
            }
            return SEClay_FloatingAttachPointType::LeftTop;
        }


        /// Converts the value to clay declaration representation.
        ///
        /// @param decl `decl` value used by the operation.
        /// @param z `z` value used by the operation.
        ///
        /// @return Returns the value converted to clay declaration representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SEClay_ElementDeclaration to_clay_declaration(const ElementDecl &decl, i32 z) noexcept {
            SEClay_ElementDeclaration result{};
            result.layout.sizing = SEClay_Sizing{
                .width = to_clay_sizing_axis(decl.sizing.width),
                .height = to_clay_sizing_axis(decl.sizing.height),
            };
            result.layout.padding = SEClay_Padding{decl.padding.left, decl.padding.right, decl.padding.top, decl.padding.bottom};
            if (decl.inset_for_corners) {
                const f32 tl = corner_content_inset(decl.corner_radius.top_left, decl.corner_shapes.top_left);
                const f32 tr = corner_content_inset(decl.corner_radius.top_right, decl.corner_shapes.top_right);
                const f32 bl = corner_content_inset(decl.corner_radius.bottom_left, decl.corner_shapes.bottom_left);
                const f32 br = corner_content_inset(decl.corner_radius.bottom_right, decl.corner_shapes.bottom_right);
                const auto grow = [](u16 declared, f32 needed) {
                    return static_cast<u16>(std::max<f32>(declared, std::min<f32>(std::ceil(needed), 65535.0f)));
                };
                result.layout.padding.left = grow(decl.padding.left, std::max(tl, bl));
                result.layout.padding.right = grow(decl.padding.right, std::max(tr, br));
                result.layout.padding.top = grow(decl.padding.top, std::max(tl, tr));
                result.layout.padding.bottom = grow(decl.padding.bottom, std::max(bl, br));
            }
            result.layout.childGap = decl.child_gap;
            result.layout.childAlignment = SEClay_ChildAlignment{
                .x = to_clay_align_x(decl.child_alignment.x),
                .y = to_clay_align_y(decl.child_alignment.y),
            };
            result.layout.layoutDirection = to_clay_direction(decl.direction);
            result.backgroundColor = to_clay_color(decl.background_color);
            result.cornerRadius = to_clay_corner_radius(decl.corner_radius, decl.corner_shapes);
            result.border = SEClay_BorderElementConfig{
                .color = to_clay_color(decl.border.color),
                .width = to_clay_border_width(decl.border.width),
            };
            result.clip = SEClay_ClipElementConfig{
                .horizontal = decl.clip.horizontal,
                .vertical = decl.clip.vertical,


            };
            if (decl.id.size() != 0) {
                const string_view id_view = decl.id.cpp_string_view();
                result.id = SEClay_GetElementId(
                    SEClay_String::from_view(id_view));
            }
            if (decl.floating.attach_to != FloatingAttachTo::None) {
                SEClay_FloatingAttachToElement attach_to = SEClay_FloatingAttachToElement::None;
                u32 parent_id = 0;
                switch (decl.floating.attach_to) {
                    case FloatingAttachTo::None: break;
                    case FloatingAttachTo::Parent: attach_to = SEClay_FloatingAttachToElement::Parent; break;
                    case FloatingAttachTo::Root: attach_to = SEClay_FloatingAttachToElement::Root; break;
                    case FloatingAttachTo::ElementWithId: {
                        attach_to = SEClay_FloatingAttachToElement::ElementWithId;
                        const string_view parent_id_view = decl.floating.parent_id.cpp_string_view();
                        parent_id = SEClay_GetElementId(SEClay_String::from_view(parent_id_view))
                                       .id;
                        break;
                    }
                }
                result.floating = SEClay_FloatingElementConfig{
                    .offset = SEClay_Vector2{.x = decl.floating.offset.x, .y = decl.floating.offset.y},
                    .expand = SEClay_Dimensions{},
                    .parentId = parent_id,
                    .zIndex = decl.floating.z_index,
                    .attachPoints = SEClay_FloatingAttachPoints{
                        .element = to_clay_attach_point(decl.floating.element_attach_point),
                        .parent = to_clay_attach_point(decl.floating.parent_attach_point),
                    },
                    .pointerCaptureMode = decl.floating.capture_pointer ? SEClay_PointerCaptureMode::Capture
                                                                        : SEClay_PointerCaptureMode::Passthrough,
                    .attachTo = attach_to,
                    .clipTo = decl.floating.clip_to == FloatingClipTo::AttachedParent
                                 ? SEClay_FloatingClipToElement::AttachedParent
                                 : SEClay_FloatingClipToElement::None,
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
            return static_cast<i32>(static_cast<u32>(reinterpret_cast<uintptr_t>(user_data) & 0xFFFFFFFFull));
        }

        /// A text element's user data: its z in the low 32 bits and, above them, one more than the index of its decoration/shadow entry
        /// (zero = none).
        [[nodiscard]] void *pack_text_user_data(i32 z, usize extras_index_plus_one) noexcept {
            return reinterpret_cast<void *>((static_cast<uintptr_t>(extras_index_plus_one) << 32) | static_cast<uintptr_t>(static_cast<u32>(z)));
        }
        [[nodiscard]] usize unpack_text_extras(void *user_data) noexcept {
            return static_cast<usize>(reinterpret_cast<uintptr_t>(user_data) >> 32);
        }

        /// Converts the value to clay wrap mode representation.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns the value converted to clay wrap mode representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SEClay_TextElementConfigWrapMode to_clay_wrap_mode(TextWrapMode mode) noexcept {
            switch (mode) {
                case TextWrapMode::Words: return SEClay_TextElementConfigWrapMode::Words;
                case TextWrapMode::Newlines: return SEClay_TextElementConfigWrapMode::Newlines;
                case TextWrapMode::None: return SEClay_TextElementConfigWrapMode::None;
            }
            return SEClay_TextElementConfigWrapMode::Words;
        }

        /// Converts the value to clay text align representation.
        ///
        /// @param align `align` value used by the operation.
        ///
        /// @return Returns the value converted to clay text align representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SEClay_TextAlignment to_clay_text_align(TextAlign align) noexcept {
            switch (align) {
                case TextAlign::Left: return SEClay_TextAlignment::Left;
                case TextAlign::Center: return SEClay_TextAlignment::Center;
                case TextAlign::Right: return SEClay_TextAlignment::Right;
            }
            return SEClay_TextAlignment::Left;
        }

        /// Converts the value to clay text config representation.
        ///
        /// @param style `style` value used by the operation.
        /// @param z `z` value used by the operation.
        ///
        /// @return Returns the value converted to clay text config representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SEClay_TextElementConfig to_clay_text_config(const TextStyle &style, i32 z) noexcept {
            return SEClay_TextElementConfig{
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

        /// The layout engine's error callback. Hands the problem to the owning context as plain data; nothing here throws or unwinds,
        /// because this runs inside the engine's layout code (and, for foreign callers, inside a call that cannot propagate exceptions).
        void clay_error_handler(SEClay_ErrorData error) {
            Context::LayoutErrorInfo info;
            info.type = static_cast<u32>(error.errorType);
            info.detail = static_cast<u32>(error.detail);
            info.value = error.value;
            info.limit = error.limit;
            info.message = error.errorText.chars;
            info.message_length = error.errorText.length > 0 ? static_cast<usize>(error.errorText.length) : 0;
            info.source_file = error.sourceFile;
            info.source_function = error.sourceFunction;
            info.source_line = error.sourceLine;
            info.frame = error.frame;
            Context::dispatch_layout_error(error.userData, info);
        }

        /// Maps a layout-engine problem onto the stable UI error code and a severity.
        void classify(const Context::LayoutErrorInfo &info, bool growable, UiErrorCode &code, UiSeverity &severity) noexcept {
            // Capacity problems heal themselves in a growable context (it resizes before the next frame); in a fixed one they persist.
            const UiSeverity capacity_severity = growable ? UiSeverity::Warning : UiSeverity::Error;
            severity = UiSeverity::Error;
            switch (static_cast<SEClay_ErrorDetail>(info.detail)) {
                case SEClay_ErrorDetail::Elements: code = UiErrorCode::ElementLimit; severity = capacity_severity; return;
                case SEClay_ErrorDetail::ElementConfigs: code = UiErrorCode::ElementConfigLimit; severity = capacity_severity; return;
                case SEClay_ErrorDetail::WrappedTextLines: code = UiErrorCode::TextLineLimit; severity = capacity_severity; return;
                case SEClay_ErrorDetail::RenderCommands: code = UiErrorCode::DrawCommandLimit; severity = capacity_severity; return;
                case SEClay_ErrorDetail::TextMeasureItems:
                case SEClay_ErrorDetail::TextMeasureWords: code = UiErrorCode::TextCacheLimit; severity = UiSeverity::Warning; return;
                case SEClay_ErrorDetail::ElementIdMap: code = UiErrorCode::ElementLimit; severity = capacity_severity; return;
                case SEClay_ErrorDetail::DynamicStrings: code = UiErrorCode::DynamicStringLimit; severity = UiSeverity::Warning; return;
                case SEClay_ErrorDetail::TraversalStack: code = UiErrorCode::TraversalOverflow; return;
                case SEClay_ErrorDetail::UnbalancedClose: code = UiErrorCode::UnbalancedClose; return;
                case SEClay_ErrorDetail::BadTextArgument: code = UiErrorCode::InvalidArgument; severity = UiSeverity::Warning; return;
                case SEClay_ErrorDetail::OutOfBoundsAccess: code = UiErrorCode::InternalBoundsViolation; return;
                case SEClay_ErrorDetail::ArenaTooSmall: code = UiErrorCode::FixedArenaTooSmall; return;
                case SEClay_ErrorDetail::SettingIgnored: code = UiErrorCode::SettingIgnored; severity = UiSeverity::Warning; return;
                case SEClay_ErrorDetail::MemoryLimit: code = UiErrorCode::MemoryLimitReached; return;
                case SEClay_ErrorDetail::Allocation: code = UiErrorCode::AllocationFailed; return;
                case SEClay_ErrorDetail::Grew:
                case SEClay_ErrorDetail::Shrank: code = UiErrorCode::Resized; severity = UiSeverity::Info; return;
                case SEClay_ErrorDetail::MeasureFunction: code = UiErrorCode::MeasureFunctionMissing; return;
                case SEClay_ErrorDetail::DuplicateId: code = UiErrorCode::DuplicateId; severity = UiSeverity::Warning; return;
                case SEClay_ErrorDetail::FloatingParent: code = UiErrorCode::FloatingParentMissing; severity = UiSeverity::Warning; return;
                case SEClay_ErrorDetail::Percentage: code = UiErrorCode::PercentageOverOne; severity = UiSeverity::Warning; return;
                case SEClay_ErrorDetail::None: break;
            }
            code = UiErrorCode::Other;
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
                SEClay_SetCurrentContext(context_);
                SEClay__CloseElement();
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
            SEClay_SetCurrentContext(context_);
            SEClay__CloseElement();
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
          diagnostics_(std::move(other.diagnostics_)),
          api_site_(other.api_site_),
          text_bridge_(std::move(other.text_bridge_)),
          text_storage_(std::move(other.text_storage_)),
          image_storage_(std::move(other.image_storage_)),
          custom_storage_(std::move(other.custom_storage_)),
          outline_cache_(std::move(other.outline_cache_)),
          layout_extent_(other.layout_extent_),
          layout_size_(other.layout_size_),
          pixel_scale_(other.pixel_scale_),
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


        if (diagnostics_) {
            diagnostics_->owner = this;
        }
        if (context_ != nullptr) {
            SEClay_SetCurrentContext(context_);
            SEClay_SetMeasureTextFunction(&TextBridge::measure_callback, &text_bridge_);
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
            // The host's long-lived settings survive being replaced: the UI is rebuilt wholesale on a device change or an HDR toggle
            // (the engine move-assigns a fresh context over the live one), and the app's diagnostics callback, its collected reports and
            // its debug-view choice must not be silently reset by that.
            const bool keep_debug_view = context_ != nullptr && debug_view_enabled();
            std::unique_ptr<DiagnosticsHub> kept_hub = std::move(diagnostics_);
            destroy();
            context_ = std::exchange(other.context_, nullptr);
            arena_memory_ = std::move(other.arena_memory_);
            diagnostics_ = kept_hub ? std::move(kept_hub) : std::move(other.diagnostics_);
            if (diagnostics_) {
                diagnostics_->owner = this;
            }
            api_site_ = other.api_site_;
            text_bridge_ = std::move(other.text_bridge_);
            text_storage_ = std::move(other.text_storage_);
            image_storage_ = std::move(other.image_storage_);
            custom_storage_ = std::move(other.custom_storage_);
            outline_cache_ = std::move(other.outline_cache_);
            layout_extent_ = other.layout_extent_;
            layout_size_ = other.layout_size_;
            pixel_scale_ = other.pixel_scale_;
            text_raster_preference_ = other.text_raster_preference_;
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
                SEClay_SetCurrentContext(context_);
                SEClay_SetMeasureTextFunction(&TextBridge::measure_callback, &text_bridge_);
                // The incoming layout context reports to the hub it was created with; point it at the one we kept.
                if (diagnostics_) {
                    SEClay_SetErrorHandler(SEClay_ErrorHandler{.errorHandlerFunction = &clay_error_handler, .userData = diagnostics_.get()});
                }
                if (keep_debug_view) {
                    SEClay_SetDebugModeEnabled(true);
                }
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
        // SEClay_SetMaxElementCount()/SEClay_MinMemorySize() both silently operate on whatever context is
        // currently "active" (SEClay_GetCurrentContext()) rather than being parameterized by which context
        // is actually being built here. WorkbenchUi (and anything else creating more than one UI::Context
        // — one per surface/window) leaves the *previous* surface's context set as "current" from its
        // last frame's rendering, so without this, SEClay_SetMaxElementCount would silently overwrite THAT
        // unrelated, already-initialized context's element budget instead of setting the default this
        // brand-new context is about to be sized from, and SEClay_MinMemorySize would size *this* context's
        // arena off THAT other context's (possibly different) budget instead of `config.max_element_count`.
        // Forcing "no current context" here routes both calls through Clay's actual global-default path,
        // independent of whatever other UI::Context instances already exist.
        SEClay_SetCurrentContext(nullptr);

        Context result;
        result.diagnostics_->owner = &result;
        const SEClay_ErrorHandler handler{.errorHandlerFunction = &clay_error_handler, .userData = result.diagnostics_.get()};
        SEClay_Context *context = nullptr;
        if (config.arena_capacity_bytes != 0) {
            // Fixed-size context: the caller chose the exact memory, so it is a hard limit.
            if (config.max_element_count != 0) {
                SEClay_SetMaxElementCount(static_cast<i32>(config.max_element_count));
            }
            SEClay_SetMaxMeasureTextCacheWordCount(131072);
            result.arena_memory_.resize(config.arena_capacity_bytes);
            SEClay_Arena arena = SEClay_CreateArenaWithCapacityAndMemory(config.arena_capacity_bytes, result.arena_memory_.data());
            context = SEClay_Initialize(arena, SEClay_Dimensions{.width = 1.0f, .height = 1.0f}, handler);
        } else {
            SEClay_GrowableConfig growable{};
            growable.layoutDimensions = SEClay_Dimensions{.width = 1.0f, .height = 1.0f};
            growable.errorHandler = handler;
            growable.initialElementCount = static_cast<i32>(config.max_element_count);
            // A launcher shows far more distinct words than a small default (logs, mod lists, descriptions); the cache grows past this.
            growable.initialMeasureWordCount = 32768;
            growable.memoryLimitBytes = config.memory_limit_bytes;
            growable.allowShrink = config.allow_shrink;
            context = SEClay_InitializeGrowable(&growable);
        }
        if (context == nullptr) {
            std::string reason = config.arena_capacity_bytes != 0
                                     ? "UI layout context could not be created: the fixed arena is too small for the requested element count."
                                     : "UI layout context could not be created: the allocator refused the initial memory.";
            // Whatever the layout engine reported while failing (with the numbers) is the most useful detail.
            if (!result.diagnostics_->store.reports().empty()) {
                const UiErrorReport &first = result.diagnostics_->store.reports().front();
                reason += std::format(" Detail: {} (wanted {}, had {}).", first.message, first.value, first.limit);
            }
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, reason);
        }
        result.context_ = context;
        result.text_bridge_.set_load_default_font(config.load_default_font);
        SEClay_SetMeasureTextFunction(&TextBridge::measure_callback, &result.text_bridge_);
        return result;
    }

    /// Sets the current for this `UI`.
    ///
    /// @return Returns the current set current value.
    /// @note This function does not throw exceptions.
    void Context::set_current() const noexcept {
        if (context_ != nullptr) {
            SEClay_SetCurrentContext(context_);
        }
    }

    void Context::set_debug_view_enabled(bool enabled) noexcept {
        if (context_ != nullptr) {
            set_current();
            SEClay_SetDebugModeEnabled(enabled);
        }
    }

    bool Context::debug_view_enabled() const noexcept {
        if (context_ == nullptr) {
            return false;
        }
        set_current();
        return SEClay_IsDebugModeEnabled();
    }

    u32 Context::debug_view_width() noexcept { return SEClay__debugViewWidth; }

    namespace {
        [[nodiscard]] SEClay_Color to_debug_clay_color(const Color &color) noexcept {
            return SEClay_Color{static_cast<f32>(color.r), static_cast<f32>(color.g), static_cast<f32>(color.b), static_cast<f32>(color.a)};
        }
        [[nodiscard]] Color from_debug_clay_color(const SEClay_Color &color) noexcept {
            return Color{color.r, color.g, color.b, color.a};
        }
    } // namespace

    void Context::set_debug_view_theme(const DebugViewTheme &theme) noexcept {
        SEClay_DebugViewTheme native{};
        native.surface = to_debug_clay_color(theme.surface);
        native.surfaceAlt = to_debug_clay_color(theme.surface_alt);
        native.muted = to_debug_clay_color(theme.muted);
        native.text = to_debug_clay_color(theme.text);
        native.selectedRow = to_debug_clay_color(theme.selected_row);
        native.highlight = to_debug_clay_color(theme.highlight);
        native.close = to_debug_clay_color(theme.close);
        native.warning = to_debug_clay_color(theme.warning);
        // By element configuration kind: none, border, floating, scroll, aspect, image, text, custom, shared.
        native.tags[0] = to_debug_clay_color(Color{0.0, 0.0, 0.0, 1.0});
        native.tags[1] = to_debug_clay_color(theme.tag_border);
        native.tags[2] = to_debug_clay_color(theme.tag_floating);
        native.tags[3] = to_debug_clay_color(theme.tag_scroll);
        native.tags[4] = to_debug_clay_color(theme.tag_aspect);
        native.tags[5] = to_debug_clay_color(theme.tag_image);
        native.tags[6] = to_debug_clay_color(theme.tag_text);
        native.tags[7] = to_debug_clay_color(theme.tag_custom);
        native.tags[8] = to_debug_clay_color(theme.tag_shared);
        SEClay_SetDebugViewTheme(&native);
    }

    DebugViewTheme Context::debug_view_theme() noexcept {
        const SEClay_DebugViewTheme native = SEClay_GetDebugViewTheme();
        DebugViewTheme theme;
        theme.surface = from_debug_clay_color(native.surface);
        theme.surface_alt = from_debug_clay_color(native.surfaceAlt);
        theme.muted = from_debug_clay_color(native.muted);
        theme.text = from_debug_clay_color(native.text);
        theme.selected_row = from_debug_clay_color(native.selectedRow);
        theme.highlight = from_debug_clay_color(native.highlight);
        theme.close = from_debug_clay_color(native.close);
        theme.warning = from_debug_clay_color(native.warning);
        theme.tag_border = from_debug_clay_color(native.tags[1]);
        theme.tag_floating = from_debug_clay_color(native.tags[2]);
        theme.tag_scroll = from_debug_clay_color(native.tags[3]);
        theme.tag_aspect = from_debug_clay_color(native.tags[4]);
        theme.tag_image = from_debug_clay_color(native.tags[5]);
        theme.tag_text = from_debug_clay_color(native.tags[6]);
        theme.tag_custom = from_debug_clay_color(native.tags[7]);
        theme.tag_shared = from_debug_clay_color(native.tags[8]);
        return theme;
    }


    Context::MemoryStats Context::memory_stats() const noexcept {
        MemoryStats out;
        if (context_ == nullptr) {
            return out;
        }
        set_current();
        const SEClay_MemoryStats s = SEClay_GetMemoryStats();
        out.growable = s.growable;
        out.truncated_last_frame = s.truncatedLastLayout;
        out.bytes_reserved = s.bytesReserved;
        out.bytes_limit = s.bytesLimit;
        out.element_budget = static_cast<u32>(s.elementBudget);
        out.grow_count = s.growCount;
        out.shrink_count = s.shrinkCount;
        out.elements_used = static_cast<u32>(s.usedElements);
        out.elements_capacity = static_cast<u32>(s.capacityElements);
        out.configs_used = static_cast<u32>(s.usedElementConfigs);
        out.configs_capacity = static_cast<u32>(s.capacityElementConfigs);
        out.text_lines_used = static_cast<u32>(s.usedWrappedTextLines);
        out.text_lines_capacity = static_cast<u32>(s.capacityWrappedTextLines);
        out.draw_commands_used = static_cast<u32>(s.usedRenderCommands);
        out.draw_commands_capacity = static_cast<u32>(s.capacityRenderCommands);
        return out;
    }

    void Context::dispatch_layout_error(void *hub, const LayoutErrorInfo &info) noexcept {
        if (hub == nullptr) {
            return;
        }
        auto *typed = static_cast<DiagnosticsHub *>(hub);
        if (typed->owner != nullptr) {
            typed->owner->report_layout_error(info);
        }
    }

    void Context::report_layout_error(const LayoutErrorInfo &info) noexcept {
        if (!diagnostics_) {
            return;
        }
        try {
            const bool growable = context_ != nullptr && SEClay_GetCurrentContext() == context_ && SEClay_GetMemoryStats().growable;
            UiErrorCode code = UiErrorCode::Other;
            UiSeverity severity = UiSeverity::Error;
            classify(info, growable, code, severity);
            UiDiagnostics &store = diagnostics_->store;
            // The chain of open elements, as ids. Cheap, and it is what tells two problems raised by the same check apart.
            SEClay_ElementPathEntry path[48];
            const i32 depth = SEClay_GetOpenElementPath(path, static_cast<i32>(std::size(path)));
            u64 key_hash = 1469598103934665603ull;
            const auto mix = [&key_hash](u64 value) noexcept { key_hash = (key_hash ^ value) * 1099511628211ull; };
            for (i32 i = 0; i < depth && i < static_cast<i32>(std::size(path)); ++i) {
                mix(path[i].id);
                mix(static_cast<u64>(static_cast<u32>(path[i].childIndex)));
            }
            // For problems that are about one specific id, that id is part of the identity (the value is the id hash); for capacity
            // problems the value is a changing count and must not be.
            if (code == UiErrorCode::DuplicateId || code == UiErrorCode::FloatingParentMissing) {
                mix(static_cast<u64>(info.value));
            }
            // A per-frame bug repeats every frame: once it has been seen twice, only count it (no path labels, no native stack).
            if (store.note_repeat(code, info.source_file, info.source_line, key_hash, info.frame, info.value, info.limit)) {
                return;
            }
            UiErrorReport report;
            report.code = code;
            report.severity = severity;
            report.value = info.value;
            report.limit = info.limit;
            report.last_frame = info.frame;
            report.source_key_file = info.source_file;
            report.source_key_line = info.source_line;
            report.key_hash = key_hash;
            if (info.message != nullptr) {
                report.message.assign(info.message, info.message_length);
            }
            if (code == UiErrorCode::Resized) {
                report.message = std::format("The UI layout context resized itself from {} to {} elements.", info.limit, info.value);
            } else if (code == UiErrorCode::ElementLimit || code == UiErrorCode::ElementConfigLimit || code == UiErrorCode::TextLineLimit ||
                       code == UiErrorCode::DrawCommandLimit) {
                report.message += growable ? " (this context will grow to fit before the next frame)" : " (this context is fixed-size and cannot grow)";
            }
            if (info.source_file != nullptr) {
                report.engine_source = std::format("{}:{} ({})", info.source_file, info.source_line,
                                                   info.source_function != nullptr ? info.source_function : "?");
            }
            if (api_site_.file != nullptr) {
                report.api_call_site.language = "c++";
                report.api_call_site.file = api_site_.file;
                report.api_call_site.line = api_site_.line;
                report.api_call_site.column = api_site_.column;
                report.api_call_site.function = api_site_.function != nullptr ? api_site_.function : "";
            }
            report.foreign_call_site = Foundation::foreign_call_site();

            // The same chain, by name where the name is known this frame.
            constexpr string_view root_name = "SEClay__RootContainer";
            const u32 root_id = SEClay_GetElementId(SEClay_String::from_view(root_name, true)).id;
            for (i32 i = 0; i < depth && i < static_cast<i32>(std::size(path)); ++i) {
                const SEClay_ElementPathEntry &entry = path[i];
                if (entry.id == root_id) {
                    report.element_path.emplace_back("<root>");
                    continue;
                }
                std::string label;
                for (const string &name : current_frame_ids_) {
                    const SEClay_ElementId candidate = SEClay_GetElementId(SEClay_String::from_view(name));
                    if (candidate.id == entry.id) {
                        label = name;
                        break;
                    }
                }
                if (label.empty()) {
                    label = std::format("<anonymous child {}>", entry.childIndex);
                }
                report.element_path.push_back(std::move(label));
            }
            if (store.capture_backtraces()) {
                // Prefer the trace recorded where the underlying error was found, else the stack at this report.
                if (auto origin = Foundation::take_trace_origin()) {
                    report.trace = std::move(*origin);
                } else {
                    report.trace = Foundation::TraceCapture::capture(1);
                }
            }
            store.record(std::move(report));
        } catch (...) {
            // Reporting must never become a second failure inside the layout; the problem is simply not recorded.
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

    void Context::unregister_font(FontId font_id) { text_bridge_.unregister_font(font_id); }

    void Context::set_font_features(FontId font_id, string_view comma_separated_features) {
        text_bridge_.set_font_features(font_id, comma_separated_features);
    }

    /// Performs the begin layout operation for `UI` using the supplied arguments.
    ///
    /// @param viewport_size Requested or available size for the operation.
    /// @param pointer Pointer to the object or storage used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::begin_layout(glm::vec2 viewport_size, const PointerState &pointer, f32 delta_seconds, std::source_location site) {
        note_call_site(site);
        const auto layout_dimension = [](f32 value) noexcept {
            constexpr f32 max_exact_pixel_dimension = 16777216.0f;
            if (!std::isfinite(value) || value <= 0.0f) {
                return 1u;
            }
            return static_cast<u32>(std::clamp(std::round(value), 1.0f, max_exact_pixel_dimension));
        };
        layout_extent_ = Core::Extent2D{layout_dimension(viewport_size.x), layout_dimension(viewport_size.y)};
        layout_size_ = glm::max(glm::vec2{layout_extent_} / pixel_scale_, glm::vec2{1.0f});
        const f32 to_logical = 1.0f / pixel_scale_;

        set_current();
        text_bridge_.begin_frame();
        text_storage_.clear();
        image_storage_.clear();
        custom_storage_.clear();
        current_frame_ids_.clear();
        current_frame_clip_ids_.clear();
        border_edges_storage_.clear();
        text_extras_.clear();


        desired_cursor_ = CursorIcon::Default;


        z_stack_.assign(1, ZFrame{});
        hit_records_.clear();
        hit_record_by_id_.clear();
        SEClay_SetLayoutDimensions(SEClay_Dimensions{
            .width = layout_size_.x,
            .height = layout_size_.y,
        });


        const bool was_down = pointer_down_;
        pointer_position_ = pointer.position * to_logical;
        pointer_pressed_this_frame_ = pointer.pressed || (pointer.down && !was_down);
        if (pointer_pressed_this_frame_) {
            pointer_press_position_ = pointer.press_position.value_or(pointer.position) * to_logical;
        }
        pointer_released_this_frame_ = pointer.released || (!pointer.down && was_down);
        pointer_cancelled_this_frame_ = pointer.cancelled;
        pointer_down_ = pointer.down;
        SEClay_SetPointerState(SEClay_Vector2{.x = pointer_position_.x, .y = pointer_position_.y}, pointer.down);


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
        SEClay_UpdateScrollContainers(scroll_settings_.click_and_drag_scroll,
                                    SEClay_Vector2{.x = applied_scroll_delta.x, .y = applied_scroll_delta.y},
                                    delta_seconds);
        SEClay_BeginLayout();
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
    ElementScope Context::element(const ElementDecl &decl, std::source_location site) {
        note_call_site(site);
        set_current();
        if (!decl.id.empty()) {
            current_frame_ids_.push_back(decl.id.cpp_string());
        }
        update_desired_cursor(decl);


        const i32 effective_z = decl.z != 0 ? decl.z : z_stack_.back().z;
        z_stack_.push_back(ZFrame{effective_z});
        SEClay__OpenElement();
        SEClay_ElementDeclaration declaration = to_clay_declaration(decl, effective_z);
        if (decl.clip.horizontal || decl.clip.vertical) {


            if (!decl.id.empty()) {
                current_frame_clip_ids_.push_back(decl.id.cpp_string());
                const auto cached = last_frame_scroll_offsets_.find(decl.id.cpp_string());
                if (cached != last_frame_scroll_offsets_.end()) {
                    declaration.clip.childOffset = SEClay_Vector2{.x = cached->second.x, .y = cached->second.y};
                }
            } else {


                declaration.clip.childOffset = SEClay_GetScrollOffset();
            }
        }
        // A shaped clip (the element's own corners, or a path) is an effect that runs after the declared one.
        std::shared_ptr<const UiEffect> effect = decl.effect;
        if (decl.clip.follow_corners || decl.clip.path) {
            effect = shaped_clip_effect(decl);
        }
        if (effect) {
            if (const std::optional<UString> problem = validate_effect(*effect)) {
                // A bad effect must not take the widget with it: draw it plainly and say why.
                SFT_LOG_ERROR_THROTTLED(5000, "UI effect ignored, the element is drawn without it: {}", problem->cpp_string());
            } else {
                // SEClay keeps only this raw pointer until the layout ends; effect_storage_ keeps the object alive, and
                // finish_frame() hands that to the snapshot.
                effect_storage_.push_back(effect);
                declaration.custom.effectData = const_cast<UiEffect *>(effect.get());
            }
        }
        if (decl.border_edges) {
            // The edges are read when the frame is finished; storage keeps them alive until then. The single colour only has to be visible so the
            // layout emits a Border command at all.
            border_edges_storage_.push_back(decl.border_edges);
            declaration.border.edgeData = decl.border_edges.get();
            declaration.border.color = SEClay_Color{255.0f, 255.0f, 255.0f, 255.0f};
        }
        SEClay__ConfigureOpenElement(declaration);
        record_hit(decl, effect.get());
        return ElementScope{context_, &z_stack_};
    }

    /// Performs the text operation for `UI` using the supplied arguments.
    ///
    /// @param content `content` value used by the operation.
    /// @param style `style` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::text(const ustr &content, const TextStyle &style, std::source_location site) {
        note_call_site(site);
        set_current();
        const UString utf8{content};
        text_storage_.push_back(string{utf8.cpp_string_view()});
        const string &stored = text_storage_.back();
        const SEClay_String clay_string = SEClay_String::from_view(stored);


        SEClay_TextElementConfig text_config = to_clay_text_config(style, z_stack_.back());
        if (style.decoration.lines != TextDecorationLine::None || style.shadow.enabled) {
            text_extras_.push_back(TextExtras{.decoration = style.decoration, .shadow = style.shadow});
            text_config.userData = pack_text_user_data(z_stack_.back(), text_extras_.size());
        }
        SEClay_TextElementConfig *config = SEClay__StoreTextElementConfig(text_config);
        SEClay__OpenTextElement(clay_string, config);
        record_text_hit();
    }

    /// Computes the hit test text byte offset required by the supplied values.
    ///
    /// @param style `style` value used by the operation.
    /// @param utf8_content `utf8_content` value used by the operation.
    /// @param local_x `local_x` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    glm::vec2 Context::measure_text(const TextStyle &style, string_view utf8_content) {
        if (utf8_content.empty()) {
            return glm::vec2{0.0f};
        }
        const CachedShape *shape = text_bridge_.shape_and_cache(style, utf8_content);
        return shape != nullptr ? glm::vec2{shape->width_px, shape->height_px} : glm::vec2{0.0f};
    }

    f32 Context::text_ascent(const TextStyle &style) {
        const Text::FontStack *stack = text_bridge_.font_stack(style.font_id);
        if (stack == nullptr || stack->primary == nullptr) {
            return 0.0f;
        }
        const Text::Font &font = *stack->primary;
        const u32 units_per_em = font.units_per_em();
        return units_per_em > 0 ? static_cast<f32>(font.ascender()) * static_cast<f32>(style.font_size) / static_cast<f32>(units_per_em) : 0.0f;
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
    void Context::image(const ElementDecl &decl, Renderer::TextureHandle texture, ImageSampling sampling, std::source_location site) {
        image(decl, texture, ImageFitOptions{}, sampling, site);
    }

    void Context::image(const ElementDecl &decl, Renderer::TextureHandle texture, const ImageFitOptions &fit, ImageSampling sampling,
                        std::source_location site) {
        note_call_site(site);
        set_current();
        if (!decl.id.empty()) {
            current_frame_ids_.push_back(decl.id.cpp_string());
        }
        update_desired_cursor(decl);
        image_storage_.push_back(ImageRef{.texture = texture, .sampling = sampling, .fit = fit});
        ImageRef *stored = &image_storage_.back();
        const i32 effective_z = decl.z != 0 ? decl.z : z_stack_.back().z;
        SEClay_ElementDeclaration declaration = to_clay_declaration(decl, effective_z);
        declaration.image = SEClay_ImageElementConfig{.imageData = stored};
        SEClay__OpenElement();
        SEClay__ConfigureOpenElement(declaration);
        record_leaf(decl);
        SEClay__CloseElement();
    }

    /// Performs the svg operation for `UI` using the supplied arguments.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param texture Texture used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::svg(const ElementDecl &decl, Renderer::TextureHandle texture) { image(decl, texture); }

    /// Draws one or more anti-aliased polylines for `UI` using the supplied arguments.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param paths Paths used or affected by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::stroke_paths(const ElementDecl &decl, std::span<const StrokePath> paths) {
        set_current();
        if (!decl.id.empty()) {
            current_frame_ids_.push_back(decl.id.cpp_string());
        }
        update_desired_cursor(decl);
        stroke_storage_.push_back(StrokePolylineData{
            .paths = vector<StrokePath>(paths.begin(), paths.end()),
        });
        StrokePolylineData *stored = &stroke_storage_.back();
        const i32 effective_z = decl.z != 0 ? decl.z : z_stack_.back().z;
        SEClay_ElementDeclaration declaration = to_clay_declaration(decl, effective_z);
        declaration.custom.customData = stored; // not `custom = {...}`: that would drop a declared effect
        SEClay__OpenElement();
        SEClay__ConfigureOpenElement(declaration);
        record_leaf(decl);
        SEClay__CloseElement();
    }

    /// Convenience wrapper around stroke_paths() for a single polyline.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param points Points used or affected by the operation.
    /// @param style `style` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::stroke_polyline(const ElementDecl &decl, std::span<const glm::vec2> points, const StrokeStyle &style) {
        const StrokePath path{.points = vector<glm::vec2>(points.begin(), points.end()), .style = style};
        stroke_paths(decl, span<const StrokePath>{&path, 1});
    }

    /// Draws one or more filled rects for `UI` using the supplied arguments.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param quads Quads used or affected by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::fill_quads(const ElementDecl &decl, std::span<const FillQuad> quads) {
        set_current();
        if (!decl.id.empty()) {
            current_frame_ids_.push_back(decl.id.cpp_string());
        }
        update_desired_cursor(decl);
        fill_storage_.push_back(FillQuadListData{
            .quads = vector<FillQuad>(quads.begin(), quads.end()),
        });
        FillQuadListData *stored = &fill_storage_.back();
        const i32 effective_z = decl.z != 0 ? decl.z : z_stack_.back().z;
        SEClay_ElementDeclaration declaration = to_clay_declaration(decl, effective_z);
        declaration.custom.customData = stored; // not `custom = {...}`: that would drop a declared effect
        SEClay__OpenElement();
        SEClay__ConfigureOpenElement(declaration);
        record_leaf(decl);
        SEClay__CloseElement();
    }

    /// Draws one or more annular sectors for `UI` using the supplied arguments.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param sectors Sectors used or affected by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::fill_sectors(const ElementDecl &decl, std::span<const Sector> sectors) {
        set_current();
        if (!decl.id.empty()) {
            current_frame_ids_.push_back(decl.id.cpp_string());
        }
        update_desired_cursor(decl);
        sector_storage_.push_back(SectorListData{
            .sectors = vector<Sector>(sectors.begin(), sectors.end()),
        });
        SectorListData *stored = &sector_storage_.back();
        const i32 effective_z = decl.z != 0 ? decl.z : z_stack_.back().z;
        SEClay_ElementDeclaration declaration = to_clay_declaration(decl, effective_z);
        declaration.custom.customData = stored; // not `custom = {...}`: that would drop a declared effect
        SEClay__OpenElement();
        SEClay__ConfigureOpenElement(declaration);
        record_leaf(decl);
        SEClay__CloseElement();
    }

    /// Draws a shader-driven polyline for `UI` using the supplied arguments.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param points Points used or affected by the operation.
    /// @param half_width Requested or available size for the operation.
    /// @param feather_px Requested or available size for the operation.
    /// @param shader Shader used or affected by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Context::stroke_custom(const ElementDecl &decl, std::span<const glm::vec2> points, f32 half_width,
                                f32 feather_px, const CustomShaderRef &shader) {
        set_current();
        if (!decl.id.empty()) {
            current_frame_ids_.push_back(decl.id.cpp_string());
        }
        update_desired_cursor(decl);
        custom_stroke_storage_.push_back(CustomStrokeElementData{
            .points = vector<glm::vec2>(points.begin(), points.end()),
            .half_width = half_width,
            .feather_px = feather_px,
            .shader = shader,
        });
        CustomStrokeElementData *stored = &custom_stroke_storage_.back();
        const i32 effective_z = decl.z != 0 ? decl.z : z_stack_.back().z;
        SEClay_ElementDeclaration declaration = to_clay_declaration(decl, effective_z);
        declaration.custom.customData = stored; // not `custom = {...}`: that would drop a declared effect
        SEClay__OpenElement();
        SEClay__ConfigureOpenElement(declaration);
        record_leaf(decl);
        SEClay__CloseElement();
    }

    /// Performs the custom element operation for `UI` using the supplied arguments.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param shader Shader used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ElementScope Context::custom_element(const ElementDecl &decl, const CustomShaderRef &shader, std::source_location site) {
        note_call_site(site);
        set_current();
        if (!decl.id.empty()) {
            current_frame_ids_.push_back(decl.id.cpp_string());
        }
        update_desired_cursor(decl);
        custom_storage_.push_back(shader);
        CustomShaderRef *stored = &custom_storage_.back();
        const i32 effective_z = decl.z != 0 ? decl.z : z_stack_.back().z;
        z_stack_.push_back(ZFrame{effective_z});
        SEClay__OpenElement();
        SEClay_ElementDeclaration declaration = to_clay_declaration(decl, effective_z);
        if (decl.clip.horizontal || decl.clip.vertical) {
            declaration.clip.childOffset = SEClay_GetScrollOffset();
        }
        declaration.custom.customData = stored; // not `custom = {...}`: that would drop a declared effect
        if (decl.border_edges) {
            // The edges are read when the frame is finished; storage keeps them alive until then. The single colour only has to be visible so the
            // layout emits a Border command at all.
            border_edges_storage_.push_back(decl.border_edges);
            declaration.border.edgeData = decl.border_edges.get();
            declaration.border.color = SEClay_Color{255.0f, 255.0f, 255.0f, 255.0f};
        }
        SEClay__ConfigureOpenElement(declaration);
        record_hit(decl, decl.effect.get());
        return ElementScope{context_, &z_stack_};
    }

    /// Performs the hovered operation for `UI` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Context::record_leaf(const ElementDecl &decl) {
        const i32 index = static_cast<i32>(hit_records_.size());
        hit_records_.push_back(HitRecord{.clay_id = SEClay__GetOpenLayoutElement()->id,
                                         .parent = z_stack_.back().record,
                                         .z = decl.z != 0 ? decl.z : z_stack_.back().z,
                                         .solid = true,
                                         .effect = nullptr});
        if (!decl.id.empty()) {
            hit_record_by_id_.insert_or_assign(decl.id.cpp_string(), index);
        }
    }

    void Context::record_text_hit() {
        // The text element was the last layout element created.
        if (context_ == nullptr || context_->layoutElements.length <= 0) {
            return;
        }
        const SEClay_LayoutElement *text = SEClay_LayoutElementArray_Get(&context_->layoutElements, context_->layoutElements.length - 1);
        if (text == nullptr) {
            return;
        }
        hit_records_.push_back(HitRecord{.clay_id = text->id, .parent = z_stack_.back().record, .z = z_stack_.back().z, .solid = true, .effect = nullptr});
    }

    std::shared_ptr<const UiEffect> Context::shaped_clip_effect(const ElementDecl &decl) {
        const f32 scale = std::max(pixel_scale_, 0.0001f);
        const glm::vec4 radii{decl.corner_radius.top_left * scale, decl.corner_radius.top_right * scale, decl.corner_radius.bottom_left * scale,
                              decl.corner_radius.bottom_right * scale};
        const u32 codes = pack_corner_shapes(decl.corner_shapes);
        // The same clip on the same effects is the same effect: build it once and share it across elements and frames.
        std::string key;
        key.append(reinterpret_cast<const char *>(&radii), sizeof(radii));
        key.append(reinterpret_cast<const char *>(&codes), sizeof(codes));
        const void *base = decl.effect.get();
        const void *path = decl.clip.path.get();
        key.append(reinterpret_cast<const char *>(&base), sizeof(base));
        key.append(reinterpret_cast<const char *>(&path), sizeof(path));
        key.push_back(decl.clip.follow_corners ? '1' : '0');
        if (const auto found = shaped_clip_cache_.find(key); found != shaped_clip_cache_.end()) {
            return found->second.effect;
        }
        std::shared_ptr<const UiEffect> result = decl.effect;
        if (decl.clip.follow_corners) {
            result = Effects::then_clip(result, *Effects::clip_box(radii, codes));
        }
        if (decl.clip.path) {
            result = Effects::then_clip(result, *decl.clip.path);
        }
        if (shaped_clip_cache_.size() > 512) {
            shaped_clip_cache_.clear();
        }
        // The cache keeps what it was built from alive, so the pointers in the key cannot be reused by something else.
        shaped_clip_cache_.emplace(key, ShapedClip{.effect = result, .base = decl.effect, .path = decl.clip.path});
        return result;
    }

    void Context::record_hit(const ElementDecl &decl, const UiEffect *effect) {
        const i32 parent = z_stack_.size() >= 2 ? z_stack_[z_stack_.size() - 2].record : -1;
        const i32 index = static_cast<i32>(hit_records_.size());
        z_stack_.back().record = index;
        hit_records_.push_back(HitRecord{.clay_id = SEClay__GetOpenLayoutElement()->id,
                                         .parent = parent,
                                         .z = z_stack_.back().z,
                                         .solid = decl.background_color.a > 0.0 || decl.border.color.a > 0.0 || decl.border_edges != nullptr ||
                                                  effect != nullptr,
                                         .effect = effect});
        if (!decl.id.empty()) {
            hit_record_by_id_.insert_or_assign(decl.id.cpp_string(), index);
        }
    }

    namespace {

        /// Whether `point` (layout units, in the element's own space) is inside a clip shape drawn on an element occupying `box`.
        [[nodiscard]] bool inside_clip_shape(const UiClipShape &shape, const ElementBounds &box, glm::vec2 point, f32 pixel_scale) noexcept {
            const f32 unit = 1.0f / std::max(pixel_scale, 0.0001f); // shape lengths are surface pixels
            const glm::vec2 local = point - box.position;
            switch (shape.kind) {
                case UiClipShape::Kind::Inset: {
                    const glm::vec2 low{shape.inset.w * unit, shape.inset.x * unit};
                    const glm::vec2 high = box.size - glm::vec2{shape.inset.y * unit, shape.inset.z * unit};
                    if (!(high.x > low.x && high.y > low.y)) {
                        return false;
                    }
                    const glm::vec2 half = (high - low) * 0.5f;
                    const f32 radius = std::min(shape.radius * unit, std::min(half.x, half.y));
                    const glm::vec2 q = glm::abs(local - (low + high) * 0.5f) - (half - glm::vec2{radius});
                    return glm::length(glm::max(q, glm::vec2{0.0f})) + std::min(std::max(q.x, q.y), 0.0f) - radius <= 0.0f;
                }
                case UiClipShape::Kind::RoundedBox: {
                    // The shader's `shapedBoxSDF`, on the CPU, in layout units.
                    const glm::vec2 half = box.size * 0.5f;
                    const glm::vec2 center = local - half;
                    const bool left = center.x <= 0.0f;
                    const bool top = center.y <= 0.0f;
                    const f32 raw = left ? (top ? shape.corner_radii.x : shape.corner_radii.z) : (top ? shape.corner_radii.y : shape.corner_radii.w);
                    const u32 code = (shape.corner_codes >> (8u * (left ? (top ? 0u : 2u) : (top ? 1u : 3u)))) & 0xFFu;
                    const f32 k = decode_corner_shape_k(static_cast<u8>(code));
                    const f32 radius = std::min(raw * unit, std::min(half.x, half.y));
                    const glm::vec2 edge = glm::abs(center) - half;
                    const f32 box_dist = std::min(std::max(edge.x, edge.y), 0.0f) + glm::length(glm::max(edge, glm::vec2{0.0f}));
                    if (radius <= 0.0f) {
                        return box_dist <= 0.0f;
                    }
                    const glm::vec2 q = edge + glm::vec2{radius};
                    const auto superellipse = [](glm::vec2 p, f32 n) {
                        if (n >= 16.0f) return std::max(p.x, p.y) - 1.0f;
                        const glm::vec2 s = glm::clamp(p, glm::vec2{1e-4f}, glm::vec2{4.0f});
                        const glm::vec2 pn{std::pow(s.x, n), std::pow(s.y, n)};
                        const f32 f = std::pow(pn.x + pn.y, 1.0f / n);
                        const glm::vec2 grad = (pn / s) * std::pow(f, 1.0f - n);
                        return (f - 1.0f) / std::max(glm::length(grad), 1e-4f);
                    };
                    if (k >= 0.0f) {
                        if (q.x > 0.0f && q.y > 0.0f) {
                            return superellipse(q / radius, std::exp2(std::min(k, 4.0f))) * radius <= 0.0f;
                        }
                        return std::min(std::max(q.x, q.y), 0.0f) + glm::length(glm::max(q, glm::vec2{0.0f})) - radius <= 0.0f;
                    }
                    const glm::vec2 mirrored = glm::abs(glm::vec2{1.0f} - q / radius);
                    const f32 cut = -superellipse(mirrored, std::exp2(std::min(-k, 4.0f))) * radius;
                    return std::max(box_dist, cut) <= 0.0f;
                }
                case UiClipShape::Kind::Circle:
                    return glm::length(local - shape.center * box.size) <= shape.radius * unit;
                case UiClipShape::Kind::Ellipse: {
                    const glm::vec2 d = (local - shape.center * box.size) / glm::max(shape.radii * unit, glm::vec2{1e-4f});
                    return glm::dot(d, d) <= 1.0f;
                }
                case UiClipShape::Kind::Polygon: {
                    bool inside = false;
                    const u32 count = shape.point_count;
                    for (u32 i = 0, j = count - 1; i < count; j = i++) {
                        const glm::vec2 a = shape.points[i] * box.size;
                        const glm::vec2 b = shape.points[j] * box.size;
                        if (((a.y > local.y) != (b.y > local.y)) && (local.x < (b.x - a.x) * (local.y - a.y) / (b.y - a.y) + a.x)) {
                            inside = !inside;
                        }
                    }
                    return inside;
                }
            }
            return true;
        }

    } // namespace

    f32 Context::mask_value_at(const UiMaskShape &mask, const ElementBounds &box, glm::vec2 point) const noexcept {
        const glm::vec2 size = glm::max(box.size, glm::vec2{1.0f});
        const glm::vec2 uv = (point - box.position) / size;
        f32 value = 1.0f;
        if (mask.image) {
            const std::optional<f32> sampled = mask_alpha_sampler_ ? mask_alpha_sampler_(mask.texture, uv, mask.luminance) : std::nullopt;
            if (!sampled) {
                return 1.0f; // pixels unknown: do not hide anything
            }
            value = *sampled;
        } else {
            const glm::vec2 direction{std::cos(mask.angle_radians), std::sin(mask.angle_radians)};
            const glm::vec2 centered = (uv - glm::vec2{0.5f}) * size;
            const f32 extent = std::abs(direction.x) * size.x + std::abs(direction.y) * size.y;
            const f32 t = glm::dot(centered, direction) / std::max(extent, 1e-4f) + 0.5f;
            const f32 span = std::max(mask.end - mask.start, 1e-4f);
            const f32 f = std::clamp((t - mask.start) / span, 0.0f, 1.0f);
            value = mask.alpha_start + (mask.alpha_end - mask.alpha_start) * f;
        }
        return mask.invert ? 1.0f - value : value;
    }

    bool Context::hit_laid_out(i32 index, glm::vec2 point) const noexcept {
        // Walk from the outermost ancestor down to the element: each transform maps the pointer back into that element's own space, each clip
        // shape cuts it.
        i32 chain[64];
        usize depth = 0;
        for (i32 cursor = index; cursor >= 0 && depth < std::size(chain); cursor = last_frame_hits_[static_cast<usize>(cursor)].parent) {
            chain[depth++] = cursor;
        }
        for (usize i = depth; i-- > 0;) {
            const HitLaidOut &record = last_frame_hits_[static_cast<usize>(chain[i])];
            if (record.transform) {
                const UiTransform2D &t = *record.transform;
                const glm::vec2 origin = record.box.position + record.box.size * t.origin;
                const f32 c = std::cos(t.angle_radians);
                const f32 s = std::sin(t.angle_radians);
                const glm::vec2 offset = point - origin - t.translate;
                const glm::vec2 unrotated{c * offset.x + s * offset.y, -s * offset.x + c * offset.y};
                point = origin + glm::vec2{unrotated.x / (std::abs(t.scale.x) < 1e-6f ? 1e-6f : t.scale.x),
                                           unrotated.y / (std::abs(t.scale.y) < 1e-6f ? 1e-6f : t.scale.y)};
            }
            if (record.clip && !inside_clip_shape(*record.clip, record.box, point, pixel_scale_)) {
                return false;
            }
            if (record.mask && mask_value_at(*record.mask, record.box, point) < 0.1f) {
                return false;
            }
        }
        const ElementBounds &box = last_frame_hits_[static_cast<usize>(index)].box;
        return point.x >= box.position.x && point.y >= box.position.y && point.x <= box.position.x + box.size.x &&
               point.y <= box.position.y + box.size.y;
    }

    std::optional<bool> Context::shaped_hit(const UString &id, glm::vec2 point) const noexcept {
        if (last_frame_hits_.empty()) {
            return std::nullopt;
        }
        const auto found = last_frame_hit_by_id_.find(id.cpp_string());
        if (found == last_frame_hit_by_id_.end() || !last_frame_hits_[static_cast<usize>(found->second)].shaped_ancestry) {
            return std::nullopt;
        }
        const i32 target = found->second;
        if (!hit_laid_out(target, point)) {
            return false;
        }
        const auto related = [this](i32 a, i32 b) {
            for (i32 cursor = a; cursor >= 0; cursor = last_frame_hits_[static_cast<usize>(cursor)].parent) {
                if (cursor == b) return true;
            }
            return false;
        };
        const HitLaidOut &element = last_frame_hits_[static_cast<usize>(target)];
        for (i32 other = 0; other < static_cast<i32>(last_frame_hits_.size()); ++other) {
            const HitLaidOut &candidate = last_frame_hits_[static_cast<usize>(other)];
            // Drawn above it (a higher z, or the same z declared later), drawing something, and not part of the same branch.
            const bool above = candidate.z > element.z || (candidate.z == element.z && other > target);
            if (!above || !candidate.solid || related(other, target) || related(target, other)) {
                continue;
            }
            if (hit_laid_out(other, point)) {
                return false; // covered
            }
        }
        return true;
    }

    bool Context::hovered(const UString &id) const noexcept {
        if (context_ == nullptr || id.size() == 0) {
            return false;
        }
        if (const std::optional<bool> shaped = shaped_hit(id, pointer_position_)) {
            return *shaped;
        }
        set_current();
        const string_view id_view = id.cpp_string_view();
        const SEClay_ElementId element_id = SEClay_GetElementId(
            SEClay_String::from_view(id_view));
        return SEClay_PointerOver(element_id);
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
        if (const std::optional<bool> shaped = shaped_hit(id, pointer_press_position_)) {
            return *shaped;
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
        [[nodiscard]] SEClay_ElementId to_clay_element_id(const UString &id) noexcept {
            const string_view id_view = id.cpp_string_view();
            return SEClay_GetElementId(
                SEClay_String::from_view(id_view));
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

        const SEClay_ScrollContainerData scroll = SEClay_GetScrollContainerData(to_clay_element_id(container_id));
        if (!scroll.found || scroll.scrollPosition == nullptr) {
            return false;
        }
        const SEClay_ElementData container = SEClay_GetElementData(to_clay_element_id(container_id));
        const SEClay_ElementData target = SEClay_GetElementData(to_clay_element_id(target_id));
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
        const SEClay_ScrollContainerData scroll = SEClay_GetScrollContainerData(to_clay_element_id(container_id));
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
        const SEClay_ScrollContainerData scroll = SEClay_GetScrollContainerData(to_clay_element_id(container_id));
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
        [[nodiscard]] glm::vec4 clay_color_to_linear(SEClay_Color color) noexcept {
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
        [[nodiscard]] RHI::Rect2D intersect_rect(const RHI::Rect2D &parent, const SEClay_BoundingBox &box, f32 pixel_scale) noexcept {
            // `parent` is already in framebuffer pixels; `box` is in logical layout units.
            const i32 parent_x1 = parent.x + static_cast<i32>(parent.width);
            const i32 parent_y1 = parent.y + static_cast<i32>(parent.height);
            i32 x0 = std::max(parent.x, static_cast<i32>(std::floor(box.x * pixel_scale)));
            i32 y0 = std::max(parent.y, static_cast<i32>(std::floor(box.y * pixel_scale)));
            i32 x1 = std::min(parent_x1, static_cast<i32>(std::ceil((box.x + box.width) * pixel_scale)));
            i32 y1 = std::min(parent_y1, static_cast<i32>(std::ceil((box.y + box.height) * pixel_scale)));
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
                                     unordered_map<OutlineCacheKey, Text::GlyphOutline, OutlineCacheKeyHash> &outline_cache,
                                     Text::RasterPreference raster_preference) {
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
                        .format = glyph.is_color ? Text::RasterFormat::Color : Text::select_raster_format(static_cast<f32>(font_size), raster_preference),
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
    FrameSnapshot Context::finish_frame(std::source_location site) {
        note_call_site(site);
        set_current();
        FrameSnapshot snapshot;
        snapshot.full_viewport_scissor_ = RHI::Rect2D{
            .x = 0,
            .y = 0,
            .width = layout_extent_.x,
            .height = layout_extent_.y,
        };

        SEClay_RenderCommandArray commands = SEClay_EndLayout();
        if (diagnostics_) {
            // Capacity problems were noticed part-way through the frame, when only a lower bound of the need was known; now the
            // frame is complete, put the real totals in the reports.
            const SEClay_MemoryStats stats = SEClay_GetMemoryStats();
            if (stats.truncatedLastLayout) {
                UiDiagnostics &store = diagnostics_->store;
                store.update_numbers(UiErrorCode::ElementLimit, stats.frame, stats.usedElements, stats.capacityElements);
                store.update_numbers(UiErrorCode::ElementConfigLimit, stats.frame, stats.usedElementConfigs, stats.capacityElementConfigs);
                store.update_numbers(UiErrorCode::TextLineLimit, stats.frame, stats.usedWrappedTextLines, stats.capacityWrappedTextLines);
                store.update_numbers(UiErrorCode::DrawCommandLimit, stats.frame, stats.usedRenderCommands, stats.capacityRenderCommands);
            }
        }

        unordered_map<string, ElementBounds> completed_bounds;
        completed_bounds.reserve(current_frame_ids_.size());
        for (const string &id : current_frame_ids_) {
            const SEClay_ElementId element_id = SEClay_GetElementId(SEClay_String::from_view(id));
            const SEClay_ElementData data = SEClay_GetElementData(element_id);
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
            const SEClay_ElementId element_id = SEClay_GetElementId(SEClay_String::from_view(id));
            const SEClay_ScrollContainerData scroll = SEClay_GetScrollContainerData(element_id);
            if (scroll.found && scroll.scrollPosition != nullptr) {
                completed_scroll_offsets.insert_or_assign(id, glm::vec2{scroll.scrollPosition->x, scroll.scrollPosition->y});
            }
        }
        last_frame_scroll_offsets_ = std::move(completed_scroll_offsets);
        last_frame_bounds_ = std::move(completed_bounds);
        last_frame_hits_.clear();
        last_frame_hit_by_id_.clear();
        const bool any_shape = std::ranges::any_of(hit_records_, [this](const HitRecord &record) {
            return record.effect != nullptr && (record.effect->transform || record.effect->clip_shape ||
                                                (hit_test_mode_ == HitTestMode::Fixed && record.effect->mask));
        });
        if (any_shape) {
            last_frame_hits_.reserve(hit_records_.size());
            const f32 scale = std::max(pixel_scale_, 0.0001f);
            for (const HitRecord &record : hit_records_) {
                HitLaidOut laid;
                const SEClay_ElementData data = SEClay_GetElementData(SEClay_ElementId{.id = record.clay_id});
                if (data.found) {
                    laid.box = ElementBounds{.position = {data.boundingBox.x, data.boundingBox.y},
                                             .size = {data.boundingBox.width, data.boundingBox.height}};
                }
                laid.parent = record.parent;
                laid.z = record.z;
                laid.solid = record.solid && data.found;
                if (record.effect != nullptr) {
                    if (record.effect->transform) {
                        laid.transform = *record.effect->transform;
                        laid.transform->translate /= scale; // the effect's translation is surface pixels
                    }
                    laid.clip = record.effect->clip_shape;
                    if (hit_test_mode_ == HitTestMode::Fixed) {
                        laid.mask = record.effect->mask;
                    }
                }
                laid.shaped_ancestry = laid.transform.has_value() || laid.clip.has_value() || laid.mask.has_value() ||
                                       (record.parent >= 0 && last_frame_hits_[static_cast<usize>(record.parent)].shaped_ancestry);
                last_frame_hits_.push_back(laid);
            }
            last_frame_hit_by_id_ = hit_record_by_id_;
        }
        if (!focused_id_.empty() && !last_frame_bounds_.contains(focused_id_)) {
            focused_id_.clear();
        }
        if (!pointer_capture_id_.empty() && !last_frame_bounds_.contains(pointer_capture_id_)) {
            pointer_capture_id_.clear();
        }

        vector<RHI::Rect2D> scissor_stack{snapshot.full_viewport_scissor_};
        // Records, per clip element id, the ambient scissor that was active when that element was
        // opened during Clay's normal (non-floating) tree traversal -- i.e. already intersected with
        // every real ancestor clip (a scroll area's viewport, a dock panel's bounds, ...). Floating
        // roots (tick labels, legends, tooltips -- anything using FloatingConfig) are appended by Clay
        // *after* the whole main tree finishes, so `scissor_stack` has already unwound back to the base
        // viewport by the time their own SCISSOR_START command is processed; without this map they'd
        // only clip to their own attached element's bounding box and freely overflow whatever scrollable
        // or windowed container that element itself sits inside. Clay's floating SCISSOR_START commands
        // carry the clip element's real id in `.id` (patched into clay.h — see the comment there) so
        // they can look themselves up here.
        unordered_map<u32, RHI::Rect2D> ambient_scissor_by_clip_id;
        // Indices into snapshot.effect_groups_ of the effect groups open at this point of the command stream, innermost last.
        vector<i32> effect_stack;

        for (i32 i = 0; i < commands.length; ++i) {
            const SEClay_RenderCommand &command = *SEClay_RenderCommandArray_Get(&commands, i);
            const RHI::Rect2D active_scissor = scissor_stack.back();


            const PaintKey paint{unpack_z(command.userData), static_cast<u32>(i)};
            switch (command.commandType) {
                case SEClay_RenderCommandType::Rectangle: {
                    const SEClay_RectangleRenderData &data = command.renderData.rectangle;
                    snapshot.quads_.push_back(QuadDraw{
                        .instance = UiQuadInstance{
                            .position = {command.boundingBox.x, command.boundingBox.y},
                            .size = {command.boundingBox.width, command.boundingBox.height},
                            .corner_radius = {data.cornerRadius.topLeft, data.cornerRadius.topRight,
                                            data.cornerRadius.bottomLeft, data.cornerRadius.bottomRight},
                            .corner_shape_codes = data.cornerRadius.shapeCodes,
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
                case SEClay_RenderCommandType::Border: {
                    const SEClay_BorderRenderData &data = command.renderData.border;
                    if (data.edgeData != nullptr) {
                        append_edge_borders(snapshot.quads_, *static_cast<const BorderEdges *>(data.edgeData), {command.boundingBox.x, command.boundingBox.y},
                                            {command.boundingBox.width, command.boundingBox.height}, data, active_scissor, paint);
                        break;
                    }
                    snapshot.quads_.push_back(QuadDraw{
                        .instance = UiQuadInstance{
                            .position = {command.boundingBox.x, command.boundingBox.y},
                            .size = {command.boundingBox.width, command.boundingBox.height},
                            .corner_radius = {data.cornerRadius.topLeft, data.cornerRadius.topRight,
                                            data.cornerRadius.bottomLeft, data.cornerRadius.bottomRight},
                            .corner_shape_codes = data.cornerRadius.shapeCodes,
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
                case SEClay_RenderCommandType::Image: {
                    const SEClay_ImageRenderData &data = command.renderData.image;
                    const auto *image_ref = static_cast<const ImageRef *>(data.imageData);
                    const ImagePlacement placement = place_image(image_ref != nullptr ? image_ref->fit : ImageFitOptions{},
                                                                 {command.boundingBox.width, command.boundingBox.height});
                    snapshot.quads_.push_back(QuadDraw{
                        .instance = UiQuadInstance{
                            .position = glm::vec2{command.boundingBox.x, command.boundingBox.y} + placement.offset,
                            .size = placement.size,
                            // The corners belong to the whole box: only an image that fills it takes them.
                            .corner_radius = placement.full_box ? glm::vec4{data.cornerRadius.topLeft, data.cornerRadius.topRight,
                                                                           data.cornerRadius.bottomLeft, data.cornerRadius.bottomRight}
                                                                : glm::vec4{0.0f},
                            .corner_shape_codes = placement.full_box ? data.cornerRadius.shapeCodes : 0u,
                            .fill_color = clay_color_to_linear(data.backgroundColor),
                            .uv_min = placement.uv_min,
                            .uv_max = placement.uv_max,
                            .kind = static_cast<f32>(image_ref != nullptr && image_ref->sampling == ImageSampling::Nearest
                                                                  ? UiQuadKind::ImageNearest
                                                                  : UiQuadKind::Image),
                        },
                        .image_ref = image_ref,
                        .scissor = active_scissor,
                        .paint = paint,
                    });
                    break;
                }
                case SEClay_RenderCommandType::Text: {
                    const SEClay_TextRenderData &data = command.renderData.text;
                    const CachedShape *shape = text_bridge_.shape_and_cache(
                        TextStyle{.font_id = data.fontId, .font_size = data.fontSize, .letter_spacing = data.letterSpacing,
                                 .line_height = data.lineHeight},
                        string_view{data.stringContents.chars, static_cast<usize>(data.stringContents.length)});
                    if (shape != nullptr) {
                        const usize extras_index = unpack_text_extras(command.userData);
                        const TextExtras *extras = extras_index > 0 && extras_index <= text_extras_.size() ? &text_extras_[extras_index - 1] : nullptr;
                        const glm::vec2 text_origin{command.boundingBox.x, command.boundingBox.y};
                        const glm::vec4 text_color = clay_color_to_linear(data.textColor);
                        if (extras != nullptr && extras->shadow.enabled) {
                            // Under the text: drawn first, moved by the shadow's offset (layout units), in its own colour.
                            const f32 to_pixels = pixel_scale_;
                            const Foundation::Color::Linear shadow = extras->shadow.color.to_linear();
                            append_glyph_placements(snapshot.glyphs_, snapshot.glyph_scissors_, snapshot.glyph_paint_, active_scissor, paint,
                                                    *shape, data.fontSize,
                                                    glm::vec4{static_cast<f32>(shadow.r), static_cast<f32>(shadow.g), static_cast<f32>(shadow.b),
                                                              static_cast<f32>(shadow.a)},
                                                    text_origin + extras->shadow.offset * to_pixels, outline_cache_, text_raster_preference_);
                        }
                        append_glyph_placements(snapshot.glyphs_, snapshot.glyph_scissors_, snapshot.glyph_paint_,
                                               active_scissor, paint, *shape, data.fontSize, text_color, text_origin, outline_cache_,
                                               text_raster_preference_);
                        if (extras != nullptr && extras->decoration.lines != TextDecorationLine::None && shape->fonts != nullptr &&
                            shape->fonts->primary != nullptr) {
                            const Text::Font &font = *shape->fonts->primary;
                            const f32 em = font.units_per_em() > 0 ? static_cast<f32>(data.fontSize) / static_cast<f32>(font.units_per_em()) : 0.0f;
                            const f32 baseline = text_origin.y + static_cast<f32>(font.ascender()) * em;
                            const f32 size = static_cast<f32>(data.fontSize);
                            const f32 thickness = extras->decoration.thickness > 0.0f ? extras->decoration.thickness * pixel_scale_
                                                                                       : std::max(1.0f, std::round(size / 14.0f));
                            glm::vec4 line_color = text_color;
                            if (extras->decoration.color) {
                                const Foundation::Color::Linear linear = extras->decoration.color->to_linear();
                                line_color = glm::vec4{static_cast<f32>(linear.r), static_cast<f32>(linear.g), static_cast<f32>(linear.b),
                                                       static_cast<f32>(linear.a)};
                            }
                            const f32 length = shape->width_px;
                            const auto line = [&](f32 center_y) {
                                append_text_decoration_line(snapshot.quads_, text_origin.x, center_y - thickness * 0.5f, length, thickness,
                                                            extras->decoration.style, line_color, active_scissor, paint);
                            };
                            const u8 lines = static_cast<u8>(extras->decoration.lines);
                            if ((lines & static_cast<u8>(TextDecorationLine::Underline)) != 0) line(baseline + size * 0.12f);
                            if ((lines & static_cast<u8>(TextDecorationLine::Overline)) != 0) line(text_origin.y + thickness * 0.5f);
                            if ((lines & static_cast<u8>(TextDecorationLine::LineThrough)) != 0) line(baseline - size * 0.28f);
                        }
                    }
                    break;
                }
                case SEClay_RenderCommandType::ScissorStart: {
                    // A floating root's clip element was possibly already opened earlier this frame with
                    // a real (already ancestor-intersected) ambient scissor recorded below; prefer that
                    // over `active_scissor`, which for a floating root is just the base viewport at this
                    // point in the command stream. Elements opened normally still record their own entry
                    // the same way, so a self-lookup here (before it exists) simply misses and falls back
                    // to `active_scissor`, unchanged from before.
                    const auto ambient = ambient_scissor_by_clip_id.find(command.id);
                    const RHI::Rect2D base_scissor = ambient != ambient_scissor_by_clip_id.end() ? ambient->second : active_scissor;
                    ambient_scissor_by_clip_id.emplace(command.id, active_scissor);
                    scissor_stack.push_back(intersect_rect(base_scissor, command.boundingBox, pixel_scale_));
                    break;
                }
                case SEClay_RenderCommandType::ScissorEnd: {
                    if (scissor_stack.size() > 1) {
                        scissor_stack.pop_back();
                    }
                    break;
                }
                case SEClay_RenderCommandType::Custom: {
                    const SEClay_CustomRenderData &data = command.renderData.custom;
                    if (data.customData != nullptr) {
                        // See UiCustomCommandKind's own doc comment: the tag is always the first
                        // member of whichever payload struct customData actually points to.
                        const auto command_kind = *static_cast<const UiCustomCommandKind *>(data.customData);
                        if (command_kind == UiCustomCommandKind::Stroke) {
                            const auto *stroke_data = static_cast<const StrokePolylineData *>(data.customData);
                            const glm::vec2 origin{command.boundingBox.x, command.boundingBox.y};
                            vector<StrokePath> resolved_paths;
                            resolved_paths.reserve(stroke_data->paths.size());
                            for (const StrokePath &local_path : stroke_data->paths) {
                                vector<glm::vec2> resolved_points;
                                resolved_points.reserve(local_path.points.size());
                                for (const glm::vec2 &local_point : local_path.points) {
                                    resolved_points.push_back(origin + local_point);
                                }
                                resolved_paths.push_back(StrokePath{
                                    .points = std::move(resolved_points),
                                    .style = local_path.style,
                                });
                            }
                            snapshot.strokes_.push_back(StrokeDraw{
                                .paths = std::move(resolved_paths),
                                .scissor = active_scissor,
                                .paint = paint,
                            });
                        } else if (command_kind == UiCustomCommandKind::Fill) {
                            const auto *fill_data = static_cast<const FillQuadListData *>(data.customData);
                            const glm::vec2 origin{command.boundingBox.x, command.boundingBox.y};
                            vector<FillQuad> resolved_quads;
                            resolved_quads.reserve(fill_data->quads.size());
                            for (FillQuad quad : fill_data->quads) {
                                quad.position += origin;
                                resolved_quads.push_back(quad);
                            }
                            snapshot.fills_.push_back(FillQuadDraw{
                                .quads = std::move(resolved_quads),
                                .scissor = active_scissor,
                                .paint = paint,
                            });
                        } else if (command_kind == UiCustomCommandKind::Sector) {
                            const auto *sector_data = static_cast<const SectorListData *>(data.customData);
                            const glm::vec2 origin{command.boundingBox.x, command.boundingBox.y};
                            vector<Sector> resolved_sectors;
                            resolved_sectors.reserve(sector_data->sectors.size());
                            for (Sector sector : sector_data->sectors) {
                                sector.center += origin;
                                resolved_sectors.push_back(sector);
                            }
                            snapshot.sectors_.push_back(SectorDraw{
                                .sectors = std::move(resolved_sectors),
                                .scissor = active_scissor,
                                .paint = paint,
                            });
                        } else if (command_kind == UiCustomCommandKind::CustomStroke) {
                            const auto *custom_stroke_data = static_cast<const CustomStrokeElementData *>(data.customData);
                            const glm::vec2 origin{command.boundingBox.x, command.boundingBox.y};
                            vector<glm::vec2> resolved_points;
                            resolved_points.reserve(custom_stroke_data->points.size());
                            for (const glm::vec2 &local_point : custom_stroke_data->points) {
                                resolved_points.push_back(origin + local_point);
                            }
                            // Copies into the same stable storage CustomDraw::shader points into, so
                            // the resolved pointer survives for the snapshot's lifetime (see
                            // custom_storage_'s move into the snapshot below).
                            custom_storage_.push_back(custom_stroke_data->shader);
                            snapshot.custom_strokes_.push_back(CustomStrokeDraw{
                                .points = std::move(resolved_points),
                                .half_width = custom_stroke_data->half_width,
                                .feather_px = custom_stroke_data->feather_px,
                                .shader = &custom_storage_.back(),
                                .scissor = active_scissor,
                                .paint = paint,
                            });
                        } else {
                            snapshot.custom_draws_.push_back(CustomDraw{
                                .position = {command.boundingBox.x, command.boundingBox.y},
                                .size = {command.boundingBox.width, command.boundingBox.height},
                                .shader = static_cast<const CustomShaderRef *>(data.customData),
                                .scissor = active_scissor,
                                .paint = paint,
                            });
                        }
                    }
                    break;
                }
                case SEClay_RenderCommandType::EffectStart: {
                    const auto *effect = static_cast<const UiEffect *>(command.renderData.effect.effectData);
                    if (effect == nullptr) {
                        break; // cannot happen (SEClay only emits the pair for a non-null pointer); skipped rather than trusted
                    }
                    snapshot.effect_groups_.push_back(EffectGroupDraw{
                        .effect = effect,
                        .position = {command.boundingBox.x, command.boundingBox.y},
                        .size = {command.boundingBox.width, command.boundingBox.height},
                        .scissor = active_scissor,
                        .paint = paint,
                        .last_order = paint.order,
                        .parent = effect_stack.empty() ? -1 : effect_stack.back(),
                        .pixel_scale = pixel_scale_,
                    });
                    effect_stack.push_back(static_cast<i32>(snapshot.effect_groups_.size()) - 1);
                    break;
                }
                case SEClay_RenderCommandType::EffectEnd: {
                    if (!effect_stack.empty()) {
                        snapshot.effect_groups_[static_cast<usize>(effect_stack.back())].last_order = paint.order;
                        effect_stack.pop_back();
                    }
                    break;
                }
                case SEClay_RenderCommandType::None:
                default:
                    break;
            }
        }

        if (pixel_scale_ != 1.0f) {
            scale_snapshot_to_pixels(snapshot);
        }

        snapshot.effect_storage_ = std::move(effect_storage_);
        effect_storage_.clear();
        snapshot.image_storage_ = std::move(image_storage_);
        image_storage_.clear();
        snapshot.custom_storage_ = std::move(custom_storage_);
        custom_storage_.clear();
        // Unlike image_storage_/custom_storage_, StrokeDraw owns its resolved points outright (see
        // its own doc comment), so nothing in the snapshot points back into stroke_storage_ — it only
        // needed to stay alive for the resolve loop above, not for the snapshot's own lifetime.
        stroke_storage_.clear();
        // Same rationale as stroke_storage_ above — FillQuadDraw/SectorDraw/CustomStrokeDraw own
        // their resolved points outright (CustomStrokeDraw's shader pointer is a separate copy kept
        // alive in custom_storage_, moved into the snapshot just above).
        fill_storage_.clear();
        sector_storage_.clear();
        custom_stroke_storage_.clear();
        text_storage_.clear();
        if (!pointer_down_) {
            pointer_capture_id_.clear();
        }
        return snapshot;
    }

    /// Sets the logical-unit-to-pixel scale used from the next `begin_layout()`.
    ///
    /// @param scale Pixels per logical unit.
    ///
    /// @note This function does not throw exceptions.
    void Context::set_pixel_scale(f32 scale) noexcept {
        pixel_scale_ = std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
    }

    /// Converts every logical-unit coordinate, extent and pixel-denominated style value in `snapshot`
    /// to framebuffer pixels. Scissors are already built in pixels (see `intersect_rect`).
    ///
    /// @param snapshot Snapshot produced by the resolve loop in `finish_frame()`.
    ///
    /// @note This function does not throw exceptions.
    void Context::scale_snapshot_to_pixels(FrameSnapshot &snapshot) const noexcept {
        const f32 s = pixel_scale_;
        for (EffectGroupDraw &group : snapshot.effect_groups_) {
            group.position *= s;
            group.size *= s;
        }
        for (QuadDraw &quad : snapshot.quads_) {
            quad.instance.position *= s;
            quad.instance.size *= s;
            quad.instance.corner_radius *= s;
            quad.instance.border_width *= s;
        }
        for (StrokeDraw &stroke : snapshot.strokes_) {
            for (StrokePath &path : stroke.paths) {
                for (glm::vec2 &point : path.points) {
                    point *= s;
                }
                path.style.width *= s;
                path.style.feather_px *= s;
                path.style.dash_length *= s;
                path.style.dash_gap *= s;
            }
        }
        for (FillQuadDraw &fill : snapshot.fills_) {
            for (FillQuad &quad : fill.quads) {
                quad.position *= s;
                quad.size *= s;
                quad.corner_radius.top_left *= s;
                quad.corner_radius.top_right *= s;
                quad.corner_radius.bottom_left *= s;
                quad.corner_radius.bottom_right *= s;
            }
        }
        for (SectorDraw &draw : snapshot.sectors_) {
            for (Sector &sector : draw.sectors) {
                sector.center *= s;
                sector.inner_radius *= s;
                sector.outer_radius *= s;
                sector.style.feather_px *= s;
            }
        }
        for (CustomStrokeDraw &stroke : snapshot.custom_strokes_) {
            for (glm::vec2 &point : stroke.points) {
                point *= s;
            }
            stroke.half_width *= s;
            stroke.feather_px *= s;
        }
        for (CustomDraw &draw : snapshot.custom_draws_) {
            draw.position *= s;
            draw.size *= s;
        }
        // Shaping is unhinted (advances are em-relative), so scaling the placed glyphs is exact; the
        // raster size is scaled too so the atlas renders them at full resolution.
        for (Renderer::GlyphPlacement &glyph : snapshot.glyphs_) {
            glyph.position *= s;
            glyph.size *= s;
            glyph.pixel_size *= s;
            if (glyph.format != Text::RasterFormat::Color) {
                glyph.format = Text::select_raster_format(glyph.pixel_size, text_raster_preference_);
            }
        }
    }

    /// Destroys or releases the `UI` resource represented by the supplied parameters.
    ///
    /// @return Returns the current destroy value.
    /// @note This function does not throw exceptions.
    void Context::destroy() noexcept {


        if (context_ != nullptr) {
            // Clay keeps a few buffers on the heap outside its arena (scroll container storage that outgrew
            // its arena slice, and the scroll id index) -- freeing arena_memory_ below wouldn't release them.
            // Frees everything a growable context owns (its blocks and the context itself); for a fixed-size one only the few heap
            // buffers it can own, since its arena (arena_memory_) is released below.
            SEClay_DestroyContext(context_);
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
