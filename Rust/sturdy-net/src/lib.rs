//! The engine's Tokio and Rayon, for Rust.
//!
//! Async TCP / UDP / TLS, an HTTP client and server (HTTP/1.1, HTTP/2, HTTP/3), WebSocket, QUIC (streams and datagrams), peer-to-peer
//! links, NAT traversal, files, child processes, file watching, and data-parallel loops — all running on the engine's own network
//! reactor and worker pool, with no Rust async runtime required. Everything that waits is an ordinary [`Future`]; [`block_on`] drives one
//! from synchronous code, and any executor (Tokio's, `futures`', your own) can drive them too.
//!
//! ```ignore
//! use sturdy_net::{block_on, TcpListener, TcpStream};
//!
//! block_on(async {
//!     let listener = TcpListener::listen("127.0.0.1", 0, None).await?;
//!     let port = listener.port()?;
//!     let (server, client) = (listener.accept(), TcpStream::connect("127.0.0.1", port, None, None));
//!     let (mut server, mut client) = (server.await?, client.await?);
//!     client.send(b"hello").await?;
//!     assert_eq!(server.receive_exact(5).await?, b"hello");
//!     Ok::<_, sturdy_net::Error>(())
//! })?;
//! ```
//!
//! This crate is a safe layer over the C ABI (`FFI/src/FFI/Sturdy.h`), the same one the `sturdy-plugin` crate uses. The C functions are
//! *not linked* here: an executable links the engine's library, a plugin leaves them for the loader. All the networking entry points work
//! without an engine session — no window, no GPU.
//!
//! Semantics worth knowing:
//! * Futures are lazy (nothing starts until first polled) and **cancel on drop**: dropping a pending future stops the operation.
//! * Completions are delivered on the engine's network/timer threads; your future's waker is called from there, so executors that are
//!   `Send` work as usual.
//! * Security defaults match the C++ API: TLS verifies chain and host unless you pin a certificate; there is no "skip verification".
//!   [`TlsClient::identify_peer_by_certificate_only`] and [`TlsServer::accept_any_client_certificate`] are the two loud peer-to-peer
//!   opt-ins, after which *you* must compare the peer's fingerprint.

#[path = "../../../FFI/bindings/rust/sturdy_ffi.rs"]
pub mod ffi;

mod fsio;
mod net;
mod prims;
mod quic;
mod serve;
mod sync;
mod system;

pub use fsio::*;
pub use net::*;
pub use prims::*;
pub use quic::*;
pub use serve::*;
pub use sync::*;
pub use system::*;

use core::ffi::{c_char, c_void};
use std::ffi::CString;
use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll, Wake, Waker};
use std::time::Duration;

/// What went wrong.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorKind {
    NotFound,
    PermissionDenied,
    AlreadyExists,
    InvalidArgument,
    ConnectionRefused,
    ConnectionReset,
    TimedOut,
    Unsupported,
    Unknown,
    /// The peer closed the stream before a fixed-size read was satisfied.
    UnexpectedEof,
    /// The local object was closed before or during the operation.
    Closed,
    Tls,
    /// A configured size or count limit was exceeded.
    LimitExceeded,
    /// The peer violated the protocol.
    Protocol,
    /// The operation was cancelled before it finished.
    Cancelled,
    /// The C ABI itself refused the call (bad argument, expired handle, ...): the raw `SturdyResult` code.
    Abi(i32),
}

/// A failed operation or call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    pub kind: ErrorKind,
    pub message: String,
}

impl Error {
    pub fn new(kind: ErrorKind, message: impl Into<String>) -> Self {
        Self { kind, message: message.into() }
    }
}

impl core::fmt::Display for Error {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{} ({:?})", self.message, self.kind)
    }
}

impl std::error::Error for Error {}

