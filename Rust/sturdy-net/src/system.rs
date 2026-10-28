//! Files, file watching, child processes, and the data-parallel half (Rayon's role).

use crate::ffi;
use crate::{bytes_ptr, check, flag, millis, run, run_object, Error, ErrorKind, Lower, Object, Result};
use core::ffi::c_void;
use std::time::Duration;

// ------------------------------------------------------------------------------------------------------------------------------
// Files
// ------------------------------------------------------------------------------------------------------------------------------

/// How a file is opened. Combine the constants with `|`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OpenMode(pub u32);

impl OpenMode {
    pub const READ: OpenMode = OpenMode(1);
    pub const WRITE: OpenMode = OpenMode(2);
    /// Create the file if missing (needs write or append).
    pub const CREATE: OpenMode = OpenMode(4);
    pub const TRUNCATE: OpenMode = OpenMode(8);
    /// Every write goes to the end.
    pub const APPEND: OpenMode = OpenMode(16);
    /// Create, failing if it exists (atomic).
    pub const CREATE_NEW: OpenMode = OpenMode(32);
}

impl core::ops::BitOr for OpenMode {
    type Output = OpenMode;
    fn bitor(self, other: OpenMode) -> OpenMode {
        OpenMode(self.0 | other.0)
    }
}

/// Where a [`File::seek`] offset counts from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SeekFrom {
    Start,
    Current,
    End,
}

/// A file for streaming and random access. Positional reads and writes use io_uring on Linux when available: nothing occupies a worker
/// thread while the kernel works.
pub struct File {
    pub(crate) object: Object,
}

impl File {
    pub async fn open(path: &str, mode: OpenMode) -> Result<File> {
        let mut cx = Lower::default();
        let path = cx.string(path);
        let object = run_object(|out| unsafe { ffi::sturdy_file_open(path, mode.0, out) }).await?;
        Ok(File { object })
    }

    /// Reads up to `max_bytes` at the cursor (advancing it); empty at end of file.
    pub async fn read(&self, max_bytes: usize) -> Result<Vec<u8>> {
        Ok(run(|out| unsafe { ffi::sturdy_file_read(self.object.raw(), max_bytes as u64, out) }).await?.bytes())
    }

    /// Reads at `offset` without touching the cursor.
    pub async fn read_at(&self, offset: u64, max_bytes: usize) -> Result<Vec<u8>> {
        Ok(run(|out| unsafe { ffi::sturdy_file_read_at(self.object.raw(), offset, max_bytes as u64, out) }).await?.bytes())
    }

    /// Reads from the cursor to the end; fails if more than `max_bytes` remain.
    pub async fn read_to_end(&self, max_bytes: usize) -> Result<Vec<u8>> {
        Ok(run(|out| unsafe { ffi::sturdy_file_read_to_end(self.object.raw(), max_bytes as u64, out) }).await?.bytes())
    }

    /// Writes all of `data` at the cursor; returns the byte count.
    pub async fn write(&self, data: &[u8]) -> Result<usize> {
        let (ptr, len) = bytes_ptr(data);
        Ok(run(|out| unsafe { ffi::sturdy_file_write(self.object.raw(), ptr, len, out) }).await?.number() as usize)
    }

    pub async fn write_at(&self, offset: u64, data: &[u8]) -> Result<usize> {
        let (ptr, len) = bytes_ptr(data);
        Ok(run(|out| unsafe { ffi::sturdy_file_write_at(self.object.raw(), offset, ptr, len, out) }).await?.number() as usize)
    }

    /// Moves the cursor; returns the new position.
    pub async fn seek(&self, offset: i64, from: SeekFrom) -> Result<u64> {
        let origin = match from {
            SeekFrom::Start => 0,
            SeekFrom::Current => 1,
            SeekFrom::End => 2,
        };
        Ok(run(|out| unsafe { ffi::sturdy_file_seek(self.object.raw(), offset, origin, out) }).await?.number() as u64)
    }

    pub async fn size(&self) -> Result<u64> {
        Ok(run(|out| unsafe { ffi::sturdy_file_size(self.object.raw(), out) }).await?.number() as u64)
    }

