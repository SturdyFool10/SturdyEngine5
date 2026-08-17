#pragma once


#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <string>
#include <vector>
#pragma endregion

#include "Adapter.hpp"
#include "Backend.hpp"

using std::string;
using std::vector;

namespace SFT::RHI {


    struct GpuApiSupport {
        AdapterInfo adapter;
        FeatureSet supported_features;
        FeatureProperties feature_properties;
        DeviceLimits limits;
        vector<ExtensionId> supported_extensions;
        vector<QueueInfo> queues;
    };


    struct PhysicalGpu {
        string name;
        string vendor;
        DeviceType device_type = DeviceType::Other;
        u32 vendor_id = 0;
        u32 device_id = 0;
        string physical_device_id;
        vector<GpuApiSupport> api_support;
    };


    struct BackendDiscoveryFailure {
        BackendType backend = BackendType::Vulkan;
        string message;
    };

    struct GpuInventory {
        vector<PhysicalGpu> gpus;
        vector<BackendDiscoveryFailure> failures;
    };


    /// Enumerates GPU inventory using the supplied arguments and current state.
    ///
    /// @param registry `registry` value used by the operation.
    /// @param instance Instance used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] GpuInventory enumerate_gpu_inventory(const BackendRegistry &registry, const InstanceDesc &instance);

} // namespace SFT::RHI
