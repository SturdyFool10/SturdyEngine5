#include "Inventory.hpp"

namespace SFT::RHI {

    namespace {

        [[nodiscard]] GpuApiSupport snapshot_adapter(const RhiAdapter &adapter) {
            GpuApiSupport support{};
            support.adapter = adapter.info();
            support.supported_features = adapter.supported_features();
            support.feature_properties = adapter.feature_properties();
            support.limits = adapter.limits();
            support.supported_extensions.assign(adapter.supported_extensions().begin(), adapter.supported_extensions().end());
            support.queues.assign(adapter.queue_infos().begin(), adapter.queue_infos().end());
            return support;
        }

        [[nodiscard]] PhysicalGpu *find_physical_gpu(GpuInventory &inventory, const AdapterInfo &adapter) {
            if (adapter.physical_device_id.empty()) {
                return nullptr;
            }
            for (PhysicalGpu &gpu : inventory.gpus) {
                if (gpu.physical_device_id == adapter.physical_device_id) {
                    return &gpu;
                }
            }
            return nullptr;
        }

        void add_adapter(GpuInventory &inventory, const RhiAdapter &adapter) {
            GpuApiSupport support = snapshot_adapter(adapter);
            PhysicalGpu *gpu = find_physical_gpu(inventory, support.adapter);
            if (gpu == nullptr) {
                PhysicalGpu discovered{};
                discovered.name = support.adapter.name;
                discovered.vendor = support.adapter.vendor;
                discovered.device_type = support.adapter.device_type;
                discovered.vendor_id = support.adapter.vendor_id;
                discovered.device_id = support.adapter.device_id;
                discovered.physical_device_id = support.adapter.physical_device_id;
                inventory.gpus.push_back(std::move(discovered));
                gpu = &inventory.gpus.back();
            }
            gpu->api_support.push_back(std::move(support));
        }

    } // namespace

    GpuInventory enumerate_gpu_inventory(const BackendRegistry &registry, const InstanceDesc &instance_desc) {
        GpuInventory inventory{};
        for (const BackendRegistration &registration : registry.backends()) {
            RhiExpected<unique_ptr<RhiInstance>> instance = registry.create_instance(registration.backend, instance_desc);
            if (!instance) {
                inventory.failures.push_back({.backend = registration.backend, .message = instance.error().message});
                continue;
            }

            RhiExpected<vector<unique_ptr<RhiAdapter>>> adapters = (*instance)->enumerate_adapters();
            if (!adapters) {
                inventory.failures.push_back({.backend = registration.backend, .message = adapters.error().message});
                continue;
            }
            for (const unique_ptr<RhiAdapter> &adapter : *adapters) {
                if (adapter != nullptr) {
                    add_adapter(inventory, *adapter);
                }
            }
        }
        return inventory;
    }

} // namespace SFT::RHI