    pub async fn set_len(&self, length: u64) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_file_set_len(self.object.raw(), length, out) }).await.map(|_| ())
    }

    /// Flushes to stable storage (`fdatasync` when `data_only`).
    pub async fn sync(&self, data_only: bool) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_file_sync(self.object.raw(), flag(data_only), out) }).await.map(|_| ())
    }

    pub fn close(&self) {
        unsafe { ffi::sturdy_file_close(self.object.raw()) };
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Watching
// ------------------------------------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FileEventKind {
    Created,
    Modified,
    Removed,
    Renamed,
    /// Events were lost: rescan what you care about.
    Overflow,
}

#[derive(Debug, Clone)]
pub struct FileEvent {
    pub kind: FileEventKind,
    pub path: String,
    pub old_path: String,
    pub is_directory: bool,
}

impl FileEvent {
    /// Decodes one item of [`FileWatcher::events`].
    pub fn from_frames(item: &[u8]) -> Result<FileEvent> {
        let frames = crate::unpack_frames(item)?;
        let text = |i: usize| frames.get(i).map(|f| String::from_utf8_lossy(f).into_owned()).unwrap_or_default();
        Ok(FileEvent {
            kind: match text(0).as_str() {
                "0" => FileEventKind::Created,
                "1" => FileEventKind::Modified,
                "2" => FileEventKind::Removed,
                "3" => FileEventKind::Renamed,
                _ => FileEventKind::Overflow,
            },
            path: text(1),
            old_path: text(2),
            is_directory: text(3) == "1",
        })
    }
}

/// Watches a path for changes (inotify on Linux, polling elsewhere).
pub struct FileWatcher {
    object: Object,
}

/// Settings for [`FileWatcher::watch_with`]. `Default` gives the engine's defaults.
#[derive(Debug, Clone, Default)]
pub struct WatchOptions {
    pub recursive: bool,
    /// Bursts of writes within this window become one event; `None`: 50 ms.
    pub debounce: Option<Duration>,
    /// Use the portable polling backend even where the OS offers events.
    pub force_polling: bool,
    /// The polling backend's scan period; `None`: 250 ms.
    pub poll_interval: Option<Duration>,
    /// Events buffered before an overflow event is reported (0: 4096).
    pub max_pending_events: u64,
}

impl FileWatcher {
    pub fn watch(path: &str, recursive: bool, debounce: Duration, force_polling: bool) -> Result<FileWatcher> {
        Self::watch_with(path, &WatchOptions { recursive, debounce: Some(debounce), force_polling, ..WatchOptions::default() })
    }

