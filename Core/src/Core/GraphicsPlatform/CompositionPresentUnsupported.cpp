#include <Core/GraphicsPlatform/CompositionPresent.hpp>


#if !defined(STURDY_GRAPHICS_PLATFORM_WINDOWS)

namespace SFT::Core::GraphicsPlatform {

    /// Returns the current or globally available composition present compiled value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool composition_present_compiled() noexcept {
        return false;
    }

    /// Returns the current or globally available composition present available value.
    ///
    /// @return Returns the current composition present available value.
    /// @note This function does not throw exceptions.
    QueryMessage composition_present_available() noexcept {
        return QueryMessage{QueryStatus::Unsupported,
                            "Composition present is implemented only on Windows; this platform's graphics API is "
                            "expected to present transparent surfaces through its own swapchain."};
    }

    /// Creates a composition presenter from the supplied parameters.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `QueryStatus::Unsupported`.
    QueryResult<std::unique_ptr<CompositionPresenter>> create_composition_presenter(
        const CompositionPresenterDesc &         ) {
        return QueryResult<std::unique_ptr<CompositionPresenter>>{
            nullptr,
            QueryMessage{QueryStatus::Unsupported,
                         "Composition present is not implemented on this platform."},
        };
    }

} // namespace SFT::Core::GraphicsPlatform

#endif // !STURDY_GRAPHICS_PLATFORM_WINDOWS
