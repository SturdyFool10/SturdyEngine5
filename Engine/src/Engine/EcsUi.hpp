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
        /// Bounds in the UI context's logical units (what `UI::Context::element_bounds` returns).
        UI::ElementBounds field_bounds;
        UI::ElementBounds caret_bounds;
        bool ime_enabled = true;
        /// Framebuffer pixels per logical unit: the context's `UI::Context::pixel_scale()`.
        f32 pixel_scale = 1.0f;
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
        /// Resolves an image file to the texture handle it is drawn through. Never blocks: the handle belongs to a
        /// progressive texture asset (`AssetManager::load_texture_progressive`), so it is valid at once, shows nothing
        /// until the first pass of the image is decoded, and then sharpens by itself.
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
        /// Resolves an SVG file to the texture handle it is drawn through. The handle belongs to a texture asset
        /// (`AssetManager::load_svg`) and never changes for a file: asking for a larger `target_px` re-rasterises the
        /// asset in place, so handles already handed out keep working and now show the sharper raster.
        ///
        /// @param assets Asset manager that owns the texture.
        /// @param path SVG file.
        /// @param target_px Largest dimension the SVG is drawn at, in pixels (the caller multiplies in any supersampling).
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::DecodeFailure`.
        [[nodiscard]] AssetExpected<Renderer::TextureHandle> resolve(
            AssetManager &assets, const std::filesystem::path &path, f32 target_px);


        /// [`resolve`] for SVG text held in memory (`AssetManager::load_svg_data`): nothing is cached on the filesystem.
        [[nodiscard]] AssetExpected<Renderer::TextureHandle> resolve_data(
            AssetManager &assets, std::string_view name, std::string_view svg, f32 target_px);

        /// Forgets what was asked for, without touching any texture.
        ///
        /// @note This function does not throw exceptions.
        void clear() noexcept;

        /// Starts a new sizing epoch: the next `resolve` of each file sets its raster to exactly the size then asked for
        /// (shrinking it if need be) instead of only ever growing. Call it when the sizes the UI draws icons at changed
        /// for good (a UI-scale or anti-aliasing setting). Nothing is unloaded and no handle becomes invalid.
        void clear_and_unload(AssetManager &assets) noexcept;

      private:
        struct Entry {
            Asset asset{};
            u64 epoch = 0;
        };
        std::unordered_map<std::string, Entry> by_path_;
        /// (path, requested size) -> handle, so a repeat request costs one lookup.
        std::unordered_map<std::string, Renderer::TextureHandle> by_key_;
        u64 epoch_ = 1;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::UiTextInputState, "sturdy.engine.ui_text_input_state");
SFT_ECS_RESOURCE(SFT::Engine::UiImageCache, "sturdy.engine.ui_image_cache");
SFT_ECS_RESOURCE(SFT::Engine::UiSvgCache, "sturdy.engine.ui_svg_cache");
