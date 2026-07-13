module;

#pragma region Imports
#include <type_traits>
#pragma endregion

export module Sturdy.RHI:Queues;

import Sturdy.Foundation;
import :Flags;

export namespace SFT::RHI {

    // ─── Queue topology ───────────────────────────────────────────────────────────
    //
    // Graphics APIs expose parallel execution through queues/command queues/command buffers, but their
    // exact vocabulary differs:
    //   - Vulkan: queue families with one or more queues and explicit family-ownership transfers.
    //   - D3D12: direct/compute/copy command queues plus fences.
    //   - Metal: command queues, parallel render encoders, fences/events.
    //   - WebGPU: a mostly-single-queue model with optional render bundles.
    //
    // The RHI names the portable scheduling intent instead of the native object: a queue *class* plus a
    // lane index. A backend maps that to a concrete queue/family when it can, or aliases classes/lanes
    // onto the same native queue when the platform cannot run them independently. If true overlap is a
    // hard requirement, request/require the matching feature (e.g. Feature::AsyncCompute) at device
    // creation; otherwise code remains portable and just gets less parallel on narrower APIs.

    enum class QueueClass : u32 {
        Graphics,    // draw/dispatch/copy-capable on most APIs; the guaranteed default
        Compute,     // compute-focused scheduling lane; may alias Graphics without AsyncCompute
        Transfer,    // copy/blit/resolve-focused lane; may alias Graphics without AsyncTransfer
        Sparse,      // sparse/tiled residency binding work
        VideoDecode, // future video queues, API-gated where available
        VideoEncode,
    };

    enum class QueueCapability : u32 {
        None = 0,
        Graphics = 1u << 0,
        Compute = 1u << 1,
        Transfer = 1u << 2,
        Present = 1u << 3,
        SparseBinding = 1u << 4,
        VideoDecode = 1u << 5,
        VideoEncode = 1u << 6,
    };

    template <>
    struct enable_flag_ops<QueueCapability> : std::true_type {};

    struct QueueLane {
        QueueClass queue = QueueClass::Graphics;
        u32 index = 0;
    };

    struct QueueInfo {
        QueueClass queue = QueueClass::Graphics;
        QueueCapability capabilities = QueueCapability::Graphics | QueueCapability::Compute |
                                       QueueCapability::Transfer;
        // Number of independently-submittable lanes the backend exposes for this class. `1` is the
        // portable baseline; values >1 let a scheduler fan out submissions without inventing native
        // queues itself. Lanes may still share a native queue unless the matching async feature is
        // enabled, so queue overlap should be treated as capability, not as a timing guarantee.
        u32 lane_count = 1;

        // Queues in the same physical group are known to alias the same underlying execution resource
        // (for example Vulkan classes mapped onto one queue family, Metal/WebGPU single-queue aliases,
        // or D3D12 queues implemented on the same engine). Different groups are allowed to overlap, but
        // only `likely_parallel_with_graphics` / Feature::Async* should be used as a scheduling promise.
        u32 physical_group = 0;
        bool likely_parallel_with_graphics = false;

        // true when this class maps to a native queue/family/engine not used by Graphics. Dedicated is
        // a topology fact; `likely_parallel_with_graphics` is the scheduler-facing performance hint.
        bool dedicated = false;
        const char *label = nullptr;
    };

    struct QueueRequest {
        QueueClass queue = QueueClass::Graphics;
        // `min_lanes` is a hard requirement. Leave it 0 for "use whatever the backend exposes".
        u32 min_lanes = 0;
        // `preferred_lanes` is a hint for APIs where queues are created at device creation.
        u32 preferred_lanes = 0;
        // If true, creating the device fails unless this queue class maps to a dedicated native queue.
        bool require_dedicated = false;
    };

    // Optional queue-family/ownership handoff attached to a resource barrier. Vulkan needs this for
    // resources moving between queue families; D3D12/Metal usually ignore it because resource ownership
    // is not represented the same way. Keeping it on the barrier preserves Vulkan's maximum control
    // without forcing higher layers to name queue-family indices.
    struct QueueOwnershipTransfer {
        QueueLane src{};
        QueueLane dst{};
        bool enabled = false;
    };

} // namespace SFT::RHI
