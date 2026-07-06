// `Sturdy.Async` — concurrency primitives and task scheduling.
//
// `Mutex<T>` owns the data it protects (lock() hands back a transparent, auto-unlocking
// `MutexGuard<T>`); `Arc<T>` is a never-null, atomically-refcounted accessor to a shared `T` —
// combine the two as `Arc<Mutex<T>>` for shared, mutable state. `Scheduler` is a lazily-started
// work-stealing thread pool (`spawn()` returns a `TaskHandle` you can `wait()` on);
// `run_on_main_thread()`/`pump_main_thread()` cover the other half of task scheduling — work that
// must run on a specific (main) thread rather than any worker. `AsyncRuntime` (see :Runtime) is the
// abstraction over both: `ParallelRuntime` (native, the two pieces above) and `SynchronousRuntime`
// (Web, no threads — everything runs inline) present the same shape, so higher-level code written
// once against `DefaultRuntime` gets real parallelism natively and correct, serial behavior on Web.
// `AsyncWork` (also :Runtime) is the companion callable concept — a function safe to hand to that
// runtime, as opposed to `Sturdy.Foundation`'s plain, single-threaded `SyncWork`.
// `Ranges::for_each()`/`Ranges::transform()` (see :Ranges) run a `std::ranges` algorithm's work split
// across the runtime instead of on one thread; `ParIter`/`par_iter()` (see :ParIter) wrap that same
// chunk-and-spawn model as a chainable, Rayon-flavored parallel iterator — the parallel counterpart
// to `Sturdy.Foundation`'s sequential `Iter`/`iter()`. Depends only on `Sturdy.Foundation`, so any
// layer above it can use these freely.
export module Sturdy.Async;

export import :Mutex;
export import :Arc;
export import :Task;
export import :Scheduler;
export import :MainThread;
export import :Runtime;
export import :Ranges;
export import :ParIter;
export import :Affinity;
export import :IoError;
export import :File;
export import :Net;
