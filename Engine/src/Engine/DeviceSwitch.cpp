#include <Engine/DeviceSwitch.hpp>

#include <algorithm>
#include <array>
#include <format>

namespace SFT::Engine {

    namespace {

        constexpr std::array<RHI::BackendType, 4> backend_priority{
            RHI::BackendType::Vulkan, RHI::BackendType::D3D12, RHI::BackendType::Metal, RHI::BackendType::WebGpu};

        [[nodiscard]] bool gpu_supports(const RHI::PhysicalGpu &gpu, RHI::BackendType backend) {
            return std::ranges::any_of(gpu.api_support,
                                       [backend](const RHI::GpuApiSupport &api) { return api.adapter.backend == backend; });
        }

        [[nodiscard]] bool passes_constraint(const RHI::PhysicalGpu &gpu, DevicePreference preference) {
            switch (preference) {
                case DevicePreference::DiscreteOnly: return gpu.device_type == RHI::DeviceType::DiscreteGpu;
                case DevicePreference::IntegratedOnly: return gpu.device_type == RHI::DeviceType::IntegratedGpu;
                default: return true;
            }
        }

        [[nodiscard]] RHI::PowerPreference power_for(DevicePreference preference) {
            return preference == DevicePreference::LowPower || preference == DevicePreference::IntegratedOnly
                       ? RHI::PowerPreference::LowPower
                       : RHI::PowerPreference::HighPerformance;
        }

        [[nodiscard]] const char *preference_name(DevicePreference preference) {
            switch (preference) {
                case DevicePreference::Default: return "default";
                case DevicePreference::HighPerformance: return "high-performance";
                case DevicePreference::LowPower: return "low-power";
                case DevicePreference::DiscreteOnly: return "discrete-only";
                case DevicePreference::IntegratedOnly: return "integrated-only";
            }
            return "default";
        }

        /// Best GPU by the preference's ranking among those passing `accept`; the inventory order breaks ties
        /// (`max_element` keeps the first of equals).
        template <typename Accept>
        [[nodiscard]] optional<usize> best_gpu(const RHI::GpuInventory &inventory, DevicePreference preference, Accept accept) {
            optional<usize> best;
            i64 best_score = 0;
            for (usize i = 0; i < inventory.gpus.size(); ++i) {
                const RHI::PhysicalGpu &gpu = inventory.gpus[i];
                if (!passes_constraint(gpu, preference) || !accept(gpu)) {
                    continue;
                }
                const i64 score = RHI::score_adapter(gpu.device_type, power_for(preference));
                if (!best || score > best_score) {
                    best = i;
                    best_score = score;
                }
            }
            return best;
        }

        [[nodiscard]] bool id_matches(const RHI::PhysicalGpu &gpu, const string &id) {
            return adapter_stable_id(gpu) == id || (!gpu.physical_device_id.empty() && gpu.physical_device_id == id);
        }

