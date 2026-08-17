#pragma once

// The pre-device seam: a DXGI factory as `RhiInstance`, each IDXGIAdapter as `RhiAdapter`, and the
// capability probing that turns D3D12's tier/option model into the RHI's flat `FeatureSet`.
//
// The shape of that translation is worth stating, because it is the single biggest structural
// difference between this backend and the Vulkan one. Vulkan answers "is feature X supported" with a
// boolean per feature, so its mapping is one-to-one. D3D12 answers with **tiers and options
// structures** — D3D12_FEATURE_DATA_D3D12_OPTIONS through OPTIONS21, plus shader model and root
// signature version queries — where a single tier value implies a whole bundle of RHI features at
// once (ResourceBindingTier 3 implies unbounded descriptor arrays, dynamic indexing, and
// update-after-bind-equivalent behaviour together). So `probe_features()` below reads the options
// structures once and fans each tier out to every RHI `Feature` it actually guarantees, rather than
// pretending there is a bit per feature.
//
// Probing has to happen on a real device, not on the adapter: D3D12 has no
// vkGetPhysicalDeviceFeatures equivalent. `D3D12Adapter` therefore creates a throwaway device at
// construction to answer supported_features()/limits(), exactly as it must, and `create_device()`
// reuses that same device rather than creating a second one.

#include <D3D12/D3D12Common.hpp>

#pragma region Imports
#include <memory>
#include <vector>
#pragma endregion

namespace SFT::D3D12 {

    using std::unique_ptr;
    using std::vector;

    // Everything probed off a live ID3D12Device, in the RHI's own vocabulary.
    struct DeviceCapabilities {
        rhi::FeatureSet features;
        rhi::FeatureProperties properties;
        rhi::DeviceLimits limits;
        vector<rhi::QueueInfo> queue_infos;
        // Enhanced barriers are load-bearing enough to be worth hoisting out of the feature set: the
        // command encoder picks an entirely different barrier implementation on it.
        bool enhanced_barriers = false;
        // DXGI_FEATURE_PRESENT_ALLOW_TEARING — required for any real "VSync off" present. A property
        // of the *factory*, not the device, so it is filled in by the instance and carried through.
        bool allow_tearing = false;
        // D3D12_SHADER_CACHE_SUPPORT_LIBRARY (D3D12_FEATURE_SHADER_CACHE) — whether this device can
        // back an ID3D12PipelineLibrary, which D3D12Device uses as an on-disk PSO cache to avoid
        // recompiling every pipeline permutation cold on every run.
        bool pipeline_library_supported = false;
        // D3D12_FEATURE_DATA_D3D12_OPTIONS16::GPUUploadHeapSupported — Resizable-BAR-backed memory
        // that is simultaneously CPU-mappable and GPU-local. Hoisted out like enhanced_barriers
        // because create_buffer() picks a different heap type on it, not just a FeatureSet bit.
        bool gpu_upload_heap_supported = false;
    };

    [[nodiscard]] DeviceCapabilities probe_capabilities(ID3D12Device *device);

    class D3D12Adapter final : public rhi::RhiAdapter {
      public:
        // `device` is the probing device described in this header's comment — already created, already
        // the one create_device() will hand over.
        D3D12Adapter(ComPtr<IDXGIFactory6> factory, ComPtr<IDXGIAdapter4> adapter, ComPtr<ID3D12Device> device, rhi::AdapterInfo info, DeviceCapabilities capabilities, bool debug_layer_enabled);

        [[nodiscard]] const rhi::AdapterInfo &info() const noexcept override;
        [[nodiscard]] const rhi::FeatureSet &supported_features() const noexcept override;
        [[nodiscard]] const rhi::FeatureProperties &feature_properties() const noexcept override;
        [[nodiscard]] span<const rhi::ExtensionId> supported_extensions() const noexcept override;
        [[nodiscard]] span<const rhi::QueueInfo> queue_infos() const noexcept override;
        [[nodiscard]] const rhi::DeviceLimits &limits() const noexcept override;
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
        D3D12Instance(ComPtr<IDXGIFactory6> factory, bool debug_layer_enabled, bool allow_tearing);

        [[nodiscard]] rhi::BackendType backend_type() const noexcept override;
        [[nodiscard]] rhi::RhiExpected<vector<unique_ptr<rhi::RhiAdapter>>> enumerate_adapters() override;

      private:
        ComPtr<IDXGIFactory6> factory_;
        bool debug_layer_enabled_ = false;
        bool allow_tearing_ = false;
    };

    // The backend's instance factory — `InstanceFactory` in RHI/Backend.hpp. Enables the D3D12 debug
    // layer and DRED when `desc.enable_validation` is set (which must happen *before* any device is
    // created, hence here rather than at device creation).
    [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::RhiInstance>> create_d3d12_instance(const rhi::InstanceDesc &desc);

    // Ready-to-register descriptor for RHI::BackendRegistry. Registration remains application-owned,
    // matching the RHI's no-global-state policy.
    [[nodiscard]] rhi::BackendRegistration d3d12_backend_registration() noexcept;

} // namespace SFT::D3D12
