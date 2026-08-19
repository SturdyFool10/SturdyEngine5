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


#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#endif


namespace SFT::Core::GraphicsPlatform {

    namespace {


        template <typename T>
        class ComPtr {
          public:
            /// Constructs a `ComPtr` in its default state.
            ///
            /// @note This function does not throw exceptions.
            ComPtr() noexcept = default;
            /// Destroys the `ComPtr` and releases resources owned by it.
            ///
            /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
            ~ComPtr() { reset(); }

            /// Constructs a `ComPtr` from another instance.
            ///
            /// @param other Other object used by the operation.
            ///
            /// @note This function does not throw exceptions.
            ComPtr(const ComPtr &other) noexcept : ptr_(other.ptr_) {
                if (ptr_ != nullptr) {
                    ptr_->AddRef();
                }
            }
            /// Constructs a `ComPtr` from another instance.
            ///
            /// @param other Other object used by the operation.
            ///
            /// @note This function does not throw exceptions.
            ComPtr(ComPtr &&other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}

            /// Assigns a new value to this `ComPtr`.
            ///
            /// @param other Other object used by the operation.
            ///
            /// @return Returns `*this` so the operation can be chained.
            /// @note This function does not throw exceptions.
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
            /// Assigns a new value to this `ComPtr`.
            ///
            /// @param other Other object used by the operation.
            ///
            /// @return Returns `*this` so the operation can be chained.
            /// @note This function does not throw exceptions.
            ComPtr &operator=(ComPtr &&other) noexcept {
                if (this != &other) {
                    reset();
                    ptr_ = std::exchange(other.ptr_, nullptr);
                }
                return *this;
            }

            /// Resets the object to its baseline state.
            ///
            /// @note This function does not throw exceptions.
            void reset() noexcept {
                if (ptr_ != nullptr) {
                    ptr_->Release();
                    ptr_ = nullptr;
                }
            }

            /// Returns the value or resource currently represented by `ComPtr`.
            ///
            /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
            /// @note This function does not throw exceptions.
            [[nodiscard]] T *get() const noexcept { return ptr_; }
            /// Accesses the object referenced by this `ComPtr`.
            ///
            /// @return Returns a pointer through which the referenced object can be accessed.
            /// @note This function does not throw exceptions.
            T *operator->() const noexcept { return ptr_; }
            /// Converts the `ComPtr` to `bool`.
            ///
            /// @return Returns the boolean result of the operation.
            /// @note This function does not throw exceptions.
            explicit operator bool() const noexcept { return ptr_ != nullptr; }


            /// Returns the current or globally available put value.
            ///
            /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
            /// @note This function does not throw exceptions.
            T **put() noexcept {
                reset();
                return &ptr_;
            }
            /// Returns the current or globally available put void value.
            ///
            /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
            /// @note This function does not throw exceptions.
            void **put_void() noexcept { return reinterpret_cast<void **>(put()); }

          private:
            T *ptr_ = nullptr;
        };

