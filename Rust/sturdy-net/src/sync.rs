//! Cancellation, channels and synchronisation primitives (Tokio's `sync` module), tasks and join sets, the runtime, timers and signals,
//! and the stream pipeline. Messages are byte strings.

use crate::{bytes_ptr, check, flag, millis, run, run_object, Error, ErrorKind, Lower, Object, Op, Result};
use crate::ffi;
use core::ffi::{c_char, c_void};
use std::any::Any;
use std::sync::Mutex;
use std::time::Duration;

fn leak<T>(value: T) -> *mut c_void {
    Box::into_raw(Box::new(value)) as *mut c_void
}

fn slice<'a>(data: *const u8, size: u64) -> &'a [u8] {
    if data.is_null() || size == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(data, size as usize) }
    }
}

fn bytes_out(fetch: impl Fn(*mut u8, u64, *mut u64) -> i32) -> Result<Vec<u8>> {
    let mut length = 0u64;
    let probe = fetch(core::ptr::null_mut(), 0, &mut length);
    check(probe)?;
    let mut buffer = vec![0u8; length as usize];
    if !buffer.is_empty() {
        check(fetch(buffer.as_mut_ptr(), buffer.len() as u64, &mut length))?;
    }
    Ok(buffer)
}

fn object_pair(begin: impl FnOnce(*mut ffi::SturdyAsyncObject, *mut ffi::SturdyAsyncObject) -> i32) -> Result<(Object, Object)> {
    let (mut a, mut b) = (ffi::SturdyAsyncObject::default(), ffi::SturdyAsyncObject::default());
    check(begin(&mut a, &mut b))?;
    Ok((Object::from_raw(a), Object::from_raw(b)))
}

fn one_object(begin: impl FnOnce(*mut ffi::SturdyAsyncObject) -> i32) -> Result<Object> {
    let mut raw = ffi::SturdyAsyncObject::default();
    check(begin(&mut raw))?;
    Ok(Object::from_raw(raw))
}

/// Packs several byte strings into one (a 32-bit count, then length-prefixed frames): what stream items and some operations use when they
/// carry more than one value.
pub fn pack_frames(frames: &[&[u8]]) -> Result<Vec<u8>> {
    let views: Vec<ffi::SturdyBytesView> = frames.iter().map(|f| ffi::SturdyBytesView { data: f.as_ptr(), size: f.len() as u64 }).collect();
    bytes_out(|buffer, capacity, length| unsafe { ffi::sturdy_frames_pack(views.as_ptr(), views.len() as u32, buffer, capacity, length) })
}

/// The inverse of [`pack_frames`].
pub fn unpack_frames(packed: &[u8]) -> Result<Vec<Vec<u8>>> {
    let mut count = 0u32;
    check(unsafe { ffi::sturdy_frames_count(packed.as_ptr(), packed.len() as u64, &mut count) })?;
    (0..count)
        .map(|index| bytes_out(|buffer, capacity, length| unsafe { ffi::sturdy_frames_get(packed.as_ptr(), packed.len() as u64, index, buffer, capacity, length) }))
        .collect()
}

// ------------------------------------------------------------------------------------------------------------------------------
// Cancellation
// ------------------------------------------------------------------------------------------------------------------------------

/// Cancel once, and everything armed on its tokens reacts.
pub struct CancelSource {
    object: Object,
}

impl CancelSource {
    pub fn new() -> Result<CancelSource> {
        Ok(CancelSource { object: one_object(|out| unsafe { ffi::sturdy_cancel_source_create(out) })? })
    }

    /// Cancels. True only for the first call.
    pub fn cancel(&self) -> bool {
        let mut first = 0u8;
        unsafe { ffi::sturdy_cancel_source_cancel(self.object.raw(), &mut first) };
        first != 0
    }

    pub fn is_cancelled(&self) -> bool {
        let mut cancelled = 0u8;
        unsafe { ffi::sturdy_cancel_source_is_cancelled(self.object.raw(), &mut cancelled) };
        cancelled != 0
    }

    /// A token observing this source.
    pub fn token(&self) -> Result<CancelToken> {
        Ok(CancelToken { raw: one_object(|out| unsafe { ffi::sturdy_cancel_source_token(self.object.raw(), out) })?.into_raw(), owned: true })
    }
}

/// Observes a [`CancelSource`]. A task's token is borrowed: valid only inside the task.
pub struct CancelToken {
    raw: ffi::SturdyAsyncObject,
    owned: bool,
}

impl CancelToken {
    pub fn is_cancelled(&self) -> bool {
        let mut cancelled = 0u8;
        unsafe { ffi::sturdy_cancel_token_is_cancelled(self.raw, &mut cancelled) };
        cancelled != 0
    }

    /// Completes when the token is cancelled (at once if it already is).
    pub async fn cancelled(&self) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_cancel_token_wait(self.raw, out) }).await.map(|_| ())
    }

    pub(crate) fn raw(&self) -> ffi::SturdyAsyncObject {
        self.raw
    }

    /// A token valid only while the engine's callback runs (it is not released).
    pub(crate) fn borrowed(raw: ffi::SturdyAsyncObject) -> CancelToken {
        CancelToken { raw, owned: false }
    }
}

impl Drop for CancelToken {
    fn drop(&mut self) {
        if self.owned {
            unsafe { ffi::sturdy_object_release(self.raw) };
        }
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Channels
// ------------------------------------------------------------------------------------------------------------------------------

/// The result of a non-blocking receive.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TryRecv {
    Message(Vec<u8>),
    Empty,
    Closed,
}

/// The result of a non-blocking send.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrySend {
    Sent,
    Full,
    Closed,
}

/// Creates a bounded multi-producer channel holding up to `capacity` messages (at least 1). It closes when the last [`Sender`] is dropped
/// or [`Sender::close`] / [`Receiver::close`] is called; buffered messages are still delivered.
pub fn channel(capacity: u64) -> Result<(Sender, Receiver)> {
    let (tx, rx) = object_pair(|a, b| unsafe { ffi::sturdy_channel_create(capacity, a, b) })?;
    Ok((Sender { object: tx }, Receiver { object: rx }))
}

pub struct Sender {
    object: Object,
}

impl Sender {
    /// Finishes once the message is buffered or handed to a receiver (waits while the channel is full); `Closed` when closed.
    pub async fn send(&self, data: &[u8]) -> Result<()> {
        let (ptr, len) = bytes_ptr(data);
        run(|out| unsafe { ffi::sturdy_channel_send(self.object.raw(), ptr, len, out) }).await.map(|_| ())
    }

