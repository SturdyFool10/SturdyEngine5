module;

#pragma region Imports
#include <span>
#include <type_traits>
#pragma endregion

export module Sturdy.RHI:Execution;

import Sturdy.Foundation;
import :Flags;
import :Handles;
import :Queues;
import :Barrier;
import :Swapchain;

using std::span;

export namespace SFT::RHI {

    // ─── Timeline synchronization / submission ───────────────────────────────────
    //
    // The base RHI synchronization primitive is timeline-style: a monotonically increasing u64 value
    // carried by a semaphore/fence-like native object (Vulkan timeline semaphore, D3D12 fence, Metal
    // shared event or fence-backed emulation). Binary acquire/present semaphores are backend-internal
    // presentation details; cross-queue and CPU/GPU dependencies should be expressed with these values.

    inline constexpr u64 wait_forever = ~0ull;

    struct SemaphoreDesc {
        u64 initial_value = 0;
        const char *label = nullptr;
    };

    struct FenceDesc {
        bool signaled = false;
        const char *label = nullptr;
    };

    struct QueueSemaphoreWait {
        SemaphoreHandle semaphore{};
        u64 value = 0;
        // Vulkan consumes a destination stage mask for waits. APIs without stage-specific waits ignore
        // this and wait at queue scope. `AllCommands` is always correct; tighter masks preserve overlap.
        PipelineStage stages = PipelineStage::AllCommands;
    };

    struct QueueSemaphoreSignal {
        SemaphoreHandle semaphore{};
        u64 value = 0;
        // Mostly documentation today, but maps to APIs/extensions that can associate signal scope with
        // a stage and lets validation/debug layers explain producer intent.
        PipelineStage stages = PipelineStage::AllCommands;
    };

    enum class SubmitFlags : u32 {
        None = 0,
        // Backend may optimize for one-shot command buffers and recycle them after the associated
        // fence/timeline value completes. Do not set for reusable command buffers.
        OneShot = 1u << 0,
    };

    struct SubmitDesc {
        QueueLane queue{};
        span<const CommandBufferHandle> command_buffers;
        span<const QueueSemaphoreWait> waits;
        span<const QueueSemaphoreSignal> signals;
        // Acquired swapchain textures this submission renders into and makes ready for presentation.
        // Backends map this portable producer intent to their native WSI synchronization (Vulkan uses
        // internal binary render-finished semaphores; timeline semaphores remain the public cross-queue primitive).
        span<const SurfaceTexture> presented_textures;
        FenceHandle fence{}; // optional host fence signaled when the submission completes
        SubmitFlags flags = SubmitFlags::None;
        const char *label = nullptr;
    };

    template <>
    struct enable_flag_ops<SubmitFlags> : std::true_type {};

} // namespace SFT::RHI
