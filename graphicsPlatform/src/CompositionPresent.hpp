#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "GraphicsPlatform.hpp"

// Composition presentation — an OS-compositor present path for graphics APIs whose own swapchain
// cannot produce a transparent window.
//
// Why this exists: per-pixel window transparency is a *presentation* capability, and on some
// platform/driver combinations the graphics API's native swapchain simply does not expose it. The
// canonical case is Vulkan on Win32, where VkSurfaceCapabilitiesKHR::supportedCompositeAlpha very
// commonly contains VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR and nothing else (every current AMD Windows
// driver, for one). A swapchain created that way silently discards whatever alpha the app renders
// and the window composites over black — there is no error and no fallback available *within* the
// graphics API. The only way out is to stop presenting through that API's WSI and hand the finished
// frame to the OS compositor instead.
//
// What this is NOT: a swapchain abstraction. The renderer keeps owning the swapchain's lifetime,
// its configuration (format, buffer count, size, sync interval), and how its images are used —
// exactly as it does for a native swapchain. This module only replaces the final "get these pixels
// onto the screen with their alpha intact" step. Every policy decision stays with the caller.
//
// Backend choice (Windows): DirectComposition *and* DXGI together, not one or the other. They are
// complementary layers rather than competing specs, and neither alone can do this:
//   - IDXGIFactory2::CreateSwapChainForHwnd is the ordinary path and is a dead end here. Its
//     DXGI_SWAP_CHAIN_DESC1::AlphaMode must be DXGI_ALPHA_MODE_UNSPECIFIED; HWND swapchains cannot
//     be transparent, which is the same wall Vulkan's WSI hits.
//   - IDXGIFactory2::CreateSwapChainForComposition creates an HWND-less swapchain that *does* honor
//     DXGI_ALPHA_MODE_PREMULTIPLIED, but it has no window to present to on its own.
//   - DirectComposition supplies the missing half: a visual tree bound to the HWND
//     (IDCompositionDesktopDevice::CreateTargetForHwnd) whose content is that composition swapchain.
// So the implementation uses DXGI for the transparent-capable swapchain and DirectComposition to
// attach it to the window. Within DirectComposition, DCompositionCreateDevice3 is the newest of the
// three device factories and is what this module asks for first. Windows.UI.Composition (the WinRT
// compositor) is newer still, but it is a WinRT API aimed at XAML/WinUI apps — it requires apartment
// initialization and drags in a dependency surface a native engine has no reason to take on, and it
// offers nothing DirectComposition lacks for presenting a swapchain to a plain HWND.
//
// Interop model: DXGI back buffers cannot be shared into another graphics API, so the presenter
// cannot simply hand its back buffers over. Instead it allocates its own ring of cross-API shared
// textures, publishes them as NT handles for the graphics API to import as native images, and blits
// the one the caller finished into the current back buffer before presenting. That costs one
// full-surface GPU copy per frame — the unavoidable price of leaving the native WSI, and the reason
// this path should only be taken when the native swapchain genuinely cannot do the job.

namespace SFT::GraphicsPlatform {