    pub fn try_send(&self, data: &[u8]) -> Result<TrySend> {
        let (ptr, len) = bytes_ptr(data);
        let mut status = 0;
        check(unsafe { ffi::sturdy_channel_try_send(self.object.raw(), ptr, len, &mut status) })?;
        Ok(match status {
            ffi::STURDY_TRY_OK => TrySend::Sent,
            ffi::STURDY_TRY_WOULD_BLOCK => TrySend::Full,
            _ => TrySend::Closed,
        })
    }

    pub fn close(&self) {
        unsafe { ffi::sturdy_channel_close(self.object.raw()) };
    }

    pub(crate) fn raw(&self) -> ffi::SturdyAsyncObject {
        self.object.raw()
    }
}

impl Clone for Sender {
    fn clone(&self) -> Self {
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_channel_sender_clone(self.object.raw(), &mut raw) }).expect("cloning a channel sender");
        Sender { object: Object::from_raw(raw) }
    }
}

pub struct Receiver {
    object: Object,
}

impl Receiver {
    /// The next message, or `None` once the channel is closed and drained.
    pub async fn recv(&self) -> Result<Option<Vec<u8>>> {
        let reply = run(|out| unsafe { ffi::sturdy_channel_recv(self.object.raw(), out) }).await?;
        Ok((reply.number() == 1).then(|| reply.bytes()))
    }

    pub fn try_recv(&self) -> Result<TryRecv> {
        let mut status = 0;
        let mut op = ffi::SturdyAsyncOp::default();
        check(unsafe { ffi::sturdy_channel_try_recv(self.object.raw(), &mut op, &mut status) })?;
        Ok(match status {
            ffi::STURDY_TRY_OK => {
                let reply = crate::Reply::from_raw(op);
                TryRecv::Message(reply.bytes())
            }
            ffi::STURDY_TRY_WOULD_BLOCK => TryRecv::Empty,
            _ => TryRecv::Closed,
        })
    }

    pub fn close(&self) {
        unsafe { ffi::sturdy_channel_close(self.object.raw()) };
    }

    /// The messages as a [`Stream`].
    pub fn into_stream(self) -> Result<Stream> {
        Stream::make(Vec::new(), |out| unsafe { ffi::sturdy_stream_from_channel(self.object.raw(), out) })
    }
}

impl Clone for Receiver {
    fn clone(&self) -> Self {
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_channel_receiver_clone(self.object.raw(), &mut raw) }).expect("cloning a channel receiver");
        Receiver { object: Object::from_raw(raw) }
    }
}

/// One-shot value hand-off. A [`OneshotSender`] dropped without sending fails the receiver with `Closed`.
pub fn oneshot() -> Result<(OneshotSender, OneshotReceiver)> {
    let (tx, rx) = object_pair(|a, b| unsafe { ffi::sturdy_oneshot_create(a, b) })?;
    Ok((OneshotSender { object: tx }, OneshotReceiver { object: rx }))
}

pub struct OneshotSender {
    object: Object,
}

impl OneshotSender {
    /// True when this call set the value.
    pub fn send(&self, data: &[u8]) -> Result<bool> {
        let (ptr, len) = bytes_ptr(data);
        let mut first = 0u8;
        check(unsafe { ffi::sturdy_oneshot_send(self.object.raw(), ptr, len, &mut first) })?;
        Ok(first != 0)
    }

    /// Fails the receiver with `message`. True when this call settled it.
    pub fn fail(&self, message: &str) -> Result<bool> {
        let mut cx = Lower::default();
        let message = cx.string(message);
        let mut first = 0u8;
        check(unsafe { ffi::sturdy_oneshot_fail(self.object.raw(), message, &mut first) })?;
        Ok(first != 0)
    }
}

pub struct OneshotReceiver {
    object: Object,
}

impl OneshotReceiver {
    /// The value. A receiver can be waited on once.
    pub async fn recv(&self) -> Result<Vec<u8>> {
        Ok(run(|out| unsafe { ffi::sturdy_oneshot_recv(self.object.raw(), out) }).await?.bytes())
    }
}

/// A single value many tasks observe; each [`WatchReceiver`] tracks what it has seen on its own.
pub fn watch(initial: &[u8]) -> Result<(WatchSender, WatchReceiver)> {
    let (ptr, len) = bytes_ptr(initial);
    let (tx, rx) = object_pair(|a, b| unsafe { ffi::sturdy_watch_value_create(ptr, len, a, b) })?;
    Ok((WatchSender { object: tx }, WatchReceiver { object: rx }))
}

pub struct WatchSender {
    object: Object,
}

impl WatchSender {
    pub fn send(&self, data: &[u8]) -> Result<()> {
        let (ptr, len) = bytes_ptr(data);
        check(unsafe { ffi::sturdy_watch_value_send(self.object.raw(), ptr, len) })
    }

    pub fn subscribe(&self) -> Result<WatchReceiver> {
        Ok(WatchReceiver { object: one_object(|out| unsafe { ffi::sturdy_watch_value_subscribe(self.object.raw(), out) })? })
    }
}

pub struct WatchReceiver {
    object: Object,
}

impl WatchReceiver {
    /// The current value; does not mark it as seen.
    pub fn borrow(&self) -> Result<Vec<u8>> {
        bytes_out(|b, c, l| unsafe { ffi::sturdy_watch_value_borrow(self.object.raw(), b, c, l) })
    }

    /// The current value, marked as seen.
    pub fn borrow_and_update(&self) -> Result<Vec<u8>> {
        bytes_out(|b, c, l| unsafe { ffi::sturdy_watch_value_borrow_and_update(self.object.raw(), b, c, l) })
    }

    pub fn has_changed(&self) -> bool {
        let mut changed = 0u8;
        unsafe { ffi::sturdy_watch_value_has_changed(self.object.raw(), &mut changed) };
        changed != 0
    }

    /// Completes when the value changed since this receiver last looked; `Closed` once the sender is gone and nothing new remains.
    pub async fn changed(&self) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_watch_value_changed(self.object.raw(), out) }).await.map(|_| ())
    }
}

impl Clone for WatchReceiver {
    fn clone(&self) -> Self {
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_watch_value_receiver_clone(self.object.raw(), &mut raw) }).expect("cloning a watch receiver");
        WatchReceiver { object: Object::from_raw(raw) }
    }
}

/// What a [`BroadcastReceiver`] got.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Broadcast {
    Message(Vec<u8>),
    /// This receiver fell behind and missed this many messages.
    Lagged(u64),
    Closed,
}

