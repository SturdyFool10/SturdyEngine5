#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <type_traits>
#pragma endregion

#include "Flags.hpp"
#include "Handles.hpp"
#include "Queues.hpp"

namespace SFT::RHI {











                                                                                                      
                                                                                                    
                                                                                                    
    enum class PipelineStage : u64 {
        None = 0,
        DrawIndirect = 1ull << 0,
        VertexInput = 1ull << 1,
        VertexShader = 1ull << 2,
        TessControlShader = 1ull << 3,
        TessEvalShader = 1ull << 4,
        GeometryShader = 1ull << 5,
        FragmentShader = 1ull << 6,
        EarlyFragmentTests = 1ull << 7,
        LateFragmentTests = 1ull << 8,
        ColorAttachmentOutput = 1ull << 9,
        ComputeShader = 1ull << 10,
        Transfer = 1ull << 11,
        Host = 1ull << 12,
        TaskShader = 1ull << 13,
        MeshShader = 1ull << 14,
        RayTracingShader = 1ull << 15,
        AccelerationStructureBuild = 1ull << 16,

        AllGraphics = VertexInput | VertexShader | TessControlShader | TessEvalShader | GeometryShader |
                      FragmentShader | EarlyFragmentTests | LateFragmentTests | ColorAttachmentOutput |
                      TaskShader | MeshShader,
        AllCommands = ~0ull,
    };

                                                                                                      
                                                                                                    
                                                    
    enum class AccessFlags : u64 {
        None = 0,
        IndirectCommandRead = 1ull << 0,
        IndexRead = 1ull << 1,
        VertexAttributeRead = 1ull << 2,
        UniformRead = 1ull << 3,
        ShaderRead = 1ull << 4,
        ShaderWrite = 1ull << 5,
        ColorAttachmentRead = 1ull << 6,
        ColorAttachmentWrite = 1ull << 7,
        DepthStencilAttachmentRead = 1ull << 8,
        DepthStencilAttachmentWrite = 1ull << 9,
        TransferRead = 1ull << 10,
        TransferWrite = 1ull << 11,
        HostRead = 1ull << 12,
        HostWrite = 1ull << 13,
        AccelerationStructureRead = 1ull << 14,
        AccelerationStructureWrite = 1ull << 15,
        MemoryRead = 1ull << 16,
        MemoryWrite = 1ull << 17,
    };

                                                                                                  
                                                                                                        
                                                                                                   
                              
    enum class TextureLayout : u32 {
        Undefined,
        General,
        ColorAttachment,
        DepthStencilAttachment,
        DepthStencilReadOnly,
        ShaderReadOnly,
        TransferSrc,
        TransferDst,
        Present,
    };

                                                                                                      
                                              
    struct TextureSubresourceRange {
        u32 base_mip_level = 0;
        u32 mip_level_count = ~0u;
        u32 base_array_layer = 0;
        u32 array_layer_count = ~0u;
    };

                                                                                                     
                                                                                          
    struct GlobalBarrier {
        PipelineStage src_stage = PipelineStage::None;
        AccessFlags src_access = AccessFlags::None;
        PipelineStage dst_stage = PipelineStage::None;
        AccessFlags dst_access = AccessFlags::None;
    };

                                                                                           
                                                                                                      
                                                                                                     
                                                                                        
    struct BufferBarrier {
        BufferHandle buffer{};
        PipelineStage src_stage = PipelineStage::None;
        AccessFlags src_access = AccessFlags::None;
        PipelineStage dst_stage = PipelineStage::None;
        AccessFlags dst_access = AccessFlags::None;
        QueueOwnershipTransfer ownership{};
        u64 offset = 0;
        u64 size = 0;
    };

                                                                                                 
                                                                                                       
                                                   
    struct TextureBarrier {
        TextureHandle texture{};
        PipelineStage src_stage = PipelineStage::None;
        AccessFlags src_access = AccessFlags::None;
        PipelineStage dst_stage = PipelineStage::None;
        AccessFlags dst_access = AccessFlags::None;
        QueueOwnershipTransfer ownership{};
        TextureLayout old_layout = TextureLayout::Undefined;
        TextureLayout new_layout = TextureLayout::Undefined;
        TextureSubresourceRange range{};
    };

    template <>
    struct enable_flag_ops<PipelineStage> : std::true_type {};
    template <>
    struct enable_flag_ops<AccessFlags> : std::true_type {};

} // namespace SFT::RHI
