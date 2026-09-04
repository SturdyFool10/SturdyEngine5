#include <RHI/Inventory.hpp>

#include <algorithm>

namespace SFT::RHI {

    namespace {

        /// Performs the snapshot adapter operation for `RHI` using the supplied arguments.
        ///
        /// @param adapter `adapter` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

        /// Finds physical GPU in the available state.
        ///
        /// @param inventory `inventory` value used by the operation.
        /// @param adapter `adapter` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] PhysicalGpu *find_physical_gpu(GpuInventory &inventory, const AdapterInfo &adapter) {
            if (!adapter.physical_device_id.empty()) {
                for (PhysicalGpu &gpu : inventory.gpus) {
                    if (gpu.physical_device_id == adapter.physical_device_id) {
                        return &gpu;
                    }
                }
                return nullptr;
            }

            // No stable device identifier. Vulkan and D3D12 always have one (a device UUID or an
            // adapter LUID), but WebGPU deliberately exposes neither -- it reports only the PCI
            // vendor/device IDs and a name. Without a fallback the same physical GPU would appear
            // once per backend, which defeats the point of an inventory keyed on physical GPUs.
            //
            // Matching on vendor/device/name alone would be wrong: two identical cards share all
            // three. So a candidate only matches if it does not already list this adapter's
            // backend. Two identical cards found by one backend still produce two entries (the
            // first already has that backend, so the second cannot merge into it), and each is then
            // matched in turn by the next backend.
            for (PhysicalGpu &gpu : inventory.gpus) {
                const bool same_device = gpu.name == adapter.name &&
                                         !gpu.api_support.empty() &&
                                         gpu.api_support.front().adapter.vendor_id == adapter.vendor_id &&
                                         gpu.api_support.front().adapter.device_id == adapter.device_id;
                if (!same_device) {
                    continue;
                }
                const bool backend_already_listed =
                    std::any_of(gpu.api_support.begin(), gpu.api_support.end(),
                                [&adapter](const GpuApiSupport &support) {
                                    return support.adapter.backend == adapter.backend;
                                });
                if (!backend_already_listed) {
                    return &gpu;
                }
            }
            return nullptr;
        }

        /// Adds adapter using the supplied arguments and current state.
        ///
        /// @param inventory `inventory` value used by the operation.
        /// @param adapter `adapter` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Enumerates GPU inventory using the supplied arguments and current state.
    ///
    /// @param registry `registry` value used by the operation.
    /// @param instance_desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