/// Every receiver sees every message; one that falls `capacity` behind is told how many it missed.
pub fn broadcast(capacity: u64) -> Result<(BroadcastSender, BroadcastReceiver)> {
    let (tx, rx) = object_pair(|a, b| unsafe { ffi::sturdy_broadcast_create(capacity, a, b) })?;
    Ok((BroadcastSender { object: tx }, BroadcastReceiver { object: rx }))
}

pub struct BroadcastSender {
    object: Object,
}

impl BroadcastSender {
    /// Returns the number of live receivers.
    pub fn send(&self, data: &[u8]) -> Result<u64> {
        let (ptr, len) = bytes_ptr(data);
        let mut receivers = 0u64;
        check(unsafe { ffi::sturdy_broadcast_send(self.object.raw(), ptr, len, &mut receivers) })?;
        Ok(receivers)
    }

    /// A receiver that sees messages sent from now on.
    pub fn subscribe(&self) -> Result<BroadcastReceiver> {
        Ok(BroadcastReceiver { object: one_object(|out| unsafe { ffi::sturdy_broadcast_subscribe(self.object.raw(), out) })? })
    }
}

pub struct BroadcastReceiver {
    object: Object,
}

impl BroadcastReceiver {
    /// The next message for this receiver. Cannot be cancelled once started.
    pub async fn recv(&self) -> Result<Broadcast> {
        let reply = run(|out| unsafe { ffi::sturdy_broadcast_recv(self.object.raw(), out) }).await?;
        Ok(match reply.number() {
            0 => Broadcast::Message(reply.bytes()),
            1 => Broadcast::Lagged(reply.field("skipped").and_then(|s| s.parse().ok()).unwrap_or(0)),
            _ => Broadcast::Closed,
        })
    }
}

impl Clone for BroadcastReceiver {
    fn clone(&self) -> Self {
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_broadcast_receiver_clone(self.object.raw(), &mut raw) }).expect("cloning a broadcast receiver");
        BroadcastReceiver { object: Object::from_raw(raw) }
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Semaphore, locks, notify, barrier
// ------------------------------------------------------------------------------------------------------------------------------

/// A counting semaphore. Permits are returned when their [`Permit`] is dropped.
pub struct Semaphore {
    object: Object,
}

impl Semaphore {
    pub fn new(permits: u64) -> Result<Semaphore> {
        Ok(Semaphore { object: one_object(|out| unsafe { ffi::sturdy_semaphore_create(permits, out) })? })
    }

    /// Waits for `count` permits; `Closed` if the semaphore closes first.
    pub async fn acquire(&self, count: u64) -> Result<Permit> {
        let object = run_object(|out| unsafe { ffi::sturdy_semaphore_acquire(self.object.raw(), count, out) }).await?;
        Ok(Permit { object })
    }

    /// `None` when the permits are not available.
    pub fn try_acquire(&self, count: u64) -> Result<Option<Permit>> {
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_semaphore_try_acquire(self.object.raw(), count, &mut raw) })?;
        Ok((raw.token != 0).then(|| Permit { object: Object::from_raw(raw) }))
    }

    pub fn add_permits(&self, count: u64) {
        unsafe { ffi::sturdy_semaphore_add_permits(self.object.raw(), count) };
    }

    /// Fails every waiter and every future acquire.
    pub fn close(&self) {
        unsafe { ffi::sturdy_semaphore_close(self.object.raw()) };
    }

    pub fn available(&self) -> u64 {
        let mut permits = 0u64;
        unsafe { ffi::sturdy_semaphore_available(self.object.raw(), &mut permits) };
        permits
    }

    pub fn is_closed(&self) -> bool {
        let mut closed = 0u8;
        unsafe { ffi::sturdy_semaphore_is_closed(self.object.raw(), &mut closed) };
        closed != 0
    }
}

/// Holds permits; returns them on drop.
pub struct Permit {
    object: Object,
}

impl Permit {
    pub fn count(&self) -> u64 {
        let mut count = 0u64;
        unsafe { ffi::sturdy_permit_count(self.object.raw(), &mut count) };
        count
    }

    /// Keeps the permits consumed forever.
    pub fn forget(self) {
        unsafe { ffi::sturdy_permit_forget(self.object.raw()) };
    }
}

impl Drop for Permit {
    fn drop(&mut self) {
        // Released explicitly: the permit's object also stays reachable through the op that produced it until that is released.
        unsafe { ffi::sturdy_permit_release(self.object.raw()) };
    }
}

/// An async mutex protecting a byte-string value; waiting costs no thread.
pub struct AsyncMutex {
    object: Object,
}

impl AsyncMutex {
    pub fn new(initial: &[u8]) -> Result<AsyncMutex> {
        let (ptr, len) = bytes_ptr(initial);
        Ok(AsyncMutex { object: one_object(|out| unsafe { ffi::sturdy_async_mutex_create(ptr, len, out) })? })
    }

    pub async fn lock(&self) -> Result<Guard> {
        let object = run_object(|out| unsafe { ffi::sturdy_async_mutex_lock(self.object.raw(), out) }).await?;
        Ok(Guard { object })
    }

    /// `None` when the mutex is held.
    pub fn try_lock(&self) -> Result<Option<Guard>> {
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_async_mutex_try_lock(self.object.raw(), &mut raw) })?;
        Ok((raw.token != 0).then(|| Guard { object: Object::from_raw(raw) }))
    }
}

/// An async read-write lock: many readers or one writer; waiters queue in order, so a waiting writer is not starved.
pub struct AsyncRwLock {
    object: Object,
}

impl AsyncRwLock {
    pub fn new(initial: &[u8]) -> Result<AsyncRwLock> {
        let (ptr, len) = bytes_ptr(initial);
        Ok(AsyncRwLock { object: one_object(|out| unsafe { ffi::sturdy_async_rwlock_create(ptr, len, out) })? })
    }

    pub async fn read(&self) -> Result<Guard> {
        Ok(Guard { object: run_object(|out| unsafe { ffi::sturdy_async_rwlock_read(self.object.raw(), out) }).await? })
    }

    pub async fn write(&self) -> Result<Guard> {
        Ok(Guard { object: run_object(|out| unsafe { ffi::sturdy_async_rwlock_write(self.object.raw(), out) }).await? })
    }
}

/// A held lock. Unlocks on drop.
pub struct Guard {
    object: Object,
}

