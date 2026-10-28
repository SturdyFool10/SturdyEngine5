//! Shared-state primitives that other languages (and C++) use too: the engine's blocking mutex, wait-free SPSC ring, reference-counted
//! `Arc`, `Promise` and the `Execution` sender layer. Rust has its own `Mutex` and `Arc`; these are for state shared *with the engine and
//! with other languages*, and they show up in the profiler like the C++ ones.

use crate::sync::{CancelToken, Lane, TaskOutput, TaskResult};
use crate::{bytes_ptr, check, flag, millis, run, Error, ErrorKind, Lower, Object, Op, Reply, Result};
use crate::ffi;
use core::ffi::c_void;
use core::marker::PhantomData;
use std::any::Any;
use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};

fn one_object(begin: impl FnOnce(*mut ffi::SturdyAsyncObject) -> i32) -> Result<Object> {
    let mut raw = ffi::SturdyAsyncObject::default();
    check(begin(&mut raw))?;
    Ok(Object::from_raw(raw))
}

fn bytes_out(fetch: impl Fn(*mut u8, u64, *mut u64) -> i32) -> Result<Vec<u8>> {
    let mut length = 0u64;
    check(fetch(core::ptr::null_mut(), 0, &mut length))?;
    let mut buffer = vec![0u8; length as usize];
    if !buffer.is_empty() {
        check(fetch(buffer.as_mut_ptr(), buffer.len() as u64, &mut length))?;
    }
    Ok(buffer)
}

// ------------------------------------------------------------------------------------------------------------------------------
// Blocking mutex
// ------------------------------------------------------------------------------------------------------------------------------

/// The engine's blocking mutex around a byte-string value. It blocks the calling thread: never lock it on a network or timer thread and
/// never hold it across a wait. (`AsyncMutex` waits without a thread.)
pub struct SyncMutex {
    object: Object,
}

impl SyncMutex {
    /// `debug_name` is what the profiler shows for this lock.
    pub fn new(initial: &[u8], debug_name: &str) -> Result<SyncMutex> {
        let mut cx = Lower::default();
        let name = cx.string(debug_name);
        let (ptr, len) = bytes_ptr(initial);
        Ok(SyncMutex { object: one_object(|out| unsafe { ffi::sturdy_sync_mutex_create(ptr, len, name, out) })? })
    }

    /// Blocks until the lock is free.
    pub fn lock(&self) -> Result<SyncGuard<'_>> {
        let object = one_object(|out| unsafe { ffi::sturdy_sync_mutex_lock(self.object.raw(), out) })?;
        Ok(SyncGuard { object, _not_send: PhantomData, _mutex: PhantomData })
    }

    /// `None` when the lock is held.
    pub fn try_lock(&self) -> Result<Option<SyncGuard<'_>>> {
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_sync_mutex_try_lock(self.object.raw(), &mut raw) })?;
        Ok((raw.token != 0).then(|| SyncGuard { object: Object::from_raw(raw), _not_send: PhantomData, _mutex: PhantomData }))
    }
}

/// A held [`SyncMutex`] lock. Unlocks on drop; it cannot leave the thread that locked it.
pub struct SyncGuard<'a> {
    object: Object,
    _not_send: PhantomData<*const ()>,
    _mutex: PhantomData<&'a SyncMutex>,
}

impl SyncGuard<'_> {
    /// The protected value.
    pub fn get(&self) -> Result<Vec<u8>> {
        bytes_out(|b, c, l| unsafe { ffi::sturdy_guard_get(self.object.raw(), b, c, l) })
    }

    /// Replaces the value.
    pub fn set(&self, data: &[u8]) -> Result<()> {
        let (ptr, len) = bytes_ptr(data);
        check(unsafe { ffi::sturdy_guard_set(self.object.raw(), ptr, len) })
    }
}

impl Drop for SyncGuard<'_> {
    fn drop(&mut self) {
        unsafe { ffi::sturdy_guard_release(self.object.raw()) };
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// SPSC ring
// ------------------------------------------------------------------------------------------------------------------------------

/// What a ring operation found.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RingStatus {
    Ok,
    Full,
    Empty,
    /// The item is larger than the ring's slot size.
    TooLarge,
}

fn ring_status(raw: i32) -> RingStatus {
    match raw {
        ffi::STURDY_RING_OK => RingStatus::Ok,
        ffi::STURDY_RING_FULL => RingStatus::Full,
        ffi::STURDY_RING_EMPTY => RingStatus::Empty,
        _ => RingStatus::TooLarge,
    }
}

