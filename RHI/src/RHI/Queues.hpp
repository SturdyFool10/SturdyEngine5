#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <type_traits>
#pragma endregion

#include "Flags.hpp"

namespace SFT::RHI {
















    enum class QueueClass : u32 {
        Graphics,
        Compute,
        Transfer,
        Sparse,
        VideoDecode,
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
                                                                                                    
                                                                                                     
                                                                                                    
                                                                                                 
        u32 lane_count = 1;

                                                                                                       
                                                                                                        
                                                                                                         
                                                                                                          
        u32 physical_group = 0;
        bool likely_parallel_with_graphics = false;

                                                                                                        
                                                                                                      
        bool dedicated = false;
        const char *label = nullptr;
    };

    struct QueueRequest {
        QueueClass queue = QueueClass::Graphics;
                                                                                                 
        u32 min_lanes = 0;
                                                                                             
        u32 preferred_lanes = 0;
                                                                                                        
        bool require_dedicated = false;
    };

                                                                                                     
                                                                                                         
                                                                                                     
                                                                   
    struct QueueOwnershipTransfer {
        QueueLane src{};
        QueueLane dst{};
        bool enabled = false;
    };

} // namespace SFT::RHI