impl Guard {
    /// The protected value.
    pub fn get(&self) -> Result<Vec<u8>> {
        bytes_out(|b, c, l| unsafe { ffi::sturdy_guard_get(self.object.raw(), b, c, l) })
    }

    /// Replaces the value (mutex and write guards only).
    pub fn set(&self, data: &[u8]) -> Result<()> {
        let (ptr, len) = bytes_ptr(data);
        check(unsafe { ffi::sturdy_guard_set(self.object.raw(), ptr, len) })
    }
}

impl Drop for Guard {
    fn drop(&mut self) {
        unsafe { ffi::sturdy_guard_release(self.object.raw()) };
    }
}

/// A wake-up signal with at most one stored permit.
pub struct Notify {
    object: Object,
}

impl Notify {
    pub fn new() -> Result<Notify> {
        Ok(Notify { object: one_object(|out| unsafe { ffi::sturdy_notify_create(out) })? })
    }

    /// Completes when notified; a [`Notify::notify_one`] that happened earlier with nobody waiting is consumed first.
    pub async fn notified(&self) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_notify_notified(self.object.raw(), out) }).await.map(|_| ())
    }

    pub fn notify_one(&self) {
        unsafe { ffi::sturdy_notify_one(self.object.raw()) };
    }

    pub fn notify_waiters(&self) {
        unsafe { ffi::sturdy_notify_waiters(self.object.raw()) };
    }
}

/// A rendezvous of `parties` callers.
pub struct Barrier {
    object: Object,
}

impl Barrier {
    pub fn new(parties: u64) -> Result<Barrier> {
        Ok(Barrier { object: one_object(|out| unsafe { ffi::sturdy_barrier_create(parties, out) })? })
    }

    /// Completes when all parties arrived; true for exactly one of them (the last).
    pub async fn wait(&self) -> Result<bool> {
        Ok(run(|out| unsafe { ffi::sturdy_barrier_wait(self.object.raw(), out) }).await?.number() == 1)
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Tasks
// ------------------------------------------------------------------------------------------------------------------------------

/// What a task returns.
#[derive(Debug, Clone, Default)]
pub struct TaskOutput {
    pub number: i64,
    pub bytes: Vec<u8>,
    pub text: String,
}

/// A task's result: its output, or an error message that fails it.
pub type TaskResult = std::result::Result<TaskOutput, String>;

/// Where a task runs.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Lane {
    /// The worker pool, for short work.
    Light,
    /// The worker pool, for long work.
    Heavy,
    /// The bounded blocking-IO lane, for work that waits on the OS.
    Blocking,
}

impl Lane {
    fn raw(self) -> i32 {
        match self {
            Lane::Light => ffi::STURDY_LANE_LIGHT,
            Lane::Heavy => ffi::STURDY_LANE_HEAVY,
            Lane::Blocking => ffi::STURDY_LANE_BLOCKING,
        }
    }
}

unsafe extern "C" fn work_trampoline<F: FnOnce(&CancelToken) -> TaskResult>(user_data: *mut c_void, token: ffi::SturdyAsyncObject, output: *mut ffi::SturdyTaskOutput) {
    let work = *unsafe { Box::from_raw(user_data as *mut F) };
    let token = CancelToken { raw: token, owned: false };
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| work(&token))).unwrap_or_else(|_| Err("the task panicked".to_string()));
    let mut cx = Lower::default();
    match result {
        Ok(out) => {
            let text = cx.string(&out.text);
            let (ptr, len) = bytes_ptr(&out.bytes);
            unsafe { ffi::sturdy_task_output_set(output, out.number, ptr, len, text, core::ptr::null()) };
        }
        Err(message) => {
            let error = cx.string(if message.is_empty() { "the task failed" } else { &message });
            unsafe { ffi::sturdy_task_output_set(output, 0, core::ptr::null(), 0, core::ptr::null(), error) };
        }
    }
}

fn spawn_with<F: FnOnce(&CancelToken) -> TaskResult + Send + 'static>(work: F, begin: impl FnOnce(ffi::SturdyTaskWorkFn, *mut c_void) -> i32) -> Result<()> {
    let user_data = leak(work);
    let code = begin(work_trampoline::<F>, user_data);
    if code != ffi::STURDY_OK {
        // The engine never ran the work: take it back so it is freed.
        drop(unsafe { Box::from_raw(user_data as *mut F) });
    }
    check(code)
}

/// Runs `work` on `lane`. Await the [`Op`] for the task's result (number, bytes, text); an `Err` fails it. Cancelling the op sets the token
/// `work` polls.
pub fn spawn<F: FnOnce(&CancelToken) -> TaskResult + Send + 'static>(lane: Lane, work: F) -> Result<Op> {
    let mut slot = None;
    spawn_with(work, |function, user_data| {
        match Op::start(|out| unsafe { ffi::sturdy_task_spawn(Some(function), user_data, lane.raw(), out) }) {
            Ok(op) => {
                slot = Some(op);
                ffi::STURDY_OK
            }
            Err(error) => match error.kind {
                ErrorKind::Abi(code) => code,
                _ => ffi::STURDY_ERROR_INITIALIZATION_FAILED,
            },
        }
    })?;
    slot.ok_or_else(|| Error::new(ErrorKind::Unknown, "the task did not start"))
}

/// Same, on a custom pool ([`crate::ThreadPool`]).
pub fn spawn_on<F: FnOnce(&CancelToken) -> TaskResult + Send + 'static>(pool: &crate::ThreadPool, work: F) -> Result<Op> {
    let mut slot = None;
    spawn_with(work, |function, user_data| {
        match Op::start(|out| unsafe { ffi::sturdy_task_spawn_on(pool.raw(), Some(function), user_data, out) }) {
            Ok(op) => {
                slot = Some(op);
                ffi::STURDY_OK
            }
            Err(error) => match error.kind {
                ErrorKind::Abi(code) => code,
                _ => ffi::STURDY_ERROR_INITIALIZATION_FAILED,
            },
        }
    })?;
    slot.ok_or_else(|| Error::new(ErrorKind::Unknown, "the task did not start"))
}

/// How one task of a [`JoinSet`] ended.
#[derive(Debug, Clone)]
pub enum Joined {
    Done(TaskOutput),
    Failed(String),
    /// The set had no tasks left.
    Empty,
}

/// A dynamic set of tasks joined in completion order.
pub struct JoinSet {
    object: Object,
}

impl JoinSet {
    pub fn new() -> Result<JoinSet> {
        Ok(JoinSet { object: one_object(|out| unsafe { ffi::sturdy_join_set_create(out) })? })
    }

