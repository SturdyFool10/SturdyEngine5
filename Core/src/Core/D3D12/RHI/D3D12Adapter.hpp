#pragma once


#include <Core/D3D12/RHI/D3D12Common.hpp>

#pragma region Imports
#include <memory>
#include <vector>
#pragma endregion

namespace SFT::D3D12 {

    using std::unique_ptr;
    using std::vector;


    struct DeviceCapabilities {
        rhi::FeatureSet features;
        rhi::FeatureProperties properties;
        rhi::DeviceLimits limits;
        vector<rhi::QueueInfo> queue_infos;


        bool enhanced_barriers = false;


        bool allow_tearing = false;


        bool pipeline_library_supported = false;


        bool gpu_upload_heap_supported = false;
    };

    /// Performs the probe capabilities operation using the supplied arguments.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] DeviceCapabilities probe_capabilities(ID3D12Device *device);

    class D3D12Adapter final : public rhi::RhiAdapter {
      public:


        /// Constructs a `D3D12Adapter` from the supplied initialization values.
        ///
        /// @param factory `factory` value used by the operation.
        /// @param adapter `adapter` value used by the operation.
        /// @param device Device used or affected by the operation.
        /// @param info Description of the resource or operation to perform.
        /// @param capabilities `capabilities` value used by the operation.
        /// @param debug_layer_enabled `debug_layer_enabled` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        D3D12Adapter(ComPtr<IDXGIFactory6> factory, ComPtr<IDXGIAdapter4> adapter, ComPtr<ID3D12Device> device, rhi::AdapterInfo info, DeviceCapabilities capabilities, bool debug_layer_enabled);

        /// Returns the current or globally available info value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const rhi::AdapterInfo &info() const noexcept override;
        /// Returns the current or globally available supported features value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const rhi::FeatureSet &supported_features() const noexcept override;
        /// Returns the current or globally available feature properties value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const rhi::FeatureProperties &feature_properties() const noexcept override;
        /// Returns the current or globally available supported extensions value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] span<const rhi::ExtensionId> supported_extensions() const noexcept override;
        /// Returns the current or globally available queue infos value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] span<const rhi::QueueInfo> queue_infos() const noexcept override;
        /// Returns the current or globally available limits value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const rhi::DeviceLimits &limits() const noexcept override;
        /// Creates a device from the supplied parameters.
        ///
        /// @param request `request` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::RhiDevice>> create_device(const rhi::DeviceRequest &request) override;

      private:
        ComPtr<IDXGIFactory6> factory_;
        ComPtr<IDXGIAdapter4> adapter_;
        ComPtr<ID3D12Device> device_;
        rhi::AdapterInfo info_{};
        DeviceCapabilities capabilities_{};
        vector<rhi::ExtensionId> supported_extensions_;
        bool debug_layer_enabled_ = false;
    };

    class D3D12Instance final : public rhi::RhiInstance {
      public:
        /// Constructs a `D3D12Instance` from the supplied initialization values.
        ///
        /// @param factory `factory` value used by the operation.
        /// @param debug_layer_enabled `debug_layer_enabled` value used by the operation.
        /// @param allow_tearing `allow_tearing` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        D3D12Instance(ComPtr<IDXGIFactory6> factory, bool debug_layer_enabled, bool allow_tearing);

        /// Returns the current or globally available backend type value.
        ///
        /// @return Returns the current backend type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] rhi::BackendType backend_type() const noexcept override;
        /// Enumerates adapters using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<vector<unique_ptr<rhi::RhiAdapter>>> enumerate_adapters() override;

      private:
        ComPtr<IDXGIFactory6> factory_;
        bool debug_layer_enabled_ = false;
        bool allow_tearing_ = false;
    };


    /// Creates a D3D12 instance from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::RhiInstance>> create_d3d12_instance(const rhi::InstanceDesc &desc);


    /// Returns the current or globally available D3D12 backend registration value.
    ///
    /// @return Returns the current D3D12 backend registration value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] rhi::BackendRegistration d3d12_backend_registration() noexcept;

} // namespace SFT::D3D12
