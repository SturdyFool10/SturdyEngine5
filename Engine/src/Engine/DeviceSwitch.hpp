#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <optional>
#include <string>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>

using std::optional;
using std::string;
using std::vector;

namespace SFT::Engine {

    /// Which kind of adapter a device request prefers when no explicit adapter is named (or the named one is gone).
    enum class DevicePreference : u8 {
        /// The engine's default ranking (same as `HighPerformance`).
        Default,
        HighPerformance,
        LowPower,
        /// Only discrete GPUs are acceptable; no match is a failure, never a downgrade.
        DiscreteOnly,
        /// Only integrated GPUs are acceptable; no match is a failure, never a downgrade.
        IntegratedOnly,
    };

    /// What a caller wants the live device to become. Every field is optional so a settings screen can change one
    /// thing (say the API) and leave the rest on whatever is running.
    struct DeviceRequest {
        /// `nullopt` = Auto: keep the running backend when the chosen adapter supports it, else the first it does.
        optional<RHI::BackendType> backend = std::nullopt;
        /// A saved adapter: either `AdapterDescription::stable_id` (survives driver updates and backend changes)
        /// or the backend-specific `physical_device_id`. When it no longer exists the request falls back to
        /// `preference` and the result is `SwitchedWithFallback` rather than a failure.
        optional<string> adapter_id = std::nullopt;
        DevicePreference preference = DevicePreference::Default;
        /// Rebuild the device even when the request resolves to the one already running (a manual "reset graphics",
        /// or after a driver update the app wants picked up).
        bool force_recreate = false;
    };

    enum class DeviceSwitchOutcome : u8 {
        /// The device now matches the request exactly.
        Switched,
        /// A device was created, but not the exact one asked for; `reason` says why.
        SwitchedWithFallback,
        /// Nothing changed (or the previous device was restored); `reason` says why. Never leaves a dead window.
        Failed,
    };

    /// Whether an adapter can present to a window surface.
    enum class AdapterPresentability : u8 {
        /// Not probed: answering needs a surface on that adapter's own instance, which the engine only has for
        /// the running device.
        Unknown,
        Yes,
        No,
    };

    /// One graphics API an adapter can be driven through.
    struct BackendOffer {
        RHI::BackendType backend = RHI::BackendType::Vulkan;
        string driver_version;
        string api_version;
        AdapterPresentability presentable = AdapterPresentability::Unknown;
        /// This offer is the device the engine is running on right now.
        bool active = false;
    };

    /// One physical GPU, as a settings screen lists it. Built from the startup inventory; creates no device.
    struct AdapterDescription {
        /// Hash of vendor id, device id and name: stable across backends, driver updates and reboots. Persist this.
        string stable_id;
        /// The engine's own per-GPU identifier (device UUID / LUID); not stable across every driver change.
        string physical_device_id;
        string name;
        string vendor;
        u32 vendor_id = 0;
        u32 device_id = 0;
        RHI::DeviceType device_type = RHI::DeviceType::Other;
        vector<BackendOffer> backends;
    };

    struct DeviceSwitchResult {
        DeviceSwitchOutcome outcome = DeviceSwitchOutcome::Failed;
        /// Empty for an exact `Switched`; otherwise why the request was relaxed or refused.
        string reason;
        /// The backend and adapter running after the call (the previous ones on `Failed`).
        RHI::BackendType backend = RHI::BackendType::Vulkan;
        string adapter_name;
        string adapter_stable_id;
        /// `Engine::device_generation()` after the call. Changes exactly when GPU handles the caller created itself
        /// (raw RHI buffers, pipelines, ...) stopped being valid; engine-managed handles are re-created in place.
        u64 generation = 0;
        /// A replacement was attempted, failed, and the previous device was rebuilt.
        bool rolled_back = false;
    };

    /// Why the device changed under the app; delivered to `Engine::on_device_change` observers.
    enum class DeviceChangeKind : u8 {
        /// About to be replaced: the old device is still alive, release GPU objects tied to it now.
        Switching,
        /// The replacement is live (or the old one was restored after a failed switch).
        Switched,
        /// The driver reported device loss and the engine rebuilt the device on the same request.
        Recovered,
        /// Device loss that could not be recovered; the renderer is not usable until the next successful switch.
        LostUnrecoverable,
    };

    struct DeviceChangeEvent {
        DeviceChangeKind kind = DeviceChangeKind::Switched;
        u64 generation = 0;
        RHI::BackendType backend = RHI::BackendType::Vulkan;
        string adapter_name;
        string message;
    };

    /// Result of picking a GPU and API from an inventory; pure, so it is testable without any GPU.
    struct DeviceResolution {
        bool ok = false;
        /// Index into the inventory's `gpus`; valid when `ok`.
        usize gpu_index = 0;
        RHI::BackendType backend = RHI::BackendType::Vulkan;
        /// Non-empty when the exact request could not be honoured and a fallback was chosen.
        string fallback_reason;
        /// Non-empty when `!ok`.
        string error;
    };

    /// The stable identity of `gpu`: FNV-1a over vendor id, device id and name, as 16 hex digits.
    [[nodiscard]] string adapter_stable_id(const RHI::PhysicalGpu &gpu);

    /// Lists `inventory` for a settings screen. `active_backend`/`active_physical_device_id` mark the running
    /// device (`active_physical_device_id` empty = none), which is the one adapter known to present.
    [[nodiscard]] vector<AdapterDescription> describe_adapters(const RHI::GpuInventory &inventory,
                                                               optional<RHI::BackendType> active_backend,
                                                               string_view active_physical_device_id);

    /// Backends at least one GPU in `inventory` can be driven through, in `BackendType` order.
    [[nodiscard]] vector<RHI::BackendType> available_backends(const RHI::GpuInventory &inventory);

    /// Resolves `request` against `inventory`. `current_backend` is what Auto keeps when possible.
    /// Relaxations (saved adapter gone, saved adapter lacks the requested API) are reported through
    /// `fallback_reason`; an explicitly requested backend that no usable GPU supports, or a `DiscreteOnly` /
    /// `IntegratedOnly` constraint nothing satisfies, is an error rather than a silent downgrade.
    [[nodiscard]] DeviceResolution resolve_device_request(const RHI::GpuInventory &inventory,
                                                          const DeviceRequest &request,
                                                          RHI::BackendType current_backend);

} // namespace SFT::Engine
