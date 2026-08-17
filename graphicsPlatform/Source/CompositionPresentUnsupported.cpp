#include "CompositionPresent.hpp"















#if !defined(STURDY_GRAPHICS_PLATFORM_WINDOWS)

namespace SFT::GraphicsPlatform {

    bool composition_present_compiled() noexcept {
        return false;
    }

    QueryMessage composition_present_available() noexcept {
        return QueryMessage{QueryStatus::Unsupported,
                            "Composition present is implemented only on Windows; this platform's graphics API is "
                            "expected to present transparent surfaces through its own swapchain."};
    }

    QueryResult<std::unique_ptr<CompositionPresenter>> create_composition_presenter(
        const CompositionPresenterDesc &         ) {
        return QueryResult<std::unique_ptr<CompositionPresenter>>{
            nullptr,
            QueryMessage{QueryStatus::Unsupported,
                         "Composition present is not implemented on this platform."},
        };
    }

} // namespace SFT::GraphicsPlatform

#endif // !STURDY_GRAPHICS_PLATFORM_WINDOWS
