//! Live graphics-device changes: pick the graphics API and GPU while the app runs, with no relaunch.
//!
//! A settings screen calls these on the [`Engine`] it is handed in a [`GameLogic`](crate::GameLogic)
//! callback:
//!
//! ```ignore
//! // List what can be chosen (no device is created):
//! let adapters = engine.available_adapters();
//! // Apply the user's saved choice. A GPU that was unplugged degrades to the profile, with a reason:
//! let request = DeviceRequest::new()
//!     .backend(Backend::Vulkan)
//!     .adapter(saved_stable_id)
//!     .preference(DevicePreference::HighPerformance);
//! match engine.switch_device(&request).outcome {
//!     SwitchOutcome::Switched => {}
//!     SwitchOutcome::SwitchedWithFallback(reason) => notify(reason),
//!     SwitchOutcome::Failed(reason) => notify(reason), // the previous device is still running
//! }
//! ```
//!
//! ## What survives a switch
//!
//! Re-created in place on the new device, handles unchanged: assets (meshes, textures, materials),
//! offscreen render targets, window surfaces and swapchains, renderer pipeline caches, and the UI
//! renderer. Window ids, [`SurfaceHandle`](crate::render::SurfaceHandle)s and every asset handle the
//! engine handed out stay valid.
//!
//! What does **not** survive: GPU objects *you* created through [`crate::rhi`] (buffers, textures,
//! pipelines, bind groups). Compare [`Engine::device_generation`] against the value you built them at,
//! or watch [`Engine::take_device_change_events`], and rebuild them when it moves.
//!
//! ## Failure
//!
//! A failed switch is a value, never a panic. If the old device had already been torn down the engine
//! rebuilds it (`rolled_back`), so there is never a dead window; the app should tell the user the
//! reason and keep running.
//!
//! ## Threading
//!
//! Call from the frame callback, between frames. The switch blocks until the GPU is idle and the new
//! device is up (typically hundreds of milliseconds).
//!
//! Automatic recovery from a driver-reported device loss reuses the same path (same request as the
//! running device) and is reported as [`DeviceChangeKind::Recovered`].

use sturdy_sys::device::ffi;

use crate::Engine;

/// A graphics API.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Backend {
    Vulkan,
    D3D12,
    Metal,
    WebGpu,
}

impl Backend {
    fn from_ffi(value: ffi::DeviceBackend) -> Option<Self> {
        match value {
            ffi::DeviceBackend::Vulkan => Some(Self::Vulkan),
            ffi::DeviceBackend::D3D12 => Some(Self::D3D12),
            ffi::DeviceBackend::Metal => Some(Self::Metal),
            ffi::DeviceBackend::WebGpu => Some(Self::WebGpu),
            _ => None,
        }
    }

    fn to_ffi(value: Option<Self>) -> ffi::DeviceBackend {
        match value {
            None => ffi::DeviceBackend::Auto,
            Some(Self::Vulkan) => ffi::DeviceBackend::Vulkan,
            Some(Self::D3D12) => ffi::DeviceBackend::D3D12,
            Some(Self::Metal) => ffi::DeviceBackend::Metal,
            Some(Self::WebGpu) => ffi::DeviceBackend::WebGpu,
        }
    }
}

/// Which adapter a request prefers when it names none, or the named one is gone.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum DevicePreference {
    #[default]
    Default,
    HighPerformance,
    LowPower,
    /// Only discrete GPUs; nothing matching is a failure, never a downgrade.
    DiscreteOnly,
    /// Only integrated GPUs; nothing matching is a failure, never a downgrade.
    IntegratedOnly,
}

impl DevicePreference {
    fn to_ffi(self) -> ffi::DevicePreference {
        match self {
            Self::Default => ffi::DevicePreference::Default,
            Self::HighPerformance => ffi::DevicePreference::HighPerformance,
            Self::LowPower => ffi::DevicePreference::LowPower,
            Self::DiscreteOnly => ffi::DevicePreference::DiscreteOnly,
            Self::IntegratedOnly => ffi::DevicePreference::IntegratedOnly,
        }
    }
}

/// What the app wants the live device to become. Everything is optional: leave a field alone to keep
/// the running value.
#[derive(Debug, Clone, Default)]
pub struct DeviceRequest {
    backend: Option<Backend>,
    adapter_id: Option<String>,
    preference: DevicePreference,
    force_recreate: bool,
}

impl DeviceRequest {
    pub fn new() -> Self {
        Self::default()
    }

    /// The graphics API. Unset (Auto) keeps the running one when the chosen adapter supports it.
    pub fn backend(mut self, backend: Backend) -> Self {
        self.backend = Some(backend);
        self
    }

    /// A saved adapter: [`Adapter::stable_id`] (survives driver updates and API changes) or its
    /// `physical_device_id`. A missing adapter falls back to [`preference`](Self::preference) and the
    /// result is [`SwitchOutcome::SwitchedWithFallback`].
    pub fn adapter(mut self, id: impl Into<String>) -> Self {
        self.adapter_id = Some(id.into());
        self
    }

    pub fn preference(mut self, preference: DevicePreference) -> Self {
        self.preference = preference;
        self
    }

    /// Rebuild the device even if the request resolves to the one already running.
    pub fn force_recreate(mut self, force: bool) -> Self {
        self.force_recreate = force;
        self
    }

    fn to_ffi(&self) -> ffi::DeviceRequestInfo {
        ffi::DeviceRequestInfo {
            backend: Backend::to_ffi(self.backend),
            adapter_id: self.adapter_id.clone().unwrap_or_default(),
            preference: self.preference.to_ffi(),
            force_recreate: self.force_recreate,
        }
    }
}