impl From<Error> for std::io::Error {
    fn from(error: Error) -> Self {
        use std::io::ErrorKind as Io;
        let kind = match error.kind {
            ErrorKind::NotFound => Io::NotFound,
            ErrorKind::PermissionDenied => Io::PermissionDenied,
            ErrorKind::AlreadyExists => Io::AlreadyExists,
            ErrorKind::InvalidArgument => Io::InvalidInput,
            ErrorKind::ConnectionRefused => Io::ConnectionRefused,
            ErrorKind::ConnectionReset => Io::ConnectionReset,
            ErrorKind::TimedOut => Io::TimedOut,
            ErrorKind::Unsupported => Io::Unsupported,
            ErrorKind::UnexpectedEof => Io::UnexpectedEof,
            ErrorKind::Closed => Io::BrokenPipe,
            ErrorKind::Cancelled => Io::Interrupted,
            _ => Io::Other,
        };
        std::io::Error::new(kind, error.message)
    }
}

pub type Result<T> = core::result::Result<T, Error>;

fn last_error_message() -> String {
    let pointer = unsafe { ffi::sturdy_last_error_message() };
    if pointer.is_null() {
        return String::new();
    }
    unsafe { core::ffi::CStr::from_ptr(pointer) }.to_string_lossy().into_owned()
}

/// Turns a `SturdyResult` into `Ok(())` or the engine's error text.
pub(crate) fn check(code: i32) -> Result<()> {
    if code == ffi::STURDY_OK {
        Ok(())
    } else {
        Err(Error::new(ErrorKind::Abi(code), last_error_message()))
    }
}

pub(crate) fn kind_of(code: i32) -> ErrorKind {
    match code {
        ffi::STURDY_IO_NOT_FOUND => ErrorKind::NotFound,
        ffi::STURDY_IO_PERMISSION_DENIED => ErrorKind::PermissionDenied,
        ffi::STURDY_IO_ALREADY_EXISTS => ErrorKind::AlreadyExists,
        ffi::STURDY_IO_INVALID_ARGUMENT => ErrorKind::InvalidArgument,
        ffi::STURDY_IO_CONNECTION_REFUSED => ErrorKind::ConnectionRefused,
        ffi::STURDY_IO_CONNECTION_RESET => ErrorKind::ConnectionReset,
        ffi::STURDY_IO_TIMED_OUT => ErrorKind::TimedOut,
        ffi::STURDY_IO_UNSUPPORTED => ErrorKind::Unsupported,
        ffi::STURDY_IO_UNEXPECTED_EOF => ErrorKind::UnexpectedEof,
        ffi::STURDY_IO_CLOSED => ErrorKind::Closed,
        ffi::STURDY_IO_TLS_FAILURE => ErrorKind::Tls,
        ffi::STURDY_IO_LIMIT_EXCEEDED => ErrorKind::LimitExceeded,
        ffi::STURDY_IO_PROTOCOL_ERROR => ErrorKind::Protocol,
        _ => ErrorKind::Unknown,
    }
}

/// Reads a string out of the C ABI's "query the size, then fill" convention.
fn read_string(fetch: impl Fn(*mut c_char, u64, *mut u64) -> i32) -> Result<String> {
    let mut length = 0u64;
    check(fetch(core::ptr::null_mut(), 0, &mut length))?;
    let mut buffer = vec![0u8; length as usize];
    check(fetch(buffer.as_mut_ptr() as *mut c_char, buffer.len() as u64, &mut length))?;
    if buffer.last() == Some(&0) {
        buffer.pop();
    }
    Ok(String::from_utf8_lossy(&buffer).into_owned())
}

// ------------------------------------------------------------------------------------------------------------------------------
// Operations
// ------------------------------------------------------------------------------------------------------------------------------

struct Shared {
    done: AtomicBool,
    waker: Mutex<Option<Waker>>,
}

unsafe extern "C" fn op_finished(user_data: *mut c_void, _op: ffi::SturdyAsyncOp) {
    // The engine calls this exactly once per registration, so the reference taken in `Op::start` is returned here.
    let shared = unsafe { Arc::from_raw(user_data as *const Shared) };
    shared.done.store(true, Ordering::Release);
    let waker = shared.waker.lock().ok().and_then(|mut slot| slot.take());
    if let Some(waker) = waker {
        waker.wake();
    }
}

/// A pending operation. `.await` it for a [`Reply`], or call [`Op::wait`] to block. Dropping a pending one cancels it.
pub struct Op {
    raw: ffi::SturdyAsyncOp,
    shared: Arc<Shared>,
    taken: bool,
}

