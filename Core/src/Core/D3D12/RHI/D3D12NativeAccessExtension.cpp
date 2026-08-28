#include <Core/D3D12/RHI/D3D12NativeAccessExtension.hpp>

#include <Core/D3D12/RHI/D3D12CommandEncoder.hpp>

namespace SFT::D3D12 {

    /// Constructs a `D3D12NativeAccessExtension` from the supplied initialization values.
    ///
    /// @param factory Factory the device was created from.
    /// @param adapter Adapter the device was created on.
    /// @param device Device to publish.
    /// @param graphics_queue Primary graphics queue.
    /// @param queue_lookup_context Context passed to `queue_lookup`.
    /// @param queue_lookup Resolves per-lane queues, or null to report only the graphics queue.
    ///
    /// @note This function does not throw exceptions.
    D3D12NativeAccessExtension::D3D12NativeAccessExtension(IDXGIFactory6 *factory,
                                                           IDXGIAdapter4 *adapter,
                                                           ID3D12Device *device,
                                                           ID3D12CommandQueue *graphics_queue,
                                                           void *queue_lookup_context,
                                                           NativeQueueLookup queue_lookup) noexcept
        : factory_(factory), adapter_(adapter), device_(device), graphics_queue_(graphics_queue),
          queue_lookup_context_(queue_lookup_context), queue_lookup_(queue_lookup) {}

    /// Returns the identifier this extension is published under.
    ///
    /// @return Returns the current extension ID value.
    /// @note This function does not throw exceptions.
    rhi::ExtensionId D3D12NativeAccessExtension::extension_id() const noexcept { return id(); }

    /// Returns the borrowed DXGI factory.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred.
    /// @note This function does not throw exceptions.
    IDXGIFactory6 *D3D12NativeAccessExtension::native_factory() const noexcept { return factory_; }

    /// Returns the borrowed DXGI adapter.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred.
    /// @note This function does not throw exceptions.
    IDXGIAdapter4 *D3D12NativeAccessExtension::native_adapter() const noexcept { return adapter_; }

    /// Returns the borrowed D3D12 device.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred.
    /// @note This function does not throw exceptions.
    ID3D12Device *D3D12NativeAccessExtension::native_device() const noexcept { return device_; }

    /// Returns the borrowed primary graphics command queue.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred.
    /// @note This function does not throw exceptions.
    ID3D12CommandQueue *D3D12NativeAccessExtension::native_graphics_queue() const noexcept {
        return graphics_queue_;
    }

    /// Returns the borrowed command queue backing `lane`.
    ///
    /// @param lane Queue lane to resolve.
    ///
    /// @return The lane's queue, or the graphics queue when no per-lane lookup was supplied.
    /// @note This function does not throw exceptions.
    ID3D12CommandQueue *D3D12NativeAccessExtension::native_queue(rhi::QueueLane lane) const noexcept {
        if (queue_lookup_ == nullptr) {
            return graphics_queue_;
        }
        return queue_lookup_(queue_lookup_context_, lane);
    }

    /// Returns the borrowed command list `encoder` is recording into.
    ///
    /// @param encoder Encoder to inspect.
    ///
    /// @return The underlying command list, or null when `encoder` is not a D3D12 encoder.
    /// @note This function does not throw exceptions.
    ID3D12GraphicsCommandList *D3D12NativeAccessExtension::native_command_list(
        const rhi::CommandEncoder &encoder) const noexcept {
        // dynamic_cast rather than static_cast for the same reason the Vulkan extension does it: a
        // caller can legitimately hold an encoder produced by a different backend, and identifying
        // that case is the whole point of returning null instead of reinterpreting the object.
        if (const auto *d3d12_encoder = dynamic_cast<const D3D12CommandEncoder *>(&encoder)) {
            return d3d12_encoder->native_command_list();
        }
        return nullptr;
    }

} // namespace SFT::D3D12
