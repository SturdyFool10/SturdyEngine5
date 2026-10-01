#pragma once

#include <Foundation/Types.hpp>

#include <array>
#include <chrono>

namespace SFT::Foundation {

    /// Quality statistics over a `FramePacer`'s most recent intervals (the time between successive `pace()`
    /// returns). Everything is in seconds.
    struct FramePacerStats {
        u32 sample_count = 0;
        f64 mean_interval = 0.0;
        /// Standard deviation of the interval: the frame-to-frame jitter a viewer perceives as uneven motion.
        f64 jitter = 0.0;
        f64 min_interval = 0.0;
        f64 max_interval = 0.0;
        /// 99th-percentile interval ("1% low" expressed as a frame time).
        f64 p99_interval = 0.0;
        /// Paces (over the whole lifetime, not just the window) that woke more than half a period after their
        /// deadline — a late wake the display can see.
        u64 late_wakes = 0;
        /// Times the grid was re-anchored because the loop fell more than a whole period behind (a slow frame,
        /// or another clock — typically a blocking vsync present — pacing the loop more slowly than the target).
        u64 resyncs = 0;
        /// Current sleep/spin split: how much of each wait is left to the spin loop.
        f64 spin_margin = 0.0;
    };

    /// What one `FramePacer::pace()` call did.
    struct FramePaceResult {
        std::chrono::steady_clock::time_point wake_time{};
        /// The deadline this pace aimed for (equal to `wake_time` when no cap was active).
        std::chrono::steady_clock::time_point deadline{};
        /// Seconds actually spent waiting in this call.
        f64 waited = 0.0;
        /// True when the call did not wait because the deadline had already passed.
        bool deadline_already_passed = false;
    };

    /// A precise frame limiter that schedules on a fixed time grid.
    ///
    /// * **Monotonic clock.** `std::chrono::steady_clock`, never `high_resolution_clock` (which is
    ///   `system_clock` on libstdc++ and moves with NTP adjustments).
    /// * **Fixed grid.** Each deadline is the previous deadline plus one period, not "now plus one period", so a
    ///   late wake is absorbed by the next wait instead of pushing every later frame back (no drift; the long-run
    ///   rate is exactly the target). If the loop falls more than a whole period behind, the grid re-anchors at
    ///   the current time rather than rushing through a burst of catch-up frames.
    /// * **Sleep, then spin.** OS sleeps are quantized (Windows ~15.6 ms by default, Linux ~50-100 µs), so the
    ///   bulk of a wait is slept — on Windows with a high-resolution waitable timer — and a short, adaptively
    ///   sized remainder is spun with a CPU relax hint against the monotonic clock.
    /// * **Composes with other clocks.** If something else (a blocking vsync present) already makes the loop
    ///   slower than the target, the deadline has always passed and this never adds a wait.
    ///
    /// One pacer per loop; not thread-safe.
    class FramePacer {
      public:
        using clock = std::chrono::steady_clock;

        FramePacer() noexcept = default;
        ~FramePacer();
        FramePacer(const FramePacer &) = delete;
        FramePacer &operator=(const FramePacer &) = delete;
        FramePacer(FramePacer &&other) noexcept;
        FramePacer &operator=(FramePacer &&other) noexcept;

        /// Waits for the next grid deadline of `target_fps`, then returns.
        ///
        /// Call once per loop iteration at the point the next frame should start (for the lowest latency: right
        /// before sampling input). `target_fps <= 0` never waits and drops the grid, so re-enabling a cap starts a
        /// fresh one. Changing the target re-anchors the grid at the current time.
        ///
        /// @note This function does not throw exceptions.
        FramePaceResult pace(f64 target_fps) noexcept;

        /// Waits until an absolute deadline with the same precise sleep/spin as `pace()`, without touching the
        /// grid. Used by callers that compute their own deadline (e.g. a just-in-time frame start derived from
        /// display timing feedback).
        ///
        /// @return Seconds actually spent waiting.
        /// @note This function does not throw exceptions.
        f64 wait_until(clock::time_point deadline) noexcept;

        /// Statistics over the most recent `interval_window` intervals.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] FramePacerStats stats() const noexcept;

        /// Forgets the grid and the interval history (e.g. after a window was hidden for a while).
        ///
        /// @note This function does not throw exceptions.
        void reset() noexcept;

        static constexpr u32 interval_window = 240;

      private:
        void record_interval(clock::time_point now) noexcept;
        void precise_sleep_until(clock::time_point deadline) noexcept;

        clock::time_point next_deadline_{};
        clock::duration period_{};
        bool grid_active_ = false;

        clock::time_point last_return_{};
        bool has_last_return_ = false;
        std::array<f64, interval_window> intervals_{};
        u32 interval_count_ = 0;
        u32 interval_head_ = 0;
        u64 late_wakes_ = 0;
        u64 resyncs_ = 0;

        // How much of each wait is spun rather than slept. Grows immediately when a sleep overshoots its request,
        // decays slowly while sleeps land well inside it, so the spin cost tracks what this OS actually needs.
        f64 spin_margin_seconds_ = 0.001;

        // Windows high-resolution waitable timer (HANDLE), created lazily. Unused elsewhere.
        void *waitable_timer_ = nullptr;
    };

} // namespace SFT::Foundation