    /// Watches `path` with every option.
    pub fn watch_with(path: &str, options: &WatchOptions) -> Result<FileWatcher> {
        let mut cx = Lower::default();
        let path = cx.string(path);
        let lowered = ffi::SturdyWatchOptions {
            recursive: flag(options.recursive),
            debounce_ms: millis(options.debounce),
            force_polling: flag(options.force_polling),
            poll_interval_ms: millis(options.poll_interval),
            max_pending_events: options.max_pending_events,
        };
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_watch_path(path, &lowered, &mut raw) })?;
        Ok(FileWatcher { object: Object::from_raw(raw) })
    }

    /// Whether the OS's event API is used (false: polling).
    pub fn uses_native_backend(&self) -> bool {
        let mut native = 0u8;
        unsafe { ffi::sturdy_watch_uses_native_backend(self.object.raw(), &mut native) };
        native != 0
    }

    /// The remaining events as a [`crate::Stream`] (do not call [`FileWatcher::next`] meanwhile). Each item decodes with [`FileEvent::from_frames`].
    pub fn events(&self) -> Result<crate::Stream> {
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_watch_events_stream(self.object.raw(), &mut raw) })?;
        Ok(crate::Stream::from_object(Object::from_raw(raw)))
    }

    /// The next change.
    pub async fn next(&self) -> Result<FileEvent> {
        let reply = run(|out| unsafe { ffi::sturdy_watch_next(self.object.raw(), out) }).await?;
        Ok(FileEvent {
            kind: match reply.number() {
                0 => FileEventKind::Created,
                1 => FileEventKind::Modified,
                2 => FileEventKind::Removed,
                3 => FileEventKind::Renamed,
                _ => FileEventKind::Overflow,
            },
            path: reply.text(),
            old_path: reply.field("old_path").unwrap_or_default(),
            is_directory: reply.field("is_directory").as_deref() == Some("1"),
        })
    }

    pub fn close(&self) {
        unsafe { ffi::sturdy_watch_close(self.object.raw()) };
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Processes
// ------------------------------------------------------------------------------------------------------------------------------

/// A program to run.
#[derive(Debug, Clone, Default)]
pub struct Command {
    /// Executable name (PATH lookup when it has no directory) or path.
    pub program: String,
    pub args: Vec<String>,
    pub working_directory: Option<String>,
    /// Added to (or, with `clear_environment`, replacing) the environment.
    pub environment: Vec<(String, String)>,
    pub clear_environment: bool,
    /// Terminate the child when its handle is dropped while it still runs.
    pub kill_on_drop: bool,
    /// What the child's standard streams connect to (default: pipes). A stream that is not a pipe cannot be read or written.
    pub stdin: Stdio,
    pub stdout: Stdio,
    pub stderr: Stdio,
}

/// What a child's standard stream is connected to.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Stdio {
    #[default]
    Pipe,
    /// The null device (reads end at once, writes vanish).
    Null,
    /// Shared with this process.
    Inherit,
}

impl Stdio {
    fn raw(self) -> u32 {
        match self {
            Stdio::Pipe => ffi::STURDY_STDIO_PIPE as u32,
            Stdio::Null => ffi::STURDY_STDIO_NULL as u32,
            Stdio::Inherit => ffi::STURDY_STDIO_INHERIT as u32,
        }
    }
}

impl Command {
    pub fn new(program: impl Into<String>) -> Self {
        Self { program: program.into(), ..Self::default() }
    }

    pub fn arg(mut self, arg: impl Into<String>) -> Self {
        self.args.push(arg.into());
        self
    }

    pub fn args<I: IntoIterator<Item = S>, S: Into<String>>(mut self, args: I) -> Self {
        self.args.extend(args.into_iter().map(Into::into));
        self
    }

    pub fn env(mut self, key: impl Into<String>, value: impl Into<String>) -> Self {
        self.environment.push((key.into(), value.into()));
        self
    }

    fn lower(&self, cx: &mut Lower) -> ffi::SturdyProcessOptions {
        let (args, arg_count) = cx.list(&self.args);
        let (environment, environment_count) = cx.pairs(&self.environment);
        ffi::SturdyProcessOptions {
            program: cx.string(&self.program),
            args,
            arg_count,
            working_directory: cx.optional(&self.working_directory),
            environment,
            environment_count,
            clear_environment: flag(self.clear_environment),
            kill_on_release: flag(self.kill_on_drop),
            stdin_mode: self.stdin.raw(),
            stdout_mode: self.stdout.raw(),
            stderr_mode: self.stderr.raw(),
        }
    }

    /// Runs to completion, feeding `stdin` and collecting stdout and stderr. The child is killed past `max_output_bytes` (0: 16 MiB) or
    /// `timeout` (`None`: no limit).
    pub async fn run(&self, stdin: &[u8], timeout: Option<Duration>, max_output_bytes: u64) -> Result<Output> {
        let mut cx = Lower::default();
        let lowered = self.lower(&mut cx);
        let (stdin_ptr, stdin_len) = bytes_ptr(stdin);
        let reply = run(|out| unsafe { ffi::sturdy_process_run(&lowered, stdin_ptr, stdin_len, millis(timeout), max_output_bytes, out) }).await?;
        Ok(Output {
            stdout: reply.bytes(),
            stderr: reply.text(),
            code: reply.number() as i32,
            signaled: reply.field("signaled").as_deref() == Some("1"),
            success: reply.field("success").as_deref() == Some("1"),
        })
    }

