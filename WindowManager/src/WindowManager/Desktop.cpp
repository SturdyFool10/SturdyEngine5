#include <WindowManager/Desktop.hpp>

#include <SDL3/SDL.h>

#include <atomic>
#include <memory>

#include <WindowManager/Window.hpp>
#include <WindowManager/WindowLog.hpp>

using std::expected;
using std::unexpected;

namespace SFT::WindowManager::Desktop {

    namespace {

        [[nodiscard]] WindowError sdl_failure(WindowErrorCode code, const char *fallback) noexcept {
            const char *message = SDL_GetError();
            return WindowError{code, message != nullptr && message[0] != '\0' ? message : fallback};
        }

        /// The SDL window behind a parent, when the parent is an SDL3 window; null otherwise.
        [[nodiscard]] SDL_Window *sdl_parent(Window *parent) noexcept {
            if (parent == nullptr || parent->backend_kind() != WindowBackendKind::SDL3) {
                return nullptr;
            }
            const auto handle = parent->native_backend_handle();
            return handle ? static_cast<SDL_Window *>(*handle) : nullptr;
        }

        /// Everything SDL reads asynchronously must outlive the call, so one heap block carries it until the dialog
        /// reports back.
        struct FileDialogContext {
            FileDialogOptions options;
            std::vector<SDL_DialogFileFilter> sdl_filters;
            FileDialogCallback callback;
        };

        void SDLCALL file_dialog_done(void *userdata, const char *const *filelist, int filter) {
            std::unique_ptr<FileDialogContext> context{static_cast<FileDialogContext *>(userdata)};
            FileDialogResult result;
            if (filelist == nullptr) {
                result.failed = true;
                result.error = SDL_GetError();
            } else if (filelist[0] == nullptr) {
                result.cancelled = true;
            } else {
                for (int i = 0; filelist[i] != nullptr; ++i) {
                    result.paths.emplace_back(filelist[i]);
                }
            }
            result.filter_index = filter;
            if (context->callback) {
                context->callback(std::move(result));
            }
        }