/// Whether an adapter can present to a window surface.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Presentability {
    /// Not probed. Only the running device is known to present; the others need a surface on their
    /// own instance, which the engine does not create for a settings list.
    Unknown,
    Yes,
    No,
}

/// One graphics API an [`Adapter`] can be driven through.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BackendOffer {
    pub backend: Backend,
    pub driver_version: String,
    pub api_version: String,
    pub presentable: Presentability,
    /// This is the device the engine is running on right now.
    pub active: bool,
}

/// One physical GPU as a settings screen lists it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Adapter {
    /// Hash of vendor id, device id and name: stable across APIs, driver updates and reboots. Persist this.
    pub stable_id: String,
    /// The engine's per-GPU identifier (device UUID / LUID).
    pub physical_device_id: String,
    pub name: String,
    pub vendor: String,
    pub vendor_id: u32,
    pub device_id: u32,
    /// `"Discrete GPU"`, `"Integrated GPU"`, ... (the engine's own naming).
    pub device_type: String,
    pub backends: Vec<BackendOffer>,
}

/// How a [`switch_device`](Engine::switch_device) call ended.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SwitchOutcome {
    /// The device now matches the request exactly.
    Switched,
    /// A device was created, but not exactly the one asked for; the string says why (e.g. the saved GPU is gone).
    SwitchedWithFallback(String),
    /// Nothing changed or the previous device was restored; the string says why. The app keeps running.
    Failed(String),
}

/// The result of [`Engine::switch_device`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DeviceSwitch {
    pub outcome: SwitchOutcome,
    /// The API running after the call (the previous one on failure). `None` only if no device exists.
    pub backend: Option<Backend>,
    pub adapter_name: String,
    pub adapter_stable_id: String,
    /// [`Engine::device_generation`] after the call.
    pub generation: u64,
    /// A replacement was attempted, failed, and the previous device was rebuilt.
    pub rolled_back: bool,
}

/// Why the device changed under the app.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceChangeKind {
    /// About to be replaced; the old device is still alive.
    Switching,
    /// The replacement is live (or the old device was restored after a failed switch).
    Switched,
    /// The driver reported device loss and the engine rebuilt the device automatically.
    Recovered,
    /// Device loss that could not be recovered; rendering is unavailable until a later switch succeeds.
    LostUnrecoverable,
}

/// A completed device change. See [`Engine::take_device_change_events`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DeviceChange {
    pub kind: DeviceChangeKind,
    pub generation: u64,
    pub backend: Option<Backend>,
    pub adapter_name: String,
    pub message: String,
}

impl Engine<'_> {
    /// The graphics APIs at least one discovered GPU supports. Creates no device.
    pub fn available_backends(&self) -> Vec<Backend> {
        ffi::available_backends(self.view()).into_iter().filter_map(Backend::from_ffi).collect()
    }

    /// Every discovered GPU with its APIs, drivers and stable ids. Creates no device.
    pub fn available_adapters(&mut self) -> Vec<Adapter> {
        ffi::available_adapters(self.view_mut())
            .into_iter()
            .map(|a| Adapter {
                stable_id: a.stable_id,
                physical_device_id: a.physical_device_id,
                name: a.name,
                vendor: a.vendor,
                vendor_id: a.vendor_id,
                device_id: a.device_id,
                device_type: a.device_type,
                backends: a
                    .backends
                    .into_iter()
                    .filter_map(|b| {
                        Some(BackendOffer {
                            backend: Backend::from_ffi(b.backend)?,
                            driver_version: b.driver_version,
                            api_version: b.api_version,
                            presentable: match b.presentable {
                                ffi::AdapterPresentability::Yes => Presentability::Yes,
                                ffi::AdapterPresentability::No => Presentability::No,
                                _ => Presentability::Unknown,
                            },
                            active: b.active,
                        })
                    })
                    .collect(),
            })
            .collect()
    }

    /// Replaces the graphics device in place. See the [module docs](crate::device) for what survives.
    pub fn switch_device(&mut self, request: &DeviceRequest) -> DeviceSwitch {
        let r = ffi::switch_device(self.view_mut(), &request.to_ffi());
        DeviceSwitch {
            outcome: match r.outcome {
                ffi::DeviceSwitchOutcome::Switched => SwitchOutcome::Switched,
                ffi::DeviceSwitchOutcome::SwitchedWithFallback => SwitchOutcome::SwitchedWithFallback(r.reason),
                _ => SwitchOutcome::Failed(r.reason),
            },
            backend: Backend::from_ffi(r.backend),
            adapter_name: r.adapter_name,
            adapter_stable_id: r.adapter_stable_id,
            generation: r.generation,
            rolled_back: r.rolled_back,
        }
    }

    /// How many times the graphics device has been replaced. Raw [`crate::rhi`] objects you created
    /// are invalid once this differs from the value they were created at.
    pub fn device_generation(&self) -> u64 {
        ffi::device_generation(self.view())
    }

    /// Drains the device changes (switches, rollbacks, automatic recoveries) since the last call.
    pub fn take_device_change_events(&mut self) -> Vec<DeviceChange> {
        ffi::take_device_change_events(self.view_mut())
            .into_iter()
            .map(|e| DeviceChange {
                kind: match e.kind {
                    ffi::DeviceChangeKind::Switching => DeviceChangeKind::Switching,
                    ffi::DeviceChangeKind::Recovered => DeviceChangeKind::Recovered,
                    ffi::DeviceChangeKind::LostUnrecoverable => DeviceChangeKind::LostUnrecoverable,
                    _ => DeviceChangeKind::Switched,
                },
                generation: e.generation,
                backend: Backend::from_ffi(e.backend),
                adapter_name: e.adapter_name,
                message: e.message,
            })
            .collect()
    }
}
