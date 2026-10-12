#include <CxxApi/Ui.hpp>

#include <string_view>

#include <Engine/ForeignUi.hpp>

#include <CxxApi/Error.hpp>

namespace SFT::CxxApi {

    namespace {

        namespace E = SFT::Engine;

        static_assert(static_cast<std::uint32_t>(UiSizing::Percent) == static_cast<std::uint32_t>(E::ForeignUiSizing::Percent));
        static_assert(static_cast<std::uint32_t>(UiSizing::Fit) == static_cast<std::uint32_t>(E::ForeignUiSizing::Fit));
        static_assert(static_cast<std::uint32_t>(UiScaleMode::RawPixels) == static_cast<std::uint32_t>(E::ForeignUiScaleMode::RawPixels));

        template <typename T> T unwrap(E::ForeignUiResult<T> result) {
            if (!result) {
                fail(result.error().message);
            }
            if constexpr (!std::is_void_v<T>) {
                return std::move(*result);
            }
        }

        [[nodiscard]] E::ForeignUiElement to_engine(const UiElement &element, const std::string &id) {
            E::ForeignUiElement out;
            out.width_kind = static_cast<E::ForeignUiSizing>(element.width_kind);
            out.width_value = element.width_value;
            out.height_kind = static_cast<E::ForeignUiSizing>(element.height_kind);
            out.height_value = element.height_value;
            for (int i = 0; i < 4; ++i) {
                out.padding[i] = element.padding[i];
                out.background[i] = element.background[i];
                out.corner_radius[i] = element.corner_radius[i];
            }
            out.child_gap = element.child_gap;
            out.top_to_bottom = element.direction == UiDirection::TopToBottom;
            out.id = id;
            return out;
        }

    } // namespace

    void ui_register_font(E::Engine &engine, const std::string &path, std::uint32_t font_id) {
        unwrap(E::register_foreign_ui_font(engine, path, font_id));
    }

    void ui_set_scale_mode(E::Engine &engine, UiScaleMode mode) {
        unwrap(E::set_foreign_ui_scale_mode(engine, static_cast<E::ForeignUiScaleMode>(mode)));
    }

    void ui_begin(FrameBuilder &frame) {
        unwrap(E::begin_foreign_ui_frame(frame.engine, frame.info.framebuffer_width, frame.info.framebuffer_height, frame.info.delta_seconds));
    }

    void ui_end(FrameBuilder &frame) { unwrap(E::end_foreign_ui_frame(frame.engine, &frame.parameters)); }

    void ui_end_discard(FrameBuilder &frame) { unwrap(E::end_foreign_ui_frame(frame.engine, nullptr)); }

    void ui_begin_element(const UiElement &element, const std::string &id) { unwrap(E::begin_foreign_ui_element(to_engine(element, id))); }

    void ui_end_element() { unwrap(E::end_foreign_ui_element()); }

    void ui_text(const std::string &text, const UiTextStyle &style) {
        E::ForeignUiTextStyle out;
        out.font_id = style.font_id;
        out.font_size = style.font_size;
        for (int i = 0; i < 4; ++i) {
            out.color[i] = style.color[i];
        }
        unwrap(E::foreign_ui_text(text, out));
    }

    bool ui_hovered(const std::string &id) { return unwrap(E::foreign_ui_hovered(id)); }

    bool ui_clicked(const std::string &id) { return unwrap(E::foreign_ui_clicked(id)); }

    std::array<float, 2> ui_pointer_position() {
        const glm::vec2 position = unwrap(E::foreign_ui_pointer_position());
        return {position.x, position.y};
    }

    bool ui_pointer_down() { return unwrap(E::foreign_ui_pointer_down()); }

} // namespace SFT::CxxApi
