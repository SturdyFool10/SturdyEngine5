//! Raw `cxx` bindings for live graphics-device changes (`Engine/DeviceSwitch.hpp`,
//! `Engine::switch_device`): enumerating what can be chosen without creating a device, replacing the
//! running device in place, and draining the change events (switches and automatic device-loss
//! recoveries).

#[cxx::bridge(namespace = "sturdy_rs::device")]
pub mod ffi {
    /// Mirrors `std::optional<RHI::BackendType>`: `Auto` is the empty optional.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum DeviceBackend {
        Auto,
        Vulkan,
        D3D12,
        Metal,
        WebGpu,
    }

    /// Mirrors `SFT::Engine::DevicePreference`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum DevicePreference {
        Default,
        HighPerformance,
        LowPower,
        DiscreteOnly,
        IntegratedOnly,
    }

    /// Mirrors `SFT::Engine::DeviceSwitchOutcome`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum DeviceSwitchOutcome {
        Switched,
        SwitchedWithFallback,
        Failed,
    }

    /// Mirrors `SFT::Engine::AdapterPresentability`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum AdapterPresentability {
        Unknown,
        Yes,
        No,
    }

    /// Mirrors `SFT::Engine::DeviceChangeKind`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum DeviceChangeKind {
        Switching,
        Switched,
        Recovered,
        LostUnrecoverable,
    }

    #[derive(Debug, Clone)]
    struct BackendOfferInfo {
        backend: DeviceBackend,
        driver_version: String,
        api_version: String,
        presentable: AdapterPresentability,
        active: bool,
    }

    #[derive(Debug, Clone)]
    struct AdapterInfo {
        stable_id: String,
        physical_device_id: String,
        name: String,
        vendor: String,
        vendor_id: u32,
        device_id: u32,
        device_type: String,
        backends: Vec<BackendOfferInfo>,
    }

    #[derive(Debug, Clone)]
    struct DeviceRequestInfo {
        backend: DeviceBackend,
        /// Empty = no saved adapter.
        adapter_id: String,
        preference: DevicePreference,
        force_recreate: bool,
    }

    #[derive(Debug, Clone)]
    struct DeviceSwitchInfo {
        outcome: DeviceSwitchOutcome,
        reason: String,
        backend: DeviceBackend,
        adapter_name: String,
        adapter_stable_id: String,
        generation: u64,
        rolled_back: bool,
    }

    #[derive(Debug, Clone)]
    struct DeviceChangeInfo {
        kind: DeviceChangeKind,
        generation: u64,
        backend: DeviceBackend,
        adapter_name: String,
        message: String,
    }

    unsafe extern "C++" {
        include!("sturdy_rs/shim.hpp");
        include!("sturdy_rs/device.hpp");

        /// `SFT::Engine::Engine`, aliasing the root bridge's opaque type (see `diagnostics::ffi`
        /// for why the `#[namespace]` override is required).
        #[namespace = "sturdy_rs"]
        type EngineView = crate::ffi::EngineView;

        fn available_backends(engine: &EngineView) -> Vec<DeviceBackend>;
        fn available_adapters(engine: Pin<&mut EngineView>) -> Vec<AdapterInfo>;
        fn switch_device(engine: Pin<&mut EngineView>, request: &DeviceRequestInfo) -> DeviceSwitchInfo;
        fn device_generation(engine: &EngineView) -> u64;
        fn take_device_change_events(engine: Pin<&mut EngineView>) -> Vec<DeviceChangeInfo>;
    }
}
