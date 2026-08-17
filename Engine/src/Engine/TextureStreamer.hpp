#pragma once

#include "AssetManager.hpp"

#include <Renderer/Handles.hpp>
#include <RHI/RHI.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

namespace SFT::Renderer {
    class Renderer;
}

namespace SFT::Engine {

    enum class StreamingState : u8 { Pending, Uploading, Resident, Failed };


    struct StreamedTextureHandle {
        u64 id = 0;
        /// Converts the `StreamedTextureHandle` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept;
    };


    class TextureStreamer {
      public:
        /// Constructs a `TextureStreamer` from the supplied initialization values.
        ///
        /// @param renderer Renderer used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit TextureStreamer(SFT::Renderer::Renderer &renderer);
        /// Destroys the `TextureStreamer` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~TextureStreamer();

        /// Disables this construction form for `TextureStreamer`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        TextureStreamer(const TextureStreamer &) = delete;
        /// Assigns a new value to this `TextureStreamer`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        TextureStreamer &operator=(const TextureStreamer &) = delete;
        /// Disables this construction form for `TextureStreamer`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        TextureStreamer(TextureStreamer &&) = delete;
        /// Assigns a new value to this `TextureStreamer`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        TextureStreamer &operator=(TextureStreamer &&) = delete;


        /// Requests texture load using the supplied arguments and current state.
        ///
        /// @param source Source value or resource.
        /// @param color_space `color_space` value used by the operation.
        /// @param kind `kind` value used by the operation.
        /// @param placeholder `placeholder` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] StreamedTextureHandle request_texture_load(
            std::filesystem::path source, TextureColorSpace color_space = TextureColorSpace::Srgb,
            TextureKind kind = TextureKind::ColorAlpha,
            RHI::ClearColor placeholder = RHI::ClearColor{1.0f, 0.0f, 1.0f, 1.0f}, UString label = {});

        /// Performs the state operation for `TextureStreamer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] StreamingState state(StreamedTextureHandle handle) const noexcept;
        /// Reports whether resident holds for this `TextureStreamer`.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_resident(StreamedTextureHandle handle) const noexcept;


        /// Returns the texture handle associated with this `TextureStreamer`.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SFT::Renderer::TextureHandle texture_handle(StreamedTextureHandle handle) const noexcept;


        /// Performs the dimensions operation for `TextureStreamer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::pair<u32, u32> dimensions(StreamedTextureHandle handle) const noexcept;


        /// Waits for the associated operation or synchronization primitive to complete.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void wait(StreamedTextureHandle handle) noexcept;


        /// Handles the resident event.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param callback Callable invoked by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void on_resident(StreamedTextureHandle handle, std::function<void(SFT::Renderer::TextureHandle)> callback);


        /// Pumps the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void pump();

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace SFT::Engine
