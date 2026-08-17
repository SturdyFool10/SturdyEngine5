#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "GraphicsPlatform.hpp"


namespace SFT::GraphicsPlatform {


    enum class CompositionFormat : std::uint32_t {

        Bgra8Unorm,
        Rgba8Unorm,


        Rgba16Float,


        Rgb10a2Unorm,
    };

    /// Returns a human-readable name for the supplied composition format value.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr std::string_view composition_format_name(CompositionFormat format) noexcept {
        switch (format) {
            case CompositionFormat::Bgra8Unorm: return "Bgra8Unorm";
            case CompositionFormat::Rgba8Unorm: return "Rgba8Unorm";
            case CompositionFormat::Rgba16Float: return "Rgba16Float";
            case CompositionFormat::Rgb10a2Unorm: return "Rgb10a2Unorm";
        }
        return "Unknown";
    }

    enum class CompositionAlphaMode : std::uint32_t {


        Premultiplied,


        Straight,


        Ignore,
    };


    struct CompositionSharedImage {


        void *nt_handle = nullptr;


        std::uint64_t allocation_size_bytes = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        CompositionFormat format = CompositionFormat::Bgra8Unorm;
    };


    struct CompositionSharedFences {


        void *render_complete_nt_handle = nullptr;


        void *present_complete_nt_handle = nullptr;
    };

    struct CompositionPresenterDesc {


        NativeSurfaceHandle surface{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;


        std::uint32_t buffer_count = 2;


        std::uint32_t shared_image_count = 0;
        CompositionFormat format = CompositionFormat::Bgra8Unorm;
        CompositionAlphaMode alpha_mode = CompositionAlphaMode::Premultiplied;


        std::uint64_t adapter_luid = 0;


        bool use_adapter_luid = false;
    };


    struct CompositionAcquisition {
        std::uint32_t image_index = 0;


        std::uint64_t wait_fence_value = 0;
    };


    class CompositionPresenter {
      public:
        /// Destroys the `CompositionPresenter` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~CompositionPresenter() = default;

        /// Disables this construction form for `CompositionPresenter`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        CompositionPresenter(const CompositionPresenter &) = delete;
        /// Assigns a new value to this `CompositionPresenter`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        CompositionPresenter &operator=(const CompositionPresenter &) = delete;


        /// Returns the current or globally available shared images value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual std::span<const CompositionSharedImage> shared_images() const noexcept = 0;
        /// Returns the current or globally available shared fences value.
        ///
        /// @return Returns the current shared fences value.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual CompositionSharedFences shared_fences() const noexcept = 0;


        /// Acquires next image.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual QueryResult<CompositionAcquisition> acquire_next_image() = 0;


        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param image_index Zero-based index of the target element or entry.
        /// @param render_complete_value Value consumed by the operation.
        /// @param sync_interval `sync_interval` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] virtual QueryMessage present(std::uint32_t image_index,
                                                   std::uint64_t render_complete_value,
                                                   std::uint32_t sync_interval) = 0;


        /// Changes the logical size to the requested value, creating or removing elements as needed.
        ///
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        ///
        /// @return Returns the value produced by the operation.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] virtual QueryMessage resize(std::uint32_t width, std::uint32_t height) = 0;


        /// Sets the live scale for this `CompositionPresenter`.
        ///
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        ///
        /// @return Returns the value produced by the operation.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] virtual QueryMessage set_live_scale(std::uint32_t width, std::uint32_t height) = 0;

        /// Returns the current or globally available width value.
        ///
        /// @return Returns the current width value.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
        /// Returns the current or globally available height value.
        ///
        /// @return Returns the current height value.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;

      protected:
        /// Constructs a `CompositionPresenter` in its default state.
        ///
        /// @note This function does not throw exceptions.
        CompositionPresenter() = default;
    };


    /// Returns the current or globally available composition present compiled value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool composition_present_compiled() noexcept;


    /// Returns the current or globally available composition present available value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] QueryMessage composition_present_available() noexcept;


    /// Creates a composition presenter from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] QueryResult<std::unique_ptr<CompositionPresenter>> create_composition_presenter(
        const CompositionPresenterDesc &desc);

} // namespace SFT::GraphicsPlatform