    pub fn spawn<F: FnOnce(&CancelToken) -> TaskResult + Send + 'static>(&self, lane: Lane, work: F) -> Result<()> {
        spawn_with(work, |function, user_data| unsafe { ffi::sturdy_join_set_spawn(self.object.raw(), Some(function), user_data, lane.raw()) })
    }

    /// The next task to complete.
    pub async fn join_next(&self) -> Result<Joined> {
        let reply = run(|out| unsafe { ffi::sturdy_join_set_join_next(self.object.raw(), out) }).await?;
        Ok(match reply.field("outcome").as_deref() {
            Some("ok") => Joined::Done(TaskOutput { number: reply.number(), bytes: reply.bytes(), text: reply.text() }),
            Some("failed") => Joined::Failed(reply.field("error").unwrap_or_default()),
            _ => Joined::Empty,
        })
    }

    pub fn len(&self) -> u64 {
        let mut len = 0u64;
        unsafe { ffi::sturdy_join_set_len(self.object.raw(), &mut len) };
        len
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Sets the cancel token of every running task in the set.
    pub fn abort_all(&self) {
        unsafe { ffi::sturdy_join_set_abort_all(self.object.raw()) };
    }
}

/// Tasks that run only on the thread that pumps the set.
pub struct LocalSet {
    object: Object,
}

impl LocalSet {
    pub fn new() -> Result<LocalSet> {
        Ok(LocalSet { object: one_object(|out| unsafe { ffi::sturdy_local_set_create(out) })? })
    }

    /// Queues `work`; it runs when this thread pumps the set ([`LocalSet::run_pending`] / [`LocalSet::block_on`]).
    pub fn spawn<F: FnOnce(&CancelToken) -> TaskResult + Send + 'static>(&self, work: F) -> Result<Op> {
        let mut slot = None;
        spawn_with(work, |function, user_data| {
            match Op::start(|out| unsafe { ffi::sturdy_local_set_spawn(self.object.raw(), Some(function), user_data, out) }) {
                Ok(op) => {
                    slot = Some(op);
                    ffi::STURDY_OK
                }
                Err(error) => match error.kind {
                    ErrorKind::Abi(code) => code,
                    _ => ffi::STURDY_ERROR_INITIALIZATION_FAILED,
                },
            }
        })?;
        slot.ok_or_else(|| Error::new(ErrorKind::Unknown, "the task did not start"))
    }

    /// Runs queued tasks on the calling thread until none are left; returns how many ran.
    pub fn run_pending(&self) -> u64 {
        let mut ran = 0u64;
        unsafe { ffi::sturdy_local_set_run_pending(self.object.raw(), &mut ran) };
        ran
    }

    /// Runs queued tasks on the calling thread until `op` finishes.
    pub fn block_on(&self, op: &Op) -> Result<()> {
        check(unsafe { ffi::sturdy_local_set_block_on(self.object.raw(), op.raw()) })
    }
}

/// A named thread that runs submitted work one at a time, in order.
pub struct DedicatedThread {
    object: Object,
}

impl DedicatedThread {
    pub fn new(name: &str) -> Result<DedicatedThread> {
        let mut cx = Lower::default();
        let name = cx.string(name);
        Ok(DedicatedThread { object: one_object(|out| unsafe { ffi::sturdy_dedicated_thread_create(name, out) })? })
    }

    pub fn run<F: FnOnce(&CancelToken) -> TaskResult + Send + 'static>(&self, work: F) -> Result<Op> {
        let mut slot = None;
        spawn_with(work, |function, user_data| {
            match Op::start(|out| unsafe { ffi::sturdy_dedicated_thread_run(self.object.raw(), Some(function), user_data, out) }) {
                Ok(op) => {
                    slot = Some(op);
                    ffi::STURDY_OK
                }
                Err(error) => match error.kind {
                    ErrorKind::Abi(code) => code,
                    _ => ffi::STURDY_ERROR_INITIALIZATION_FAILED,
                },
            }
        })?;
        slot.ok_or_else(|| Error::new(ErrorKind::Unknown, "the task did not start"))
    }

    pub fn pin_to_core(&self, core_index: u32) -> bool {
        let mut pinned = 0u8;
        unsafe { ffi::sturdy_dedicated_thread_pin_to_core(self.object.raw(), core_index, &mut pinned) };
        pinned != 0
    }

    pub fn pin_to_fastest_core(&self) -> bool {
        let mut pinned = 0u8;
        unsafe { ffi::sturdy_dedicated_thread_pin_to_fastest_core(self.object.raw(), &mut pinned) };
        pinned != 0
    }
}

unsafe extern "C" fn once_trampoline<F: FnOnce()>(user_data: *mut c_void) {
    let work = *unsafe { Box::from_raw(user_data as *mut F) };
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(work));
}

unsafe extern "C" fn borrowed_trampoline<F: FnOnce()>(user_data: *mut c_void) {
    // The closure lives on the caller's stack for the duration of the call.
    let slot = unsafe { &mut *(user_data as *mut Option<F>) };
    if let Some(work) = slot.take() {
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(work));
    }
}

/// Values that follow the flow of work: tasks spawned while a value is bound inherit it.
pub struct TaskLocal {
    object: Object,
}

impl TaskLocal {
    pub fn new() -> Result<TaskLocal> {
        Ok(TaskLocal { object: one_object(|out| unsafe { ffi::sturdy_task_local_create(out) })? })
    }

    /// Runs `body` with `value` bound.
    pub fn scope<F: FnOnce()>(&self, value: &[u8], body: F) -> Result<()> {
        let (ptr, len) = bytes_ptr(value);
        let mut slot = Some(body);
        check(unsafe { ffi::sturdy_task_local_scope(self.object.raw(), ptr, len, Some(borrowed_trampoline::<F>), &mut slot as *mut Option<F> as *mut c_void) })
    }

    /// The bound value, if any.
    pub fn get(&self) -> Option<Vec<u8>> {
        bytes_out(|b, c, l| unsafe { ffi::sturdy_task_local_get(self.object.raw(), b, c, l) }).ok()
    }
}

/// Work for the engine's main thread, run when it pumps.
pub mod main_thread {
    use super::*;

