module;
#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <type_traits>
#pragma endregion

export module Sturdy.RHI:RayTracing;

import :Flags;
import :Types;
import :Handles;
import :Shader;

using std::span;

export namespace SFT::RHI {

    // ─── Acceleration structures ─────────────────────────────────────────────────
    //
    // The descriptors below intentionally use GPU buffer handles + offsets rather than CPU pointers.
    // That matches Vulkan/D3D12/Metal ray tracing: builds are GPU work, scratch memory is caller-owned,
    // and explicit barriers make the result visible to trace/compaction/copy passes.

    enum class AccelerationStructureType : u32 {
        BottomLevel,
        TopLevel,
    };

    enum class AccelerationStructureBuildFlags : u32 {
        None = 0,
        AllowUpdate = 1u << 0,
        AllowCompaction = 1u << 1,
        PreferFastTrace = 1u << 2,
        PreferFastBuild = 1u << 3,
        MinimizeMemory = 1u << 4,
    };

    enum class AccelerationStructureGeometryFlags : u32 {
        None = 0,
        Opaque = 1u << 0,
        NoDuplicateAnyHitInvocation = 1u << 1,
    };

    enum class AccelerationStructureGeometryType : u32 {
        Triangles,
        Aabbs,
        Instances,
    };

    struct AccelerationStructureDesc {
        AccelerationStructureType type = AccelerationStructureType::BottomLevel;
        u64 size = 0;
        const char *label = nullptr;
    };

    struct AccelerationStructureTrianglesDesc {
        BufferHandle vertex_buffer{};
        u64 vertex_offset = 0;
        VertexFormat vertex_format = VertexFormat::Float32x3;
        u64 vertex_stride = 0;
        u32 max_vertex = 0;

        BufferHandle index_buffer{};
        u64 index_offset = 0;
        IndexFormat index_format = IndexFormat::Uint32;

        // Optional 3x4 affine transform matrix buffer, tightly matching Vulkan/D3D12 build inputs.
        BufferHandle transform_buffer{};
        u64 transform_offset = 0;
    };

    struct AccelerationStructureAabbsDesc {
        BufferHandle buffer{};
        u64 offset = 0;
        u64 stride = 0;
    };

    struct AccelerationStructureInstancesDesc {
        BufferHandle buffer{};
        u64 offset = 0;
        bool array_of_pointers = false;
    };

    struct AccelerationStructureGeometryDesc {
        AccelerationStructureGeometryType type = AccelerationStructureGeometryType::Triangles;
        AccelerationStructureGeometryFlags flags = AccelerationStructureGeometryFlags::Opaque;
        AccelerationStructureTrianglesDesc triangles{};
        AccelerationStructureAabbsDesc aabbs{};
        AccelerationStructureInstancesDesc instances{};
    };

    struct AccelerationStructureBuildRangeInfo {
        u32 primitive_count = 0;
        u32 primitive_offset = 0;
        u32 first_vertex = 0;
        u32 transform_offset = 0;
    };

    struct AccelerationStructureBuildSizes {
        u64 acceleration_structure_size = 0;
        u64 build_scratch_size = 0;
        u64 update_scratch_size = 0;
    };

    struct AccelerationStructureBuildDesc {
        AccelerationStructureType type = AccelerationStructureType::BottomLevel;
        AccelerationStructureBuildFlags flags = AccelerationStructureBuildFlags::PreferFastTrace;
        AccelerationStructureHandle dst{};
        AccelerationStructureHandle src{}; // set for update/refit builds, otherwise null
        BufferHandle scratch_buffer{};
        u64 scratch_offset = 0;
        span<const AccelerationStructureGeometryDesc> geometries;
        span<const AccelerationStructureBuildRangeInfo> ranges;
    };

    enum class AccelerationStructureCopyMode : u32 {
        Clone,
        Compact,
        Serialize,
        Deserialize,
    };

    struct AccelerationStructureCopyDesc {
        AccelerationStructureHandle src{};
        AccelerationStructureHandle dst{};
        AccelerationStructureCopyMode mode = AccelerationStructureCopyMode::Clone;
    };

    // ─── Ray tracing pipelines / shader binding tables ───────────────────────────

    enum class RayTracingShaderGroupType : u32 {
        General,              // raygen, miss, or callable
        TrianglesHitGroup,    // closest/any-hit for triangle geometry
        ProceduralHitGroup,   // closest/any-hit/intersection for AABB/procedural geometry
    };

    struct RayTracingShaderGroupDesc {
        RayTracingShaderGroupType type = RayTracingShaderGroupType::General;
        ShaderEntry general{};
        ShaderEntry closest_hit{};
        ShaderEntry any_hit{};
        ShaderEntry intersection{};
    };

    struct RayTracingPipelineDesc {
        PipelineLayoutHandle layout{};
        span<const RayTracingShaderGroupDesc> groups;
        u32 max_ray_recursion_depth = 1;
        const char *label = nullptr;
    };

    struct ShaderBindingTableRegion {
        BufferHandle buffer{};
        u64 offset = 0;
        u64 size = 0;
        u64 stride = 0;
    };

    struct TraceRaysDesc {
        ShaderBindingTableRegion raygen;
        ShaderBindingTableRegion miss;
        ShaderBindingTableRegion hit;
        ShaderBindingTableRegion callable;
        u32 width = 1;
        u32 height = 1;
        u32 depth = 1;
    };

    template <>
    struct enable_flag_ops<AccelerationStructureBuildFlags> : std::true_type {};
    template <>
    struct enable_flag_ops<AccelerationStructureGeometryFlags> : std::true_type {};

} // namespace SFT::RHI
