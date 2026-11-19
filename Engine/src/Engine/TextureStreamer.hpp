#pragma once

#include <Engine/AssetManager.hpp>

#include <Async/Task.hpp>

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


    /// A streamed texture after every CPU step: decoded, mip chain built, block-compressed when the device takes it. Produced
    /// off the calling thread by `TextureStreamer::prepare_texture_load`; `request_prepared_texture` is the cheap renderer-side half.
    struct PreparedStreamedTexture {
        u32 width = 0;
        u32 height = 0;
        RHI::Format format = RHI::Format::Undefined;
        u32 mip_levels = 1;
        std::vector<std::byte> pixels;
        std::filesystem::path source;
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
        /// The CPU half of `request_texture_load`, run on the engine's async runtime: the file is read with the platform's fast path
        /// (io_uring / DirectStorage / overlapped reads), then decoded, mipped and compressed as a heavy scheduler task. Returns at once;
        /// start as many as there are textures and they proceed in parallel. Call from the thread that owns the renderer so the device's
        /// block-compression support is read there.
        [[nodiscard]] Async::TaskHandle<AssetExpected<PreparedStreamedTexture>> prepare_texture_load(
            std::filesystem::path source, TextureColorSpace color_space = TextureColorSpace::Srgb,
            TextureKind kind = TextureKind::ColorAlpha);

        /// The renderer-side half: creates the GPU texture with its placeholder and queues the upload. Cheap; call it on the
        /// renderer's thread when a `prepare_texture_load` task finishes.
        [[nodiscard]] StreamedTextureHandle request_prepared_texture(
            PreparedStreamedTexture prepared, RHI::ClearColor placeholder = RHI::ClearColor{1.0f, 0.0f, 1.0f, 1.0f}, UString label = {});

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
