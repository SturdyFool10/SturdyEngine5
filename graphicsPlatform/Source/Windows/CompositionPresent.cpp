#include "CompositionPresent.hpp"

#if defined(STURDY_GRAPHICS_PLATFORM_WINDOWS)

#pragma region Imports
#include <windows.h>

#include <d3d11_4.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#pragma endregion

// IID_PPV_ARGS expands to __uuidof, which clang correctly reports as a Microsoft language extension.
// It is also the only way to obtain an interface's IID from the Windows SDK headers, so every COM
// call below would otherwise carry the same warning. Suppressed once for the file rather than a
// dozen times at the call sites; __uuidof is the sole extension token this translation unit uses, so
// the broad scope costs no real coverage. Guarded because the engine also builds under GCC, which
// would warn on the unknown pragma itself.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#endif

// Windows composition present path — see CompositionPresent.hpp for why this exists, why it needs
// DirectComposition and DXGI together rather than one or the other, and what it deliberately does
// not take ownership of.

namespace SFT::GraphicsPlatform {

    namespace {

        // Minimal intrusive-refcount pointer. Deliberately not WRL's ComPtr: this file needs about
        // four operations from it, and a local 40-line version keeps the Windows SDK's C++/WinRT-
        // adjacent headers out of a package that otherwise compiles as plain portable C++.
        template <typename T>
        class ComPtr {
          public:
            ComPtr() noexcept = default;
            ~ComPtr() { reset(); }

            ComPtr(const ComPtr &other) noexcept : ptr_(other.ptr_) {
                if (ptr_ != nullptr) {
                    ptr_->AddRef();
                }
            }
            ComPtr(ComPtr &&other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}

            ComPtr &operator=(const ComPtr &other) noexcept {
                if (this != &other) {
                    if (other.ptr_ != nullptr) {
                        other.ptr_->AddRef();
                    }
                    reset();
                    ptr_ = other.ptr_;
                }
                return *this;
            }
            ComPtr &operator=(ComPtr &&other) noexcept {
                if (this != &other) {
                    reset();
                    ptr_ = std::exchange(other.ptr_, nullptr);
                }
                return *this;
            }

            void reset() noexcept {
                if (ptr_ != nullptr) {
                    ptr_->Release();
                    ptr_ = nullptr;
                }
            }

            [[nodiscard]] T *get() const noexcept { return ptr_; }
            T *operator->() const noexcept { return ptr_; }
            explicit operator bool() const noexcept { return ptr_ != nullptr; }

            // For the ubiquitous `HRESULT f(..., void **out)` / `IID_PPV_ARGS` shape. Releases any
            // existing pointee first so a retry into the same ComPtr cannot leak.
            T **put() noexcept {
                reset();
                return &ptr_;
            }
            void **put_void() noexcept { return reinterpret_cast<void **>(put()); }

          private:
            T *ptr_ = nullptr;
        };

