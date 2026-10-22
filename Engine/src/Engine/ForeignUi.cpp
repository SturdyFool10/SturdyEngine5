#include <Engine/ForeignUi.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <Engine/ScreenUi.hpp>
#include <Renderer/Text/Font.hpp>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] std::unexpected<ForeignUiError> error(ForeignUiErrorKind kind, std::string message) {
            Foundation::note_trace_origin();
            return std::unexpected(ForeignUiError{.kind = kind, .message = std::move(message)});
        }

        /// The UI frame being built on this thread, if any. Thread-local because a layout pass belongs to whichever thread builds it: windows
        /// render on their own threads, so a shared session would let two windows interleave their element stacks.
        struct UiSession {
            bool open = false;
            UI::Context *context = nullptr;
            glm::vec2 viewport{0.0f, 0.0f};
            /// Open element scopes, innermost last. Held by pointer because `ElementScope` closes its element on destruction and must not
            /// be moved or copied out from under the layout.
            std::vector<std::unique_ptr<UI::ElementScope>> open_elements;
        };

        thread_local UiSession t_session;

        /// The on-screen UI the bindings drive. The engine owns none; a binding is an application, so it keeps its own `ScreenUi` (window
        /// input + overlay) for the engine it was handed. One engine per process, as elsewhere in the foreign surfaces.
        struct ScreenUiSlot {
            Engine *engine = nullptr;
            std::unique_ptr<ScreenUi> ui;
        };
        ScreenUiSlot g_screen_ui;
        std::mutex g_screen_ui_mutex;

        [[nodiscard]] ScreenUi &screen_ui_for(Engine &engine) {
            const std::lock_guard<std::mutex> lock{g_screen_ui_mutex};
            if (g_screen_ui.engine != &engine || !g_screen_ui.ui) {
                g_screen_ui.ui.reset();
                g_screen_ui.engine = &engine;
                g_screen_ui.ui = std::make_unique<ScreenUi>(engine);
            }
            return *g_screen_ui.ui;
        }

        /// A font loaded here, kept with the id it was registered under: `Context::register_font` borrows the font, so the storage has to
        /// outlive registration. Kept for the process lifetime. The id is retained because a registration does not necessarily survive
        /// to the first frame (see `apply_font_registrations`).
        struct FontRegistration {
            UI::FontId font_id = 0;
            std::unique_ptr<Text::Font> font;
        };

        /// Shared rather than per-thread: a font may be registered during startup and used on a window's render thread.
        std::mutex g_font_mutex;
        std::vector<FontRegistration> g_fonts;

        /// Registers every font loaded here into `context`. `ScreenUi::ensure_ready` constructs a fresh `UI::Context` and move-assigns it
        /// over the old one, so a font registered before the UI renderer came up is discarded; the engine's own C++ callers register after
        /// `ensure_ready` from inside their frame callback. A foreign caller cannot be asked to know that ordering, so a registration is
        /// accepted at any time and the whole set is replayed once the context is live. Re-registering an id replaces its entry, so replaying
        /// is idempotent.
        void apply_font_registrations(UI::Context &context) {
            const std::lock_guard<std::mutex> lock{g_font_mutex};
            for (const FontRegistration &entry : g_fonts) {
                context.register_font(entry.font_id, *entry.font);
            }
        }

        [[nodiscard]] UI::Color to_color(const f32 (&rgba)[4]) noexcept {
            return UI::Color{static_cast<f64>(rgba[0]), static_cast<f64>(rgba[1]), static_cast<f64>(rgba[2]), static_cast<f64>(rgba[3])};
        }

        [[nodiscard]] ForeignUiResult<UI::Context *> require_session() {
            if (!t_session.open || t_session.context == nullptr) {
                return error(ForeignUiErrorKind::Busy, "no UI frame is open on this thread; begin one first");
            }
            return t_session.context;
        }

        /// Closes the session, discarding any elements left open. Destroyed innermost-first so each scope closes the element it opened, which
        /// is the order the layout expects even when the caller got the pairing wrong.
        void reset_session() noexcept {
            while (!t_session.open_elements.empty()) {
                t_session.open_elements.pop_back();
            }
            t_session.open = false;
            t_session.context = nullptr;
        }

    } // namespace

    ForeignUiResult<UI::SizingAxis> to_ui_axis(ForeignUiSizing kind, f32 value) {
        switch (kind) {
            case ForeignUiSizing::Fit: return UI::SizingAxis::fit();
            case ForeignUiSizing::Grow: return UI::SizingAxis::grow();
            case ForeignUiSizing::Fixed: return UI::SizingAxis::fixed(value);
            case ForeignUiSizing::Percent: return UI::SizingAxis::percent(value);
        }
        return error(ForeignUiErrorKind::InvalidArgument, "unrecognized sizing mode");
    }

    ForeignUiResult<void> register_foreign_ui_font(Engine &engine, std::string_view path, u32 font_id) {
        const std::optional<std::string> bytes = Foundation::read_file_to_string(std::string{path});
        if (!bytes) {
            return error(ForeignUiErrorKind::NotAvailable, "could not read the font file");
        }
        const std::span<const char> chars{bytes->data(), bytes->size()};
        auto loaded = Text::Font::load(std::as_bytes(chars));
        if (!loaded) {
            return error(ForeignUiErrorKind::NotAvailable, loaded.error().message.cpp_string());
        }

        // The context borrows the font, so ownership stays here for the process lifetime.
        const UI::FontId id = static_cast<UI::FontId>(font_id);
        auto owned = std::make_unique<Text::Font>(std::move(*loaded));
        {
            const std::lock_guard<std::mutex> lock{g_font_mutex};
            auto existing = std::find_if(g_fonts.begin(), g_fonts.end(), [id](const FontRegistration &entry) { return entry.font_id == id; });
            if (existing != g_fonts.end()) {
                existing->font = std::move(owned);
            } else {
                g_fonts.push_back(FontRegistration{.font_id = id, .font = std::move(owned)});
            }
        }
        // Also register into the live context so a font loaded mid-frame is usable immediately; `begin_foreign_ui_frame` replays the set for
        // the case where the context did not exist yet.
        apply_font_registrations(screen_ui_for(engine).context());
        return {};
    }

    ForeignUiResult<void> set_foreign_ui_scale_mode(Engine &engine, ForeignUiScaleMode mode) {
        UI::UiScaleMode engine_mode{};
        switch (mode) {
            case ForeignUiScaleMode::ContentScale: engine_mode = UI::UiScaleMode::ContentScale; break;
            case ForeignUiScaleMode::RawPixels: engine_mode = UI::UiScaleMode::RawPixels; break;
            default: return error(ForeignUiErrorKind::InvalidArgument, "unknown UI scale mode");
        }
        screen_ui_for(engine).set_scale_mode(engine_mode);
        return {};
    }

    ForeignUiResult<void> begin_foreign_ui_frame(Engine &engine, u32 framebuffer_width, u32 framebuffer_height, f64 delta_seconds) {
        if (t_session.open) {
            return error(ForeignUiErrorKind::Busy, "a UI frame is already open on this thread; end it first");
        }
        RHI::RhiDevice *device = engine.rhi_device();
        if (device == nullptr) {
            return error(ForeignUiErrorKind::NotAvailable, "the engine has no active RHI device");
        }
        // Matches the format the presentation path uses, so the overlay composites without a conversion the renderer would otherwise insert.
        const RHI::Format color_format =
            engine.config().features.presentation.hdr_enabled ? RHI::Format::RGBA16Float : RHI::Format::BGRA8UnormSrgb;
        ScreenUi &screen_ui = screen_ui_for(engine);
        if (!screen_ui.ensure_ready(color_format)) {
            return error(ForeignUiErrorKind::NotAvailable, "the UI renderer is not ready");
        }
        apply_font_registrations(screen_ui.context());

        const glm::vec2 viewport{static_cast<f32>(framebuffer_width), static_cast<f32>(framebuffer_height)};
        screen_ui.begin_frame(viewport, static_cast<f32>(delta_seconds));

        t_session.open = true;
        t_session.context = &screen_ui.context();
        t_session.viewport = viewport;
        return {};
    }

    ForeignUiResult<void> end_foreign_ui_frame(Engine &engine, RenderFrameParameters *frame) {
        auto context = require_session();
        if (!context) {
            return std::unexpected(context.error());
        }
        if (!t_session.open_elements.empty()) {
            const auto leaked = t_session.open_elements.size();
            reset_session();
            return error(ForeignUiErrorKind::InvalidArgument,
                         leaked == 1 ? "one element was left open; every begin_element needs an end_element"
                                     : "elements were left open; every begin_element needs an end_element");
        }
        auto snapshot = std::make_shared<UI::FrameSnapshot>((*context)->finish_frame(t_session.viewport));
        // A null frame means "discard": the layout is still finished, so the next frame starts clean, but nothing is attached.
        if (frame != nullptr) {
            frame->overlay_passes.push_back(screen_ui_for(engine).surface().overlay_for_snapshot(std::move(snapshot), engine.renderer()));
        }
        reset_session();
        return {};
    }

    ForeignUiResult<void> begin_foreign_ui_element(const ForeignUiElement &element) {
        auto context = require_session();
        if (!context) {
            return std::unexpected(context.error());
        }
        auto width = to_ui_axis(element.width_kind, element.width_value);
        auto height = to_ui_axis(element.height_kind, element.height_value);
        if (!width || !height) {
            return error(ForeignUiErrorKind::InvalidArgument, "unrecognized sizing mode");
        }
        UI::ElementDecl decl{};
        decl.sizing = UI::Sizing{*width, *height};
        decl.padding = UI::Padding{static_cast<u16>(element.padding[0]), static_cast<u16>(element.padding[1]),
                                   static_cast<u16>(element.padding[2]), static_cast<u16>(element.padding[3])};
        decl.child_gap = static_cast<u16>(element.child_gap);
        decl.direction = element.top_to_bottom ? UI::LayoutDirection::TopToBottom : UI::LayoutDirection::LeftToRight;
        decl.background_color = to_color(element.background);
        decl.corner_radius = UI::CornerRadius{element.corner_radius[0], element.corner_radius[1], element.corner_radius[2], element.corner_radius[3]};
        if (!element.id.empty()) {
            decl.id = UString{std::string_view{element.id}};
        }
        t_session.open_elements.push_back(std::make_unique<UI::ElementScope>((*context)->element(decl)));
        return {};
    }

    ForeignUiResult<void> end_foreign_ui_element() {
        auto context = require_session();
        if (!context) {
            return std::unexpected(context.error());
        }
        if (t_session.open_elements.empty()) {
            return error(ForeignUiErrorKind::InvalidArgument, "no element is open; end_element has no matching begin_element");
        }
        t_session.open_elements.pop_back();
        return {};
    }

    ForeignUiResult<void> foreign_ui_text(std::string_view text, const ForeignUiTextStyle &style) {
        auto context = require_session();
        if (!context) {
            return std::unexpected(context.error());
        }
        UI::TextStyle engine_style{};
        engine_style.color = to_color(style.color);
        engine_style.font_id = static_cast<UI::FontId>(style.font_id);
        engine_style.font_size = static_cast<u16>(style.font_size);
        (*context)->text(ustr{text}, engine_style);
        return {};
    }

    ForeignUiResult<bool> foreign_ui_hovered(std::string_view id) {
        auto context = require_session();
        if (!context) {
            return std::unexpected(context.error());
        }
        return (*context)->hovered(UString{id});
    }

    ForeignUiResult<bool> foreign_ui_clicked(std::string_view id) {
        auto context = require_session();
        if (!context) {
            return std::unexpected(context.error());
        }
        return (*context)->clicked(UString{id});
    }

    ForeignUiResult<glm::vec2> foreign_ui_pointer_position() {
        auto context = require_session();
        if (!context) {
            return std::unexpected(context.error());
        }
        return (*context)->pointer_position();
    }

    ForeignUiResult<bool> foreign_ui_pointer_down() {
        auto context = require_session();
        if (!context) {
            return std::unexpected(context.error());
        }
        return (*context)->pointer_is_down();
    }

    void discard_foreign_ui_frame() noexcept { reset_session(); }

    ScreenUi &foreign_screen_ui(Engine &engine) { return screen_ui_for(engine); }

    UI::Context *foreign_ui_context() noexcept { return t_session.open ? t_session.context : nullptr; }

    void release_foreign_ui() noexcept {
        const std::lock_guard<std::mutex> lock{g_screen_ui_mutex};
        g_screen_ui.ui.reset();
        g_screen_ui.engine = nullptr;
    }

} // namespace SFT::Engine
