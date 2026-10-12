use sturdy_sys::ffi;

macro_rules! mirrored_enum {
    ($(#[$m:meta])* $name:ident => $ffi:ident { $($variant:ident),+ $(,)? }) => {
        $(#[$m])*
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
        pub enum $name { $($variant),+ }

        impl From<$name> for ffi::$ffi {
            fn from(value: $name) -> Self {
                match value { $($name::$variant => ffi::$ffi::$variant),+ }
            }
        }

        // The reverse direction: other subsystem bridges (e.g. `render::ffi::PresentationSettings`)
        // reuse these same root-bridge enums for their own *outputs*, so a round trip needs a way
        // back. `ffi::$ffi` isn't necessarily `sturdy_sys::ffi::$ffi` here — cxx's shared-enum reuse
        // (`#[namespace = "sturdy_rs"] type X = crate::ffi::X;`) makes it the identical type under a
        // different bridge's path, and this impl covers all of those paths at once.
        impl From<ffi::$ffi> for $name {
            fn from(value: ffi::$ffi) -> Self {
                // cxx shared enums are a repr-integer newtype with associated consts, not a closed
                // Rust enum, so the match can't be proven exhaustive — the wildcard only fires for
                // a discriminant the engine itself never produces for this type.
                match value {
                    $(ffi::$ffi::$variant => $name::$variant,)+
                    _ => unreachable!("unknown {} discriminant {:?}", stringify!($ffi), value),
                }
            }
        }
    };
}

mirrored_enum!(
    VSync => VSync { Off, On, Adaptive }
);
mirrored_enum!(
    VariableRefresh => VariableRefresh { Disabled, Automatic, Preferred }
);
mirrored_enum!(
    LatencyMode => LatencyMode { Normal, Low, Ultra }
);
mirrored_enum!(
    PresentationPreference => PresentationPreference {
        Automatic, LowestLatency, Smoothest, PowerEfficient
    }
);
mirrored_enum!(
    WindowMode => WindowMode { Windowed, BorderlessFullscreen, ExclusiveFullscreen }
);

/// Everything the runtime needs before the first frame. Built fluently:
///
/// ```
/// # use sturdy::*;
/// let config = RuntimeConfig::new("Demo").size(1600, 900).vsync(VSync::Adaptive).raytracing(true);
/// ```
#[derive(Debug, Clone)]
pub struct RuntimeConfig {
    pub(crate) window_title: String,
    pub(crate) app_name: String,
    pub(crate) shaders_directory: String,
    pub(crate) size: (u32, u32),
    pub(crate) resizable: bool,
    pub(crate) decorated: bool,
    pub(crate) high_dpi: bool,
    pub(crate) window_mode: WindowMode,
    pub(crate) raytracing: bool,
    pub(crate) vsync: VSync,
    pub(crate) variable_refresh: VariableRefresh,
    pub(crate) latency: LatencyMode,
    pub(crate) preference: PresentationPreference,
    pub(crate) title_update_interval_seconds: f64,
    pub(crate) runtime_window_management: bool,
    pub(crate) physical_device_id: String,
    pub(crate) shader_disk_cache: bool,
    pub(crate) prefer_async_compute: bool,
    pub(crate) frames_in_flight: u32,
    pub(crate) native_access: bool,
    pub(crate) required_features: Vec<String>,
    pub(crate) optional_features: Vec<String>,
    pub(crate) hdr: bool,
    pub(crate) hdr_color_space: crate::render::HdrColorSpace,
    pub(crate) transparent_composition: bool,
    pub(crate) swapchain_image_count: u32,
    pub(crate) allow_present_from_compute: bool,
    pub(crate) transparent: bool,
    pub(crate) visible: bool,
    pub(crate) position: Option<(i32, i32)>,
}

impl RuntimeConfig {
    pub fn new(title: impl Into<String>) -> Self {
        let title = title.into();
        Self {
            app_name: title.clone(),
            window_title: title,
            shaders_directory: "Shaders".into(),
            size: (1280, 720),
            resizable: true,
            decorated: true,
            high_dpi: true,
            window_mode: WindowMode::Windowed,
            raytracing: false,
            vsync: VSync::On,
            variable_refresh: VariableRefresh::Automatic,
            latency: LatencyMode::Normal,
            preference: PresentationPreference::Automatic,
            title_update_interval_seconds: 0.0,
            runtime_window_management: false,
            physical_device_id: String::new(),
            shader_disk_cache: true,
            prefer_async_compute: false,
            frames_in_flight: 0,
            native_access: false,
            required_features: Vec::new(),
            optional_features: Vec::new(),
            hdr: false,
            hdr_color_space: crate::render::HdrColorSpace::Hdr10St2084,
            transparent_composition: false,
            swapchain_image_count: 0,
            allow_present_from_compute: false,
            transparent: false,
            visible: true,
            position: None,
        }
    }

    #[must_use]
    pub fn app_name(mut self, name: impl Into<String>) -> Self {
        self.app_name = name.into();
        self
    }
    #[must_use]
    pub fn size(mut self, width: u32, height: u32) -> Self {
        self.size = (width, height);
        self
    }
    #[must_use]
    pub fn resizable(mut self, on: bool) -> Self {
        self.resizable = on;
        self
    }
    #[must_use]
    pub fn decorated(mut self, on: bool) -> Self {
        self.decorated = on;
        self
    }
    #[must_use]
    pub fn high_dpi(mut self, on: bool) -> Self {
        self.high_dpi = on;
        self
    }
    #[must_use]
    pub fn window_mode(mut self, mode: WindowMode) -> Self {
        self.window_mode = mode;
        self
    }
    #[must_use]
    pub fn raytracing(mut self, on: bool) -> Self {
        self.raytracing = on;
        self
    }
    #[must_use]
    pub fn vsync(mut self, mode: VSync) -> Self {
        self.vsync = mode;
        self
    }
    #[must_use]
    pub fn variable_refresh(mut self, mode: VariableRefresh) -> Self {
        self.variable_refresh = mode;
        self
    }
    #[must_use]
    pub fn latency(mut self, mode: LatencyMode) -> Self {
        self.latency = mode;
        self
    }
    #[must_use]
    pub fn presentation_preference(mut self, preference: PresentationPreference) -> Self {
        self.preference = preference;
        self
    }
    /// Directory the engine looks in for shader sources at runtime.
    #[must_use]
    pub fn shaders_directory(mut self, dir: impl Into<String>) -> Self {
        self.shaders_directory = dir.into();
        self
    }
    /// Refresh the window title (e.g. with frame stats) every `seconds`.
    #[must_use]
    pub fn title_update_interval(mut self, seconds: f64) -> Self {
        self.title_update_interval_seconds = seconds;
        self
    }
    #[must_use]
    pub fn runtime_window_management(mut self, on: bool) -> Self {
        self.runtime_window_management = on;
        self
    }

    /// Forces a specific GPU by its Vulkan physical device id (see the GPU inventory in
    /// [`crate::diagnostics`]); by default the engine picks the best adapter.
    #[must_use]
    pub fn physical_device_id(mut self, id: impl Into<String>) -> Self {
        self.physical_device_id = id.into();
        self
    }
    /// Cache compiled shaders on disk (default on).
    #[must_use]
    pub fn shader_disk_cache(mut self, on: bool) -> Self {
        self.shader_disk_cache = on;
        self
    }
    /// Prefer an async compute queue when the device has one.
    #[must_use]
    pub fn prefer_async_compute(mut self, on: bool) -> Self {
        self.prefer_async_compute = on;
        self
    }
    /// Frames the CPU may run ahead of the GPU (engine default 2).
    #[must_use]
    pub fn frames_in_flight(mut self, frames: u32) -> Self {
        self.frames_in_flight = frames;
        self
    }
    /// Enables native GPU handle access, required by `Diagnostics::native_vulkan` and friends.
    #[must_use]
    pub fn native_access(mut self, on: bool) -> Self {
        self.native_access = on;
        self
    }
    /// RHI features (names from [`crate::rhi::all_feature_names`]) the device must support;
    /// startup fails if it lacks one.
    #[must_use]
    pub fn require_features<S: Into<String>>(mut self, names: impl IntoIterator<Item = S>) -> Self {
        self.required_features.extend(names.into_iter().map(Into::into));
        self
    }
    /// RHI features to enable when the device supports them.
    #[must_use]
    pub fn optional_features<S: Into<String>>(mut self, names: impl IntoIterator<Item = S>) -> Self {
        self.optional_features.extend(names.into_iter().map(Into::into));
        self
    }
    /// Starts with HDR output enabled in `color_space` (when the display supports it; see
    /// [`crate::Engine::set_presentation_settings`] to change it at runtime).
    #[must_use]
    pub fn hdr(mut self, color_space: crate::render::HdrColorSpace) -> Self {
        self.hdr = true;
        self.hdr_color_space = color_space;
        self
    }
    /// Composites the swapchain with per-pixel alpha over the desktop.
    #[must_use]
    pub fn transparent_composition(mut self, on: bool) -> Self {
        self.transparent_composition = on;
        self
    }
    /// Swapchain image count; 0 lets the engine choose.
    #[must_use]
    pub fn swapchain_image_count(mut self, count: u32) -> Self {
        self.swapchain_image_count = count;
        self
    }
    #[must_use]
    pub fn allow_present_from_compute(mut self, on: bool) -> Self {
        self.allow_present_from_compute = on;
        self
    }
    /// Creates the primary window with a transparent framebuffer (composited by the OS).
    #[must_use]
    pub fn transparent(mut self, on: bool) -> Self {
        self.transparent = on;
        self
    }
    #[must_use]
    pub fn visible(mut self, on: bool) -> Self {
        self.visible = on;
        self
    }
    /// Places the primary window at a screen position instead of the OS default.
    #[must_use]
    pub fn position(mut self, x: i32, y: i32) -> Self {
        self.position = Some((x, y));
        self
    }

    /// Names in the feature lists that this engine build doesn't know.
    pub(crate) fn unknown_features(&self) -> Vec<String> {
        let known = sturdy_sys::rhi::ffi::rhi_all_feature_names();
        self.required_features
            .iter()
            .chain(&self.optional_features)
            .filter(|name| !known.iter().any(|k| k == *name))
            .cloned()
            .collect()
    }

    pub(crate) fn to_ffi(&self) -> ffi::RuntimeOptions {
        ffi::RuntimeOptions {
            window_title: self.window_title.clone(),
            app_name: self.app_name.clone(),
            shaders_directory: self.shaders_directory.clone(),
            width: self.size.0,
            height: self.size.1,
            resizable: self.resizable,
            decorated: self.decorated,
            high_dpi: self.high_dpi,
            window_mode: self.window_mode.into(),
            raytracing: self.raytracing,
            vsync: self.vsync.into(),
            variable_refresh: self.variable_refresh.into(),
            latency: self.latency.into(),
            preference: self.preference.into(),
            title_update_interval_seconds: self.title_update_interval_seconds,
            runtime_window_management: self.runtime_window_management,
            graphics_physical_device_id: self.physical_device_id.clone(),
            enable_shader_disk_cache: self.shader_disk_cache,
            prefer_async_compute: self.prefer_async_compute,
            desired_frames_in_flight: self.frames_in_flight,
            enable_native_access: self.native_access,
            required_features: self.required_features.clone(),
            optional_features: self.optional_features.clone(),
            hdr_enabled: self.hdr,
            hdr_color_space: self.hdr_color_space as u8,
            transparent_composition: self.transparent_composition,
            swapchain_image_count: self.swapchain_image_count,
            allow_present_from_compute: self.allow_present_from_compute,
            window_transparent: self.transparent,
            window_visible: self.visible,
            has_window_position: self.position.is_some(),
            window_x: self.position.map_or(0, |p| p.0),
            window_y: self.position.map_or(0, |p| p.1),
        }
    }
}
