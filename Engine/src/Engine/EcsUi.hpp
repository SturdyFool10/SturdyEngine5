#pragma once

#include <Foundation/src/Foundation.hpp>

#include "AssetManager.hpp"

#include <Async/src/Mutex.hpp>
#include <Core/Core.hpp>
#include <Ecs/src/Resource.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Renderer.hpp>
#include <UI/UI.hpp>

#include "EcsEvents.hpp"
#include "WindowRequests.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>


namespace SFT::Engine {


    class UiPointerState {
      public:
        /// Sets the position for this `UiPointerState`.
        ///
        /// @param position `position` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_position(glm::vec2 position) noexcept;
        /// Sets the down for this `UiPointerState`.
        ///
        /// @param down `down` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_down(bool down) noexcept;
        /// Cancels the outstanding operation when cancellation is still possible.
        ///
        /// @note This function does not throw exceptions.
        void cancel() noexcept;
        /// Clears transitions.
        ///
        /// @note This function does not throw exceptions.
        void clear_transitions() noexcept;


        /// Adds scroll delta using the supplied arguments and current state.
        ///
        /// @param delta `delta` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void add_scroll_delta(glm::vec2 delta) noexcept;
        /// Clears scroll delta.
        ///
        /// @note This function does not throw exceptions.
        void clear_scroll_delta() noexcept;
        /// Returns the current or globally available state value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const UI::PointerState &state() const noexcept;


        /// Sets the consumed for this `UiPointerState`.
        ///
        /// @param consumed `consumed` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_consumed(bool consumed) noexcept;
        /// Returns the current or globally available consumed value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool consumed() const noexcept;

      private:
        UI::PointerState state_{};
        bool consumed_ = false;
    };


    class UiTextInputState {
      public:


        /// Applies the supplied operation or state to `UiTextInputState`.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void apply(const TextInputEvent &event) noexcept;


        /// Applies the supplied operation or state to `UiTextInputState`.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void apply(const TextEditingEvent &event) noexcept;


        /// Applies key using the supplied arguments and current state.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void apply_key(const KeyboardEvent &event) noexcept;


        /// Performs the frame input operation for `UiTextInputState` using the supplied arguments.
        ///
        /// @param get_clipboard_text `get_clipboard_text` value used by the operation.
        /// @param set_clipboard_text `set_clipboard_text` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] UI::TextEditInput frame_input(
            std::function<UString()> get_clipboard_text = nullptr,
            std::function<void(const UString &)> set_clipboard_text = nullptr) const noexcept;


        /// Clears transitions.
        ///
        /// @note This function does not throw exceptions.
        void clear_transitions() noexcept;

      private:
        std::string typed_text_;
        std::string composition_text_;
        bool composing_ = false;
        vector<UI::EditKey> keys_;
        bool shift_down_ = false;
        bool ctrl_down_ = false;
    };


    struct TextInputFocusInfo {
        UI::ElementBounds field_bounds;
        UI::ElementBounds caret_bounds;
        bool ime_enabled = true;
    };


    /// Performs the forward text input state operation using the supplied arguments.
    ///
    /// @param requests `requests` value used by the operation.
    /// @param window Window used or affected by the operation.
    /// @param focus `focus` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void forward_text_input_state(WindowRequests &requests, Platform::Windowing::WindowId window,
                                   std::optional<TextInputFocusInfo> focus) noexcept;


    class UiContext {
        struct UiRendererState {
            Async::Mutex<std::optional<UI::UiRenderer>> renderer;
        };

      public:


        /// Finds or creates the ready required by the operation.
        ///
        /// @param device Device used or affected by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool ensure_ready(RHI::RhiDevice &device, RHI::Format color_format);

        /// Reads the requested data from the associated source.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool ready() const;
        /// Returns the current or globally available context value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] UI::Context &context() noexcept;


        /// Performs the begin layout operation for `UiContext` using the supplied arguments.
        ///
        /// @param viewport_size Requested or available size for the operation.
        /// @param pointer_state `pointer_state` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void begin_layout(glm::vec2 viewport_size, UiPointerState &pointer_state, f32 delta_seconds);


        /// Builds overlay hooks.
        ///
        /// @param snapshot `snapshot` value used by the operation.
        /// @param texture_resolver Texture used or affected by the operation.
        ///
        /// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Renderer::UiOverlayHooks build_overlay_hooks(std::shared_ptr<UI::FrameSnapshot> snapshot,
                                                                    Renderer::Renderer *texture_resolver);

        /// Destroys or releases the `UiContext` resource represented by the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        UI::Context context_{};
        std::shared_ptr<UiRendererState> renderer_state_ = std::make_shared<UiRendererState>();
        bool create_attempted_ = false;
    };


    class UiImageCache {
      public:
        /// Resolves the requested value into the concrete value used by the engine.
        ///
        /// @param assets `assets` value used by the operation.
        /// @param path Filesystem path identifying the target resource.
        /// @param color_space `color_space` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<Renderer::TextureHandle> resolve(
            AssetManager &assets, const std::filesystem::path &path,
            TextureColorSpace color_space = TextureColorSpace::Srgb);


        /// Clears the stored state or contents.
        ///
        /// @note This function does not throw exceptions.
        void clear() noexcept;

      private:
        struct Entry {
            Asset asset{};
            Renderer::TextureHandle handle{};
        };
        std::unordered_map<std::string, Entry> by_key_;
    };


    class UiSvgCache {
      public:
        /// Resolves the requested value into the concrete value used by the engine.
        ///
        /// @param assets `assets` value used by the operation.
        /// @param path Filesystem path identifying the target resource.
        /// @param target_px `target_px` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::DecodeFailure`.
        [[nodiscard]] AssetExpected<Renderer::TextureHandle> resolve(
            AssetManager &assets, const std::filesystem::path &path, f32 target_px);


        /// Clears the stored state or contents.
        ///
        /// @note This function does not throw exceptions.
        void clear() noexcept;

      private:
        std::unordered_map<std::string, Renderer::TextureHandle> by_key_;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::UiPointerState, "sturdy.engine.ui_pointer_state");
SFT_ECS_RESOURCE(SFT::Engine::UiTextInputState, "sturdy.engine.ui_text_input_state");
SFT_ECS_RESOURCE(SFT::Engine::UiContext, "sturdy.engine.ui_context");
SFT_ECS_RESOURCE(SFT::Engine::UiImageCache, "sturdy.engine.ui_image_cache");
SFT_ECS_RESOURCE(SFT::Engine::UiSvgCache, "sturdy.engine.ui_svg_cache");