/// Creates a wait-free single-producer single-consumer ring of byte messages. Items are at most `max_item_bytes` (rounded up to 32, 128,
/// 512, 2 Ki, 8 Ki or 64 Ki); the capacity is rounded up to a power of two. The two halves are `Send` but not `Clone`, which is what makes
/// "one producer, one consumer" a compile-time fact.
pub fn spsc_ring(capacity: u64, max_item_bytes: u64) -> Result<(RingProducer, RingConsumer)> {
    let object = Arc::new(one_object(|out| unsafe { ffi::sturdy_ring_create(capacity, max_item_bytes, out) })?);
    Ok((RingProducer { object: object.clone() }, RingConsumer { object }))
}

pub struct RingProducer {
    object: Arc<Object>,
}

impl RingProducer {
    /// Pushes one item. `Full` when there is no room, `TooLarge` when it does not fit a slot.
    pub fn try_push(&self, item: &[u8]) -> Result<RingStatus> {
        let (ptr, len) = bytes_ptr(item);
        let mut status = 0;
        check(unsafe { ffi::sturdy_ring_try_push(self.object.raw(), ptr, len, &mut status) })?;
        Ok(ring_status(status))
    }

    pub fn capacity(&self) -> u64 {
        let mut value = 0u64;
        unsafe { ffi::sturdy_ring_capacity(self.object.raw(), &mut value) };
        value
    }
}

pub struct RingConsumer {
    object: Arc<Object>,
}

impl RingConsumer {
    /// The oldest item, or `None` when the ring is empty.
    pub fn try_pop(&self) -> Result<Option<Vec<u8>>> {
        let mut max = 0u64;
        unsafe { ffi::sturdy_ring_max_item_bytes(self.object.raw(), &mut max) };
        let mut buffer = vec![0u8; max as usize];
        let (mut length, mut status) = (0u64, 0);
        check(unsafe { ffi::sturdy_ring_try_pop(self.object.raw(), buffer.as_mut_ptr(), buffer.len() as u64, &mut length, &mut status) })?;
        if status != ffi::STURDY_RING_OK {
            return Ok(None);
        }
        buffer.truncate(length as usize);
        Ok(Some(buffer))
    }

    /// Pops up to `max_items` (0: all that are there), in order.
    pub async fn drain(&self, max_items: u64) -> Result<Vec<Vec<u8>>> {
        let reply = run(|out| unsafe { ffi::sturdy_ring_drain(self.object.raw(), max_items, out) }).await?;
        crate::unpack_frames(&reply.bytes())
    }

    /// Items waiting right now.
    pub fn len(&self) -> u64 {
        let mut value = 0u64;
        unsafe { ffi::sturdy_ring_size(self.object.raw(), &mut value) };
        value
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Arc
// ------------------------------------------------------------------------------------------------------------------------------

/// A reference-counted byte string shared with every language: each clone is another handle to the same data, destroyed with the last.
pub struct ArcBytes {
    object: Object,
}

impl ArcBytes {
    pub fn new(data: &[u8]) -> Result<ArcBytes> {
        let (ptr, len) = bytes_ptr(data);
        Ok(ArcBytes { object: one_object(|out| unsafe { ffi::sturdy_arc_create(ptr, len, out) })? })
    }

    pub fn bytes(&self) -> Result<Vec<u8>> {
        bytes_out(|b, c, l| unsafe { ffi::sturdy_arc_bytes(self.object.raw(), b, c, l) })
    }

    /// How many references exist right now (a snapshot).
    pub fn use_count(&self) -> u64 {
        let mut count = 0u64;
        unsafe { ffi::sturdy_arc_use_count(self.object.raw(), &mut count) };
        count
    }

    /// Whether both handles are the same shared value.
    pub fn ptr_eq(&self, other: &ArcBytes) -> bool {
        let mut same = 0u8;
        unsafe { ffi::sturdy_arc_ptr_eq(self.object.raw(), other.object.raw(), &mut same) };
        same != 0
    }
}

impl Clone for ArcBytes {
    fn clone(&self) -> Self {
        ArcBytes { object: one_object(|out| unsafe { ffi::sturdy_arc_clone(self.object.raw(), out) }).expect("cloning an arc") }
    }
}

unsafe extern "C" fn drop_boxed<T>(pointer: *mut c_void) {
    drop(unsafe { Box::from_raw(pointer as *mut T) });
}

/// A reference-counted Rust value held by the engine's `Arc`: clones can be handed to C++ or any other language as opaque handles (see
/// [`SharedArc::raw`]) and the value is dropped when the last reference — in any language — goes away.
pub struct SharedArc<T: Send + Sync + 'static> {
    object: Object,
    pointer: *const T,
}

unsafe impl<T: Send + Sync + 'static> Send for SharedArc<T> {}
unsafe impl<T: Send + Sync + 'static> Sync for SharedArc<T> {}

impl<T: Send + Sync + 'static> SharedArc<T> {
    pub fn new(value: T) -> Result<SharedArc<T>> {
        let pointer = Box::into_raw(Box::new(value));
        let mut raw = ffi::SturdyAsyncObject::default();
        let code = unsafe { ffi::sturdy_arc_create_foreign(pointer as *mut c_void, Some(drop_boxed::<T>), &mut raw) };
        if code != ffi::STURDY_OK {
            drop(unsafe { Box::from_raw(pointer) });
        }
        check(code)?;
        Ok(SharedArc { object: Object::from_raw(raw), pointer })
    }

