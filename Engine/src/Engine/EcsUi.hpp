#pragma once

#include <Foundation/Foundation.hpp>

#include <Engine/AssetManager.hpp>

#include <Async/Mutex.hpp>
#include <Core/Core.hpp>
#include <Ecs/Resource.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Renderer.hpp>
#include <Renderer/UI/UI.hpp>

#include <Engine/EcsEvents.hpp>
#include <Engine/WindowRequests.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>


namespace SFT::Engine {


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
        UI::UiInput input_{};
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
    void forward_text_input_state(WindowRequests &requests, WindowManager::WindowId window,
                                   std::optional<TextInputFocusInfo> focus) noexcept;


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

SFT_ECS_RESOURCE(SFT::Engine::UiTextInputState, "sturdy.engine.ui_text_input_state");
SFT_ECS_RESOURCE(SFT::Engine::UiImageCache, "sturdy.engine.ui_image_cache");
SFT_ECS_RESOURCE(SFT::Engine::UiSvgCache, "sturdy.engine.ui_svg_cache");
