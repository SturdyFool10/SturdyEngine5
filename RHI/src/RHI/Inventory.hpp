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

    /// One API's view of a physical GPU. The feature and queue data are intentionally retained alongside
    /// AdapterInfo: an RTX card may be available through both Vulkan and D3D12, but the supported RHI
    /// feature sets need not be identical.
    struct GpuApiSupport {
        AdapterInfo adapter;
        FeatureSet supported_features;
        FeatureProperties feature_properties;
        DeviceLimits limits;
        vector<ExtensionId> supported_extensions;
        vector<QueueInfo> queues;
    };

    /// A physical GPU as seen across one or more APIs. `api_support` has one entry per successfully
    /// enumerated backend. Entries merge only when their non-empty AdapterInfo::physical_device_id
    /// values match exactly; this avoids falsely merging two identical installed GPUs.
    struct PhysicalGpu {
        string name;
        string vendor;
        DeviceType device_type = DeviceType::Other;
        u32 vendor_id = 0;
        u32 device_id = 0;
        string physical_device_id;
        vector<GpuApiSupport> api_support;
    };

    /// A registered API that could not be initialized or enumerate adapters. Discovery remains useful
    /// when one driver/loader is broken, so these are reported separately instead of failing the full scan.
    struct BackendDiscoveryFailure {
        BackendType backend = BackendType::Vulkan;
        string message;
    };

    struct GpuInventory {
        vector<PhysicalGpu> gpus;
        vector<BackendDiscoveryFailure> failures;
    };

    /// Enumerates every backend in `registry` and merges each adapter into a physical-GPU record. This
    /// performs no device creation and does not retain backend instances/adapters after returning.
    [[nodiscard]] GpuInventory enumerate_gpu_inventory(const BackendRegistry &registry, const InstanceDesc &instance);

} // namespace SFT::RHI
