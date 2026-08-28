#pragma once

#include <Core/D3D12/RHI/D3D12Common.hpp>

namespace SFT::D3D12 {

    /// Publishes the backend's raw D3D12 objects to code that needs to reach past the RHI.
    ///
    /// The D3D12 counterpart of `Core::Vulkan::VulkanNativeAccessExtension`, and deliberately the
    /// same shape: an interop layer, a profiler, or a consumer doing something the RHI has no
    /// abstraction for should be able to drop to the native API for that one thing without
    /// abandoning the engine. Reached through `rhi::RhiDevice::extension_interface(id())`, and only
    /// published when `RendererFeatureRequest::enable_native_access_extension` asked for it — so a
    /// build that never opts in cannot accidentally depend on native access.
    ///
    /// Every handle returned here is **borrowed**. `D3D12Device` owns the underlying `ComPtr`s and
    /// outlives this extension, so callers must not release them, and must not use them after the
    /// device is gone. Callers that want to retain one should `AddRef` it themselves.
    class D3D12NativeAccessExtension final : public rhi::RhiDeviceExtension {
      public:
        /// Returns the identifier this extension is published under.
        ///
        /// @return Returns the current ID value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr rhi::ExtensionId id() noexcept {
            return rhi::ExtensionId{"sturdy", "d3d12-native-access", 1};
        }

        /// Resolves the command queue backing a queue lane.
        using NativeQueueLookup = ID3D12CommandQueue *(*)(void *context, rhi::QueueLane lane) noexcept;

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
        D3D12NativeAccessExtension(IDXGIFactory6 *factory,
                                   IDXGIAdapter4 *adapter,
                                   ID3D12Device *device,
                                   ID3D12CommandQueue *graphics_queue,
                                   void *queue_lookup_context = nullptr,
                                   NativeQueueLookup queue_lookup = nullptr) noexcept;

        /// Returns the identifier this extension is published under.
        ///
        /// @return Returns the current extension ID value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] rhi::ExtensionId extension_id() const noexcept override;

        /// Returns the borrowed DXGI factory.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred.
        /// @note This function does not throw exceptions.
        [[nodiscard]] IDXGIFactory6 *native_factory() const noexcept;
        /// Returns the borrowed DXGI adapter.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred.
        /// @note This function does not throw exceptions.
        [[nodiscard]] IDXGIAdapter4 *native_adapter() const noexcept;
        /// Returns the borrowed D3D12 device.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ID3D12Device *native_device() const noexcept;
        /// Returns the borrowed primary graphics command queue.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ID3D12CommandQueue *native_graphics_queue() const noexcept;

        /// Returns the borrowed command queue backing `lane`.
        ///
        /// @param lane Queue lane to resolve.
        ///
        /// @return The lane's queue, or the graphics queue when no per-lane lookup was supplied.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ID3D12CommandQueue *native_queue(rhi::QueueLane lane) const noexcept;

        /// Returns the borrowed command list `encoder` is recording into.
        ///
        /// @param encoder Encoder to inspect.
        ///
        /// @return The underlying command list, or null when `encoder` is not a D3D12 encoder —
        ///         which is how a caller holding an encoder from a different backend finds out,
        ///         rather than by reinterpreting it.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ID3D12GraphicsCommandList *native_command_list(
            const rhi::CommandEncoder &encoder) const noexcept;

      private:
        IDXGIFactory6 *factory_ = nullptr;
        IDXGIAdapter4 *adapter_ = nullptr;
        ID3D12Device *device_ = nullptr;
        ID3D12CommandQueue *graphics_queue_ = nullptr;
        void *queue_lookup_context_ = nullptr;
        NativeQueueLookup queue_lookup_ = nullptr;
    };

} // namespace SFT::D3D12