    /// Queues `work` for the main thread.
    pub fn run<F: FnOnce() + Send + 'static>(work: F) -> Result<()> {
        let user_data = leak(work);
        let code = unsafe { ffi::sturdy_main_thread_run(Some(once_trampoline::<F>), user_data) };
        if code != ffi::STURDY_OK {
            drop(unsafe { Box::from_raw(user_data as *mut F) });
        }
        check(code)
    }

    /// Runs the queued main-thread work on the calling thread.
    pub fn pump() -> Result<()> {
        check(unsafe { ffi::sturdy_main_thread_pump() })
    }

    unsafe extern "C" fn hook_trampoline(user_data: *mut c_void) {
        let hook = unsafe { &*(user_data as *const Box<dyn Fn() + Send + Sync>) };
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| hook()));
    }

    /// Installs a callback run (on the posting thread) after every queued job, so a loop that blocks waiting for events can wake and pump.
    /// Replacing a hook leaks the previous closure (it may be mid-call on another thread).
    pub fn set_wake_hook(hook: Option<Box<dyn Fn() + Send + Sync>>) -> Result<()> {
        match hook {
            Some(hook) => check(unsafe { ffi::sturdy_main_thread_set_wake_hook(Some(hook_trampoline), leak(hook)) }),
            None => check(unsafe { ffi::sturdy_main_thread_set_wake_hook(None, core::ptr::null_mut()) }),
        }
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Runtime, topology, timers, signals
// ------------------------------------------------------------------------------------------------------------------------------

/// The worker pool and the engine's other execution lanes.
pub mod runtime {
    use super::*;

    /// Starts the worker pool (0 workers: one per hardware thread). The engine starts it on first use; call this only to choose the size
    /// first, or in a program that does not run the engine.
    pub fn initialize_scheduler(workers: u32) -> Result<()> {
        check(unsafe { ffi::sturdy_scheduler_initialize(workers) })
    }

    /// Stops the worker pool. The engine owns it: only a standalone program should call this.
    pub fn shutdown_scheduler() -> Result<()> {
        check(unsafe { ffi::sturdy_scheduler_shutdown() })
    }

    /// Whether the worker pool is running.
    pub fn scheduler_is_running() -> bool {
        let mut running = 0u8;
        unsafe { ffi::sturdy_async_is_running(&mut running) };
        running != 0
    }

    pub fn worker_count() -> u32 {
        let mut count = 0u32;
        unsafe { ffi::sturdy_async_worker_count(&mut count) };
        count
    }

    /// Whether the calling thread is a scheduler worker (do not block there).
    pub fn is_worker_thread() -> bool {
        let mut worker = 0u8;
        unsafe { ffi::sturdy_async_is_worker_thread(&mut worker) };
        worker != 0
    }

    /// Tunes the blocking-IO lane (0 workers: automatic; 0 `max_pending`: 1024). False once it already started.
    pub fn configure_io_lane(workers: u32, max_pending: u32) -> Result<bool> {
        let mut applied = 0u8;
        check(unsafe { ffi::sturdy_io_lane_configure(workers, max_pending, &mut applied) })?;
        Ok(applied != 0)
    }

    /// `backend`, `workers`, `pending`, `capacity`.
    pub async fn io_lane_stats() -> Result<Vec<(String, String)>> {
        Ok(run(|out| unsafe { ffi::sturdy_io_lane_stats(out) }).await?.fields())
    }

    /// Sets the number of network reactor threads. False once the reactor is running.
    pub fn configure_network_reactor(threads: u32) -> Result<bool> {
        let mut applied = 0u8;
        check(unsafe { ffi::sturdy_network_reactor_configure(threads, &mut applied) })?;
        Ok(applied != 0)
    }

    /// Logical (or physical) cores ranked fastest first.
    pub fn ranked_cores(physical: bool) -> Result<Vec<u32>> {
        let mut count = 0u64;
        check(unsafe { ffi::sturdy_topology_ranked_cores(flag(physical), core::ptr::null_mut(), 0, &mut count) })?;
        let mut cores = vec![0u32; count as usize];
        if !cores.is_empty() {
            check(unsafe { ffi::sturdy_topology_ranked_cores(flag(physical), cores.as_mut_ptr(), cores.len() as u64, &mut count) })?;
        }
        Ok(cores)
    }

    /// Pins the calling thread to a core. False where the platform refuses.
    pub fn pin_current_thread(core_index: u32) -> bool {
        let mut pinned = 0u8;
        unsafe { ffi::sturdy_thread_pin_current(core_index, &mut pinned) };
        pinned != 0
    }

    /// Finishes at an absolute wall-clock time (milliseconds since the Unix epoch), at once if it has passed.
    pub async fn sleep_until_unix_ms(unix_ms: i64) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_timer_sleep_until(unix_ms, out) }).await.map(|_| ())
    }

    /// The first of `signals` (SIGINT, SIGTERM, ...) delivered to the process.
    pub async fn wait_for_signal(signals: &[i32]) -> Result<i32> {
        Ok(run(|out| unsafe { ffi::sturdy_signal_wait(signals.as_ptr(), signals.len() as u32, out) }).await?.number() as i32)
    }

    /// Completes when the user interrupts the process (Ctrl-C / SIGINT).
    pub async fn ctrl_c() -> Result<i32> {
        Ok(run(|out| unsafe { ffi::sturdy_signal_ctrl_c(out) }).await?.number() as i32)
    }
}

/// A periodic timer that does not drift.
pub struct Interval {
    object: Object,
}

impl Interval {
    pub fn new(period: Duration) -> Result<Interval> {
        Ok(Interval { object: one_object(|out| unsafe { ffi::sturdy_interval_create(millis(Some(period)), out) })? })
    }

    /// Completes at each period boundary.
    pub async fn tick(&self) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_interval_tick(self.object.raw(), out) }).await.map(|_| ())
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Streams
// ------------------------------------------------------------------------------------------------------------------------------

type Keep = Box<dyn Any + Send>;

/// A pipeline of byte messages. A combinator or sink takes (consumes) its input; callbacks run on whichever thread completed the previous
/// stage, so keep them short.
pub struct Stream {
    object: Object,
    /// Closures the engine may still call; they live as long as the stream (and whatever it was combined into).
    keep: Vec<Keep>,
}

/// Frees nothing if a sink future is dropped before its operation finished: the engine may still call the closures, so they are leaked.
struct KeepGuard {
    keep: Option<Vec<Keep>>,
    finished: bool,
}

impl Drop for KeepGuard {
    fn drop(&mut self) {
        if !self.finished {
            if let Some(keep) = self.keep.take() {
                std::mem::forget(keep);
            }
        }
    }
}

