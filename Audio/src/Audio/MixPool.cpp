#include <Audio/MixPool.hpp>

#include <Audio/Realtime.hpp>

#include <algorithm>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define SFT_CPU_RELAX() _mm_pause()
#elif defined(__aarch64__)
#define SFT_CPU_RELAX() asm volatile("yield")
#else
#define SFT_CPU_RELAX() ((void)0)
#endif

namespace SFT::Audio {

    u32 MixPool::resolve_helpers(u32 requested) noexcept {
        if (requested != 0xFFFFFFFFu) {
            return std::min(requested, 32u);
        }
        // Mixing is memory-light and runs for a small part of each block: a quarter of the cores, at most three helpers, is
        // plenty and leaves the rest to the game, the renderer and the decoder threads.
        const u32 cores = std::max(1u, std::thread::hardware_concurrency());
        return std::min(3u, cores / 4);
    }

    MixPool::MixPool(u32 helpers) {
        threads_.reserve(helpers);
        for (u32 i = 0; i < helpers; ++i) {
            threads_.emplace_back([this, i] { helper_main(i + 1); });
        }
    }

    MixPool::~MixPool() {
        stop_.store(true, std::memory_order_release);
        epoch_.fetch_add(1, std::memory_order_acq_rel);
        epoch_.notify_all();
        for (std::thread &t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    void MixPool::helper_main(u32 participant) {
        RealtimeOptions options;
        options.raise_priority = true;
        prepare_realtime_thread(options);
        u32 seen = epoch_.load(std::memory_order_acquire);
        for (;;) {
            epoch_.wait(seen, std::memory_order_acquire);
            if (stop_.load(std::memory_order_acquire)) {
                return;
            }
            seen = epoch_.load(std::memory_order_acquire);
            function_(context_, participant);
            if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                remaining_.notify_one();
            }
        }
    }

    void MixPool::run(Function function, void *context) {
        if (threads_.empty()) {
            function(context, 0);
            return;
        }
        function_ = function;
        context_ = context;
        remaining_.store(static_cast<u32>(threads_.size()), std::memory_order_release);
        epoch_.fetch_add(1, std::memory_order_acq_rel);
        epoch_.notify_all();
        function(context, 0);
        // Helpers normally finish within microseconds of the caller; spin briefly, then sleep on the counter.
        for (int spin = 0; spin < 4000; ++spin) {
            if (remaining_.load(std::memory_order_acquire) == 0) {
                return;
            }
            SFT_CPU_RELAX();
        }
        for (u32 left = remaining_.load(std::memory_order_acquire); left != 0; left = remaining_.load(std::memory_order_acquire)) {
            remaining_.wait(left, std::memory_order_acquire);
        }
    }

} // namespace SFT::Audio