impl Op {
    /// Starts an operation with `begin` (which calls the C function) and arms its completion callback.
    pub(crate) fn start(begin: impl FnOnce(*mut ffi::SturdyAsyncOp) -> i32) -> Result<Op> {
        let mut raw = ffi::SturdyAsyncOp::default();
        check(begin(&mut raw))?;
        let shared = Arc::new(Shared { done: AtomicBool::new(false), waker: Mutex::new(None) });
        let user_data = Arc::into_raw(shared.clone()) as *mut c_void;
        let armed = unsafe { ffi::sturdy_op_on_complete(raw, Some(op_finished), user_data) };
        if armed != ffi::STURDY_OK {
            drop(unsafe { Arc::from_raw(user_data as *const Shared) });
            let error = check(armed).unwrap_err();
            unsafe { ffi::sturdy_op_release(raw) };
            return Err(error);
        }
        Ok(Op { raw, shared, taken: false })
    }

    pub(crate) fn raw(&self) -> ffi::SturdyAsyncOp {
        self.raw
    }

    /// Cancelling `token` cancels this operation.
    pub fn link_cancel(&self, token: &CancelToken) -> Result<()> {
        check(unsafe { ffi::sturdy_op_link_cancel(self.raw, token.raw()) })
    }

    /// An operation that finishes as this one does, or fails with `TimedOut` after `timeout` — cancelling this one then. Cancelling the new
    /// operation cancels this one.
    pub fn with_timeout(mut self, timeout: Duration) -> Result<Op> {
        let wrapped = Op::start(|out| unsafe { ffi::sturdy_op_with_timeout(self.raw, millis(Some(timeout)), out) })?;
        // The wrapper keeps the original running: release our handle without the cancel-on-drop.
        unsafe { ffi::sturdy_op_release(self.raw) };
        self.taken = true;
        Ok(wrapped)
    }

    /// Whether the operation has finished (successfully or not).
    pub fn is_done(&self) -> bool {
        self.shared.done.load(Ordering::Acquire)
    }

    /// Asks the operation to stop; it then finishes as cancelled (or with its result, if it already had one).
    pub fn cancel(&self) {
        unsafe { ffi::sturdy_op_cancel(self.raw) };
    }

    /// Blocks until the operation finishes or `timeout` passes. True when it finished.
    pub fn wait_timeout(&self, timeout: Duration) -> bool {
        let mut done = 0u8;
        let millis = timeout.as_millis().min(i64::MAX as u128) as i64;
        unsafe { ffi::sturdy_op_wait(self.raw, millis, &mut done) };
        done != 0
    }

    /// Blocks until the operation finishes and returns its result. Do not call it from a completion callback.
    pub fn wait(mut self) -> Result<Reply> {
        let mut done = 0u8;
        check(unsafe { ffi::sturdy_op_wait(self.raw, -1, &mut done) })?;
        self.finish()
    }

    fn finish(&mut self) -> Result<Reply> {
        if self.taken {
            return Err(Error::new(ErrorKind::InvalidArgument, "the operation's result was already taken"));
        }
        let mut status = 0;
        check(unsafe { ffi::sturdy_op_status(self.raw, &mut status) })?;
        match status {
            ffi::STURDY_OP_OK => {
                self.taken = true;
                Ok(Reply { raw: self.raw })
            }
            ffi::STURDY_OP_FAILED => {
                let mut code = 0;
                unsafe { ffi::sturdy_op_error_code(self.raw, &mut code) };
                let message = read_string(|buffer, capacity, length| unsafe { ffi::sturdy_op_error_message(self.raw, buffer, capacity, length) }).unwrap_or_default();
                Err(Error::new(kind_of(code), message))
            }
            ffi::STURDY_OP_CANCELLED => Err(Error::new(ErrorKind::Cancelled, "the operation was cancelled")),
            _ => Err(Error::new(ErrorKind::Unknown, "the operation has not finished")),
        }
    }
}

impl Future for Op {
    type Output = Result<Reply>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        if this.shared.done.load(Ordering::Acquire) {
            return Poll::Ready(this.finish());
        }
        if let Ok(mut slot) = this.shared.waker.lock() {
            match slot.as_ref() {
                Some(existing) if existing.will_wake(cx.waker()) => {}
                _ => *slot = Some(cx.waker().clone()),
            }
        }
        // The completion may have landed between the first check and storing the waker.
        if this.shared.done.load(Ordering::Acquire) {
            return Poll::Ready(this.finish());
        }
        Poll::Pending
    }
}

