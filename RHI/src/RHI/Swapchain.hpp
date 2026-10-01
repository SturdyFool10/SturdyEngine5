#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#pragma endregion

#include <RHI/Types.hpp>
#include <RHI/Handles.hpp>
#include <RHI/Resources.hpp>

namespace SFT::RHI {


    enum class WindowSystem : u32 {
        Unknown,
        Win32,
        Xlib,
        Xcb,
        Wayland,
        Cocoa,
        Android,
        UIKit,
        WebCanvas,
    };

    struct SurfaceDesc {
        WindowSystem system = WindowSystem::Unknown;
        void *display = nullptr;
        void *window = nullptr;
        const char *label = nullptr;
        /// CSS selector (e.g. "#canvas") identifying the HTML canvas element to present into.
        /// Only meaningful when `system == WindowSystem::WebCanvas`.
        const char *canvas_selector = nullptr;
    };


    enum class PresentMode : u32 {

        Fifo,


        FifoRelaxed,

        Mailbox,

        Immediate,


        FifoLatestReady,
    };


    /// Returns a human-readable name for the supplied present mode value.
    ///
    /// @param mode Mode controlling how the operation is performed.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr std::string_view present_mode_name(PresentMode mode) noexcept {
        switch (mode) {
            case PresentMode::Fifo: return "Fifo";
            case PresentMode::FifoRelaxed: return "FifoRelaxed";
            case PresentMode::Mailbox: return "Mailbox";
            case PresentMode::Immediate: return "Immediate";
            case PresentMode::FifoLatestReady: return "FifoLatestReady";
        }
        return "Unknown";
    }


    enum class PresentStrategy : u32 {

        Unsynchronized,


        TearFreeOrdered,


        TearFreeLatest,


        AdaptiveTearing,


        TearFreeLatestReady,


        VariableRefresh,
    };

    /// Returns a human-readable name for the supplied present strategy value.
    ///
    /// @param strategy `strategy` value used by the operation.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr std::string_view present_strategy_name(PresentStrategy strategy) noexcept {
        switch (strategy) {
            case PresentStrategy::Unsynchronized: return "Unsynchronized";
            case PresentStrategy::TearFreeOrdered: return "TearFreeOrdered";
            case PresentStrategy::TearFreeLatest: return "TearFreeLatest";
            case PresentStrategy::AdaptiveTearing: return "AdaptiveTearing";
            case PresentStrategy::TearFreeLatestReady: return "TearFreeLatestReady";
            case PresentStrategy::VariableRefresh: return "VariableRefresh";
        }
        return "Unknown";
    }


    /// Presents the completed frame to the target surface or swapchain.
    ///
    /// @param strategy `strategy` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr std::array<PresentMode, 4> present_mode_preference(PresentStrategy strategy) noexcept {
        switch (strategy) {
            case PresentStrategy::Unsynchronized:
                return {PresentMode::Immediate, PresentMode::Mailbox, PresentMode::FifoLatestReady, PresentMode::Fifo};
            case PresentStrategy::TearFreeOrdered:
                return {PresentMode::Fifo, PresentMode::Fifo, PresentMode::Fifo, PresentMode::Fifo};
            case PresentStrategy::TearFreeLatest:
                return {PresentMode::FifoLatestReady, PresentMode::Mailbox, PresentMode::FifoLatestReady, PresentMode::Fifo};
            case PresentStrategy::AdaptiveTearing:
                return {PresentMode::FifoRelaxed, PresentMode::FifoLatestReady, PresentMode::Mailbox, PresentMode::Fifo};
            case PresentStrategy::TearFreeLatestReady:
                return {PresentMode::FifoLatestReady, PresentMode::Mailbox, PresentMode::FifoLatestReady, PresentMode::Fifo};
            case PresentStrategy::VariableRefresh:
                // A G-Sync/FreeSync display paces itself: it stretches its own vblank to match whenever the
                // next frame becomes ready, which only happens if presentation isn't *also* forcing a fixed
                // cadence. Fifo (this case, before the fix below) blocks on the display's fixed refresh and
                // defeats the display's own variable one entirely -- indistinguishable from plain vsync-on.
                // The correct present mode is the same one Unsynchronized wants (tearing-capable, not
                // forcibly paced by the swapchain): the *app* self-paces to a target below the display's max
                // refresh (see the frame-rate-limit settings), and the display's own adaptive-sync hardware
                // does the rest. No tearing actually occurs while frame delivery stays inside the VRR window.
                return {PresentMode::Immediate, PresentMode::FifoRelaxed, PresentMode::Mailbox, PresentMode::Fifo};
        }
        return {PresentMode::Fifo, PresentMode::Fifo, PresentMode::Fifo, PresentMode::Fifo};
    }


    /// Selects present mode that best satisfies the supplied requirements.
    ///
    /// @param supported `supported` value used by the operation.
    /// @param strategy `strategy` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] PresentMode choose_present_mode(std::span<const PresentMode> supported,
                                                          PresentStrategy strategy) noexcept;

    enum class CompositeAlphaMode : u32 {

        Auto,
        Opaque,
        Premultiplied,
        PostMultiplied,
        Inherit,
    };


    /// Returns a human-readable name for the supplied composite alpha mode value.
    ///
    /// @param mode Mode controlling how the operation is performed.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr std::string_view composite_alpha_mode_name(CompositeAlphaMode mode) noexcept {
        switch (mode) {
            case CompositeAlphaMode::Auto: return "Auto";
            case CompositeAlphaMode::Opaque: return "Opaque";
            case CompositeAlphaMode::Premultiplied: return "Premultiplied";
            case CompositeAlphaMode::PostMultiplied: return "PostMultiplied";
            case CompositeAlphaMode::Inherit: return "Inherit";
        }
        return "Unknown";
    }


    /// Returns the current or globally available transparent composite alpha preference value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr std::array<CompositeAlphaMode, 3> transparent_composite_alpha_preference() noexcept {
        return {CompositeAlphaMode::Premultiplied, CompositeAlphaMode::PostMultiplied, CompositeAlphaMode::Inherit};
    }


    enum class ColorSpace : u32 {
        SrgbNonlinear,
        Hdr10St2084,


        ScrgbLinear,


        Hdr10Hlg,


        DolbyVision,


        AdobeRgbLinear,
        AdobeRgbNonlinear,
        DisplayP3Linear,
        DisplayP3Nonlinear,
        Bt2020Linear,
    };

    struct PresentationResolution {
        PresentStrategy strategy = PresentStrategy::TearFreeOrdered;
        PresentMode effective_mode = PresentMode::Fifo;
        bool degraded = false;


        bool present_queue_is_compute = false;


        CompositeAlphaMode effective_composite_alpha = CompositeAlphaMode::Opaque;


        bool composite_alpha_degraded = false;


        bool via_composition_present = false;


        bool supports_completion_fence = false;


        bool full_screen_exclusive_active = false;

        Format effective_format = Format::BGRA8UnormSrgb;
        ColorSpace effective_color_space = ColorSpace::SrgbNonlinear;
    };

    struct SwapchainDesc {
        SurfaceHandle surface{};
        u32 width = 0;
        u32 height = 0;
        Format format = Format::BGRA8UnormSrgb;
        ColorSpace color_space = ColorSpace::SrgbNonlinear;


        PresentStrategy present_strategy = PresentStrategy::TearFreeOrdered;


        TextureUsage usage = TextureUsage::ColorAttachment;
        CompositeAlphaMode composite_alpha = CompositeAlphaMode::Auto;
        bool clipped = true;


        u32 image_count = 0;


        u32 frames_in_flight = 0;


        SwapchainHandle old_swapchain{};


        bool allow_present_from_compute = true;


        bool request_full_screen_exclusive = false;
        const char *label = nullptr;
    };


    struct SurfaceTexture {
        SwapchainHandle swapchain{};
        TextureHandle texture{};
        TextureViewHandle view{};
        u32 image_index = 0;
        bool suboptimal = false;
        bool composition_present = false;
    };

    struct PresentDesc {
        SurfaceTexture texture{};


        FenceHandle completion_fence{};

        /// Identifies this present to the presentation engine, for `RhiDevice::wait_for_present` and
        /// `RhiDevice::drain_past_presentation_timings`. `0` = none. Must strictly increase per swapchain. Ignored
        /// unless `present_timing_capabilities(swapchain).present_id`.
        u64 present_id = 0;
        /// Ask for this present to reach the display no earlier than this `std::chrono::steady_clock` time, in
        /// nanoseconds since that clock's epoch. `0` = as soon as possible. Honoured only when
        /// `present_timing_capabilities(swapchain).target_display_time`.
        u64 target_display_time_ns = 0;
        /// Ask for the *previous* image to stay on screen at least this long before this one replaces it, in
        /// nanoseconds (e.g. `n * refresh` to show every frame for exactly `n` refreshes). `0` = no request.
        /// Honoured only when `present_timing_capabilities(swapchain).minimum_display_duration`.
        u64 minimum_display_duration_ns = 0;
        const char *label = nullptr;
    };

    /// What presentation-engine timing a swapchain supports (`VK_EXT_present_timing` / present ids / present wait
    /// on Vulkan, DXGI frame statistics on D3D12). Everything defaults to unsupported.
    struct PresentTimingCapabilities {
        /// `PresentDesc::present_id` reaches the presentation engine.
        bool present_id = false;
        /// `RhiDevice::wait_for_present` works.
        bool present_wait = false;
        /// `RhiDevice::drain_past_presentation_timings` reports when frames actually reached the display.
        bool display_timing_feedback = false;
        /// `PresentDesc::target_display_time_ns` is honoured.
        bool target_display_time = false;
        /// `PresentDesc::minimum_display_duration_ns` is honoured.
        bool minimum_display_duration = false;
    };

    /// The presentation engine's view of the display a swapchain presents to.
    struct SwapchainTiming {
        /// Refresh cycle in nanoseconds; the *minimum* cycle when `variable_refresh`. `0` = not known (yet — some
        /// platforms only learn it after the first present).
        u64 refresh_duration_ns = 0;
        /// The presentation engine reports variable-refresh (VRR) operation.
        bool variable_refresh = false;
        /// False when the presentation engine could not tell fixed from variable refresh.
        bool variable_refresh_known = false;
    };

    /// When one earlier present actually reached the display.
    struct PastPresentTiming {
        u64 present_id = 0;
        /// `std::chrono::steady_clock` time, in nanoseconds since that clock's epoch, at which the image's first
        /// pixel went out to the display. `0` = the presentation engine could not report it.
        u64 display_time_ns = 0;
    };


    enum class PresentOutcome {
        Success,
        Suboptimal,
        OutOfDate,
    };

} // namespace SFT::RHI