        [[nodiscard]] std::string hresult_text(const char *what, HRESULT hr) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s failed (hresult=0x%08lX).", what,
                          static_cast<unsigned long>(hr));
            return std::string(buffer);
        }

        [[nodiscard]] QueryMessage platform_error(const char *what, HRESULT hr) {
            return QueryMessage{QueryStatus::PlatformError, hresult_text(what, hr)};
        }

        [[nodiscard]] DXGI_FORMAT to_dxgi_format(CompositionFormat format) noexcept {
            switch (format) {
                case CompositionFormat::Bgra8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
                case CompositionFormat::Rgba8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
                case CompositionFormat::Rgba16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
                case CompositionFormat::Rgb10a2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
            }
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        }

        [[nodiscard]] std::uint32_t format_bytes_per_pixel(CompositionFormat format) noexcept {
            switch (format) {
                case CompositionFormat::Bgra8Unorm:
                case CompositionFormat::Rgba8Unorm:
                case CompositionFormat::Rgb10a2Unorm: return 4;
                case CompositionFormat::Rgba16Float: return 8;
            }
            return 4;
        }

        // DXGI composition swapchains accept PREMULTIPLIED, IGNORE and UNSPECIFIED. Straight alpha is
        // not among them — DXGI has no straight-alpha composition mode at all — so rather than
        // silently presenting a caller's straight-alpha content as if it were premultiplied (which
        // produces wrong-but-plausible dark fringing that is genuinely hard to trace back to here),
        // create() rejects it with an explanation. The enumerator stays in the portable header
        // because it is a real distinction other compositors can honor.
        [[nodiscard]] DXGI_ALPHA_MODE to_dxgi_alpha_mode(CompositionAlphaMode mode) noexcept {
            switch (mode) {
                case CompositionAlphaMode::Premultiplied: return DXGI_ALPHA_MODE_PREMULTIPLIED;
                case CompositionAlphaMode::Ignore: return DXGI_ALPHA_MODE_IGNORE;
                case CompositionAlphaMode::Straight: return DXGI_ALPHA_MODE_STRAIGHT;
            }
            return DXGI_ALPHA_MODE_PREMULTIPLIED;
        }

        // DXGI's own limits for a flip-model composition swapchain.
        constexpr std::uint32_t min_buffer_count = 2;
        constexpr std::uint32_t max_buffer_count = 16;

        class WindowsCompositionPresenter final : public CompositionPresenter {
          public:
            WindowsCompositionPresenter() = default;

            ~WindowsCompositionPresenter() override {
                // The GPU may still be reading the shared textures via a queued Present. Releasing
                // them out from under it would be a use-after-free in the compositor's copy, so drain
                // first, then tear the visual tree down before the swapchain it points at.
                wait_for_gpu_idle();
                release_shared_images();
                if (composition_target_) {
                    composition_target_->SetRoot(nullptr);
                }
                composition_visual_.reset();
                composition_target_.reset();
                if (composition_device_) {
                    composition_device_->Commit();
                }
                close_fence_handles();
            }

            [[nodiscard]] QueryMessage initialize(const CompositionPresenterDesc &desc) {
                hwnd_ = static_cast<HWND>(desc.surface.window);
                width_ = desc.width;
                height_ = desc.height;
                format_ = desc.format;
                alpha_mode_ = desc.alpha_mode;
                buffer_count_ = std::clamp(desc.buffer_count, min_buffer_count, max_buffer_count);
                shared_image_count_ =
                    desc.shared_image_count != 0 ? desc.shared_image_count : buffer_count_;

                if (QueryMessage message = create_devices(desc); !message) {
                    return message;
                }
                if (QueryMessage message = create_swapchain(); !message) {
                    return message;
                }
                if (QueryMessage message = create_fences(); !message) {
                    return message;
                }
                if (QueryMessage message = create_shared_images(); !message) {
                    return message;
                }
                return bind_composition_tree();
            }

            [[nodiscard]] std::span<const CompositionSharedImage> shared_images() const noexcept override {
                return images_;
            }

            [[nodiscard]] CompositionSharedFences shared_fences() const noexcept override {
                return CompositionSharedFences{
                    .render_complete_nt_handle = render_complete_handle_,
                    .present_complete_nt_handle = present_complete_handle_,
                };
            }

            [[nodiscard]] QueryResult<CompositionAcquisition> acquire_next_image() override {
                if (images_.empty()) {
                    return QueryResult<CompositionAcquisition>{
                        {}, QueryMessage{QueryStatus::NotAvailable, "Composition presenter has no shared images."}};
                }
                const std::uint32_t index = next_image_index_;
                next_image_index_ = (next_image_index_ + 1) % static_cast<std::uint32_t>(images_.size());
                return QueryResult<CompositionAcquisition>{
                    CompositionAcquisition{
                        .image_index = index,
                        // Zero until this slot has been presented at least once, which is exactly the
                        // "no wait needed" value the header documents.
                        .wait_fence_value = image_present_values_[index],
                    },
                    QueryMessage{},
                };
            }

            [[nodiscard]] QueryMessage present(std::uint32_t image_index,
                                               std::uint64_t render_complete_value,
                                               std::uint32_t sync_interval) override {
                if (image_index >= images_.size()) {
                    return QueryMessage{QueryStatus::InvalidArgument,
                                        "Composition present: image_index is out of range."};
                }

                // GPU-side wait, not a CPU stall: the copy below is queued behind the caller's render
                // completion rather than the CPU blocking until it happens.
                if (render_complete_value != 0) {
                    const HRESULT hr = context_->Wait(render_complete_fence_.get(), render_complete_value);
                    if (FAILED(hr)) {
                        return platform_error("ID3D11DeviceContext4::Wait", hr);
                    }
                }

                // For a flip-model (FLIP_DISCARD) swap chain the buffer that's actually writable
                // rotates every frame — index 0 is only correct on the very first present. Always
                // reading GetBuffer(0, ...) here meant every other frame wrote into the buffer DXGI
                // was still displaying while presenting the one that was actually current, which reads
                // on screen as last frame's content stuck showing through underneath this frame's.
                ComPtr<ID3D11Texture2D> back_buffer;
                const UINT back_buffer_index = swapchain_->GetCurrentBackBufferIndex();
                if (const HRESULT hr = swapchain_->GetBuffer(back_buffer_index, IID_PPV_ARGS(back_buffer.put()));
                    FAILED(hr)) {
                    return platform_error("IDXGISwapChain3::GetBuffer", hr);
                }

                // Whole-surface overwrite every frame, which is what makes FLIP_DISCARD safe here
                // despite its undefined post-present buffer contents.
                context_->CopyResource(back_buffer.get(), textures_[image_index].get());

                // A composition swapchain is always composed by DWM, so tearing is not reachable on
                // this path no matter the interval: sync_interval 0 means "do not wait for a vblank
                // before queuing this frame", not "allow tearing". Callers whose present policy is
                // built around tearing lose that specific property by using this presenter, which is
                // the honest tradeoff for getting a transparent window at all.
                if (const HRESULT hr = swapchain_->Present(sync_interval, 0); FAILED(hr)) {
                    return platform_error("IDXGISwapChain3::Present", hr);
                }

                // Keep the tracked high-water mark at a value the D3D queue has actually been asked
                // to signal. Advancing it before Signal succeeds would make teardown wait forever for
                // an impossible fence value after a device/queue failure.
                const std::uint64_t next_present_fence_value = present_fence_value_ + 1;
                if (const HRESULT hr = context_->Signal(present_complete_fence_.get(), next_present_fence_value);
                    FAILED(hr)) {
                    return platform_error("ID3D11DeviceContext4::Signal", hr);
                }
                present_fence_value_ = next_present_fence_value;
                image_present_values_[image_index] = next_present_fence_value;
                return QueryMessage{};
            }

            [[nodiscard]] QueryMessage resize(std::uint32_t width, std::uint32_t height) override {
                if (width == 0 || height == 0) {
                    return QueryMessage{QueryStatus::InvalidArgument,
                                        "Composition presenter resize requires a non-zero extent."};
                }
                if (width == width_ && height == height_) {
                    // Already the right size, so there is nothing to rebuild — but a set_live_scale()
                    // from an earlier drag step may still be published, and leaving it on would keep
                    // presenting a correctly-sized surface stretched to some other size. Cheap and a
                    // no-op whenever the scale is already identity, which is the common case.
                    return apply_visual_scale(1.0f, 1.0f);
                }

                // No wait_for_gpu_idle() here, deliberately. D3D11 tracks resource lifetime against
                // queued GPU work itself: releasing the last reference to a texture a pending
                // CopyResource still reads defers the actual free until that command retires, rather
                // than freeing underneath it. (This is the standing D3D11 difference from D3D12, where
                // the application owns that lifetime and a stall here really would be required.) The
                // one ordering this path genuinely needs is on the *importing* API's side — its own
                // submissions must be done with these images before it destroys its views of them —
                // and that is the caller's fence wait, not something a stall here could provide.
                //
                // That matters because this used to be a blocking WaitForSingleObject(INFINITE) on the
                // present fence, on the exact path an interactive resize runs every step. It bought no
                // safety D3D11 wasn't already providing and cost a full present round-trip per step,
                // which is most of why resizing a composed window could not keep up with the drag.
                release_shared_images();

                width_ = width;
                height_ = height;
                if (const HRESULT hr = swapchain_->ResizeBuffers(buffer_count_, width_, height_,
                                                                 to_dxgi_format(format_), 0);
                    FAILED(hr)) {
                    return platform_error("IDXGISwapChain3::ResizeBuffers", hr);
                }
                // The back buffers now match the client area on their own, so drop any live scale a
                // previous set_live_scale() left behind. Committing identity here (rather than at the
                // next present) is correct because the swapchain content is resized in the same
                // compositor batch: the visual never shows old-size content at identity.
                if (QueryMessage scaled = apply_visual_scale(1.0f, 1.0f); !scaled) {
                    return scaled;
                }
                return create_shared_images();
            }

            [[nodiscard]] QueryMessage set_live_scale(std::uint32_t width, std::uint32_t height) override {
                if (width == 0 || height == 0) {
                    return QueryMessage{QueryStatus::InvalidArgument,
                                        "Composition presenter live scale requires a non-zero extent."};
                }
                if (width_ == 0 || height_ == 0) {
                    return QueryMessage{QueryStatus::NotAvailable,
                                        "Composition presenter has no backing surface to scale."};
                }
                // Always relative to the backing size, never to the previously applied scale, so a
                // drag that issues hundreds of these does not accumulate error or drift.
                return apply_visual_scale(static_cast<float>(width) / static_cast<float>(width_),
                                          static_cast<float>(height) / static_cast<float>(height_));
            }

            [[nodiscard]] std::uint32_t width() const noexcept override { return width_; }
            [[nodiscard]] std::uint32_t height() const noexcept override { return height_; }

          private:
            [[nodiscard]] QueryMessage create_devices(const CompositionPresenterDesc &desc) {
                if (const HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.put())); FAILED(hr)) {
                    return platform_error("CreateDXGIFactory2", hr);
                }

                // Cross-adapter texture sharing does not work, so the interop device must land on the
                // caller's adapter rather than on whichever one DXGI happens to enumerate first.
                ComPtr<IDXGIAdapter1> adapter;
                if (desc.use_adapter_luid) {
                    LUID luid{};
                    std::memcpy(&luid, &desc.adapter_luid, sizeof(luid));
                    if (const HRESULT hr = factory_->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.put()));
                        FAILED(hr)) {
                        return platform_error("IDXGIFactory4::EnumAdapterByLuid", hr);
                    }
                }

                // BGRA support is required for DirectComposition interop. Requesting a null adapter
                // with an explicit driver type is invalid, hence the paired driver-type selection.
                const D3D_FEATURE_LEVEL feature_levels[] = {
                    D3D_FEATURE_LEVEL_11_1,
                    D3D_FEATURE_LEVEL_11_0,
                };
                ComPtr<ID3D11Device> device;
                ComPtr<ID3D11DeviceContext> context;
                const HRESULT hr = D3D11CreateDevice(
                    adapter.get(),
                    adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                    nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    feature_levels,
                    static_cast<UINT>(std::size(feature_levels)),
                    D3D11_SDK_VERSION,
                    device.put(),
                    nullptr,
                    context.put());
                if (FAILED(hr)) {
                    return platform_error("D3D11CreateDevice", hr);
                }

                // ID3D11Device5 / ID3D11DeviceContext4 are the D3D11.4 interfaces that carry
                // CreateFence and the GPU-side Wait/Signal this module's whole synchronization model
                // rests on. Absence means a pre-1703 Windows 10, where this path cannot work.
                if (const HRESULT qi = device->QueryInterface(IID_PPV_ARGS(device_.put())); FAILED(qi)) {
                    return QueryMessage{QueryStatus::Unsupported,
                                        hresult_text("ID3D11Device5 query (Windows 10 1703+ required for "
                                                     "shared fences)", qi)};
                }
                if (const HRESULT qi = context->QueryInterface(IID_PPV_ARGS(context_.put())); FAILED(qi)) {
                    return QueryMessage{QueryStatus::Unsupported,
                                        hresult_text("ID3D11DeviceContext4 query", qi)};
                }
                return QueryMessage{};
            }

            [[nodiscard]] QueryMessage create_swapchain() {
                DXGI_SWAP_CHAIN_DESC1 description{};
                description.Width = width_;
                description.Height = height_;
                description.Format = to_dxgi_format(format_);
                description.Stereo = FALSE;
                description.SampleDesc.Count = 1;
                description.SampleDesc.Quality = 0;
                description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                description.BufferCount = buffer_count_;
                // Composition swapchains require stretch scaling and a flip-model swap effect; DXGI
                // rejects DXGI_SCALING_NONE and both legacy blit effects outright.
                description.Scaling = DXGI_SCALING_STRETCH;
                description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                description.AlphaMode = to_dxgi_alpha_mode(alpha_mode_);
                description.Flags = 0;

                // The one call that makes any of this possible — CreateSwapChainForHwnd would reject
                // a non-UNSPECIFIED AlphaMode here, which is precisely the wall the native swapchain
                // path already hit. The factory method only returns IDXGISwapChain1, so the upgrade to
                // IDXGISwapChain3 (needed for GetCurrentBackBufferIndex — see present()'s doc comment
                // on why that matters) happens as a separate QueryInterface right after.
                ComPtr<IDXGISwapChain1> swapchain;
                if (const HRESULT hr = factory_->CreateSwapChainForComposition(
                        device_.get(), &description, nullptr, swapchain.put());
                    FAILED(hr)) {
                    return platform_error("IDXGIFactory2::CreateSwapChainForComposition", hr);
                }
                if (const HRESULT hr = swapchain->QueryInterface(IID_PPV_ARGS(swapchain_.put())); FAILED(hr)) {
                    return platform_error("IDXGISwapChain3 query", hr);
                }
                return QueryMessage{};
            }

            [[nodiscard]] QueryMessage create_fences() {
                if (const HRESULT hr = device_->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                                           IID_PPV_ARGS(render_complete_fence_.put()));
                    FAILED(hr)) {
                    return platform_error("ID3D11Device5::CreateFence (render complete)", hr);
                }
                if (const HRESULT hr = device_->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                                           IID_PPV_ARGS(present_complete_fence_.put()));
                    FAILED(hr)) {
                    return platform_error("ID3D11Device5::CreateFence (present complete)", hr);
                }
                if (const HRESULT hr = render_complete_fence_->CreateSharedHandle(
                        nullptr, GENERIC_ALL, nullptr, &render_complete_handle_);
                    FAILED(hr)) {
                    return platform_error("ID3D11Fence::CreateSharedHandle (render complete)", hr);
                }
                if (const HRESULT hr = present_complete_fence_->CreateSharedHandle(
                        nullptr, GENERIC_ALL, nullptr, &present_complete_handle_);
                    FAILED(hr)) {
                    return platform_error("ID3D11Fence::CreateSharedHandle (present complete)", hr);
                }
                return QueryMessage{};
            }

            [[nodiscard]] QueryMessage create_shared_images() {
                D3D11_TEXTURE2D_DESC description{};
                description.Width = width_;
                description.Height = height_;
                description.MipLevels = 1;
                description.ArraySize = 1;
                description.Format = to_dxgi_format(format_);
                description.SampleDesc.Count = 1;
                description.SampleDesc.Quality = 0;
                description.Usage = D3D11_USAGE_DEFAULT;
                description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                description.CPUAccessFlags = 0;
                // SHARED_NTHANDLE must be paired with a sharing mode. Plain SHARED rather than
                // SHARED_KEYEDMUTEX because ordering is carried by the two explicit timeline fences —
                // a keyed mutex would impose a second, redundant synchronization protocol that the
                // importing API would also have to acquire and release around every access.
                description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

                textures_.reserve(shared_image_count_);
                images_.reserve(shared_image_count_);
                image_present_values_.assign(shared_image_count_, 0);

                for (std::uint32_t index = 0; index < shared_image_count_; ++index) {
                    ComPtr<ID3D11Texture2D> texture;
                    if (const HRESULT hr = device_->CreateTexture2D(&description, nullptr, texture.put());
                        FAILED(hr)) {
                        release_shared_images();
                        return platform_error("ID3D11Device::CreateTexture2D (shared image)", hr);
                    }

                    ComPtr<IDXGIResource1> resource;
                    if (const HRESULT hr = texture->QueryInterface(IID_PPV_ARGS(resource.put())); FAILED(hr)) {
                        release_shared_images();
                        return platform_error("IDXGIResource1 query (shared image)", hr);
                    }

                    HANDLE handle = nullptr;
                    if (const HRESULT hr = resource->CreateSharedHandle(
                            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
                        FAILED(hr)) {
                        release_shared_images();
                        return platform_error("IDXGIResource1::CreateSharedHandle", hr);
                    }

                    textures_.push_back(std::move(texture));
                    images_.push_back(CompositionSharedImage{
                        .nt_handle = handle,
                        .allocation_size_bytes = static_cast<std::uint64_t>(width_) * height_ *
                                                 format_bytes_per_pixel(format_),
                        .width = width_,
                        .height = height_,
                        .format = format_,
                    });
                }

                next_image_index_ = 0;
                return QueryMessage{};
            }

            [[nodiscard]] QueryMessage bind_composition_tree() {
                // DCompositionCreateDevice3 is the newest of the three device factories and accepts a
                // direct request for IDCompositionDesktopDevice, which is the one that can target a
                // plain HWND. Older factories exist but offer nothing extra here.
                ComPtr<IDXGIDevice> dxgi_device;
                if (const HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(dxgi_device.put())); FAILED(hr)) {
                    return platform_error("IDXGIDevice query", hr);
                }
                if (const HRESULT hr = DCompositionCreateDevice3(
                        dxgi_device.get(), IID_PPV_ARGS(composition_device_.put()));
                    FAILED(hr)) {
                    return platform_error("DCompositionCreateDevice3", hr);
                }
                // topmost=TRUE puts the visual above the window's own painted content, so nothing the
                // window draws underneath shows through as an opaque backing layer.
                if (const HRESULT hr = composition_device_->CreateTargetForHwnd(hwnd_, TRUE,
                                                                                composition_target_.put());
                    FAILED(hr)) {
                    return platform_error("IDCompositionDesktopDevice::CreateTargetForHwnd", hr);
                }
                if (const HRESULT hr = composition_device_->CreateVisual(composition_visual_.put());
                    FAILED(hr)) {
                    return platform_error("IDCompositionDesktopDevice::CreateVisual", hr);
                }
                if (const HRESULT hr = composition_visual_->SetContent(swapchain_.get()); FAILED(hr)) {
                    return platform_error("IDCompositionVisual2::SetContent", hr);
                }
                if (const HRESULT hr = composition_target_->SetRoot(composition_visual_.get()); FAILED(hr)) {
                    return platform_error("IDCompositionTarget::SetRoot", hr);
                }
                // Commit publishes the visual tree to the compositor. Only needed when the tree
                // changes, not per frame — per-frame presentation goes through the swapchain, which
                // the compositor already knows about after this point.
                if (const HRESULT hr = composition_device_->Commit(); FAILED(hr)) {
                    return platform_error("IDCompositionDesktopDevice::Commit", hr);
                }
                return QueryMessage{};
            }

            // Sets the visual's scale about its top-left origin and publishes it. Compositor-only: no
            // GPU work is queued and nothing is reallocated, so this is orders of magnitude cheaper
            // than resize() and safe to call at input rate.
            [[nodiscard]] QueryMessage apply_visual_scale(float scale_x, float scale_y) {
                if (!composition_visual_ || !composition_device_) {
                    return QueryMessage{};
                }
                // A Commit is a compositor round-trip. Redundant ones are common here (a drag along one
                // axis leaves the other unchanged, and the settle frame re-asserts identity), so skip
                // any that would not move a pixel.
                if (scale_x == applied_scale_x_ && scale_y == applied_scale_y_) {
                    return QueryMessage{};
                }
                const D2D_MATRIX_3X2_F transform{
                    .m11 = scale_x,
                    .m12 = 0.0f,
                    .m21 = 0.0f,
                    .m22 = scale_y,
                    .dx = 0.0f,
                    .dy = 0.0f,
                };
                if (const HRESULT hr = composition_visual_->SetTransform(transform); FAILED(hr)) {
                    return platform_error("IDCompositionVisual2::SetTransform", hr);
                }
                if (const HRESULT hr = composition_device_->Commit(); FAILED(hr)) {
                    return platform_error("IDCompositionDesktopDevice::Commit visual scale", hr);
                }
                applied_scale_x_ = scale_x;
                applied_scale_y_ = scale_y;
                return QueryMessage{};
            }

            void release_shared_images() noexcept {
                for (CompositionSharedImage &image : images_) {
                    if (image.nt_handle != nullptr) {
                        CloseHandle(static_cast<HANDLE>(image.nt_handle));
                    }
                }
                images_.clear();
                textures_.clear();
                image_present_values_.clear();
                next_image_index_ = 0;
            }

            void close_fence_handles() noexcept {
                if (render_complete_handle_ != nullptr) {
                    CloseHandle(render_complete_handle_);
                    render_complete_handle_ = nullptr;
                }
                if (present_complete_handle_ != nullptr) {
                    CloseHandle(present_complete_handle_);
                    present_complete_handle_ = nullptr;
                }
            }

            // Blocks until every present this presenter has queued has retired. Teardown only — see
            // resize()'s comment for why that path does not need it and must not pay for it. Here the
            // wait is not about the D3D textures (whose lifetime the runtime tracks) but about the
            // visual tree and fences going away underneath queued compositor work. This is a lifetime
            // proof, not a responsiveness budget: a timeout would merely turn a slow compositor into a
            // use-after-free, and nothing is interactive during destruction anyway.
            void wait_for_gpu_idle() noexcept {
                if (!present_complete_fence_ || !context_ || present_fence_value_ == 0) {
                    return;
                }
                if (present_complete_fence_->GetCompletedValue() >= present_fence_value_) {
                    return;
                }
                HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (event == nullptr) {
                    return;
                }
                if (SUCCEEDED(present_complete_fence_->SetEventOnCompletion(present_fence_value_, event))) {
                    WaitForSingleObject(event, INFINITE);
                }
                CloseHandle(event);
            }

            HWND hwnd_ = nullptr;
            std::uint32_t width_ = 0;
            std::uint32_t height_ = 0;
            std::uint32_t buffer_count_ = min_buffer_count;
            std::uint32_t shared_image_count_ = min_buffer_count;
            CompositionFormat format_ = CompositionFormat::Bgra8Unorm;
            CompositionAlphaMode alpha_mode_ = CompositionAlphaMode::Premultiplied;

            ComPtr<IDXGIFactory4> factory_;
            ComPtr<ID3D11Device5> device_;
            ComPtr<ID3D11DeviceContext4> context_;
            // IDXGISwapChain3 (not just IDXGISwapChain1) specifically for GetCurrentBackBufferIndex()
            // — see present()'s doc comment on the bug that omitting it causes.
            ComPtr<IDXGISwapChain3> swapchain_;
            ComPtr<IDCompositionDesktopDevice> composition_device_;
            ComPtr<IDCompositionTarget> composition_target_;
            ComPtr<IDCompositionVisual2> composition_visual_;
            // Scale currently published on composition_visual_, so apply_visual_scale() can drop
            // no-op commits. Identity until the first set_live_scale(), matching a freshly created
            // visual's transform.
            float applied_scale_x_ = 1.0f;
            float applied_scale_y_ = 1.0f;

            ComPtr<ID3D11Fence> render_complete_fence_;
            ComPtr<ID3D11Fence> present_complete_fence_;
            HANDLE render_complete_handle_ = nullptr;
            HANDLE present_complete_handle_ = nullptr;
            std::uint64_t present_fence_value_ = 0;

            std::vector<ComPtr<ID3D11Texture2D>> textures_;
            std::vector<CompositionSharedImage> images_;
            // Per-slot value of present_complete_fence_ that must be reached before that slot may be
            // written again. Parallel to images_ by index.
            std::vector<std::uint64_t> image_present_values_;
            std::uint32_t next_image_index_ = 0;
        };

    } // namespace

    bool composition_present_compiled() noexcept {
        return true;
    }

    QueryMessage composition_present_available() noexcept {
        // DWM composition is unconditionally on from Windows 8 onward and cannot be turned off, so
        // this is effectively a formality on any supported OS — but it is a cheap, real check rather
        // than an assumption, and it is the difference between a clear message and a failed
        // CreateTargetForHwnd if that ever stops holding.
        BOOL composition_enabled = FALSE;
        if (const HRESULT hr = DwmIsCompositionEnabled(&composition_enabled); FAILED(hr)) {
            return platform_error("DwmIsCompositionEnabled", hr);
        }
        if (composition_enabled == FALSE) {
            return QueryMessage{QueryStatus::NotAvailable,
                                "The desktop compositor (DWM) is not running, so composition present cannot "
                                "put a transparent surface on screen."};
        }
        return QueryMessage{};
    }

    QueryResult<std::unique_ptr<CompositionPresenter>> create_composition_presenter(
        const CompositionPresenterDesc &desc) {
        using Result = QueryResult<std::unique_ptr<CompositionPresenter>>;

        if (desc.surface.system != WindowSystem::Win32 || desc.surface.window == nullptr) {
            return Result{nullptr,
                          QueryMessage{QueryStatus::InvalidArgument,
                                       "Composition present requires a Win32 HWND surface."}};
        }
        if (desc.width == 0 || desc.height == 0) {
            return Result{nullptr,
                          QueryMessage{QueryStatus::InvalidArgument,
                                       "Composition present requires a non-zero surface extent."}};
        }
        if (desc.alpha_mode == CompositionAlphaMode::Straight) {
            return Result{nullptr,
                          QueryMessage{QueryStatus::Unsupported,
                                       "DXGI composition swapchains support premultiplied or ignored alpha "
                                       "only; premultiply the rendered output and request "
                                       "CompositionAlphaMode::Premultiplied instead."}};
        }
        if (QueryMessage available = composition_present_available(); !available) {
            return Result{nullptr, std::move(available)};
        }

        auto presenter = std::make_unique<WindowsCompositionPresenter>();
        if (QueryMessage message = presenter->initialize(desc); !message) {
            return Result{nullptr, std::move(message)};
        }
        return Result{std::move(presenter), QueryMessage{}};
    }

} // namespace SFT::GraphicsPlatform

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif // STURDY_GRAPHICS_PLATFORM_WINDOWS
