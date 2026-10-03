#include <Audio/Realtime.hpp>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <avrt.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#elif defined(__linux__) || defined(__FreeBSD__)
#include <pthread.h>
#include <sched.h>
#endif

namespace SFT::Audio {

    namespace {

        void flush_denormals_on_this_thread() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            // 0x8000 = flush-to-zero, 0x0040 = denormals-are-zero.
            _mm_setcsr(_mm_getcsr() | 0x8040u);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
            unsigned long long fpcr = 0;
            asm volatile("mrs %0, fpcr" : "=r"(fpcr));
            fpcr |= (1ull << 24); // FZ
            asm volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
        }

        void raise_priority_on_this_thread(f64 period_seconds) noexcept {
#if defined(_WIN32)
            DWORD task_index = 0;
            if (AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index) == nullptr) {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            }
#elif defined(__APPLE__)
            mach_timebase_info_data_t timebase{};
            mach_timebase_info(&timebase);
            const f64 ticks_per_second = 1.0e9 * static_cast<f64>(timebase.denom) / static_cast<f64>(timebase.numer);
            thread_time_constraint_policy_data_t policy{};
            policy.period = static_cast<u32>(period_seconds * ticks_per_second);
            policy.computation = static_cast<u32>(period_seconds * 0.5 * ticks_per_second);
            policy.constraint = static_cast<u32>(period_seconds * 0.9 * ticks_per_second);
            policy.preemptible = 1;
            thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                              reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#elif defined(__linux__) || defined(__FreeBSD__)
            (void)period_seconds;
            sched_param param{};
            param.sched_priority = 10; // well below the kernel's own real-time threads
            // Needs CAP_SYS_NICE or an rtprio rlimit; without it this fails and the thread stays at normal priority.
            pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#else
            (void)period_seconds;
#endif
        }

    } // namespace

    void prepare_realtime_thread(const RealtimeOptions &options) noexcept {
        thread_local bool flushed = false;
        thread_local bool raised = false;
        if (options.flush_denormals && !flushed) {
            flush_denormals_on_this_thread();
            flushed = true;
        }
        if (options.raise_priority && !raised) {
            raise_priority_on_this_thread(options.period_seconds);
            raised = true;
        }
    }

} // namespace SFT::Audio