    /// Starts the program with piped stdin/stdout/stderr.
    pub fn spawn(&self) -> Result<Child> {
        let mut cx = Lower::default();
        let lowered = self.lower(&mut cx);
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_process_spawn(&lowered, &mut raw) })?;
        Ok(Child { object: Object::from_raw(raw) })
    }
}

/// How a child ended.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ExitStatus {
    /// Exit code (the signal number if `signaled`).
    pub code: i32,
    pub signaled: bool,
    pub success: bool,
    pub signal: i32,
}

/// What a finished program left behind.
#[derive(Debug, Clone)]
pub struct Output {
    pub stdout: Vec<u8>,
    pub stderr: String,
    /// Exit code (the signal number if `signaled`).
    pub code: i32,
    pub signaled: bool,
    pub success: bool,
}

/// A running child process.
pub struct Child {
    pub(crate) object: Object,
}

impl Child {
    pub async fn write_stdin(&self, data: &[u8]) -> Result<()> {
        let (ptr, len) = bytes_ptr(data);
        run(|out| unsafe { ffi::sturdy_process_write_stdin(self.object.raw(), ptr, len, out) }).await.map(|_| ())
    }

    pub fn close_stdin(&self) {
        unsafe { ffi::sturdy_process_close_stdin(self.object.raw()) };
    }

    /// Up to `max_bytes` of the child's stdout; empty at the end.
    pub async fn read_stdout(&self, max_bytes: usize) -> Result<Vec<u8>> {
        Ok(run(|out| unsafe { ffi::sturdy_process_read_stdout(self.object.raw(), max_bytes as u64, out) }).await?.bytes())
    }

    pub async fn read_stderr(&self, max_bytes: usize) -> Result<Vec<u8>> {
        Ok(run(|out| unsafe { ffi::sturdy_process_read_stderr(self.object.raw(), max_bytes as u64, out) }).await?.bytes())
    }

    /// Waits for exit; returns the exit code.
    pub async fn wait(&self) -> Result<i32> {
        Ok(run(|out| unsafe { ffi::sturdy_process_wait(self.object.raw(), out) }).await?.number() as i32)
    }

    /// Waits for exit; returns the full status.
    pub async fn wait_status(&self) -> Result<ExitStatus> {
        let reply = run(|out| unsafe { ffi::sturdy_process_wait(self.object.raw(), out) }).await?;
        Ok(ExitStatus {
            code: reply.number() as i32,
            signaled: reply.field("signaled").as_deref() == Some("1"),
            success: reply.field("success").as_deref() == Some("1"),
            signal: reply.field("signal").and_then(|s| s.parse().ok()).unwrap_or(0),
        })
    }

    /// The operating-system process id.
    pub fn id(&self) -> i64 {
        let mut id = 0i64;
        unsafe { ffi::sturdy_process_id(self.object.raw(), &mut id) };
        id
    }

    /// Asks the child to exit (SIGTERM / a close request); [`Child::kill`] forces it.
    pub fn request_exit(&self) {
        unsafe { ffi::sturdy_process_request_exit(self.object.raw()) };
    }

