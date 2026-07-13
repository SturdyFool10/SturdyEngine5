module;
#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cctype>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>
#pragma endregion

export module Sturdy.RHI:Selection;

import :Error;
import :Flags;
import :Features;
import :Extensions;
import :Device;  // AdapterInfo, DeviceType, BackendType, RhiDevice
import :Adapter; // RhiAdapter, RhiInstance, InstanceDesc, DeviceRequest
import :Backend; // BackendRegistry

using std::optional;
using std::span;
using std::string_view;
using std::unexpected;
using std::unique_ptr;
using std::vector;

export namespace SFT::RHI {

    // ─── Adapter selection ───────────────────────────────────────────────────────
    //
    // Given the adapters an instance enumerated, this is how a renderer decides which GPU to run on.
    // Four intents compose in one `AdapterCriteria`, each independently useful:
    //   1. Filter by capability — `required_features` / `required_extensions` drop adapters that
    //      can't run the renderer at all (the "needs hardware ray tracing" gate).
    //   2. Exclude device categories — `allowed_types` is a bitmask; the default `AllHardware`
    //      automatically excludes software (`Cpu`) adapters, and a caller can widen or narrow it.
    //   3. Explicit by name — `name_filter` keeps only adapters whose name contains the string
    //      (case-insensitive), for honoring a user's saved "use my RTX 4090" choice.
    //   4. Power preference — among everything that survived the filters, `power_preference` ranks
    //      the survivors (high-performance favors discrete, low-power favors integrated), the same
    //      knob WebGPU's requestAdapter exposes.

    enum class PowerPreference : u32 {
        None,            // no preference — a mild discrete-first ranking
        LowPower,        // favor integrated GPUs (battery/thermals)
        HighPerformance, // favor discrete GPUs (throughput)
    };

    // A bitmask over `DeviceType` for allow/deny filtering. `AllHardware` is every category except the
    // software rasterizer — the sane default that keeps a renderer off lavapipe/WARP unless it opts in.
    enum class DeviceTypeMask : u32 {
        None = 0,
        Other = 1u << 0,
        IntegratedGpu = 1u << 1,
        DiscreteGpu = 1u << 2,
        VirtualGpu = 1u << 3,
        Cpu = 1u << 4,
        AllHardware = Other | IntegratedGpu | DiscreteGpu | VirtualGpu,
        All = AllHardware | Cpu,
    };
    template <>
    struct enable_flag_ops<DeviceTypeMask> : std::true_type {};

    // The single-category bit for a device type — used to test membership in an allow mask.
    [[nodiscard]] constexpr DeviceTypeMask device_type_bit(DeviceType type) noexcept {
        switch (type) {
            case DeviceType::Other: return DeviceTypeMask::Other;
            case DeviceType::IntegratedGpu: return DeviceTypeMask::IntegratedGpu;
            case DeviceType::DiscreteGpu: return DeviceTypeMask::DiscreteGpu;
            case DeviceType::VirtualGpu: return DeviceTypeMask::VirtualGpu;
            case DeviceType::Cpu: return DeviceTypeMask::Cpu;
        }
        return DeviceTypeMask::Other;
    }

    struct AdapterCriteria {
        PowerPreference power_preference = PowerPreference::HighPerformance;
        // Hard capability gates — an adapter missing any required feature/extension is rejected.
        FeatureSet required_features;
        span<const ExtensionId> required_extensions;
        // Device categories permitted; defaults to hardware-only (software adapters excluded).
        DeviceTypeMask allowed_types = DeviceTypeMask::AllHardware;
        // Case-insensitive substring an adapter's name must contain; empty means "any name".
        string_view name_filter;
    };