    /// How many references exist right now (a snapshot).
    pub fn use_count(&self) -> u64 {
        let mut count = 0u64;
        unsafe { ffi::sturdy_arc_use_count(self.object.raw(), &mut count) };
        count
    }

    pub fn ptr_eq(&self, other: &SharedArc<T>) -> bool {
        self.pointer == other.pointer
    }

    /// The engine-side handle, for passing to other languages through the C ABI (it is still owned by this value).
    pub fn raw(&self) -> ffi::SturdyAsyncObject {
        self.object.raw()
    }
}

impl<T: Send + Sync + 'static> core::ops::Deref for SharedArc<T> {
    type Target = T;
    fn deref(&self) -> &T {
        // The pointee lives until the last reference (including this one) is dropped.
        unsafe { &*self.pointer }
    }
}

impl<T: Send + Sync + 'static> Clone for SharedArc<T> {
    fn clone(&self) -> Self {
        let object = one_object(|out| unsafe { ffi::sturdy_arc_clone(self.object.raw(), out) }).expect("cloning an arc");
        SharedArc { object, pointer: self.pointer }
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Promise
// ------------------------------------------------------------------------------------------------------------------------------

/// The producing half of an operation: complete the paired [`Op`] (a future) yourself. Only the first completion wins; a promise dropped
/// while pending fails its op with `Closed`.
pub struct Promise {
    object: Object,
}

/// Creates a promise and the operation it will complete. The op is an ordinary [`Op`]: await it, `wait` on it, time it out.
pub fn promise() -> Result<(Promise, Op)> {
    let mut raw = ffi::SturdyAsyncObject::default();
    let op = Op::start(|out| unsafe { ffi::sturdy_promise_create(&mut raw, out) })?;
    Ok((Promise { object: Object::from_raw(raw) }, op))
}

impl Promise {
    /// Completes the op with a number, bytes and text. True when this call completed it.
    pub fn set_value(&self, number: i64, bytes: &[u8], text: &str) -> Result<bool> {
        let mut cx = Lower::default();
        let text = cx.string(text);
        let (ptr, len) = bytes_ptr(bytes);
        let mut first = 0u8;
        check(unsafe { ffi::sturdy_promise_set_value(self.object.raw(), number, ptr, len, text, &mut first) })?;
        Ok(first != 0)
    }

    /// Completes the op with an error of `kind`.
    pub fn set_error(&self, kind: ErrorKind, message: &str) -> Result<bool> {
        let mut cx = Lower::default();
        let message = cx.string(message);
        let code = match kind {
            ErrorKind::NotFound => ffi::STURDY_IO_NOT_FOUND,
            ErrorKind::PermissionDenied => ffi::STURDY_IO_PERMISSION_DENIED,
            ErrorKind::AlreadyExists => ffi::STURDY_IO_ALREADY_EXISTS,
            ErrorKind::InvalidArgument => ffi::STURDY_IO_INVALID_ARGUMENT,
            ErrorKind::ConnectionRefused => ffi::STURDY_IO_CONNECTION_REFUSED,
            ErrorKind::ConnectionReset => ffi::STURDY_IO_CONNECTION_RESET,
            ErrorKind::TimedOut => ffi::STURDY_IO_TIMED_OUT,
            ErrorKind::Unsupported => ffi::STURDY_IO_UNSUPPORTED,
            ErrorKind::UnexpectedEof => ffi::STURDY_IO_UNEXPECTED_EOF,
            ErrorKind::Closed => ffi::STURDY_IO_CLOSED,
            ErrorKind::Tls => ffi::STURDY_IO_TLS_FAILURE,
            ErrorKind::LimitExceeded => ffi::STURDY_IO_LIMIT_EXCEEDED,
            ErrorKind::Protocol => ffi::STURDY_IO_PROTOCOL_ERROR,
            _ => ffi::STURDY_IO_UNKNOWN,
        };
        let mut first = 0u8;
        check(unsafe { ffi::sturdy_promise_set_error(self.object.raw(), code, message, &mut first) })?;
        Ok(first != 0)
    }

    /// Completes the op as cancelled.
    pub fn cancel(&self) -> Result<bool> {
        let mut first = 0u8;
        check(unsafe { ffi::sturdy_promise_cancel(self.object.raw(), &mut first) })?;
        Ok(first != 0)
    }

    pub fn is_completed(&self) -> bool {
        let mut completed = 0u8;
        unsafe { ffi::sturdy_promise_is_completed(self.object.raw(), &mut completed) };
        completed != 0
    }
}

/// An already-finished operation (Tokio's `ready`).
pub fn ready(number: i64, bytes: &[u8], text: &str) -> Result<Op> {
    let mut cx = Lower::default();
    let text = cx.string(text);
    let (ptr, len) = bytes_ptr(bytes);
    Op::start(|out| unsafe { ffi::sturdy_op_ready(number, ptr, len, text, out) })
}

// ------------------------------------------------------------------------------------------------------------------------------
// Execution senders
// ------------------------------------------------------------------------------------------------------------------------------

/// Lazy, composable descriptions of work (the `Execution` layer).
pub mod exec {
    use super::*;

    type Keep = Box<dyn Any + Send + Sync>;

    /// A description of work that runs later, on a [`Lane`]; it can be started any number of times.
    pub struct Sender {
        object: Object,
        /// The closures every stage of this chain calls; shared with every run so none can outlive them.
        keep: Arc<Vec<Keep>>,
    }

    unsafe extern "C" fn just_trampoline<F: Fn(&CancelToken) -> TaskResult + Send + Sync>(user_data: *mut c_void, token: ffi::SturdyAsyncObject, output: *mut ffi::SturdyTaskOutput) {
        let work = unsafe { &*(user_data as *const F) };
        let token = CancelToken::borrowed(token);
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| work(&token))).unwrap_or_else(|_| Err("the stage panicked".to_string()));
        unsafe { set_output(output, result) };
    }

    unsafe extern "C" fn then_trampoline<F: Fn(&CancelToken, &TaskOutput) -> TaskResult + Send + Sync>(
        user_data: *mut c_void,
        token: ffi::SturdyAsyncObject,
        input: *const ffi::SturdyTaskInput,
        output: *mut ffi::SturdyTaskOutput,
    ) {
        let work = unsafe { &*(user_data as *const F) };
        let token = CancelToken::borrowed(token);
        let view = unsafe { &*input };
        let bytes = if view.bytes.is_null() { Vec::new() } else { unsafe { core::slice::from_raw_parts(view.bytes, view.size as usize) }.to_vec() };
        let text = if view.text.is_null() { String::new() } else { unsafe { core::ffi::CStr::from_ptr(view.text) }.to_string_lossy().into_owned() };
        let input = TaskOutput { number: view.number, bytes, text };
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| work(&token, &input))).unwrap_or_else(|_| Err("the stage panicked".to_string()));
        unsafe { set_output(output, result) };
    }

    unsafe fn set_output(output: *mut ffi::SturdyTaskOutput, result: TaskResult) {
        let mut cx = Lower::default();
        match result {
            Ok(out) => {
                let text = cx.string(&out.text);
                let (ptr, len) = bytes_ptr(&out.bytes);
                unsafe { ffi::sturdy_task_output_set(output, out.number, ptr, len, text, core::ptr::null()) };
            }
            Err(message) => {
                let error = cx.string(if message.is_empty() { "the stage failed" } else { &message });
                unsafe { ffi::sturdy_task_output_set(output, 0, core::ptr::null(), 0, core::ptr::null(), error) };
            }
        }
    }

    fn lane(lane: Lane) -> i32 {
        match lane {
            Lane::Light => ffi::STURDY_LANE_LIGHT,
            Lane::Heavy => ffi::STURDY_LANE_HEAVY,
            Lane::Blocking => ffi::STURDY_LANE_BLOCKING,
        }
    }

    /// A sender that runs `work` on `lane`.
    pub fn just<F: Fn(&CancelToken) -> TaskResult + Send + Sync + 'static>(on: Lane, work: F) -> Result<Sender> {
        let closure = Box::new(work);
        let user_data = &*closure as *const F as *mut c_void;
        let object = one_object(|out| unsafe { ffi::sturdy_exec_just(Some(just_trampoline::<F>), user_data, lane(on), out) })?;
        Ok(Sender { object, keep: Arc::new(vec![closure as Keep]) })
    }

    impl Sender {
        /// A sender that runs `work` on `on` with the result of this one. A failure upstream skips it and propagates.
        pub fn then<F: Fn(&CancelToken, &TaskOutput) -> TaskResult + Send + Sync + 'static>(&self, on: Lane, work: F) -> Result<Sender> {
            let closure = Box::new(work);
            let user_data = &*closure as *const F as *mut c_void;
            let object = one_object(|out| unsafe { ffi::sturdy_exec_then(self.object.raw(), Some(then_trampoline::<F>), user_data, lane(on), out) })?;
            // The new chain owns the new closure and shares the old ones (the old sender may be dropped first).
            let mut keep: Vec<Keep> = vec![closure as Keep, Box::new(self.keep.clone())];
            keep.shrink_to_fit();
            Ok(Sender { object, keep: Arc::new(keep) })
        }

        /// Starts the chain. The returned future yields the last stage's result; dropping it cancels the run.
        pub fn submit(&self) -> Result<Run> {
            let op = Op::start(|out| unsafe { ffi::sturdy_exec_submit(self.object.raw(), out) })?;
            Ok(Run { op: Some(op), keep: Some(self.keep.clone()) })
        }

        /// Starts the chain and blocks for it. Never call it from a worker thread.
        pub fn sync_wait(&self) -> Result<Reply> {
            let mut raw = ffi::SturdyAsyncOp::default();
            check(unsafe { ffi::sturdy_exec_sync_wait(self.object.raw(), &mut raw) })?;
            let mut status = 0;
            check(unsafe { ffi::sturdy_op_status(raw, &mut status) })?;
            if status == ffi::STURDY_OP_OK {
                return Ok(Reply::from_raw(raw));
            }
            let mut code = 0;
            unsafe { ffi::sturdy_op_error_code(raw, &mut code) };
            let message = crate::read_string(|b, c, l| unsafe { ffi::sturdy_op_error_message(raw, b, c, l) }).unwrap_or_default();
            unsafe { ffi::sturdy_op_release(raw) };
            Err(Error::new(crate::kind_of(code), message))
        }
    }

    /// One run of a [`Sender`]. Keeps the stage closures alive until the run finishes — if it is dropped early they are leaked rather
    /// than freed under a run that may still be calling them.
    pub struct Run {
        op: Option<Op>,
        keep: Option<Arc<Vec<Keep>>>,
    }

    impl Future for Run {
        type Output = Result<Reply>;

        fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
            let Some(op) = self.op.as_mut() else {
                return Poll::Ready(Err(Error::new(ErrorKind::InvalidArgument, "the run already finished")));
            };
            match Pin::new(op).poll(cx) {
                Poll::Pending => Poll::Pending,
                Poll::Ready(result) => {
                    self.op = None;
                    self.keep = None; // the run is over: the closures can go if nothing else holds them
                    Poll::Ready(result)
                }
            }
        }
    }

    impl Drop for Run {
        fn drop(&mut self) {
            if self.op.is_some() {
                if let Some(keep) = self.keep.take() {
                    std::mem::forget(keep);
                }
            }
        }
    }
}

#[allow(dead_code)]
fn _assert_send() {
    fn is_send<T: Send>() {}
    is_send::<SyncMutex>();
    is_send::<RingProducer>();
    is_send::<RingConsumer>();
    is_send::<ArcBytes>();
    is_send::<Promise>();
    let _ = (flag(true), millis(None));
}
