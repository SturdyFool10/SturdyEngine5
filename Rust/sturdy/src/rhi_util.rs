//! Device-free RHI helpers: format classification, feature negotiation, adapter scoring, render
//! threading and swapchain present-mode selection. Useful for choosing settings before (or
//! without) touching a device.

use sturdy_sys::rhi_util::ffi;

use crate::rhi::Format;

/// Classification helpers on [`Format`] (an extension trait, since the type lives in `sturdy-sys`).
pub trait FormatExt {
    /// Block-compressed (BC/ASTC-style) format.
    fn is_block_compressed(self) -> bool;
    fn has_depth(self) -> bool;
    fn has_stencil(self) -> bool;
    fn is_depth_stencil(self) -> bool;
}

impl FormatExt for Format {
    fn is_block_compressed(self) -> bool {
        ffi::format_flags(self.repr) & 1 != 0
    }

    fn has_depth(self) -> bool {
        ffi::format_flags(self.repr) & 2 != 0
    }

    fn has_stencil(self) -> bool {
        ffi::format_flags(self.repr) & 4 != 0
    }

    fn is_depth_stencil(self) -> bool {
        ffi::format_flags(self.repr) & 6 != 0
    }
}

/// The outcome of matching wanted features against a device's supported ones.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FeatureNegotiation {
    pub supported: Vec<String>,
    pub enabled_required: Vec<String>,
    pub enabled_optional: Vec<String>,
    /// Required features the device lacks: startup can't proceed.
    pub missing_required: Vec<String>,
    /// Optional features that will simply be off.
    pub unavailable_optional: Vec<String>,
}

impl FeatureNegotiation {
    pub fn required_satisfied(&self) -> bool {
        self.missing_required.is_empty()
    }

    pub fn optional_fully_enabled(&self) -> bool {
        self.unavailable_optional.is_empty()
    }
}

/// Matches `required` and `optional` feature names (see [`crate::rhi::all_feature_names`]) against
/// the `supported` set.
pub fn negotiate_features(supported: &[&str], required: &[&str], optional: &[&str]) -> FeatureNegotiation {
    let own = |v: &[&str]| v.iter().map(|s| (*s).to_owned()).collect::<Vec<String>>();
    let r = ffi::negotiate_features(&own(supported), &own(required), &own(optional));
    FeatureNegotiation {
        supported: r.supported,
        enabled_required: r.enabled_required,
        enabled_optional: r.enabled_optional,
        missing_required: r.missing_required,
        unavailable_optional: r.unavailable_optional,
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceType {
    Other = 0,
    Integrated,
    Discrete,
    Virtual,
    Cpu,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PowerPreference {
    None = 0,
    LowPower,
    HighPerformance,
}

/// The engine's adapter ranking: higher is preferred for the given power preference.
pub fn score_adapter(device_type: DeviceType, power: PowerPreference) -> i64 {
    ffi::score_adapter(device_type as u8, power as u8)
}

/// Case-insensitive substring test (how adapter name filters match).
pub fn name_contains(haystack: &str, needle: &str) -> bool {
    ffi::name_contains_ci(haystack, needle)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RenderThreadingMode {
    SingleThreaded,
    DedicatedRenderThread,
    ParallelCommandRecording,
}

/// The threading mode the RHI recommends for these capabilities.
pub fn choose_render_threading_mode(
    backend_allows_dedicated_render_thread: bool,
    backend_allows_parallel_command_recording: bool,
    platform_allows_threads: bool,
) -> RenderThreadingMode {
    match ffi::choose_render_threading_mode(
        backend_allows_dedicated_render_thread,
        backend_allows_parallel_command_recording,
        platform_allows_threads,
    ) {
        1 => RenderThreadingMode::DedicatedRenderThread,
        2 => RenderThreadingMode::ParallelCommandRecording,
        _ => RenderThreadingMode::SingleThreaded,
    }
}

/// Swapchain present modes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PresentMode {
    Fifo = 0,
    FifoRelaxed,
    Mailbox,
    Immediate,
    FifoLatestReady,
}

impl PresentMode {
    fn from_u8(v: u8) -> Self {
        match v {
            1 => Self::FifoRelaxed,
            2 => Self::Mailbox,
            3 => Self::Immediate,
            4 => Self::FifoLatestReady,
            _ => Self::Fifo,
        }
    }

    pub fn name(self) -> String {
        ffi::present_mode_name(self as u8)
    }
}

/// The policy a present strategy expresses.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PresentStrategy {
    Unsynchronized = 0,
    TearFreeOrdered,
    TearFreeLatest,
    AdaptiveTearing,
    TearFreeLatestReady,
    VariableRefresh,
}

impl PresentStrategy {
    pub fn name(self) -> String {
        ffi::present_strategy_name(self as u8)
    }

    /// Present modes to try, best first.
    pub fn mode_preference(self) -> [PresentMode; 4] {
        ffi::present_mode_preference(self as u8).map(PresentMode::from_u8)
    }

    /// The best of `supported` for this strategy.
    pub fn choose_mode(self, supported: &[PresentMode]) -> PresentMode {
        let raw: Vec<u8> = supported.iter().map(|m| *m as u8).collect();
        PresentMode::from_u8(ffi::choose_present_mode(&raw, self as u8))
    }
}

/// Composite-alpha modes to try for a transparent window, best first (names, as the engine orders them).
pub fn transparent_composite_alpha_preference() -> Vec<&'static str> {
    ffi::transparent_composite_alpha_preference()
        .iter()
        .map(|m| match m {
            1 => "Opaque",
            2 => "Premultiplied",
            3 => "PostMultiplied",
            4 => "Inherit",
            _ => "Auto",
        })
        .collect()
}