    // Case-insensitive substring test (ASCII), used for `name_filter`.
    [[nodiscard]] inline bool name_contains_ci(string_view haystack, string_view needle) noexcept {
        if (needle.empty()) {
            return true;
        }
        if (needle.size() > haystack.size()) {
            return false;
        }
        auto lower = [](char c) noexcept {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        for (usize start = 0; start + needle.size() <= haystack.size(); ++start) {
            bool matched = true;
            for (usize i = 0; i < needle.size(); ++i) {
                if (lower(haystack[start + i]) != lower(needle[i])) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                return true;
            }
        }
        return false;
    }

    // Does `adapter` pass every hard filter in `criteria` (type, name, features, extensions)? This is
    // the yes/no gate; `score_adapter` then ranks the ones that pass.
    [[nodiscard]] inline bool adapter_matches(const RhiAdapter &adapter, const AdapterCriteria &criteria) {
        const AdapterInfo &info = adapter.info();
        if (!has_any(criteria.allowed_types, device_type_bit(info.device_type))) {
            return false;
        }
        if (!name_contains_ci(info.name, criteria.name_filter)) {
            return false;
        }
        if (!adapter.supported_features().contains_all(criteria.required_features)) {
            return false;
        }
        for (const ExtensionId &extension : criteria.required_extensions) {
            if (!contains_extension(adapter.supported_extensions(), extension)) {
                return false;
            }
        }
        return true;
    }

    // Ranks an adapter for a power preference; higher wins. Only meaningful among adapters that have
    // already passed `adapter_matches`.
    [[nodiscard]] constexpr i64 score_adapter(DeviceType type, PowerPreference preference) noexcept {
        switch (preference) {
            case PowerPreference::HighPerformance:
                switch (type) {
                    case DeviceType::DiscreteGpu: return 1000;
                    case DeviceType::IntegratedGpu: return 500;
                    case DeviceType::VirtualGpu: return 200;
                    case DeviceType::Other: return 100;
                    case DeviceType::Cpu: return 10;
                }
                break;
            case PowerPreference::LowPower:
                switch (type) {
                    case DeviceType::IntegratedGpu: return 1000;
                    case DeviceType::DiscreteGpu: return 500;
                    case DeviceType::VirtualGpu: return 200;
                    case DeviceType::Other: return 100;
                    case DeviceType::Cpu: return 10;
                }
                break;
            case PowerPreference::None:
                switch (type) {
                    case DeviceType::DiscreteGpu: return 800;
                    case DeviceType::IntegratedGpu: return 700;
                    case DeviceType::VirtualGpu: return 200;
                    case DeviceType::Other: return 100;
                    case DeviceType::Cpu: return 10;
                }
                break;
        }
        return 0;
    }

    // The single best adapter matching `criteria`, or nullptr if none qualifies. The returned pointer
    // aliases into `adapters`, so keep that vector alive while using it (create the device, then the
    // adapters may drop — an adapter need not outlive the device it creates).
    [[nodiscard]] inline RhiAdapter *select_adapter(span<const unique_ptr<RhiAdapter>> adapters,
                                                    const AdapterCriteria &criteria) {
        RhiAdapter *best = nullptr;
        i64 best_score = std::numeric_limits<i64>::min();
        for (const unique_ptr<RhiAdapter> &adapter : adapters) {
            if (adapter == nullptr || !adapter_matches(*adapter, criteria)) {
                continue;
            }
            const i64 score = score_adapter(adapter->info().device_type, criteria.power_preference);
            if (score > best_score) {
                best_score = score;
                best = adapter.get();
            }
        }
        return best;
    }

    // Every adapter matching `criteria`, ranked best-first — for presenting a chooser UI or falling
    // back down the list. Pointers alias into `adapters` (same lifetime rule as select_adapter).
    [[nodiscard]] inline vector<RhiAdapter *> filter_adapters(span<const unique_ptr<RhiAdapter>> adapters,
                                                              const AdapterCriteria &criteria) {
        vector<RhiAdapter *> matched;
        for (const unique_ptr<RhiAdapter> &adapter : adapters) {
            if (adapter != nullptr && adapter_matches(*adapter, criteria)) {
                matched.push_back(adapter.get());
            }
        }
        const PowerPreference preference = criteria.power_preference;
        std::stable_sort(matched.begin(), matched.end(), [preference](RhiAdapter *a, RhiAdapter *b) {
            return score_adapter(a->info().device_type, preference) >
                   score_adapter(b->info().device_type, preference);
        });
        return matched;
    }

    // Explicit-by-name lookup: the first adapter whose name contains `name` (case-insensitive), or
    // nullptr. For honoring a saved user choice without building a whole criteria struct.
    [[nodiscard]] inline RhiAdapter *find_adapter_by_name(span<const unique_ptr<RhiAdapter>> adapters,
                                                          string_view name) {
        for (const unique_ptr<RhiAdapter> &adapter : adapters) {
            if (adapter != nullptr && name_contains_ci(adapter->info().name, name)) {
                return adapter.get();
            }
        }
        return nullptr;
    }

    // ─── One-call selection ──────────────────────────────────────────────────────
    //
    // Ties the whole path together: choose a backend (explicit or the registry's preferred), create
    // its instance, enumerate adapters, pick one by criteria, and realize a device — surfacing a
    // precise error at whichever step fails.

    struct DeviceSelection {
        // Which graphics API to use. nullopt asks the registry for its preferred available backend —
        // the runtime D3D12-vs-Vulkan choice lives here (set it to force one).
        optional<BackendType> backend;
        InstanceDesc instance;
        AdapterCriteria adapter;
        DeviceRequest device;
    };

    // The realized result. `instance` is retained because a device must not outlive the instance it
    // came from; the chosen adapter is not (it isn't needed past device creation) so only its info is
    // kept, for logging/telemetry.
    struct SelectedDevice {
        unique_ptr<RhiInstance> instance;
        unique_ptr<RhiDevice> device;
        AdapterInfo adapter_info;
        BackendType backend = BackendType::Vulkan;
    };

    [[nodiscard]] inline RhiExpected<SelectedDevice> select_and_create_device(
        const BackendRegistry &registry, const DeviceSelection &selection) {
        optional<BackendType> backend = selection.backend;
        if (!backend.has_value()) {
            backend = registry.preferred_backend();
        }
        if (!backend.has_value()) {
            return rhi_error(RhiErrorCode::Unsupported, "No RHI graphics backend is available.");
        }

        RhiExpected<unique_ptr<RhiInstance>> instance_result = registry.create_instance(*backend, selection.instance);
        if (!instance_result.has_value()) {
            return unexpected(instance_result.error());
        }
        unique_ptr<RhiInstance> instance = std::move(*instance_result);

        RhiExpected<vector<unique_ptr<RhiAdapter>>> adapters_result = instance->enumerate_adapters();
        if (!adapters_result.has_value()) {
            return unexpected(adapters_result.error());
        }
        vector<unique_ptr<RhiAdapter>> adapters = std::move(*adapters_result);

        RhiAdapter *chosen = select_adapter(adapters, selection.adapter);
        if (chosen == nullptr) {
            return rhi_error(RhiErrorCode::Unsupported,
                             "No adapter matched the selection criteria on the chosen backend.");
        }

        AdapterInfo info = chosen->info();
        RhiExpected<unique_ptr<RhiDevice>> device_result = chosen->create_device(selection.device);
        if (!device_result.has_value()) {
            return unexpected(device_result.error());
        }

        SelectedDevice out;
        out.instance = std::move(instance);
        out.device = std::move(*device_result);
        out.adapter_info = std::move(info);
        out.backend = *backend;
        return out;
    }

} // namespace SFT::RHI
