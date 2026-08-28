/// C ABI implementation of the immediate-mode UI.
///
/// The engine's UI is built from RAII scopes — `Context::element` returns an `ElementScope` whose
/// destructor closes the element — which has no equivalent in C. This layer keeps the open scopes on
/// a stack so a caller can pair explicit `begin_element`/`end_element` calls instead, and reports an
/// unbalanced tree rather than letting it corrupt the layout.
///
/// It also owns the per-frame orchestration a C++ caller writes by hand: ready the UI renderer,
/// begin a layout sized to the surface, finish it into a snapshot, and attach the snapshot to the
/// frame as an overlay. Those five steps have to happen in order and in the right places, so
/// wrapping them in `sturdy_ui_begin`/`sturdy_ui_end` is what keeps a foreign caller from getting
/// the sequence subtly wrong.
///
/// State lives in one process-wide session rather than per engine: a UI frame is bounded by
/// begin/end on the thread building it, and only one can be open at a time.

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>

#include <Engine/Engine.hpp>
#include <Renderer/Text/Font.hpp>
#include <Renderer/UI/UI.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;

    /// The UI frame currently being built, if any.
    ///
    /// Thread-local because a layout pass belongs to whichever thread is building it — the engine
    /// renders windows on their own threads, so a process-wide session would let two windows
    /// interleave their element stacks.
    struct UiSession {
        bool open = false;
        SFT::UI::Context *context = nullptr;
        glm::vec2 viewport{0.0f, 0.0f};
        /// Open element scopes, innermost last. Held by pointer because `ElementScope` closes its
        /// element on destruction and must not be moved or copied out from under the layout.
        std::vector<std::unique_ptr<SFT::UI::ElementScope>> open_elements;
    };

    thread_local UiSession g_session;

    /// A font loaded through this ABI, kept alongside the id it was registered under.
    ///
    /// `Context::register_font` borrows the font, so the storage has to outlive registration. Kept
    /// for the process lifetime, which matches how long a registered font is usable. The id is
    /// retained too because a registration does not necessarily survive to the first frame — see
    /// `apply_font_registrations`.
    struct FontRegistration {
        SFT::UI::FontId font_id = 0;
        std::unique_ptr<SFT::Text::Font> font;
    };

    /// Fonts loaded through this ABI, process-wide rather than per-thread.
    ///
    /// Unlike the layout session, this is shared: a caller may register a font during startup and
    /// build UI on a window's render thread, and the registration has to be visible there.
    std::mutex g_font_mutex;
    std::vector<FontRegistration> g_fonts;

    /// Registers every font this ABI has loaded into `context`.
    ///
    /// `Engine::UiContext::ensure_ready` constructs a fresh `UI::Context` and move-assigns it over
    /// the old one, so any font registered before the UI renderer came up is discarded. The engine's
    /// own C++ callers avoid that by registering after `ensure_ready` from inside their frame
    /// callback. A foreign caller cannot be asked to know that ordering rule, so the ABI accepts a
    /// registration at any time and replays the whole set once the context is live. Re-registering
    /// an id replaces its entry, so replaying is idempotent.
    ///
    /// @param context Context to populate.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void apply_font_registrations(SFT::UI::Context &context) {
        const std::lock_guard<std::mutex> lock{g_font_mutex};
        for (const FontRegistration &entry : g_fonts) {
            context.register_font(entry.font_id, *entry.font);
        }
    }

    /// Converts an ABI color to the engine's sRGB color.
    ///
    /// @param rgba Four channels, non-linear sRGB with straight alpha.
    ///
    /// @return The engine-side color.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SFT::UI::Color to_engine_color(const float *rgba) noexcept {
        return SFT::UI::Color{static_cast<SFT::f64>(rgba[0]), static_cast<SFT::f64>(rgba[1]),
                              static_cast<SFT::f64>(rgba[2]), static_cast<SFT::f64>(rgba[3])};
    }

    /// Translates an ABI sizing mode into the engine's axis descriptor.
    ///
    /// @param kind Sizing mode.
    /// @param value Size, interpreted per mode.
    /// @param out_axis Receives the translated axis.
    ///
    /// @return Returns `true` when `kind` is recognized; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool to_engine_axis(SturdyUiSizing kind, float value, SFT::UI::SizingAxis *out_axis) noexcept {
        switch (kind) {
        case STURDY_UI_SIZING_FIT:
            *out_axis = SFT::UI::SizingAxis::fit();
            return true;
        case STURDY_UI_SIZING_GROW:
            *out_axis = SFT::UI::SizingAxis::grow();
            return true;
        case STURDY_UI_SIZING_FIXED:
            *out_axis = SFT::UI::SizingAxis::fixed(value);
            return true;
        case STURDY_UI_SIZING_PERCENT:
            *out_axis = SFT::UI::SizingAxis::percent(value);
            return true;
        case STURDY_UI_SIZING_FORCE_U32:
        default:
            return false;
        }
    }

    /// Reports the open session's context, or a failure explaining why there is none.
    ///
    /// @param out_context Receives the borrowed context on success.
    ///
    /// @return `STURDY_OK`, or `STURDY_ERROR_BUSY` when no UI frame is open.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult require_session(SFT::UI::Context **out_context) noexcept {
        if (!g_session.open || g_session.context == nullptr) {
            return set_error(STURDY_ERROR_BUSY,
                             "no UI frame is open on this thread; call sturdy_ui_begin first");
        }
        *out_context = g_session.context;
        return STURDY_OK;
    }

    /// Closes the session, discarding any elements the caller left open.
    ///
    /// @note This function does not throw exceptions.
    void reset_session() noexcept {
        // Destroyed innermost-first so each scope closes the element it opened, which is the order
        // the layout expects even when the caller got the pairing wrong.
        while (!g_session.open_elements.empty()) {
            g_session.open_elements.pop_back();
        }
        g_session.open = false;
        g_session.context = nullptr;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_ui_element_init(SturdyUiElement *element) {
    return guarded([&]() -> SturdyResult {
        if (element == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "element must not be null");
        }
        *element = SturdyUiElement{};
        element->struct_size = static_cast<uint32_t>(sizeof(SturdyUiElement));
        element->width_kind = STURDY_UI_SIZING_FIT;
        element->height_kind = STURDY_UI_SIZING_FIT;
        element->direction = STURDY_UI_DIRECTION_LEFT_TO_RIGHT;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_text_style_init(SturdyUiTextStyle *style) {
    return guarded([&]() -> SturdyResult {
        if (style == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "style must not be null");
        }
        *style = SturdyUiTextStyle{};
        style->struct_size = static_cast<uint32_t>(sizeof(SturdyUiTextStyle));
        style->font_size = 16;
        style->color[0] = 1.0f;
        style->color[1] = 1.0f;
        style->color[2] = 1.0f;
        style->color[3] = 1.0f;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_register_font(SturdyEngine engine,
                                                     const char *source,
                                                     uint32_t font_id) {
    return guarded([&]() -> SturdyResult {
        if (source == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "source must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const std::optional<std::string> bytes = SFT::Foundation::read_file_to_string(source);
        if (!bytes) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "could not read the font file");
        }

        const std::span<const char> chars{bytes->data(), bytes->size()};
        auto loaded = SFT::Text::Font::load(std::as_bytes(chars));
        if (!loaded) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, loaded.error().message.cpp_string_view());
        }

        // The context borrows the font, so ownership stays here for the process lifetime.
        const SFT::UI::FontId id = static_cast<SFT::UI::FontId>(font_id);
        auto owned = std::make_unique<SFT::Text::Font>(std::move(*loaded));
        {
            const std::lock_guard<std::mutex> lock{g_font_mutex};
            auto existing = std::find_if(g_fonts.begin(), g_fonts.end(),
                                         [id](const FontRegistration &entry) { return entry.font_id == id; });
            if (existing != g_fonts.end()) {
                existing->font = std::move(owned);
            } else {
                g_fonts.push_back(FontRegistration{.font_id = id, .font = std::move(owned)});
            }
        }

        // Also register into the live context so a font loaded mid-frame is usable immediately;
        // `sturdy_ui_begin` replays the set for the case where the context did not exist yet.
        apply_font_registrations(resolved_engine->ui_context().context());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_begin(SturdyEngine engine, const SturdyFrameInput *input) {
    return guarded([&]() -> SturdyResult {
        if (input == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "frame input must not be null");
        }
        if (g_session.open) {
            return set_error(STURDY_ERROR_BUSY,
                             "a UI frame is already open on this thread; call sturdy_ui_end first");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::RHI::RhiDevice *device = resolved_engine->rhi_device();
        if (device == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "the engine has no active RHI device");
        }

        // Matches the format the presentation path uses, so the overlay composites without a
        // conversion the renderer would otherwise have to insert.
        const SFT::RHI::Format color_format =
            resolved_engine->config().features.presentation.hdr_enabled ? SFT::RHI::Format::RGBA16Float
                                                                        : SFT::RHI::Format::BGRA8UnormSrgb;
        if (!resolved_engine->ui_context().ensure_ready(*device, color_format)) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "the UI renderer is not ready");
        }
        apply_font_registrations(resolved_engine->ui_context().context());

        const glm::vec2 viewport{static_cast<SFT::f32>(input->framebuffer_width),
                                 static_cast<SFT::f32>(input->framebuffer_height)};
        resolved_engine->ui_context().begin_layout(viewport, resolved_engine->ui_pointer_state(),
                                                   static_cast<SFT::f32>(input->delta_seconds));

        g_session.open = true;
        g_session.context = &resolved_engine->ui_context().context();
        g_session.viewport = viewport;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_end(SturdyEngine engine, SturdyFrame frame) {
    return guarded([&]() -> SturdyResult {
        SFT::UI::Context *context = nullptr;
        const SturdyResult session = require_session(&context);
        if (session != STURDY_OK) {
            return session;
        }

        if (!g_session.open_elements.empty()) {
            const auto leaked = g_session.open_elements.size();
            reset_session();
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             leaked == 1 ? "one element was left open; every begin_element needs an "
                                           "end_element"
                                         : "elements were left open; every begin_element needs an "
                                           "end_element");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            reset_session();
            return resolved;
        }

        auto snapshot = std::make_shared<SFT::UI::FrameSnapshot>(context->finish_frame(g_session.viewport));
        const glm::vec2 viewport = g_session.viewport;
        (void)viewport;

        // A zeroed frame handle means "discard": the layout is still finished, so the next frame
        // starts clean, but nothing is attached.
        if (frame.token != 0) {
            void *pointer = nullptr;
            const SturdyResult frame_resolved =
                SFT::Ffi::resolve_handle(frame.token, SFT::Ffi::HandleKind::Frame, &pointer);
            if (frame_resolved != STURDY_OK) {
                reset_session();
                return frame_resolved;
            }
            auto *parameters = static_cast<SFT::Engine::RenderFrameParameters *>(pointer);
            parameters->ui_overlay = resolved_engine->ui_context().build_overlay_hooks(
                std::move(snapshot), resolved_engine->renderer());
        }

        reset_session();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_begin_element(SturdyEngine engine,
                                                     const SturdyUiElement *element) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (element == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "element must not be null");
        }
        if (element->struct_size != sizeof(SturdyUiElement)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                             "SturdyUiElement size does not match this engine build");
        }

        SFT::UI::Context *context = nullptr;
        const SturdyResult session = require_session(&context);
        if (session != STURDY_OK) {
            return session;
        }

        SFT::UI::SizingAxis width{};
        SFT::UI::SizingAxis height{};
        if (!to_engine_axis(element->width_kind, element->width_value, &width) ||
            !to_engine_axis(element->height_kind, element->height_value, &height)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized sizing mode");
        }

        SFT::UI::ElementDecl decl{};
        decl.sizing = SFT::UI::Sizing{width, height};
        decl.padding = SFT::UI::Padding{
            static_cast<SFT::u16>(element->padding_left), static_cast<SFT::u16>(element->padding_right),
            static_cast<SFT::u16>(element->padding_top), static_cast<SFT::u16>(element->padding_bottom)};
        decl.child_gap = static_cast<SFT::u16>(element->child_gap);
        decl.direction = element->direction == STURDY_UI_DIRECTION_TOP_TO_BOTTOM
                             ? SFT::UI::LayoutDirection::TopToBottom
                             : SFT::UI::LayoutDirection::LeftToRight;
        decl.background_color = to_engine_color(element->background);
        decl.corner_radius = SFT::UI::CornerRadius{
            element->corner_radius[0], element->corner_radius[1], element->corner_radius[2],
            element->corner_radius[3]};
        if (element->id != nullptr) {
            decl.id = SFT::UString{element->id};
        }

        g_session.open_elements.push_back(
            std::make_unique<SFT::UI::ElementScope>(context->element(decl)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_end_element(SturdyEngine engine) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        SFT::UI::Context *context = nullptr;
        const SturdyResult session = require_session(&context);
        if (session != STURDY_OK) {
            return session;
        }
        if (g_session.open_elements.empty()) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "no element is open; end_element has no matching begin_element");
        }
        g_session.open_elements.pop_back();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_text(SturdyEngine engine,
                                            const char *text,
                                            const SturdyUiTextStyle *style) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (text == nullptr || style == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "text and style must not be null");
        }
        if (style->struct_size != sizeof(SturdyUiTextStyle)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                             "SturdyUiTextStyle size does not match this engine build");
        }

        SFT::UI::Context *context = nullptr;
        const SturdyResult session = require_session(&context);
        if (session != STURDY_OK) {
            return session;
        }

        SFT::UI::TextStyle engine_style{};
        engine_style.color = to_engine_color(style->color);
        engine_style.font_id = static_cast<SFT::UI::FontId>(style->font_id);
        engine_style.font_size = static_cast<SFT::u16>(style->font_size);
        context->text(SFT::ustr{std::string_view{text}}, engine_style);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_hovered(SturdyEngine engine,
                                               const char *id,
                                               SturdyBool *out_hovered) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (id == nullptr || out_hovered == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "id and output pointer must not be null");
        }
        SFT::UI::Context *context = nullptr;
        const SturdyResult session = require_session(&context);
        if (session != STURDY_OK) {
            return session;
        }
        *out_hovered = context->hovered(SFT::UString{id}) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_clicked(SturdyEngine engine,
                                               const char *id,
                                               SturdyBool *out_clicked) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (id == nullptr || out_clicked == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "id and output pointer must not be null");
        }
        SFT::UI::Context *context = nullptr;
        const SturdyResult session = require_session(&context);
        if (session != STURDY_OK) {
            return session;
        }
        *out_clicked = context->clicked(SFT::UString{id}) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_pointer_position(SturdyEngine engine,
                                                        float *out_x,
                                                        float *out_y) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        SFT::UI::Context *context = nullptr;
        const SturdyResult session = require_session(&context);
        if (session != STURDY_OK) {
            return session;
        }
        const glm::vec2 position = context->pointer_position();
        if (out_x != nullptr) {
            *out_x = position.x;
        }
        if (out_y != nullptr) {
            *out_y = position.y;
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_pointer_down(SturdyEngine engine, SturdyBool *out_down) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (out_down == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        SFT::UI::Context *context = nullptr;
        const SturdyResult session = require_session(&context);
        if (session != STURDY_OK) {
            return session;
        }
        *out_down = context->pointer_is_down() ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

} // extern "C"
