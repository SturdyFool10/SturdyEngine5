#pragma once

#include <Foundation/Foundation.hpp>

namespace SFT::Audio {

    /// Per-thread hardware settings that matter for audio: flushing denormals (IIR filters, reverb tails and fades decay
    /// into denormal floats that are 10-100x slower on many CPUs) and asking the OS for audio-grade scheduling.
    struct RealtimeOptions {
        /// Treat denormal floats as zero (MXCSR FTZ/DAZ on x86, FPCR.FZ on ARM64; a no-op on RISC-V, which has no slow path).
        bool flush_denormals = true;
        /// Ask for elevated scheduling: MMCSS "Pro Audio" on Windows, SCHED_FIFO on Linux when permitted, a time-constraint
        /// policy sized from the block period on macOS. Failure is silent: the thread just runs at normal priority.
        bool raise_priority = true;
        /// Expected period between wake-ups (the audio block length); used for the macOS time-constraint policy.
        f64 period_seconds = 0.005;
    };

    /// Applies `options` to the calling thread. Idempotent per thread, so a device callback can call it on every wake-up.
    void prepare_realtime_thread(const RealtimeOptions &options = {}) noexcept;

} // namespace SFT::Audio
