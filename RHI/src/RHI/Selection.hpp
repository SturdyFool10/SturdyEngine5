#pragma once

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

#include <RHI/Error.hpp>
#include <RHI/Flags.hpp>
#include <RHI/Features.hpp>
#include <RHI/Extensions.hpp>
#include <RHI/Device.hpp>
#include <RHI/Adapter.hpp>
#include <RHI/Backend.hpp>

using std::optional;
using std::span;
using std::string_view;
using std::unexpected;
using std::unique_ptr;
using std::vector;

namespace SFT::RHI {


    enum class PowerPreference : u32 {
        None,
        LowPower,
        HighPerformance,
    };


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


    /// Performs the device type bit operation using the supplied arguments.
    ///
    /// @param type Type value to inspect, select, or convert.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

        FeatureSet required_features;
        span<const ExtensionId> required_extensions;

        DeviceTypeMask allowed_types = DeviceTypeMask::AllHardware;

        string_view name_filter;
    };


    /// Performs the name contains ci operation using the supplied arguments.
    ///
    /// @param haystack `haystack` value used by the operation.
    /// @param needle `needle` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool name_contains_ci(string_view haystack, string_view needle) noexcept;


    /// Performs the adapter matches operation using the supplied arguments.
    ///
    /// @param adapter `adapter` value used by the operation.
    /// @param criteria `criteria` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] bool adapter_matches(const RhiAdapter &adapter, const AdapterCriteria &criteria);


    /// Performs the score adapter operation using the supplied arguments.
    ///
    /// @param type Type value to inspect, select, or convert.
    /// @param preference `preference` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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


    /// Selects adapter that best satisfies the supplied requirements.
    ///
    /// @param adapters `adapters` value used by the operation.
    /// @param criteria `criteria` value used by the operation.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] RhiAdapter *select_adapter(span<const unique_ptr<RhiAdapter>> adapters,
                                                    const AdapterCriteria &criteria);


    /// Performs the filter adapters operation using the supplied arguments.
    ///
    /// @param adapters `adapters` value used by the operation.
    /// @param criteria `criteria` value used by the operation.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<RhiAdapter *> filter_adapters(span<const unique_ptr<RhiAdapter>> adapters,
                                                              const AdapterCriteria &criteria);


    /// Returns a human-readable name for the supplied find adapter by value.
    ///
    /// @param adapters `adapters` value used by the operation.
    /// @param name Name used to identify or label the target.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] RhiAdapter *find_adapter_by_name(span<const unique_ptr<RhiAdapter>> adapters,
                                                          string_view name);


    struct DeviceSelection {


        optional<BackendType> backend;
        InstanceDesc instance;
        AdapterCriteria adapter;
        DeviceRequest device;
    };


    struct SelectedDevice {
        unique_ptr<RhiInstance> instance;
        unique_ptr<RhiDevice> device;
        AdapterInfo adapter_info;
        BackendType backend = BackendType::Vulkan;
    };

    /// Selects and create device that best satisfies the supplied requirements.
    ///
    /// @param registry `registry` value used by the operation.
    /// @param selection `selection` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] RhiExpected<SelectedDevice> select_and_create_device(
        const BackendRegistry &registry, const DeviceSelection &selection);

} // namespace SFT::RHI