        // SDL's video subsystem is what the screen saver switch lives in; a GLFW-only program has not started it.
        [[nodiscard]] bool ensure_sdl_video() noexcept {
            if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
                return true;
            }
#if defined(__APPLE__)
            // A second Cocoa application bring-up next to GLFW's is not safe to attempt.
            return false;
#else
            return SDL_InitSubSystem(SDL_INIT_VIDEO);
#endif
        }

    } // namespace

    expected<void, WindowError> show_file_dialog(FileDialogOptions options, FileDialogCallback callback) noexcept {
        try {
            auto context = std::make_unique<FileDialogContext>();
            context->options = std::move(options);
            context->callback = std::move(callback);
            const FileDialogOptions &opts = context->options;
            for (const FileFilter &filter : opts.filters) {
                context->sdl_filters.push_back(SDL_DialogFileFilter{filter.name.c_str(), filter.pattern.c_str()});
            }

            const SDL_PropertiesID properties = SDL_CreateProperties();
            if (properties == 0) {
                return unexpected(sdl_failure(WindowErrorCode::OperationFailed, "SDL3 could not create dialog properties."));
            }
            if (!context->sdl_filters.empty()) {
                SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, context->sdl_filters.data());
                SDL_SetNumberProperty(properties, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, static_cast<Sint64>(context->sdl_filters.size()));
            }
            if (SDL_Window *parent = sdl_parent(opts.parent)) {
                SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, parent);
            }
            if (!opts.default_location.empty()) {
                SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING, opts.default_location.c_str());
            }
            SDL_SetBooleanProperty(properties, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, opts.allow_multiple);
            if (!opts.title.empty()) {
                SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, opts.title.c_str());
            }
            if (!opts.accept_label.empty()) {
                SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, opts.accept_label.c_str());
            }
            if (!opts.cancel_label.empty()) {
                SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_CANCEL_STRING, opts.cancel_label.c_str());
            }
            const SDL_FileDialogType type = opts.kind == FileDialogKind::SaveFile     ? SDL_FILEDIALOG_SAVEFILE
                                            : opts.kind == FileDialogKind::OpenFolder ? SDL_FILEDIALOG_OPENFOLDER
                                                                                      : SDL_FILEDIALOG_OPENFILE;
            // Ownership of the context passes to SDL; `file_dialog_done` frees it exactly once.
            SDL_ShowFileDialogWithProperties(type, file_dialog_done, context.release(), properties);
            SDL_DestroyProperties(properties);
            return {};
        } catch (...) {
            return unexpected(WindowError{WindowErrorCode::OutOfMemory, "Out of memory opening the file dialog."});
        }
    }

    expected<int, WindowError> show_message_box(const MessageBoxOptions &options) noexcept {
        try {
            std::vector<SDL_MessageBoxButtonData> buttons;
            if (options.buttons.empty()) {
                buttons.push_back(SDL_MessageBoxButtonData{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "OK"});
            } else {
                for (std::size_t i = 0; i < options.buttons.size(); ++i) {
                    SDL_MessageBoxButtonFlags flags = 0;
                    if (static_cast<int>(i) == options.default_button) {
                        flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
                    }
                    if (static_cast<int>(i) == options.escape_button) {
                        flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
                    }
                    buttons.push_back(SDL_MessageBoxButtonData{flags, static_cast<int>(i), options.buttons[i].c_str()});
                }
            }
            SDL_MessageBoxData data{};
            data.flags = options.kind == MessageBoxKind::Error     ? SDL_MESSAGEBOX_ERROR
                         : options.kind == MessageBoxKind::Warning ? SDL_MESSAGEBOX_WARNING
                                                                   : SDL_MESSAGEBOX_INFORMATION;
            data.window = sdl_parent(options.parent);
            data.title = options.title.c_str();
            data.message = options.message.c_str();
            data.numbuttons = static_cast<int>(buttons.size());
            data.buttons = buttons.data();
            int pressed = -1;
            if (!SDL_ShowMessageBox(&data, &pressed)) {
                return unexpected(sdl_failure(WindowErrorCode::Unsupported, "The system could not show a message box."));
            }
            return pressed;
        } catch (...) {
            return unexpected(WindowError{WindowErrorCode::OutOfMemory, "Out of memory showing the message box."});
        }
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Tray
    // -----------------------------------------------------------------------------------------------------------------

    namespace {
        std::atomic<int> &live_trays() noexcept {
            static std::atomic<int> count{0};
            return count;
        }

        /// What an SDL tray entry's callback needs: the user's handler and, for check boxes, the entry to read back.
        struct EntryBox {
            std::function<void()> on_click;
        };

        void SDLCALL tray_entry_clicked(void *userdata, SDL_TrayEntry *) {
            const auto *box = static_cast<const EntryBox *>(userdata);
            if (box != nullptr && box->on_click) {
                box->on_click();
            }
        }

        [[nodiscard]] SDL_Surface *icon_surface(u32 width, u32 height, std::span<const std::byte> rgba) noexcept {
            if (width == 0 || height == 0 || rgba.size() < static_cast<usize>(width) * height * 4) {
                return nullptr;
            }
            return SDL_CreateSurfaceFrom(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGBA32,
                                         const_cast<std::byte *>(rgba.data()), static_cast<int>(width * 4));
        }
    } // namespace

    struct Tray::Impl {
        SDL_Tray *tray = nullptr;
        SDL_TrayMenu *menu = nullptr;
        std::vector<std::unique_ptr<EntryBox>> boxes;

        void build(SDL_TrayMenu *target, const std::vector<TrayEntry> &entries) {
            for (const TrayEntry &entry : entries) {
                if (entry.separator) {
                    SDL_InsertTrayEntryAt(target, -1, nullptr, SDL_TRAYENTRY_BUTTON);
                    continue;
                }
                SDL_TrayEntryFlags flags = !entry.submenu.empty() ? SDL_TRAYENTRY_SUBMENU
                                           : entry.checkable      ? SDL_TRAYENTRY_CHECKBOX
                                                                  : SDL_TRAYENTRY_BUTTON;
                if (!entry.enabled) {
                    flags |= SDL_TRAYENTRY_DISABLED;
                }
                if (entry.checkable && entry.checked) {
                    flags |= SDL_TRAYENTRY_CHECKED;
                }
                SDL_TrayEntry *created = SDL_InsertTrayEntryAt(target, -1, entry.label.c_str(), flags);
                if (created == nullptr) {
                    continue;
                }
                if (!entry.submenu.empty()) {
                    if (SDL_TrayMenu *sub = SDL_CreateTraySubmenu(created)) {
                        build(sub, entry.submenu);
                    }
                } else {
                    boxes.push_back(std::make_unique<EntryBox>(EntryBox{entry.on_click}));
                    SDL_SetTrayEntryCallback(created, tray_entry_clicked, boxes.back().get());
                }
            }
        }

        void clear_menu() {
            if (menu == nullptr) {
                return;
            }
            int count = 0;
            if (const SDL_TrayEntry **entries = SDL_GetTrayEntries(menu, &count)) {
                // Removing shifts the array, so the entries are taken from a copy of the pointers.
                std::vector<SDL_TrayEntry *> copy;
                copy.reserve(static_cast<usize>(count));
                for (int i = 0; i < count; ++i) {
                    copy.push_back(const_cast<SDL_TrayEntry *>(entries[i]));
                }
                for (SDL_TrayEntry *entry : copy) {
                    SDL_RemoveTrayEntry(entry);
                }
            }
            boxes.clear();
        }
    };

    Tray::Tray(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    Tray::~Tray() {
        if (impl_ != nullptr && impl_->tray != nullptr) {
            SDL_DestroyTray(impl_->tray);
            live_trays().fetch_sub(1, std::memory_order_relaxed);
        }
    }

    expected<std::unique_ptr<Tray>, WindowError> Tray::create(u32 width, u32 height, std::span<const std::byte> icon_rgba, std::string tooltip,
                                                              std::vector<TrayEntry> entries) noexcept {
        try {
            if (!ensure_sdl_video()) {
                return unexpected(WindowError{WindowErrorCode::Unsupported, "A tray icon needs an SDL window system connection."});
            }
            SDL_Surface *surface = icon_surface(width, height, icon_rgba);
            if (surface == nullptr) {
                return unexpected(WindowError{WindowErrorCode::InvalidArgument, "A tray icon needs width * height * 4 bytes of RGBA8."});
            }
            auto impl = std::make_unique<Impl>();
            impl->tray = SDL_CreateTray(surface, tooltip.c_str()); // SDL copies the icon
            SDL_DestroySurface(surface);
            if (impl->tray == nullptr) {
                return unexpected(sdl_failure(WindowErrorCode::Unsupported, "This system has no tray to put an icon in."));
            }
            live_trays().fetch_add(1, std::memory_order_relaxed);
            impl->menu = SDL_CreateTrayMenu(impl->tray);
            if (impl->menu != nullptr) {
                impl->build(impl->menu, entries);
            }
            return std::unique_ptr<Tray>{new Tray(std::move(impl))};
        } catch (...) {
            return unexpected(WindowError{WindowErrorCode::OutOfMemory, "Out of memory creating the tray icon."});
        }
    }

    expected<void, WindowError> Tray::set_icon(u32 width, u32 height, std::span<const std::byte> icon_rgba) noexcept {
        SDL_Surface *surface = icon_surface(width, height, icon_rgba);
        if (surface == nullptr) {
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "A tray icon needs width * height * 4 bytes of RGBA8."});
        }
        SDL_SetTrayIcon(impl_->tray, surface);
        SDL_DestroySurface(surface);
        return {};
    }

    void Tray::set_tooltip(const std::string &tooltip) noexcept { SDL_SetTrayTooltip(impl_->tray, tooltip.c_str()); }

    expected<void, WindowError> Tray::set_menu(std::vector<TrayEntry> entries) noexcept {
        try {
            impl_->clear_menu();
            if (impl_->menu == nullptr) {
                impl_->menu = SDL_CreateTrayMenu(impl_->tray);
            }
            if (impl_->menu == nullptr) {
                return unexpected(sdl_failure(WindowErrorCode::Unsupported, "This tray has no menu."));
            }
            impl_->build(impl_->menu, entries);
            return {};
        } catch (...) {
            return unexpected(WindowError{WindowErrorCode::OutOfMemory, "Out of memory rebuilding the tray menu."});
        }
    }

    void pump_events() noexcept {
        if (live_trays().load(std::memory_order_relaxed) == 0) {
            return;
        }
        SDL_PumpEvents();
        SDL_UpdateTrays();
    }

    expected<void, WindowError> set_screensaver_enabled(bool enabled) noexcept {
        if (!ensure_sdl_video()) {
            return unexpected(WindowError{WindowErrorCode::Unsupported, "The screen saver cannot be reached without an SDL window system connection."});
        }
        if (enabled) {
            SDL_EnableScreenSaver();
        } else {
            SDL_DisableScreenSaver();
        }
        return {};
    }

    expected<bool, WindowError> screensaver_enabled() noexcept {
        if (!ensure_sdl_video()) {
            return unexpected(WindowError{WindowErrorCode::Unsupported, "The screen saver cannot be reached without an SDL window system connection."});
        }
        return SDL_ScreenSaverEnabled();
    }

} // namespace SFT::WindowManager::Desktop