    pub fn kill(&self) {
        unsafe { ffi::sturdy_process_kill(self.object.raw()) };
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Data parallelism (Rayon's role)
// ------------------------------------------------------------------------------------------------------------------------------

/// Parallel loops, reductions and sorts on the engine's worker pool. The calling thread helps, so calling these from inside another
/// parallel body (or from a pool worker) never deadlocks.
pub mod par {
    use super::*;

    unsafe extern "C" fn for_trampoline<F: Fn(u64) + Sync>(user_data: *mut c_void, index: u64) {
        let body = unsafe { &*(user_data as *const F) };
        // A panic must not unwind through the C++ scheduler; it is swallowed here (the loop continues), so report it from inside `body`.
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| body(index)));
    }

    unsafe extern "C" fn chunk_trampoline<F: Fn(u64, u64) + Sync>(user_data: *mut c_void, begin: u64, end: u64) {
        let body = unsafe { &*(user_data as *const F) };
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| body(begin, end)));
    }

    /// Runs `body(i)` for every `i` in `0..count` across the worker pool and returns when all are done. `min_chunk` is the smallest run
    /// of indices handed to one task (0: let the engine decide).
    pub fn for_each<F: Fn(u64) + Sync>(count: u64, min_chunk: u64, body: F) -> Result<()> {
        check(unsafe { ffi::sturdy_par_for(count, min_chunk, Some(for_trampoline::<F>), &body as *const F as *mut c_void) })
    }

    /// Runs `body(begin, end)` over consecutive ranges of at most `chunk` indices.
    pub fn for_each_chunk<F: Fn(u64, u64) + Sync>(count: u64, chunk: u64, body: F) -> Result<()> {
        check(unsafe { ffi::sturdy_par_for_chunks(count, chunk, Some(chunk_trampoline::<F>), &body as *const F as *mut c_void) })
    }

    /// Element types the parallel reductions and sorts support.
    pub trait Number: Copy + 'static {
        const TYPE: i32;
        fn sum(data: &[Self]) -> Result<Self>
        where
            Self: Sized,
        {
            let _ = data;
            Err(Error::new(ErrorKind::Unsupported, "no parallel sum for this type"))
        }
        fn min_max(data: &[Self]) -> Result<(Self, Self)>
        where
            Self: Sized,
        {
            let _ = data;
            Err(Error::new(ErrorKind::Unsupported, "no parallel min/max for this type"))
        }
    }

    macro_rules! number {
        ($t:ty, $kind:expr, sum: $sum:ident, min_max: $mm:ident) => {
            impl Number for $t {
                const TYPE: i32 = $kind;
                fn sum(data: &[$t]) -> Result<$t> {
                    let mut out: $t = Default::default();
                    check(unsafe { ffi::$sum(data.as_ptr(), data.len() as u64, &mut out) })?;
                    Ok(out)
                }
                fn min_max(data: &[$t]) -> Result<($t, $t)> {
                    let (mut lo, mut hi): ($t, $t) = (Default::default(), Default::default());
                    check(unsafe { ffi::$mm(data.as_ptr(), data.len() as u64, &mut lo, &mut hi) })?;
                    Ok((lo, hi))
                }
            }
        };
        ($t:ty, $kind:expr) => {
            impl Number for $t {
                const TYPE: i32 = $kind;
            }
        };
    }
    number!(f32, ffi::STURDY_NUMERIC_F32, sum: sturdy_par_sum_f32, min_max: sturdy_par_min_max_f32);
    number!(f64, ffi::STURDY_NUMERIC_F64, sum: sturdy_par_sum_f64, min_max: sturdy_par_min_max_f64);
    number!(i64, ffi::STURDY_NUMERIC_I64, sum: sturdy_par_sum_i64, min_max: sturdy_par_min_max_i64);
    number!(u64, ffi::STURDY_NUMERIC_U64);
    number!(i32, ffi::STURDY_NUMERIC_I32);
    number!(u32, ffi::STURDY_NUMERIC_U32);

    /// Sorts in place, ascending, in parallel (floats: NaN-free input expected).
    pub fn sort<T: Number>(data: &mut [T]) -> Result<()> {
        check(unsafe { ffi::sturdy_par_sort(T::TYPE, data.as_mut_ptr() as *mut c_void, data.len() as u64) })
    }

    /// Parallel sum. Float sums use SIMD and may differ from a sequential sum by rounding.
    pub fn sum<T: Number>(data: &[T]) -> Result<T> {
        T::sum(data)
    }

    /// Parallel `(min, max)`; an error for an empty slice.
    pub fn min_max<T: Number>(data: &[T]) -> Result<(T, T)> {
        T::min_max(data)
    }

    /// The threads a parallel call can use right now.
    pub fn num_threads() -> u32 {
        let mut threads = 0u32;
        unsafe { ffi::sturdy_par_num_threads(&mut threads) };
        threads
    }

    unsafe extern "C" fn once_trampoline<F: FnOnce()>(user_data: *mut c_void) {
        let slot = unsafe { &mut *(user_data as *mut Option<F>) };
        if let Some(work) = slot.take() {
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(work));
        }
    }

    /// Runs `a` and `b` in parallel and returns when both are done.
    pub fn join<A: FnOnce() + Send, B: FnOnce() + Send>(a: A, b: B) -> Result<()> {
        let (mut left, mut right) = (Some(a), Some(b));
        check(unsafe { ffi::sturdy_par_join(Some(once_trampoline::<A>), &mut left as *mut Option<A> as *mut c_void, Some(once_trampoline::<B>), &mut right as *mut Option<B> as *mut c_void) })
    }

    /// A scope for spawning tasks that all finish before [`scope`] returns.
    pub struct Scope {
        raw: ffi::SturdyAsyncObject,
    }

    unsafe extern "C" fn scope_trampoline<F: FnOnce(&Scope)>(user_data: *mut c_void, scope: ffi::SturdyAsyncObject) {
        let slot = unsafe { &mut *(user_data as *mut Option<F>) };
        if let Some(body) = slot.take() {
            let scope = Scope { raw: scope };
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| body(&scope)));
        }
    }

    unsafe extern "C" fn spawned_trampoline<F: FnOnce()>(user_data: *mut c_void) {
        let work = *unsafe { Box::from_raw(user_data as *mut F) };
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(work));
    }

    impl Scope {
        /// Starts `work` on the worker pool; it finishes before [`scope`] returns.
        pub fn spawn<F: FnOnce() + Send + 'static>(&self, work: F) -> Result<()> {
            let user_data = Box::into_raw(Box::new(work)) as *mut c_void;
            let code = unsafe { ffi::sturdy_par_scope_spawn(self.raw, Some(spawned_trampoline::<F>), user_data) };
            if code != ffi::STURDY_OK {
                drop(unsafe { Box::from_raw(user_data as *mut F) });
            }
            check(code)
        }
    }

    /// Runs `body` with a [`Scope`]; every task spawned on it finishes before this returns.
    pub fn scope<F: FnOnce(&Scope)>(body: F) -> Result<()> {
        let mut slot = Some(body);
        check(unsafe { ffi::sturdy_par_scope(Some(scope_trampoline::<F>), &mut slot as *mut Option<F> as *mut c_void) })
    }

    unsafe extern "C" fn predicate_trampoline<F: Fn(u64) -> bool + Sync>(user_data: *mut c_void, index: u64) -> u8 {
        let f = unsafe { &*(user_data as *const F) };
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(index))).unwrap_or(false) as u8
    }

    /// Calls `body(begin)` for every window start in `0..=count - window`.
    pub fn for_windows<F: Fn(u64) + Sync>(count: u64, window: u64, body: F) -> Result<()> {
        check(unsafe { ffi::sturdy_par_for_windows(count, window, Some(for_trampoline::<F>), &body as *const F as *mut c_void) })
    }

    /// Whether `pred` holds for any index in `0..count`.
    pub fn any<F: Fn(u64) -> bool + Sync>(count: u64, pred: F) -> Result<bool> {
        let mut out = 0u8;
        check(unsafe { ffi::sturdy_par_any(count, Some(predicate_trampoline::<F>), &pred as *const F as *mut c_void, &mut out) })?;
        Ok(out != 0)
    }

    pub fn all<F: Fn(u64) -> bool + Sync>(count: u64, pred: F) -> Result<bool> {
        let mut out = 0u8;
        check(unsafe { ffi::sturdy_par_all(count, Some(predicate_trampoline::<F>), &pred as *const F as *mut c_void, &mut out) })?;
        Ok(out != 0)
    }

    /// How many indices satisfy `pred`.
    pub fn count<F: Fn(u64) -> bool + Sync>(count: u64, pred: F) -> Result<u64> {
        let mut out = 0u64;
        check(unsafe { ffi::sturdy_par_count(count, Some(predicate_trampoline::<F>), &pred as *const F as *mut c_void, &mut out) })?;
        Ok(out)
    }

    /// The lowest index satisfying `pred`.
    pub fn find_first<F: Fn(u64) -> bool + Sync>(count: u64, pred: F) -> Result<Option<u64>> {
        let mut out = -1i64;
        check(unsafe { ffi::sturdy_par_find_first(count, Some(predicate_trampoline::<F>), &pred as *const F as *mut c_void, &mut out) })?;
        Ok((out >= 0).then_some(out as u64))
    }

    /// Any index satisfying `pred` (usually faster than [`find_first`]).
    pub fn find_any<F: Fn(u64) -> bool + Sync>(count: u64, pred: F) -> Result<Option<u64>> {
        let mut out = -1i64;
        check(unsafe { ffi::sturdy_par_find_any(count, Some(predicate_trampoline::<F>), &pred as *const F as *mut c_void, &mut out) })?;
        Ok((out >= 0).then_some(out as u64))
    }

    struct MapReduce<M, R> {
        map: M,
        reduce: R,
    }

    unsafe extern "C" fn map_i64_trampoline<M: Fn(u64) -> i64 + Sync, R: Fn(i64, i64) -> i64 + Sync>(user_data: *mut c_void, index: u64) -> i64 {
        let state = unsafe { &*(user_data as *const MapReduce<M, R>) };
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| (state.map)(index))).unwrap_or(0)
    }

    unsafe extern "C" fn reduce_i64_trampoline<M: Fn(u64) -> i64 + Sync, R: Fn(i64, i64) -> i64 + Sync>(user_data: *mut c_void, a: i64, b: i64) -> i64 {
        let state = unsafe { &*(user_data as *const MapReduce<M, R>) };
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| (state.reduce)(a, b))).unwrap_or(a)
    }

    unsafe extern "C" fn map_f64_trampoline<M: Fn(u64) -> f64 + Sync, R: Fn(f64, f64) -> f64 + Sync>(user_data: *mut c_void, index: u64) -> f64 {
        let state = unsafe { &*(user_data as *const MapReduce<M, R>) };
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| (state.map)(index))).unwrap_or(0.0)
    }

    unsafe extern "C" fn reduce_f64_trampoline<M: Fn(u64) -> f64 + Sync, R: Fn(f64, f64) -> f64 + Sync>(user_data: *mut c_void, a: f64, b: f64) -> f64 {
        let state = unsafe { &*(user_data as *const MapReduce<M, R>) };
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| (state.reduce)(a, b))).unwrap_or(a)
    }

    /// Maps every index to a value and combines them with an associative `reduce` that has `identity`.
    pub fn map_reduce_i64<M: Fn(u64) -> i64 + Sync, R: Fn(i64, i64) -> i64 + Sync>(count: u64, map: M, identity: i64, reduce: R) -> Result<i64> {
        let state = MapReduce { map, reduce };
        let mut out = 0i64;
        check(unsafe { ffi::sturdy_par_map_reduce_i64(count, Some(map_i64_trampoline::<M, R>), identity, Some(reduce_i64_trampoline::<M, R>), &state as *const _ as *mut c_void, &mut out) })?;
        Ok(out)
    }

    pub fn map_reduce_f64<M: Fn(u64) -> f64 + Sync, R: Fn(f64, f64) -> f64 + Sync>(count: u64, map: M, identity: f64, reduce: R) -> Result<f64> {
        let state = MapReduce { map, reduce };
        let mut out = 0f64;
        check(unsafe { ffi::sturdy_par_map_reduce_f64(count, Some(map_f64_trampoline::<M, R>), identity, Some(reduce_f64_trampoline::<M, R>), &state as *const _ as *mut c_void, &mut out) })?;
        Ok(out)
    }

    unsafe extern "C" fn less_trampoline<T, F: Fn(&T, &T) -> bool + Sync>(user_data: *mut c_void, a: *const c_void, b: *const c_void) -> u8 {
        let f = unsafe { &*(user_data as *const F) };
        let (a, b) = unsafe { (&*(a as *const T), &*(b as *const T)) };
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(a, b))).unwrap_or(false) as u8
    }

    /// Sorts `data` in parallel with `less`; `stable` keeps equal elements in order. Elements are moved bitwise, hence `Copy`.
    pub fn sort_by<T: Copy + Send, F: Fn(&T, &T) -> bool + Sync>(data: &mut [T], stable: bool, less: F) -> Result<()> {
        check(unsafe { ffi::sturdy_par_sort_by(data.as_mut_ptr() as *mut c_void, data.len() as u64, core::mem::size_of::<T>() as u64, flag(stable), Some(less_trampoline::<T, F>), &less as *const F as *mut c_void) })
    }

    /// How [`reduce`] combines the elements.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum Reduction {
        Sum,
        Product,
        Min,
        Max,
    }

    /// The result of [`reduce`]: floats in `f64`, signed integers in `i64`, unsigned in `u64` (32-bit sums are accumulated in 64 bits).
    #[derive(Debug, Clone, Copy, Default, PartialEq)]
    pub struct Reduced {
        pub f64: f64,
        pub i64: i64,
        pub u64: u64,
    }

    /// Reduces a typed array in parallel; `None` for an empty array with min/max.
    pub fn reduce<T: Number>(data: &[T], reduction: Reduction) -> Result<Option<Reduced>> {
        let raw = match reduction {
            Reduction::Sum => ffi::STURDY_REDUCE_SUM,
            Reduction::Product => ffi::STURDY_REDUCE_PRODUCT,
            Reduction::Min => ffi::STURDY_REDUCE_MIN,
            Reduction::Max => ffi::STURDY_REDUCE_MAX,
        };
        let mut value = ffi::SturdyNumericValue::default();
        let mut has = 0u8;
        check(unsafe { ffi::sturdy_par_reduce(T::TYPE, raw, data.as_ptr() as *const c_void, data.len() as u64, &mut value, &mut has) })?;
        Ok((has != 0).then_some(Reduced { f64: value.f64, i64: value.i64, u64: value.u64 }))
    }
}