impl Drop for Op {
    fn drop(&mut self) {
        if !self.taken {
            if !self.is_done() {
                self.cancel();
            }
            unsafe { ffi::sturdy_op_release(self.raw) };
        }
    }
}

/// The result of a finished operation: some of {bytes, text, a number, named fields, an object}, as each function documents.
pub struct Reply {
    raw: ffi::SturdyAsyncOp,
}

impl Reply {
    /// The number (byte count, status code, port, ...).
    pub fn number(&self) -> i64 {
        let mut value = 0i64;
        unsafe { ffi::sturdy_op_number(self.raw, &mut value) };
        value
    }

    /// The bytes.
    pub fn bytes(&self) -> Vec<u8> {
        let mut length = 0u64;
        unsafe { ffi::sturdy_op_bytes(self.raw, core::ptr::null_mut(), 0, &mut length) };
        let mut buffer = vec![0u8; length as usize];
        if !buffer.is_empty() {
            unsafe { ffi::sturdy_op_bytes(self.raw, buffer.as_mut_ptr(), buffer.len() as u64, &mut length) };
        }
        buffer
    }

    /// The text.
    pub fn text(&self) -> String {
        read_string(|buffer, capacity, length| unsafe { ffi::sturdy_op_text(self.raw, buffer, capacity, length) }).unwrap_or_default()
    }

    /// The first field called `name` (HTTP header lookups are case-insensitive).
    pub fn field(&self, name: &str) -> Option<String> {
        let name = CString::new(name.replace('\0', "")).ok()?;
        read_string(|buffer, capacity, length| unsafe { ffi::sturdy_op_field(self.raw, name.as_ptr(), buffer, capacity, length) }).ok()
    }

    /// Every field, in order.
    pub fn fields(&self) -> Vec<(String, String)> {
        let mut count = 0u32;
        unsafe { ffi::sturdy_op_field_count(self.raw, &mut count) };
        (0..count)
            .filter_map(|index| {
                let name = read_string(|b, c, l| unsafe { ffi::sturdy_op_field_name(self.raw, index, b, c, l) }).ok()?;
                let value = read_string(|b, c, l| unsafe { ffi::sturdy_op_field_value(self.raw, index, b, c, l) }).ok()?;
                Some((name, value))
            })
            .collect()
    }

    /// The object the operation produced (a connection, a file, ...), which you now own. Consumes the reply: the engine hands out one
    /// handle per operation, so asking twice would give two owners of the same object.
    pub fn into_object(self) -> Result<Object> {
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_op_object(self.raw, &mut raw) })?;
        Ok(Object { raw: Some(raw) })
    }
}

impl Reply {
    pub(crate) fn from_raw(raw: ffi::SturdyAsyncOp) -> Reply {
        Reply { raw }
    }
}

impl Drop for Reply {
    fn drop(&mut self) {
        unsafe { ffi::sturdy_op_release(self.raw) };
    }
}

/// A long-lived thing (connection, listener, server, file, ...). Dropping it closes it.
pub struct Object {
    raw: Option<ffi::SturdyAsyncObject>,
}

impl Object {
    pub(crate) fn raw(&self) -> ffi::SturdyAsyncObject {
        self.raw.unwrap_or_default()
    }

    pub(crate) fn from_raw(raw: ffi::SturdyAsyncObject) -> Object {
        Object { raw: Some(raw) }
    }

    /// Gives up ownership of the handle without releasing it.
    pub(crate) fn into_raw(mut self) -> ffi::SturdyAsyncObject {
        self.raw.take().unwrap_or_default()
    }

    /// The object's kind name ("tcp", "websocket", "quic_stream", ...).
    pub fn kind(&self) -> String {
        read_string(|b, c, l| unsafe { ffi::sturdy_object_kind(self.raw(), b, c, l) }).unwrap_or_default()
    }
}

impl Drop for Object {
    fn drop(&mut self) {
        if let Some(raw) = self.raw.take() {
            unsafe { ffi::sturdy_object_release(raw) };
        }
    }
}

