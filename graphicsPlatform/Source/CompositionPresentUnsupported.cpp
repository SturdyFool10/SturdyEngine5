#include "CompositionPresent.hpp"

// Non-Windows fallback for the composition present path (CompositionPresent.hpp). Windows builds get
// the real implementation from Source/Windows/CompositionPresent.cpp and compile this file away
// entirely, so the two never define the same symbols.
//
// This is a whole-file guard rather than a per-OS stub in each Source/<Os> directory because there is
// exactly one Windows implementation and four platforms that need the identical "not available here"
// answer; four copies of the same three functions would be worse than one guard.
//
// Every other platform this engine targets can already do transparent windows through its native
// graphics API, so there is nothing to work around: Vulkan on Wayland and on X11 both routinely
// expose VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, and Metal composites CAMetalLayer alpha
// natively. If that ever stops being true for a platform, its equivalent belongs in
// Source/<Os>/CompositionPresent.cpp behind the same interface, and this guard shrinks accordingly.

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
        const CompositionPresenterDesc & /*desc*/) {
        return QueryResult<std::unique_ptr<CompositionPresenter>>{
            nullptr,
            QueryMessage{QueryStatus::Unsupported,
                         "Composition present is not implemented on this platform."},
        };
    }

} // namespace SFT::GraphicsPlatform

#endif // !STURDY_GRAPHICS_PLATFORM_WINDOWS
