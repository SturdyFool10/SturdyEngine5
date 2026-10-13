//! Raw `cxx` bindings for application-loop control: on-demand frames (`Engine::FrameMode`) and redraw requests
//! (`Engine::RedrawHandle`), which may be sent to and called from any thread.

#[cxx::bridge(namespace = "sturdy_rs::app")]
pub mod ffi {
    /// Mirrors `SFT::Engine::FrameMode`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FrameMode {
        Continuous,
        OnDemand,
    }

    unsafe extern "C++" {
        include!("sturdy_rs/shim.hpp");
        include!("sturdy_rs/app.hpp");

        /// See `device::ffi::EngineView` for why the namespace override is needed.
        #[namespace = "sturdy_rs"]
        type EngineView = crate::ffi::EngineView;

        /// `SFT::Engine::RedrawHandle`: shared, thread-safe.
        type RedrawHandle;

        fn set_frame_mode(engine: Pin<&mut EngineView>, mode: FrameMode);
        fn frame_mode(engine: &EngineView) -> FrameMode;
        fn redraw_handle(engine: &EngineView) -> UniquePtr<RedrawHandle>;
        fn redraw_request(handle: &RedrawHandle);
        fn redraw_request_after(handle: &RedrawHandle, nanoseconds: u64);
    }
}

// SAFETY: `SFT::Engine::RedrawHandle` holds a shared pointer to atomics and a mutex-guarded wake callback; every
// operation bound above is documented thread-safe on the C++ side.
unsafe impl Send for ffi::RedrawHandle {}
unsafe impl Sync for ffi::RedrawHandle {}
