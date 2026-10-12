//! Raw `cxx` bindings for the RHI's free helper functions that need no device: format
//! classification, feature negotiation, adapter scoring, render-threading and present-mode
//! selection.

#[cxx::bridge(namespace = "sturdy_rs::rhi_util")]
pub mod ffi {
    /// `SFT::RHI::FeatureNegotiationReport`, as feature-name lists.
    #[derive(Debug, Clone, Default)]
    struct NegotiationDesc {
        supported: Vec<String>,
        enabled_required: Vec<String>,
        enabled_optional: Vec<String>,
        missing_required: Vec<String>,
        unavailable_optional: Vec<String>,
    }

    unsafe extern "C++" {
        include!("sturdy_rs/rhi_util.hpp");

        /// Bit 0 block-compressed, bit 1 has depth, bit 2 has stencil. `format` is an `rhi::Format` value.
        fn format_flags(format: u8) -> u8;

        /// Names are `RHI::feature_name` strings; unknown names are ignored.
        fn negotiate_features(supported: &Vec<String>, required: &Vec<String>, optional: &Vec<String>) -> NegotiationDesc;

        /// `device_type`: 0 other, 1 integrated, 2 discrete, 3 virtual, 4 cpu.
        /// `power`: 0 none, 1 low power, 2 high performance.
        fn score_adapter(device_type: u8, power: u8) -> i64;
        fn name_contains_ci(haystack: &str, needle: &str) -> bool;

        /// Recommended threading mode (0 single, 1 dedicated render thread, 2 parallel recording)
        /// for these capabilities, as `RHI::choose_render_threading_mode` decides.
        fn choose_render_threading_mode(
            backend_allows_dedicated_render_thread: bool,
            backend_allows_parallel_command_recording: bool,
            platform_allows_threads: bool,
        ) -> u8;

        /// Present modes in preference order for a strategy (`PresentStrategy` ordinal); modes are
        /// 0 Fifo, 1 FifoRelaxed, 2 Mailbox, 3 Immediate, 4 FifoLatestReady.
        fn present_mode_preference(strategy: u8) -> [u8; 4];
        /// The best supported mode for the strategy.
        fn choose_present_mode(supported: &[u8], strategy: u8) -> u8;
        fn present_mode_name(mode: u8) -> String;
        fn present_strategy_name(strategy: u8) -> String;
        /// Composite-alpha modes to try for a transparent window, best first
        /// (0 auto, 1 opaque, 2 premultiplied, 3 post-multiplied, 4 inherit).
        fn transparent_composite_alpha_preference() -> [u8; 3];
    }
}
