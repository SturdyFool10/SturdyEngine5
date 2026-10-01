#include <Foundation/FramePacer.hpp>

#include <algorithm>
#include <cmath>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#elif defined(_M_ARM64)
#include <intrin.h>
#endif

namespace SFT::Foundation {

    namespace {

        /// Tells the CPU this is a spin-wait: lowers power, and on SMT cores yields execution resources to the
        /// sibling hardware thread. Unlike `std::this_thread::yield()` it never hands the OS scheduler a chance to
        /// deschedule the thread, so it costs no precision.
        inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            _mm_pause();
#elif defined(_M_ARM64)
            __yield();
#elif defined(__aarch64__) || defined(__arm__)
            __asm__ __volatile__("yield");
#endif
        }

        constexpr f64 min_spin_margin = 0.0005;
        // Large enough to cover a Windows machine where the high-resolution timer is unavailable (default timer
        // resolution ~15.6 ms); adaptivity keeps it far smaller wherever sleeps are precise.
        constexpr f64 max_spin_margin = 0.016;

    } // namespace

    FramePacer::~FramePacer() {
#if defined(_WIN32)
        if (waitable_timer_ != nullptr) {
            CloseHandle(static_cast<HANDLE>(waitable_timer_));
        }
#endif
    }

    FramePacer::FramePacer(FramePacer &&other) noexcept { *this = std::move(other); }

    FramePacer &FramePacer::operator=(FramePacer &&other) noexcept {
        if (this == &other) {
            return *this;
        }
#if defined(_WIN32)
        if (waitable_timer_ != nullptr) {
            CloseHandle(static_cast<HANDLE>(waitable_timer_));
        }
#endif
        next_deadline_ = other.next_deadline_;
        period_ = other.period_;
        grid_active_ = other.grid_active_;
        last_return_ = other.last_return_;
        has_last_return_ = other.has_last_return_;
        intervals_ = other.intervals_;
        interval_count_ = other.interval_count_;
        interval_head_ = other.interval_head_;
        late_wakes_ = other.late_wakes_;
        resyncs_ = other.resyncs_;
        spin_margin_seconds_ = other.spin_margin_seconds_;
        waitable_timer_ = std::exchange(other.waitable_timer_, nullptr);
        return *this;
    }

    void FramePacer::reset() noexcept {
        grid_active_ = false;
        has_last_return_ = false;
        interval_count_ = 0;
        interval_head_ = 0;
    }

    void FramePacer::record_interval(clock::time_point now) noexcept {
        if (has_last_return_) {
            intervals_[interval_head_] = std::chrono::duration<f64>(now - last_return_).count();
            interval_head_ = (interval_head_ + 1) % interval_window;
            interval_count_ = std::min(interval_count_ + 1, interval_window);
        }
        last_return_ = now;
        has_last_return_ = true;
    }

    void FramePacer::precise_sleep_until(clock::time_point deadline) noexcept {
        clock::time_point now = clock::now();
        const auto margin =
            std::chrono::duration_cast<clock::duration>(std::chrono::duration<f64>(spin_margin_seconds_));
        if (deadline - now > margin) {
            const clock::time_point sleep_target = deadline - margin;
#if defined(_WIN32)
            // Windows' ordinary sleeps are quantized to the system timer (~15.6 ms unless someone raised it). A
            // high-resolution waitable timer (Windows 10 1803+) wakes within ~0.5 ms without changing the global
            // timer resolution for the whole machine; older systems fall back to a plain waitable timer.
            if (waitable_timer_ == nullptr) {
                HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                      TIMER_ALL_ACCESS);
                if (timer == nullptr) {
                    timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
                }
                waitable_timer_ = timer;
            }
            bool slept = false;
            if (waitable_timer_ != nullptr) {
                const f64 seconds = std::chrono::duration<f64>(sleep_target - now).count();
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(seconds * 1.0e7); // negative = relative, 100 ns units
                if (due.QuadPart < 0 &&
                    SetWaitableTimerEx(static_cast<HANDLE>(waitable_timer_), &due, 0, nullptr, nullptr, nullptr, 0)) {
                    WaitForSingleObject(static_cast<HANDLE>(waitable_timer_), INFINITE);
                    slept = true;
                }
            }
            if (!slept) {
                std::this_thread::sleep_until(sleep_target);
            }
#else
            // libstdc++/libc++ implement this with clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) (or the
            // platform equivalent), so it is an absolute monotonic wake, not a relative one that can drift.
            std::this_thread::sleep_until(sleep_target);
#endif
            now = clock::now();
            const f64 overshoot = std::chrono::duration<f64>(now - sleep_target).count();
            if (overshoot > spin_margin_seconds_ * 0.8) {
                // Too close for comfort: grow straight to the observed need plus headroom.
                spin_margin_seconds_ = std::min(overshoot * 1.5, max_spin_margin);
            } else if (overshoot < spin_margin_seconds_ * 0.25) {
                spin_margin_seconds_ = std::max(spin_margin_seconds_ * 0.95, min_spin_margin);
            }
        }
        while (clock::now() < deadline) {
            cpu_relax();
        }
    }

    f64 FramePacer::wait_until(clock::time_point deadline) noexcept {
        const clock::time_point start = clock::now();
        if (deadline > start) {
            precise_sleep_until(deadline);
        }
        return std::chrono::duration<f64>(clock::now() - start).count();
    }

    FramePaceResult FramePacer::pace(f64 target_fps) noexcept {
        FramePaceResult result{};
        const clock::time_point start = clock::now();

        if (!(target_fps > 0.0) || !std::isfinite(target_fps)) {
            grid_active_ = false;
            result.wake_time = start;
            result.deadline = start;
            record_interval(start);
            return result;
        }

        const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<f64>(1.0 / target_fps));
        if (!grid_active_ || period != period_) {
            // First capped frame, or the target changed: anchor a fresh grid here. This frame starts now.
            period_ = period;
            next_deadline_ = start + period_;
            grid_active_ = true;
            result.wake_time = start;
            result.deadline = start;
            record_interval(start);
            return result;
        }

        const clock::time_point deadline = next_deadline_;
        result.deadline = deadline;
        if (start >= deadline) {
            result.deadline_already_passed = true;
            if (start - deadline > period_) {
                // More than a whole period behind: re-anchor instead of racing through catch-up frames.
                ++resyncs_;
                next_deadline_ = start + period_;
            } else {
                next_deadline_ = deadline + period_;
            }
            result.wake_time = start;
            record_interval(start);
            return result;
        }

        precise_sleep_until(deadline);
        const clock::time_point woke = clock::now();
        if (woke - deadline > period_ / 2) {
            ++late_wakes_;
        }
        next_deadline_ = deadline + period_;
        result.wake_time = woke;
        result.waited = std::chrono::duration<f64>(woke - start).count();
        record_interval(woke);
        return result;
    }

    FramePacerStats FramePacer::stats() const noexcept {
        FramePacerStats out{};
        out.late_wakes = late_wakes_;
        out.resyncs = resyncs_;
        out.spin_margin = spin_margin_seconds_;
        out.sample_count = interval_count_;
        if (interval_count_ == 0) {
            return out;
        }

        std::array<f64, interval_window> sorted{};
        f64 sum = 0.0;
        for (u32 i = 0; i < interval_count_; ++i) {
            sorted[i] = intervals_[i];
            sum += intervals_[i];
        }
        out.mean_interval = sum / static_cast<f64>(interval_count_);
        f64 variance = 0.0;
        for (u32 i = 0; i < interval_count_; ++i) {
            const f64 d = sorted[i] - out.mean_interval;
            variance += d * d;
        }
        out.jitter = std::sqrt(variance / static_cast<f64>(interval_count_));
        std::sort(sorted.begin(), sorted.begin() + interval_count_);
        out.min_interval = sorted[0];
        out.max_interval = sorted[interval_count_ - 1];
        const u32 p99_index =
            std::min(interval_count_ - 1, static_cast<u32>(std::ceil(0.99 * static_cast<f64>(interval_count_))) - 1u);
        out.p99_interval = sorted[p99_index];
        return out;
    }

} // namespace SFT::Foundation
