#include "Selection.hpp"

namespace SFT::RHI {

/// Performs the name contains ci operation for `RHI` using the supplied arguments.
///
/// @param haystack `haystack` value used by the operation.
/// @param needle `needle` value used by the operation.
///
/// @return Returns the boolean result of the operation.
/// @note This function does not throw exceptions.
bool name_contains_ci(string_view haystack, string_view needle) noexcept {
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

/// Performs the adapter matches operation for `RHI` using the supplied arguments.
///
/// @param adapter `adapter` value used by the operation.
/// @param criteria `criteria` value used by the operation.
///
/// @return Returns the boolean result of the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
bool adapter_matches(const RhiAdapter &adapter, const AdapterCriteria &criteria) {
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

/// Selects adapter that best satisfies the supplied requirements.
///
/// @param adapters `adapters` value used by the operation.
/// @param criteria `criteria` value used by the operation.
///
/// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RhiAdapter *select_adapter(span<const unique_ptr<RhiAdapter>> adapters,
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

/// Performs the filter adapters operation for `RHI` using the supplied arguments.
///
/// @param adapters `adapters` value used by the operation.
/// @param criteria `criteria` value used by the operation.
///
/// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
vector<RhiAdapter *> filter_adapters(span<const unique_ptr<RhiAdapter>> adapters,
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

/// Returns a human-readable name for the supplied find adapter by value.
///
/// @param adapters `adapters` value used by the operation.
/// @param name Name used to identify or label the target.
///
/// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
/// @note Absence is represented by a null pointer rather than an exception.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RhiAdapter *find_adapter_by_name(span<const unique_ptr<RhiAdapter>> adapters,
                                                          string_view name) {
        for (const unique_ptr<RhiAdapter> &adapter : adapters) {
            if (adapter != nullptr && name_contains_ci(adapter->info().name, name)) {
                return adapter.get();
            }
        }
        return nullptr;
    }

/// Selects and create device that best satisfies the supplied requirements.
///
/// @param registry `registry` value used by the operation.
/// @param selection `selection` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`.
RhiExpected<SelectedDevice> select_and_create_device(
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