        /// Performs the hresult text operation for `GraphicsPlatform` using the supplied arguments.
        ///
        /// @param what `what` value used by the operation.
        /// @param hr `hr` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::string hresult_text(const char *what, HRESULT hr) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s failed (hresult=0x%08lX).", what,
                          static_cast<unsigned long>(hr));
            return std::string(buffer);
        }

        /// Performs the platform error operation for `GraphicsPlatform` using the supplied arguments.
        ///
        /// @param what `what` value used by the operation.
        /// @param hr `hr` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Error/status alternatives explicitly produced by this implementation include `QueryStatus::PlatformError`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] QueryMessage platform_error(const char *what, HRESULT hr) {
            return QueryMessage{QueryStatus::PlatformError, hresult_text(what, hr)};
        }

        /// Converts the value to DXGI format representation.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the value converted to DXGI format representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DXGI_FORMAT to_dxgi_format(CompositionFormat format) noexcept {
            switch (format) {
                case CompositionFormat::Bgra8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
                case CompositionFormat::Rgba8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
                case CompositionFormat::Rgba16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
                case CompositionFormat::Rgb10a2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
            }
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        }

        /// Formats bytes per pixel using the supplied arguments and current state.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::uint32_t format_bytes_per_pixel(CompositionFormat format) noexcept {
            switch (format) {
                case CompositionFormat::Bgra8Unorm:
                case CompositionFormat::Rgba8Unorm:
                case CompositionFormat::Rgb10a2Unorm: return 4;
                case CompositionFormat::Rgba16Float: return 8;
            }
            return 4;
        }


        /// Converts the value to DXGI alpha mode representation.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns the value converted to DXGI alpha mode representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DXGI_ALPHA_MODE to_dxgi_alpha_mode(CompositionAlphaMode mode) noexcept {
            switch (mode) {
                case CompositionAlphaMode::Premultiplied: return DXGI_ALPHA_MODE_PREMULTIPLIED;
                case CompositionAlphaMode::Ignore: return DXGI_ALPHA_MODE_IGNORE;
                case CompositionAlphaMode::Straight: return DXGI_ALPHA_MODE_STRAIGHT;
            }
            return DXGI_ALPHA_MODE_PREMULTIPLIED;
        }


        constexpr std::uint32_t min_buffer_count = 2;
        constexpr std::uint32_t max_buffer_count = 16;

        class WindowsCompositionPresenter final : public CompositionPresenter {
          public:
            /// Constructs a `WindowsCompositionPresenter` in its default state.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            WindowsCompositionPresenter() = default;

            /// Destroys the `WindowsCompositionPresenter` and releases resources owned by it.
            ///
            /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
            ~WindowsCompositionPresenter() override {


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

            /// Initializes the `WindowsCompositionPresenter` for use.
            ///
            /// @param desc Description of the resource or operation to perform.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

            /// Returns the current or globally available shared images value.
            ///
            /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
            /// @note This function does not throw exceptions.
            [[nodiscard]] std::span<const CompositionSharedImage> shared_images() const noexcept override {
                return images_;
            }

            /// Returns the current or globally available shared fences value.
            ///
            /// @return Returns the current shared fences value.
            /// @note This function does not throw exceptions.
            [[nodiscard]] CompositionSharedFences shared_fences() const noexcept override {
                return CompositionSharedFences{
                    .render_complete_nt_handle = render_complete_handle_,
                    .present_complete_nt_handle = present_complete_handle_,
                };
            }

            /// Acquires next image.
            ///
            /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
            /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
            /// @note Error/status alternatives explicitly produced by this implementation include `QueryStatus::NotAvailable`.
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


                        .wait_fence_value = image_present_values_[index],
                    },
                    QueryMessage{},
                };
            }

            /// Presents the completed frame to the target surface or swapchain.
            ///
            /// @param image_index Zero-based index of the target element or entry.
            /// @param render_complete_value Value consumed by the operation.
            /// @param sync_interval `sync_interval` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] QueryMessage present(std::uint32_t image_index,
                                               std::uint64_t render_complete_value,
                                               std::uint32_t sync_interval) override {
                if (image_index >= images_.size()) {
                    return QueryMessage{QueryStatus::InvalidArgument,
                                        "Composition present: image_index is out of range."};
                }


                if (render_complete_value != 0) {
                    const HRESULT hr = context_->Wait(render_complete_fence_.get(), render_complete_value);
                    if (FAILED(hr)) {
                        return platform_error("ID3D11DeviceContext4::Wait", hr);
                    }
                }


                ComPtr<ID3D11Texture2D> back_buffer;
                const UINT back_buffer_index = swapchain_->GetCurrentBackBufferIndex();
                if (const HRESULT hr = swapchain_->GetBuffer(back_buffer_index, IID_PPV_ARGS(back_buffer.put()));
                    FAILED(hr)) {
                    return platform_error("IDXGISwapChain3::GetBuffer", hr);
                }


                context_->CopyResource(back_buffer.get(), textures_[image_index].get());


                if (const HRESULT hr = swapchain_->Present(sync_interval, 0); FAILED(hr)) {
                    return platform_error("IDXGISwapChain3::Present", hr);
                }


                const std::uint64_t next_present_fence_value = present_fence_value_ + 1;
                if (const HRESULT hr = context_->Signal(present_complete_fence_.get(), next_present_fence_value);
                    FAILED(hr)) {
                    return platform_error("ID3D11DeviceContext4::Signal", hr);
                }
                present_fence_value_ = next_present_fence_value;
                image_present_values_[image_index] = next_present_fence_value;
                return QueryMessage{};
            }

            /// Changes the logical size to the requested value, creating or removing elements as needed.
            ///
            /// @param width Width of the target extent.
            /// @param height Height of the target extent.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] QueryMessage resize(std::uint32_t width, std::uint32_t height) override {
                if (width == 0 || height == 0) {
                    return QueryMessage{QueryStatus::InvalidArgument,
                                        "Composition presenter resize requires a non-zero extent."};
                }
                if (width == width_ && height == height_) {


                    return apply_visual_scale(1.0f, 1.0f);
                }


                release_shared_images();

                width_ = width;
                height_ = height;
                if (const HRESULT hr = swapchain_->ResizeBuffers(buffer_count_, width_, height_,
                                                                 to_dxgi_format(format_), 0);
                    FAILED(hr)) {
                    return platform_error("IDXGISwapChain3::ResizeBuffers", hr);
                }


                if (QueryMessage scaled = apply_visual_scale(1.0f, 1.0f); !scaled) {
                    return scaled;
                }
                return create_shared_images();
            }

            /// Sets the live scale for this `WindowsCompositionPresenter`.
            ///
            /// @param width Width of the target extent.
            /// @param height Height of the target extent.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] QueryMessage set_live_scale(std::uint32_t width, std::uint32_t height) override {
                if (width == 0 || height == 0) {
                    return QueryMessage{QueryStatus::InvalidArgument,
                                        "Composition presenter live scale requires a non-zero extent."};
                }
                if (width_ == 0 || height_ == 0) {
                    return QueryMessage{QueryStatus::NotAvailable,
                                        "Composition presenter has no backing surface to scale."};
                }


                return apply_visual_scale(static_cast<float>(width) / static_cast<float>(width_),
                                          static_cast<float>(height) / static_cast<float>(height_));
            }

            /// Returns the current or globally available width value.
            ///
            /// @return Returns the current width value.
            /// @note This function does not throw exceptions.
            [[nodiscard]] std::uint32_t width() const noexcept override { return width_; }
            /// Returns the current or globally available height value.
            ///
            /// @return Returns the current height value.
            /// @note This function does not throw exceptions.
            [[nodiscard]] std::uint32_t height() const noexcept override { return height_; }

          private:
            /// Creates a devices from the supplied parameters.
            ///
            /// @param desc Description of the resource or operation to perform.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] QueryMessage create_devices(const CompositionPresenterDesc &desc) {
                if (const HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.put())); FAILED(hr)) {
                    return platform_error("CreateDXGIFactory2", hr);
                }


                ComPtr<IDXGIAdapter1> adapter;
                if (desc.use_adapter_luid) {
                    LUID luid{};
                    std::memcpy(&luid, &desc.adapter_luid, sizeof(luid));
                    if (const HRESULT hr = factory_->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.put()));
                        FAILED(hr)) {
                        return platform_error("IDXGIFactory4::EnumAdapterByLuid", hr);
                    }
                }


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

            /// Creates a swapchain from the supplied parameters.
            ///
            /// @return Returns the current create swapchain value.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


                description.Scaling = DXGI_SCALING_STRETCH;
                description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                description.AlphaMode = to_dxgi_alpha_mode(alpha_mode_);
                description.Flags = 0;


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

            /// Creates a fences from the supplied parameters.
            ///
            /// @return Returns the current create fences value.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

            /// Creates a shared images from the supplied parameters.
            ///
            /// @return Returns the current create shared images value.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

            /// Binds composition tree for subsequent operations.
            ///
            /// @return Returns the current bind composition tree value.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] QueryMessage bind_composition_tree() {


                ComPtr<IDXGIDevice> dxgi_device;
                if (const HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(dxgi_device.put())); FAILED(hr)) {
                    return platform_error("IDXGIDevice query", hr);
                }
                if (const HRESULT hr = DCompositionCreateDevice3(
                        dxgi_device.get(), IID_PPV_ARGS(composition_device_.put()));
                    FAILED(hr)) {
                    return platform_error("DCompositionCreateDevice3", hr);
                }


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


                if (const HRESULT hr = composition_device_->Commit(); FAILED(hr)) {
                    return platform_error("IDCompositionDesktopDevice::Commit", hr);
                }
                return QueryMessage{};
            }


            /// Applies visual scale using the supplied arguments and current state.
            ///
            /// @param scale_x `scale_x` value used by the operation.
            /// @param scale_y `scale_y` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] QueryMessage apply_visual_scale(float scale_x, float scale_y) {
                if (!composition_visual_ || !composition_device_) {
                    return QueryMessage{};
                }


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

            /// Releases shared images using the supplied arguments and current state.
            ///
            /// @note This function does not throw exceptions.
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

            /// Closes fence handles using the supplied arguments and current state.
            ///
            /// @note This function does not throw exceptions.
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


            /// Waits for for GPU idle to complete.
            ///
            /// @note This function does not throw exceptions.
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


            ComPtr<IDXGISwapChain3> swapchain_;
            ComPtr<IDCompositionDesktopDevice> composition_device_;
            ComPtr<IDCompositionTarget> composition_target_;
            ComPtr<IDCompositionVisual2> composition_visual_;


            float applied_scale_x_ = 1.0f;
            float applied_scale_y_ = 1.0f;

            ComPtr<ID3D11Fence> render_complete_fence_;
            ComPtr<ID3D11Fence> present_complete_fence_;
            HANDLE render_complete_handle_ = nullptr;
            HANDLE present_complete_handle_ = nullptr;
            std::uint64_t present_fence_value_ = 0;

            std::vector<ComPtr<ID3D11Texture2D>> textures_;
            std::vector<CompositionSharedImage> images_;


            std::vector<std::uint64_t> image_present_values_;
            std::uint32_t next_image_index_ = 0;
        };

    } // namespace

    /// Returns the current or globally available composition present compiled value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool composition_present_compiled() noexcept {
        return true;
    }

    /// Returns the current or globally available composition present available value.
    ///
    /// @return Returns the current composition present available value.
    /// @note This function does not throw exceptions.
    QueryMessage composition_present_available() noexcept {


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

    /// Creates a composition presenter from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `QueryStatus::InvalidArgument`, `QueryStatus::Unsupported`.
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

} // namespace SFT::Core::GraphicsPlatform

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif // STURDY_GRAPHICS_PLATFORM_WINDOWS