unsafe fn sink_set(sink: *mut ffi::SturdyBytesSink, data: &[u8]) {
    let (ptr, len) = bytes_ptr(data);
    unsafe { ffi::sturdy_bytes_sink_set(sink, ptr, len) };
}

unsafe extern "C" fn map_trampoline<F: Fn(&[u8]) -> Vec<u8>>(user_data: *mut c_void, data: *const u8, size: u64, out: *mut ffi::SturdyBytesSink) {
    let f = unsafe { &*(user_data as *const F) };
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(slice(data, size)))).unwrap_or_default();
    unsafe { sink_set(out, &result) };
}

unsafe extern "C" fn predicate_trampoline<F: Fn(&[u8]) -> bool>(user_data: *mut c_void, data: *const u8, size: u64) -> u8 {
    let f = unsafe { &*(user_data as *const F) };
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(slice(data, size)))).unwrap_or(false) as u8
}

unsafe extern "C" fn filter_map_trampoline<F: Fn(&[u8]) -> Option<Vec<u8>>>(user_data: *mut c_void, data: *const u8, size: u64, out: *mut ffi::SturdyBytesSink) -> u8 {
    let f = unsafe { &*(user_data as *const F) };
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(slice(data, size)))).unwrap_or(None) {
        Some(value) => {
            unsafe { sink_set(out, &value) };
            1
        }
        None => 0,
    }
}

unsafe extern "C" fn each_trampoline<F: Fn(&[u8])>(user_data: *mut c_void, data: *const u8, size: u64) {
    let f = unsafe { &*(user_data as *const F) };
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(slice(data, size))));
}

unsafe extern "C" fn fold_trampoline<F: Fn(&[u8], &[u8]) -> Vec<u8>>(user_data: *mut c_void, acc: *const u8, acc_size: u64, item: *const u8, item_size: u64, out: *mut ffi::SturdyBytesSink) {
    let f = unsafe { &*(user_data as *const F) };
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(slice(acc, acc_size), slice(item, item_size)))).unwrap_or_default();
    unsafe { sink_set(out, &result) };
}

unsafe extern "C" fn source_trampoline<F: FnMut() -> Option<Vec<u8>>>(user_data: *mut c_void, out: *mut ffi::SturdyBytesSink) -> u8 {
    let f = unsafe { &*(user_data as *const Mutex<F>) };
    let item = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| (f.lock().unwrap())())).unwrap_or(None);
    match item {
        Some(value) => {
            unsafe { sink_set(out, &value) };
            1
        }
        None => 0,
    }
}

impl Stream {
    fn make(keep: Vec<Keep>, begin: impl FnOnce(*mut ffi::SturdyAsyncObject) -> i32) -> Result<Stream> {
        Ok(Stream { object: one_object(begin)?, keep })
    }

    pub(crate) fn from_object(object: Object) -> Stream {
        Stream { object, keep: Vec::new() }
    }

