#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <type_traits>
#pragma endregion

#include <RHI/Flags.hpp>

namespace SFT::RHI {











    enum class QueryType : u32 {
                                                                                                     
                                                                                                        
                                     
        Occlusion,
                                                                                                       
                                                                                              
        Timestamp,
                                                                                                          
                                                                                                    
                                                  
        PipelineStatistics,
    };

                                                                                                    
                                                                                     
                                                                                
    enum class PipelineStatistic : u32 {
        None = 0,
        InputAssemblyVertices = 1u << 0,
        InputAssemblyPrimitives = 1u << 1,
        VertexShaderInvocations = 1u << 2,
        GeometryShaderInvocations = 1u << 3,
        GeometryShaderPrimitives = 1u << 4,
        ClippingInvocations = 1u << 5,
        ClippingPrimitives = 1u << 6,
        FragmentShaderInvocations = 1u << 7,
        TessControlShaderPatches = 1u << 8,
        TessEvaluationShaderInvocations = 1u << 9,
        ComputeShaderInvocations = 1u << 10,
        TaskShaderInvocations = 1u << 11,
        MeshShaderInvocations = 1u << 12,
    };

                                                                                                       
                                                                                         
    enum class QueryResultFlags : u32 {
        None = 0,
        Result64Bit = 1u << 0,
        Wait = 1u << 1,
        WithAvailability = 1u << 2,
        Partial = 1u << 3,
    };

    struct QuerySetDesc {
        QueryType type = QueryType::Timestamp;
        u32 count = 0;
                                                                                                         
        PipelineStatistic statistics = PipelineStatistic::None;
        const char *label = nullptr;
    };

    template <>
    struct enable_flag_ops<PipelineStatistic> : std::true_type {};
    template <>
    struct enable_flag_ops<QueryResultFlags> : std::true_type {};

} // namespace SFT::RHI
