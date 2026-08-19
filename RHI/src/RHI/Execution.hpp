#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <type_traits>
#pragma endregion

#include <RHI/Flags.hpp>
#include <RHI/Handles.hpp>
#include <RHI/Queues.hpp>
#include <RHI/Barrier.hpp>
#include <RHI/Swapchain.hpp>

using std::span;

namespace SFT::RHI {








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
                                                                                                        
                                                                                                          
        PipelineStage stages = PipelineStage::AllCommands;
    };

    struct QueueSemaphoreSignal {
        SemaphoreHandle semaphore{};
        u64 value = 0;
                                                                                                        
                                                                             
        PipelineStage stages = PipelineStage::AllCommands;
    };

    enum class SubmitFlags : u32 {
        None = 0,
                                                                                                   
                                                                                    
        OneShot = 1u << 0,
    };

    struct SubmitDesc {
        QueueLane queue{};
        span<const CommandBufferHandle> command_buffers;
        span<const QueueSemaphoreWait> waits;
        span<const QueueSemaphoreSignal> signals;
                                                                                                      
                                                                                                       
                                                                                                                     
        span<const SurfaceTexture> presented_textures;
        FenceHandle fence{};
        SubmitFlags flags = SubmitFlags::None;
        const char *label = nullptr;
    };

    template <>
    struct enable_flag_ops<SubmitFlags> : std::true_type {};

} // namespace SFT::RHI