unsafe extern "C" fn task_trampoline<F: FnOnce()>(user_data: *mut c_void) {
    // The closure is taken exactly once.
    let slot = unsafe { &mut *(user_data as *mut Option<F>) };
    if let Some(task) = slot.take() {
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(task));
    }
}

/// A separate worker pool (Rayon's `ThreadPoolBuilder`): work run through it stays on its own threads, so a long job cannot starve the
/// engine's scheduler. Dropping it shuts its threads down.
pub struct ThreadPool {
    object: Object,
}

impl ThreadPool {
    /// `threads` 0: the machine's core count.
    pub fn new(threads: u32, name: &str) -> Result<ThreadPool> {
        let mut cx = Lower::default();
        let name = cx.string(name);
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_pool_create(threads, name, &mut raw) })?;
        Ok(ThreadPool { object: Object::from_raw(raw) })
    }

    pub(crate) fn raw(&self) -> ffi::SturdyAsyncObject {
        self.object.raw()
    }

    pub fn threads(&self) -> u32 {
        let mut count = 0u32;
        unsafe { ffi::sturdy_pool_threads(self.object.raw(), &mut count) };
        count
    }

    /// `par::for_each` on this pool's own threads.
    pub fn for_each<F: Fn(u64) + Sync>(&self, count: u64, min_chunk: u64, body: F) -> Result<()> {
        check(unsafe { ffi::sturdy_pool_par_for(self.object.raw(), count, min_chunk, Some(par_trampoline::<F>), &body as *const F as *mut c_void) })
    }

    /// Runs `task` on the pool and waits for it (anything it parallelises stays on the pool).
    pub fn install<F: FnOnce() + Send>(&self, task: F) -> Result<()> {
        let mut slot = Some(task);
        check(unsafe { ffi::sturdy_pool_run(self.object.raw(), Some(task_trampoline::<F>), &mut slot as *mut Option<F> as *mut c_void) })
    }
}

unsafe extern "C" fn par_trampoline<F: Fn(u64) + Sync>(user_data: *mut c_void, index: u64) {
    let body = unsafe { &*(user_data as *const F) };
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| body(index)));
}

impl File {
    pub fn is_open(&self) -> bool {
        let mut open = 0u8;
        unsafe { ffi::sturdy_file_is_open(self.object.raw(), &mut open) };
        open != 0
    }
}