        /// Backend for `gpu` when the caller did not insist on one: the current one if supported, else the first
        /// supported in priority order.
        [[nodiscard]] optional<RHI::BackendType> auto_backend(const RHI::PhysicalGpu &gpu, RHI::BackendType current) {
            if (gpu_supports(gpu, current)) {
                return current;
            }
            for (RHI::BackendType backend : backend_priority) {
                if (gpu_supports(gpu, backend)) {
                    return backend;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] DeviceResolution failure(string message) {
            DeviceResolution out;
            out.error = std::move(message);
            return out;
        }

    } // namespace

    string adapter_stable_id(const RHI::PhysicalGpu &gpu) {
        u64 hash = 0xcbf29ce484222325ull;
        const auto mix = [&hash](u8 byte) {
            hash ^= byte;
            hash *= 0x100000001b3ull;
        };
        for (u32 value : {gpu.vendor_id, gpu.device_id}) {
            for (int shift = 0; shift < 32; shift += 8) {
                mix(static_cast<u8>(value >> shift));
            }
        }
        for (char c : gpu.name) {
            mix(static_cast<u8>(c));
        }
        return std::format("{:016x}", hash);
    }

    vector<AdapterDescription> describe_adapters(const RHI::GpuInventory &inventory,
                                                 optional<RHI::BackendType> active_backend,
                                                 string_view active_physical_device_id) {
        vector<AdapterDescription> out;
        out.reserve(inventory.gpus.size());
        for (const RHI::PhysicalGpu &gpu : inventory.gpus) {
            AdapterDescription description;
            description.stable_id = adapter_stable_id(gpu);
            description.physical_device_id = gpu.physical_device_id;
            description.name = gpu.name;
            description.vendor = gpu.vendor;
            description.vendor_id = gpu.vendor_id;
            description.device_id = gpu.device_id;
            description.device_type = gpu.device_type;
            const bool is_running_gpu = active_backend.has_value() && !active_physical_device_id.empty() &&
                                        gpu.physical_device_id == active_physical_device_id;
            for (const RHI::GpuApiSupport &api : gpu.api_support) {
                BackendOffer offer;
                offer.backend = api.adapter.backend;
                offer.driver_version = api.adapter.driver_version;
                offer.api_version = api.adapter.api_version;
                offer.active = is_running_gpu && api.adapter.backend == *active_backend;
                if (offer.active) {
                    offer.presentable = AdapterPresentability::Yes;
                }
                description.backends.push_back(std::move(offer));
            }
            out.push_back(std::move(description));
        }
        return out;
    }

    vector<RHI::BackendType> available_backends(const RHI::GpuInventory &inventory) {
        vector<RHI::BackendType> out;
        for (RHI::BackendType backend : backend_priority) {
            if (std::ranges::any_of(inventory.gpus, [backend](const RHI::PhysicalGpu &gpu) { return gpu_supports(gpu, backend); })) {
                out.push_back(backend);
            }
        }
        return out;
    }

    DeviceResolution resolve_device_request(const RHI::GpuInventory &inventory,
                                            const DeviceRequest &request,
                                            RHI::BackendType current_backend) {
        if (inventory.gpus.empty()) {
            return failure("No graphics adapters were discovered.");
        }
        const auto supports_requested = [&](const RHI::PhysicalGpu &gpu) {
            return !request.backend || gpu_supports(gpu, *request.backend);
        };

        DeviceResolution out;
        optional<usize> chosen;

        if (request.adapter_id && !request.adapter_id->empty()) {
            for (usize i = 0; i < inventory.gpus.size(); ++i) {
                if (id_matches(inventory.gpus[i], *request.adapter_id)) {
                    const RHI::PhysicalGpu &gpu = inventory.gpus[i];
                    if (!passes_constraint(gpu, request.preference)) {
                        out.fallback_reason = std::format("Saved adapter '{}' does not satisfy the {} constraint.",
                                                          gpu.name, preference_name(request.preference));
                    } else if (!supports_requested(gpu)) {
                        out.fallback_reason = std::format("Saved adapter '{}' does not support {}.", gpu.name,
                                                          RHI::backend_type_name(*request.backend));
                    } else {
                        chosen = i;
                    }
                    break;
                }
            }
            if (!chosen && out.fallback_reason.empty()) {
                out.fallback_reason = std::format("Saved adapter '{}' is no longer available.", *request.adapter_id);
            }
        }

        if (!chosen) {
            chosen = best_gpu(inventory, request.preference, supports_requested);
            if (!chosen) {
                if (request.backend) {
                    return failure(std::format("No {} adapter satisfies the {} preference.",
                                               RHI::backend_type_name(*request.backend), preference_name(request.preference)));
                }
                return failure(std::format("No adapter satisfies the {} preference.", preference_name(request.preference)));
            }
        }

        const RHI::PhysicalGpu &gpu = inventory.gpus[*chosen];
        const optional<RHI::BackendType> backend = request.backend ? request.backend : auto_backend(gpu, current_backend);
        if (!backend) {
            return failure(std::format("Adapter '{}' exposes no usable graphics API.", gpu.name));
        }
        out.ok = true;
        out.gpu_index = *chosen;
        out.backend = *backend;
        return out;
    }

} // namespace SFT::Engine
