#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <functional>
#include <optional>
#include <span>
#include <string>
#pragma endregion

#include <Renderer/UI/Button.hpp>
#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/Style.hpp>
#include <Renderer/UI/WidgetComposition.hpp>

using std::span;


namespace SFT::UI {


    struct DropdownOption {
        std::function<void(Context &)> build;
    };

    struct DropdownStyle {
        ButtonStyle trigger{};
        Color list_background{0.14, 0.15, 0.19, 0.98};
        Color option_hovered{0.24, 0.26, 0.32, 1.0};
        CornerRadius corner_radius = CornerRadius::all(8.0f);
        BorderStyle border{};
        u16 option_padding = 8;
        u16 list_z_index = 100;


        bool show_arrow_indicator = true;
        Color arrow_color{1.0, 1.0, 1.0, 1.0};
        FontId arrow_font_id = 0;
        u16 arrow_font_size = 14;


        FloatingAttachPoint arrow_attach_point = FloatingAttachPoint::RightCenter;
        glm::vec2 arrow_offset{-10.0f, 0.0f};


        f32 arrow_reserved_space = 22.0f;


        bool center_trigger_content_vertically = true;
    };

    enum class DropdownVisualPart : u8 {
        Anchor,
        Trigger,
        Indicator,
        List,
        Option,
        Header,
        Footer,
        Empty,
        Tooltip,
    };

    /// Performs the dropdown part ID operation using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    /// @param part `part` value used by the operation.
    /// @param option_index Zero-based index of the target element or entry.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] UString dropdown_part_id(const UString &id, DropdownVisualPart part, std::optional<usize> option_index = std::nullopt);

    struct DropdownPartContext {
        DropdownVisualPart part = DropdownVisualPart::Anchor;
        PartVisualState visual{};
        UString id;
        bool open = false;
        usize selected_index = 0;
        std::optional<usize> option_index;
        const DropdownOption *option = nullptr;
        std::optional<ElementBounds> trigger_bounds;
        std::optional<ElementBounds> bounds;
    };

    struct DropdownComposition {
        PartSlot<DropdownPartContext> anchor{};
        PartSlot<DropdownPartContext> trigger{};
        PartSlot<DropdownPartContext> indicator{};
        PartSlot<DropdownPartContext> list{};
        PartSlot<DropdownPartContext> option{};
        PartSlot<DropdownPartContext> header{.visible = false, .render_default = false};
        PartSlot<DropdownPartContext> footer{.visible = false, .render_default = false};
        PartSlot<DropdownPartContext> empty{.visible = false, .render_default = false};
        PartSlot<DropdownPartContext> tooltip{.visible = false, .render_default = false};


        std::function<bool(usize)> option_enabled;
    };

    class DropdownState {
      public:
        bool open = false;
        ButtonState trigger_state{};
    };

    struct DropdownResult {


        bool changed = false;
        usize selected_index = 0;
    };


    /// Performs the dropdown operation using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param id Identifier of the target object or resource.
    /// @param trigger_decl `trigger_decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param selected_index Zero-based index of the target element or entry.
    /// @param options Configuration values controlling the operation.
    /// @param enabled Whether the associated behavior is enabled.
    /// @param composition `composition` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] DropdownResult dropdown(Context &ctx, const UString &id, const ElementDecl &trigger_decl, const DropdownStyle &style, DropdownState &state, f32 delta_seconds, usize selected_index, span<const DropdownOption> options, bool enabled, const DropdownComposition &composition);

    /// Performs the dropdown operation for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param id Identifier of the target object or resource.
    /// @param trigger_decl `trigger_decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param selected_index Zero-based index of the target element or entry.
    /// @param options Configuration values controlling the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] DropdownResult dropdown(Context &ctx, const UString &id, const ElementDecl &trigger_decl, const DropdownStyle &style, DropdownState &state, f32 delta_seconds, usize selected_index, span<const DropdownOption> options, bool enabled = true);

} // namespace SFT::UI