    /// Builds the stream a combinator returns: the input's closures move along with it.
    fn chain<C: Send + 'static>(mut self, closure: Option<Box<C>>, begin: impl FnOnce(ffi::SturdyAsyncObject, *mut c_void, *mut ffi::SturdyAsyncObject) -> i32) -> Result<Stream> {
        let user_data = closure.as_ref().map_or(core::ptr::null_mut(), |c| &**c as *const C as *mut c_void);
        let mut raw = ffi::SturdyAsyncObject::default();
        check(begin(self.object.raw(), user_data, &mut raw))?;
        if let Some(closure) = closure {
            self.keep.push(closure);
        }
        Ok(Stream { object: Object::from_raw(raw), keep: std::mem::take(&mut self.keep) })
    }

    // ---- sources ----

    pub fn empty() -> Result<Stream> {
        Stream::make(Vec::new(), |out| unsafe { ffi::sturdy_stream_empty(out) })
    }

    pub fn of_items(items: &[&[u8]]) -> Result<Stream> {
        let views: Vec<ffi::SturdyBytesView> = items.iter().map(|i| ffi::SturdyBytesView { data: i.as_ptr(), size: i.len() as u64 }).collect();
        Stream::make(Vec::new(), |out| unsafe { ffi::sturdy_stream_of_items(views.as_ptr(), views.len() as u32, out) })
    }

    /// Items are 8-byte little-endian steady-clock nanoseconds, one per period.
    pub fn interval(period: Duration) -> Result<Stream> {
        Stream::make(Vec::new(), |out| unsafe { ffi::sturdy_stream_interval(millis(Some(period)), out) })
    }

    /// Calls `source` for each pull on a blocking-lane thread (it may block); `None` ends the stream.
    pub fn generate<F: FnMut() -> Option<Vec<u8>> + Send + 'static>(source: F) -> Result<Stream> {
        let closure = Box::new(Mutex::new(source));
        let user_data = &*closure as *const Mutex<F> as *mut c_void;
        Stream::make(vec![closure as Keep], |out| unsafe { ffi::sturdy_stream_generate(Some(source_trampoline::<F>), user_data, out) })
    }

    // ---- adapters ----

    pub fn map<F: Fn(&[u8]) -> Vec<u8> + Send + Sync + 'static>(self, f: F) -> Result<Stream> {
        self.chain(Some(Box::new(f)), |s, u, out| unsafe { ffi::sturdy_stream_map(s, Some(map_trampoline::<F>), u, out) })
    }

    pub fn filter<F: Fn(&[u8]) -> bool + Send + Sync + 'static>(self, f: F) -> Result<Stream> {
        self.chain(Some(Box::new(f)), |s, u, out| unsafe { ffi::sturdy_stream_filter(s, Some(predicate_trampoline::<F>), u, out) })
    }

    pub fn filter_map<F: Fn(&[u8]) -> Option<Vec<u8>> + Send + Sync + 'static>(self, f: F) -> Result<Stream> {
        self.chain(Some(Box::new(f)), |s, u, out| unsafe { ffi::sturdy_stream_filter_map(s, Some(filter_map_trampoline::<F>), u, out) })
    }

    pub fn inspect<F: Fn(&[u8]) + Send + Sync + 'static>(self, f: F) -> Result<Stream> {
        self.chain(Some(Box::new(f)), |s, u, out| unsafe { ffi::sturdy_stream_inspect(s, Some(each_trampoline::<F>), u, out) })
    }

    pub fn take(self, count: u64) -> Result<Stream> {
        self.chain::<()>(None, |s, _, out| unsafe { ffi::sturdy_stream_take(s, count, out) })
    }

    pub fn skip(self, count: u64) -> Result<Stream> {
        self.chain::<()>(None, |s, _, out| unsafe { ffi::sturdy_stream_skip(s, count, out) })
    }

    pub fn take_while<F: Fn(&[u8]) -> bool + Send + Sync + 'static>(self, f: F) -> Result<Stream> {
        self.chain(Some(Box::new(f)), |s, u, out| unsafe { ffi::sturdy_stream_take_while(s, Some(predicate_trampoline::<F>), u, out) })
    }

    /// Items become frames {index as 8-byte little-endian, item} (see [`unpack_frames`]).
    pub fn enumerate(self) -> Result<Stream> {
        self.chain::<()>(None, |s, _, out| unsafe { ffi::sturdy_stream_enumerate(s, out) })
    }

    /// Items become packed frames of up to `count` items.
    pub fn chunks(self, count: u64) -> Result<Stream> {
        self.chain::<()>(None, |s, _, out| unsafe { ffi::sturdy_stream_chunks(s, count, out) })
    }

    /// Like [`Stream::chunks`], but also emits a partial batch once `max_wait` passed since its first item.
    pub fn chunks_timeout(self, count: u64, max_wait: Duration) -> Result<Stream> {
        self.chain::<()>(None, |s, _, out| unsafe { ffi::sturdy_stream_chunks_timeout(s, count, millis(Some(max_wait)), out) })
    }

    pub fn throttle(self, period: Duration) -> Result<Stream> {
        self.chain::<()>(None, |s, _, out| unsafe { ffi::sturdy_stream_throttle(s, millis(Some(period)), out) })
    }

    /// An item that takes longer than `limit` fails the next pull with `TimedOut`.
    pub fn timeout_each(self, limit: Duration) -> Result<Stream> {
        self.chain::<()>(None, |s, _, out| unsafe { ffi::sturdy_stream_timeout_each(s, millis(Some(limit)), out) })
    }

    /// Runs `f` on the worker pool for up to `concurrency` items at once (0: twice the worker count); `ordered` keeps the input order.
    pub fn map_concurrent<F: Fn(&[u8]) -> Vec<u8> + Send + Sync + 'static>(self, f: F, concurrency: u64, ordered: bool) -> Result<Stream> {
        self.chain(Some(Box::new(f)), |s, u, out| unsafe { ffi::sturdy_stream_map_concurrent(s, Some(map_trampoline::<F>), u, concurrency, flag(ordered), out) })
    }

    fn combine(self, other: Stream, begin: impl FnOnce(ffi::SturdyAsyncObject, ffi::SturdyAsyncObject, *mut ffi::SturdyAsyncObject) -> i32) -> Result<Stream> {
        let mut keep = self.keep;
        keep.extend(other.keep);
        let mut raw = ffi::SturdyAsyncObject::default();
        check(begin(self.object.raw(), other.object.raw(), &mut raw))?;
        Ok(Stream { object: Object::from_raw(raw), keep })
    }

    pub fn chain_with(self, other: Stream) -> Result<Stream> {
        self.combine(other, |a, b, out| unsafe { ffi::sturdy_stream_chain(a, b, out) })
    }

    pub fn merge(self, other: Stream) -> Result<Stream> {
        self.combine(other, |a, b, out| unsafe { ffi::sturdy_stream_merge(a, b, out) })
    }

    /// Items become frames {a, b}.
    pub fn zip(self, other: Stream) -> Result<Stream> {
        self.combine(other, |a, b, out| unsafe { ffi::sturdy_stream_zip(a, b, out) })
    }

    // ---- sinks ----

    /// The next item, or `None` at the end. Does not consume the stream; cannot be cancelled (close the source to stop it).
    pub async fn next(&self) -> Result<Option<Vec<u8>>> {
        let reply = run(|out| unsafe { ffi::sturdy_stream_next(self.object.raw(), out) }).await?;
        Ok((reply.number() == 1).then(|| reply.bytes()))
    }

    async fn drain(self, begin: impl FnOnce(ffi::SturdyAsyncObject, *mut ffi::SturdyAsyncOp) -> i32) -> Result<crate::Reply> {
        let mut guard = KeepGuard { keep: Some(self.keep), finished: false };
        let raw = self.object.raw();
        let result = run(|out| begin(raw, out)).await;
        guard.finished = true;
        result
    }

    /// Drains the stream into a list of items.
    pub async fn collect(self) -> Result<Vec<Vec<u8>>> {
        let reply = self.drain(|s, out| unsafe { ffi::sturdy_stream_collect(s, out) }).await?;
        unpack_frames(&reply.bytes())
    }

    pub async fn for_each<F: Fn(&[u8]) + Send + Sync + 'static>(mut self, f: F) -> Result<()> {
        let closure = Box::new(f);
        let user_data = &*closure as *const F as *mut c_void;
        self.keep.push(closure);
        self.drain(|s, out| unsafe { ffi::sturdy_stream_for_each(s, Some(each_trampoline::<F>), user_data, out) }).await.map(|_| ())
    }

    /// Folds the items into an accumulator.
    pub async fn fold<F: Fn(&[u8], &[u8]) -> Vec<u8> + Send + Sync + 'static>(mut self, initial: &[u8], f: F) -> Result<Vec<u8>> {
        let closure = Box::new(f);
        let user_data = &*closure as *const F as *mut c_void;
        self.keep.push(closure);
        let (ptr, len) = bytes_ptr(initial);
        Ok(self.drain(|s, out| unsafe { ffi::sturdy_stream_fold(s, ptr, len, Some(fold_trampoline::<F>), user_data, out) }).await?.bytes())
    }

    pub async fn count(self) -> Result<u64> {
        Ok(self.drain(|s, out| unsafe { ffi::sturdy_stream_count(s, out) }).await?.number() as u64)
    }

    pub async fn last(self) -> Result<Option<Vec<u8>>> {
        let reply = self.drain(|s, out| unsafe { ffi::sturdy_stream_last(s, out) }).await?;
        Ok((reply.number() == 1).then(|| reply.bytes()))
    }

    /// Sends every item into a channel (does not close it).
    pub async fn forward(self, sender: &Sender) -> Result<()> {
        let target = sender.raw();
        self.drain(|s, out| unsafe { ffi::sturdy_stream_forward(s, target, out) }).await.map(|_| ())
    }
}

#[allow(dead_code)]
fn _assert_send_sync() {
    fn is<T: Send + Sync>() {}
    is::<Sender>();
    is::<Receiver>();
    is::<Semaphore>();
    is::<AsyncMutex>();
    is::<Notify>();
    let _ = c_char::default();
}