/// Starts an operation on an object and awaits its [`Reply`].
pub(crate) async fn run(begin: impl FnOnce(*mut ffi::SturdyAsyncOp) -> i32) -> Result<Reply> {
    Op::start(begin)?.await
}

/// Starts an operation that produces an object and awaits it.
pub(crate) async fn run_object(begin: impl FnOnce(*mut ffi::SturdyAsyncOp) -> i32) -> Result<Object> {
    run(begin).await?.into_object()
}

/// A bit set of optional parts of this build, from [`features`].
pub mod feature {
    pub const QUIC: u32 = 1;
    pub const HTTP2: u32 = 2;
    pub const HTTP3: u32 = 4;
    pub const IO_URING: u32 = 8;
    pub const PROCESS: u32 = 16;
    pub const GZIP: u32 = 32;
    pub const NATIVE_FILE_WATCH: u32 = 64;
}

/// Which optional protocols this build contains / this machine supports (see [`feature`]).
pub fn features() -> u32 {
    let mut bits = 0u32;
    unsafe { ffi::sturdy_async_features(&mut bits) };
    bits
}

// ------------------------------------------------------------------------------------------------------------------------------
// block_on
// ------------------------------------------------------------------------------------------------------------------------------

struct ThreadWaker(std::thread::Thread);

impl Wake for ThreadWaker {
    fn wake(self: Arc<Self>) {
        self.0.unpark();
    }
    fn wake_by_ref(self: &Arc<Self>) {
        self.0.unpark();
    }
}

/// Runs a future to completion on the calling thread. A small executor for synchronous code and tests; the operations themselves run on
/// the engine's threads, so blocking here only parks the caller.
pub fn block_on<F: Future>(future: F) -> F::Output {
    let mut future = Box::pin(future);
    let waker = Waker::from(Arc::new(ThreadWaker(std::thread::current())));
    let mut cx = Context::from_waker(&waker);
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut cx) {
            return value;
        }
        std::thread::park();
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Lowering Rust values to the C ABI's pointer-and-count shapes
// ------------------------------------------------------------------------------------------------------------------------------

/// Owns the C strings and arrays a lowered options struct points into, so the pointers stay valid for the duration of the call.
#[derive(Default)]
pub(crate) struct Lower {
    strings: Vec<CString>,
    lists: Vec<Vec<*const c_char>>,
    pairs: Vec<Vec<ffi::SturdyKeyValue>>,
}

impl Lower {
    pub(crate) fn string(&mut self, value: &str) -> *const c_char {
        // An interior NUL cannot be represented in a C string; it is dropped rather than truncating the rest silently at the NUL.
        let text = CString::new(value.replace('\0', "")).unwrap_or_default();
        let pointer = text.as_ptr();
        self.strings.push(text);
        pointer
    }

    pub(crate) fn optional(&mut self, value: &Option<String>) -> *const c_char {
        match value {
            Some(text) => self.string(text),
            None => core::ptr::null(),
        }
    }

    pub(crate) fn list(&mut self, values: &[String]) -> (*const *const c_char, u32) {
        if values.is_empty() {
            return (core::ptr::null(), 0);
        }
        let pointers: Vec<*const c_char> = values.iter().map(|v| self.string(v)).collect();
        let result = (pointers.as_ptr(), pointers.len() as u32);
        self.lists.push(pointers);
        result
    }

    pub(crate) fn pairs(&mut self, values: &[(String, String)]) -> (*const ffi::SturdyKeyValue, u32) {
        if values.is_empty() {
            return (core::ptr::null(), 0);
        }
        let pairs: Vec<ffi::SturdyKeyValue> = values.iter().map(|(n, v)| ffi::SturdyKeyValue { name: self.string(n), value: self.string(v) }).collect();
        let result = (pairs.as_ptr(), pairs.len() as u32);
        self.pairs.push(pairs);
        result
    }
}

pub(crate) fn flag(value: bool) -> u8 {
    value as u8
}

pub(crate) fn bytes_ptr(data: &[u8]) -> (*const u8, u64) {
    (data.as_ptr(), data.len() as u64)
}

pub(crate) fn millis(duration: Option<Duration>) -> i64 {
    duration.map_or(0, |d| d.as_millis().min(i64::MAX as u128) as i64)
}