    // Pixel formats a composition swapchain can present with alpha intact. Deliberately a short list:
    // DXGI's composition path does not accept arbitrary formats, and the ones here are the ones that
    // both compose correctly and have an unambiguous equivalent in every graphics API that would use
    // this module.
    enum class CompositionFormat : std::uint32_t {
        // The DXGI-native 8-bit ordering and the safest default for SDR composition.
        Bgra8Unorm,
        Rgba8Unorm,
        // The right choice for HDR or wide-gamut composition: scRGB-style linear content, where the
        // extra range matters and 8-bit banding in a composited alpha edge would be visible.
        Rgba16Float,
        // Accepted by the compositor, but note the alpha channel is only 2 bits — four distinct
        // opacity levels. Fine for fully-opaque-or-fully-transparent content, poor for soft edges or
        // gradients. Prefer Rgba16Float when both 10-bit color and real alpha are wanted.
        Rgb10a2Unorm,
    };

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
        // Color channels are already scaled by alpha. This is what the compositor wants and what the
        // renderer's own transparent-overlay output convention already produces, so it is the mode
        // this module is built around.
        Premultiplied,
        // Straight (non-premultiplied) alpha. Supported for callers whose output convention differs;
        // costs the compositor a conversion.
        Straight,
        // Alpha present in the format but ignored at composition time — the window stays opaque.
        // Useful for A/B testing this path against the native swapchain without changing anything
        // else about how the frame was produced.
        Ignore,
    };

    // One cross-API shared render target owned by the presenter and imported by the graphics API.
    //
    // The graphics API imports `nt_handle` as one of its own images and renders into it exactly as it
    // would into a swapchain image. The handle is owned by the presenter and stays valid until the
    // presenter is destroyed or resized; importers must duplicate it if they need independent
    // lifetime, and must not close the original.
    struct CompositionSharedImage {
        // NT HANDLE to the shared texture. Vulkan imports this with
        // VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT via VK_KHR_external_memory_win32.
        void *nt_handle = nullptr;
        // Best-effort byte size of the underlying allocation. Provided for logging and sanity checks
        // only — importers must NOT use this as the authoritative allocation size. Vulkan's
        // VkMemoryAllocateInfo::allocationSize for an imported D3D11 texture has to come from
        // vkGetImageMemoryRequirements() on an image the importer created with matching properties,
        // because the driver's real allocation includes tiling and alignment this number cannot know.
        std::uint64_t allocation_size_bytes = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        CompositionFormat format = CompositionFormat::Bgra8Unorm;
    };

    // The two shared timeline fences that order work across the API boundary. Both are NT handles to
    // the same kind of object (a D3D fence on Windows), imported by Vulkan as timeline semaphores
    // with VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT via VK_KHR_external_semaphore_win32.
    //
    // Ownership of an image alternates between the two sides, and each fence carries one direction of
    // that handoff. Skipping either one races: the compositor would copy a half-rendered image, or
    // the renderer would overwrite an image the compositor is still reading.
    struct CompositionSharedFences {
        // Signaled by the graphics API when it has finished rendering into an image; waited on by the
        // presenter before it copies that image. Caller picks the values and passes the one it
        // signaled to present().
        void *render_complete_nt_handle = nullptr;
        // Signaled by the presenter once it has finished reading an image and the graphics API may
        // render into it again; waited on by the graphics API before reusing that image. Read the
        // value to wait for from acquire_next_image().
        void *present_complete_nt_handle = nullptr;
    };

    struct CompositionPresenterDesc {
        // The window to compose into. Must be a Win32 HWND on Windows; other window systems have no
        // implementation yet and are reported as Unsupported rather than assumed.
        NativeSurfaceHandle surface{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        // Back buffers in the composition swapchain. The caller's choice, clamped to what DXGI
        // accepts for a flip-model composition swapchain (2..16).
        std::uint32_t buffer_count = 2;
        // Shared images the graphics API renders into. Independent of buffer_count: this sizes the
        // caller's own in-flight ring, exactly as the caller's frames-in-flight count would for a
        // native swapchain. 0 means "match buffer_count".
        std::uint32_t shared_image_count = 0;
        CompositionFormat format = CompositionFormat::Bgra8Unorm;
        CompositionAlphaMode alpha_mode = CompositionAlphaMode::Premultiplied;
        // The adapter the *calling* graphics API's device lives on, as an opaque 64-bit LUID. This
        // matters and is not optional on any multi-adapter machine: cross-API texture sharing only
        // works between devices on the same physical adapter, so the presenter must build its interop
        // device on the caller's adapter rather than on whichever one DXGI enumerates first. Vulkan
        // callers pass VkPhysicalDeviceIDProperties::deviceLUID (8 bytes, copied into these bits) and
        // set use_adapter_luid whenever deviceLUIDValid is true.
        std::uint64_t adapter_luid = 0;
        // False means "no adapter preference" — the presenter uses the default adapter. Correct only
        // for single-adapter machines; prefer supplying a real LUID.
        bool use_adapter_luid = false;
    };

    // Which image to render into next, and the fence value proving the presenter is done with it.
    struct CompositionAcquisition {
        std::uint32_t image_index = 0;
        // Wait for this value on present_complete_nt_handle before writing to the image. 0 means the
        // image has never been presented and needs no wait.
        std::uint64_t wait_fence_value = 0;
    };

    // An OS-compositor present path bound to one window.
    //
    // Threading: not internally synchronized, in line with the graphics APIs it serves. Treat it the
    // way the owning renderer already treats a swapchain — one thread drives acquire/present for a
    // given presenter at a time.
    class CompositionPresenter {
      public:
        virtual ~CompositionPresenter() = default;

        CompositionPresenter(const CompositionPresenter &) = delete;
        CompositionPresenter &operator=(const CompositionPresenter &) = delete;

        // The shared images to import, in a stable order that image_index refers to. Invalidated by
        // resize(): the old handles are closed and the graphics API must re-import.
        [[nodiscard]] virtual std::span<const CompositionSharedImage> shared_images() const noexcept = 0;
        [[nodiscard]] virtual CompositionSharedFences shared_fences() const noexcept = 0;

        // Next image for the caller to render into, plus the fence value to wait on first.
        [[nodiscard]] virtual QueryResult<CompositionAcquisition> acquire_next_image() = 0;

        // Copies the finished image into the current back buffer and presents it to the compositor.
        // `render_complete_value` is the value the caller signaled on render_complete_nt_handle after
        // its last write to that image; the presenter waits for it on the GPU, so this does not block
        // the CPU. `sync_interval` matches DXGI's meaning — 0 for no vblank wait, 1 to present on
        // every vblank — and stays a per-present caller decision so the renderer's own present-mode
        // policy continues to apply on this path.
        [[nodiscard]] virtual QueryMessage present(std::uint32_t image_index,
                                                   std::uint64_t render_complete_value,
                                                   std::uint32_t sync_interval) = 0;

        // Rebuilds the swapchain and the shared-image ring at a new size. Invalidates every handle
        // previously returned by shared_images(); the caller must re-import them afterwards. The
        // fence handles survive a resize and do not need re-importing. Clears any live scale.
        [[nodiscard]] virtual QueryMessage resize(std::uint32_t width, std::uint32_t height) = 0;

        // Makes the already-presented content cover a `width` x `height` client area *without*
        // reallocating anything: a compositor-only rescale of the existing surface, no GPU work, no
        // fence wait, no handle invalidation. Cheap enough to call on every step of an interactive
        // resize.
        //
        // This exists because an OS compositor visual is not implicitly tied to its window's client
        // size the way a native HWND swapchain is. When the window manager resizes the frame, the
        // frame moves immediately while the next rendered frame is still in flight, and nothing
        // stretches the old content to cover the gap — the content visibly trails the border, which
        // reads as a "jelly" attachment. Rescaling here keeps the surface locked to the frame during
        // that interval, which is the same thing the desktop compositor does for opaque windows for
        // free.
        //
        // The tradeoff is deliberate: content is *scaled*, so it is momentarily soft and laid out for
        // the previous size. That is a stopgap for the frames between a size change and a real
        // resize(), not a substitute for one. Callers should still resize() at whatever cadence they
        // can sustain; each resize() re-establishes crisp, correctly laid out content and resets the
        // scale. Scaling is relative to the current backing size, so repeated calls do not compound.
        [[nodiscard]] virtual QueryMessage set_live_scale(std::uint32_t width, std::uint32_t height) = 0;

        [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
        [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;

      protected:
        CompositionPresenter() = default;
    };

    // Whether this build has a composition present implementation at all. Compile-time constant in
    // practice (true only on Windows builds), so callers can gate feature reporting on it without
    // constructing anything.
    [[nodiscard]] bool composition_present_compiled() noexcept;

    // Whether this *machine* can actually do it — the OS composition APIs are present and a
    // compositor is running. Distinct from composition_present_compiled(): a Windows build with DWM
    // composition disabled compiles the path in but cannot use it.
    [[nodiscard]] QueryMessage composition_present_available() noexcept;

    // Creates a presenter for one window. Returns Unsupported (never a crash or a silently opaque
    // presenter) on platforms or configurations that cannot do this, so callers can fall back to the
    // native swapchain path on the same decision they would make for any other unsupported feature.
    [[nodiscard]] QueryResult<std::unique_ptr<CompositionPresenter>> create_composition_presenter(
        const CompositionPresenterDesc &desc);

} // namespace SFT::GraphicsPlatform
