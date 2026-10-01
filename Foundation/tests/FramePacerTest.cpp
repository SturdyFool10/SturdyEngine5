/// CPU-only correctness/precision checks for `Foundation::FramePacer`. Real-time timing tests have to tolerate
/// scheduler noise on a loaded CI machine, so the bounds are generous where noise matters (a single interval) and
/// tight where the design makes a strong promise (the long-run rate of a fixed grid, which cannot drift).

#include <Foundation/FramePacer.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

namespace {

    using namespace SFT;
    using clock = std::chrono::steady_clock;

    int g_failures = 0;

    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    f64 seconds_since(clock::time_point start) { return std::chrono::duration<f64>(clock::now() - start).count(); }

} // namespace

int main() {
    // No cap never waits.
    {
        Foundation::FramePacer pacer;
        const auto start = clock::now();
        for (int i = 0; i < 10; ++i) {
            (void)pacer.pace(0.0);
            (void)pacer.pace(-5.0);
        }
        check(seconds_since(start) < 0.01, "a non-positive target never waits");
    }

    // The first capped pace anchors the grid and returns immediately; the next waits one period.
    {
        Foundation::FramePacer pacer;
        const auto first = pacer.pace(100.0);
        check(first.waited == 0.0, "the first capped pace anchors the grid without waiting");
        const auto start = clock::now();
        const auto second = pacer.pace(100.0);
        const f64 elapsed = seconds_since(start);
        check(elapsed > 0.008 && elapsed < 0.02, "the second pace waits about one 10 ms period");
        check(!second.deadline_already_passed, "the second pace had a deadline in the future");
    }

    // Fixed grid: the long-run rate is exact, not "slightly under" as with now+period scheduling. Deliberately
    // add a variable amount of fake work each frame; the grid must absorb it.
    {
        Foundation::FramePacer pacer;
        constexpr f64 target_fps = 250.0; // 4 ms
        constexpr int frames = 250;       // one second
        (void)pacer.pace(target_fps);
        const auto start = clock::now();
        for (int i = 0; i < frames; ++i) {
            const auto work_end = clock::now() + std::chrono::microseconds(200 + (i % 7) * 300);
            while (clock::now() < work_end) {
            }
            (void)pacer.pace(target_fps);
        }
        const f64 elapsed = seconds_since(start);
        const f64 expected = static_cast<f64>(frames) / target_fps;
        const Foundation::FramePacerStats stats = pacer.stats();
        std::cout << "grid: expected " << expected << " s, measured " << elapsed << " s; mean interval "
                  << stats.mean_interval * 1000.0 << " ms, jitter " << stats.jitter * 1.0e6 << " us, p99 "
                  << stats.p99_interval * 1000.0 << " ms, max " << stats.max_interval * 1000.0 << " ms, spin margin "
                  << stats.spin_margin * 1.0e6 << " us, late " << stats.late_wakes << ", resyncs "
                  << stats.resyncs << "\n";
        // A grid cannot drift: total time is frames * period plus at most one late wake at the very end.
        check(std::abs(elapsed - expected) < 0.004, "the grid's long-run rate matches the target (no drift)");
        check(std::abs(stats.mean_interval - 1.0 / target_fps) < 0.0002, "the mean interval equals the period");
        check(stats.jitter < 0.0015, "frame-to-frame jitter stays well under a millisecond-and-a-half");
    }

    // Another, slower clock (e.g. a blocking vsync present) already paces the loop: the pacer must never add a
    // wait on top, and should re-anchor rather than accumulate debt.
    {
        Foundation::FramePacer pacer;
        (void)pacer.pace(1000.0); // 1 ms target, but each "frame" takes 5 ms
        f64 total_waited = 0.0;
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto r = pacer.pace(1000.0);
            total_waited += r.waited;
            check(r.deadline_already_passed, "a loop slower than the target never waits");
        }
        check(total_waited == 0.0, "no wait is ever added under a slower external clock");
        check(pacer.stats().resyncs >= 19, "falling a whole period behind re-anchors the grid");
    }

    // A single slow frame (less than a period behind) is absorbed without a resync: the next deadline stays on
    // the original grid, so the average rate is preserved.
    {
        Foundation::FramePacer pacer;
        (void)pacer.pace(100.0);
        (void)pacer.pace(100.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(14)); // 4 ms over a 10 ms period
        const auto late = pacer.pace(100.0);
        check(late.deadline_already_passed, "the slow frame's deadline had passed");
        const auto start = clock::now();
        (void)pacer.pace(100.0);
        const f64 elapsed = seconds_since(start);
        check(elapsed < 0.009, "the frame after a slow one waits only for the remainder of its grid slot");
        check(pacer.stats().resyncs == 0, "a sub-period hitch does not re-anchor the grid");
    }

    // Changing the target re-anchors immediately rather than waiting out the old period.
    {
        Foundation::FramePacer pacer;
        (void)pacer.pace(10.0); // 100 ms grid
        const auto start = clock::now();
        const auto r = pacer.pace(1000.0);
        check(r.waited == 0.0 && seconds_since(start) < 0.01, "a target change starts a fresh grid");
    }

    // wait_until() is precise to well under a millisecond.
    {
        Foundation::FramePacer pacer;
        f64 worst = 0.0;
        for (int i = 0; i < 20; ++i) {
            const auto deadline = clock::now() + std::chrono::milliseconds(3);
            (void)pacer.wait_until(deadline);
            worst = std::max(worst, std::chrono::duration<f64>(clock::now() - deadline).count());
        }
        std::cout << "wait_until worst overshoot " << worst * 1.0e6 << " us\n";
        check(worst < 0.0005, "wait_until wakes within half a millisecond of its deadline");
    }

    // Moving a pacer keeps its state.
    {
        Foundation::FramePacer a;
        (void)a.pace(100.0);
        (void)a.pace(100.0);
        Foundation::FramePacer b = std::move(a);
        check(b.stats().sample_count == 1, "a moved pacer keeps its interval history");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "FramePacerTest passed\n";
    return 0;
}
