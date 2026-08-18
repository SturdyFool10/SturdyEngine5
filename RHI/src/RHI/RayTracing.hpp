#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <array>
#include <span>
#include <type_traits>
#pragma endregion

#include "Flags.hpp"
#include "Types.hpp"
#include "Handles.hpp"
#include "Shader.hpp"

using std::span;

namespace SFT::RHI {


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


        BufferHandle transform_buffer{};
        u64 transform_offset = 0;

        // Requires Feature::OpacityMicromap. Links a prebuilt OpacityMicromapHandle (see
        // create_opacity_micromap()) onto this triangle geometry, letting hardware skip any-hit
        // shader invocation for alpha-tested regions the micromap classifies as fully opaque or fully
        // transparent -- the main real-time payoff being avoided any-hit dispatches on foliage/
        // vegetation/alpha-tested geometry, not a rasterization-time effect. Leave
        // opacity_micromap invalid (the default) for geometry that doesn't use one.
        OpacityMicromapHandle opacity_micromap{};
        // One entry per triangle in this geometry, indexing into opacity_micromap's packed usage
        // regions; required whenever opacity_micromap is valid.
        BufferHandle opacity_micromap_index_buffer{};
        u64 opacity_micromap_index_offset = 0;
        IndexFormat opacity_micromap_index_format = IndexFormat::Uint32;
    };

    // The number of opacity states a micromap's 11 (or fewer)-bit-per-microtriangle format encodes.
    // TwoState: Transparent/Opaque only. FourState additionally distinguishes "Unknown" (ambiguous,
    // still needs an any-hit shader) from "known" Transparent/Opaque -- the practically useful format
    // for most real geometry, since TwoState forces every ambiguous microtriangle to round to Opaque
    // (safe but loses the any-hit-skip optimization there).
    enum class OpacityMicromapFormat : u32 {
        TwoState,
        FourState,
    };

    // One entry in an opacity micromap's usage-counts table: how many microtriangle regions of a
    // given (format, subdivision_level) pair the micromap's packed data buffer contains. Mirrors
    // Vulkan's VkMicromapUsageEXT; every backend that implements Feature::OpacityMicromap needs this
    // same shape to size/build the micromap; it is not otherwise a hardware-agnostic-safe extension
    // point.
    struct OpacityMicromapUsageCount {
        u32 count = 0;
        u32 subdivision_level = 0;
        OpacityMicromapFormat format = OpacityMicromapFormat::TwoState;
    };

    struct OpacityMicromapDesc {
        span<const OpacityMicromapUsageCount> usage_counts;
        const char *label = nullptr;
    };

    struct OpacityMicromapBuildSizes {
        u64 micromap_size = 0;
        u64 build_scratch_size = 0;
    };

    struct OpacityMicromapBuildDesc {
        OpacityMicromapHandle dst{};
        BufferHandle scratch_buffer{};
        u64 scratch_offset = 0;
        // Packed 1 (TwoState) or 2 (FourState) bit-per-microtriangle opacity data, laid out per
        // Vulkan's VK_EXT_opacity_micromap encoding (the only backend that currently builds this --
        // see create_opacity_micromap()'s own doc comment on D3D12 support).
        BufferHandle data_buffer{};
        u64 data_buffer_offset = 0;
        span<const OpacityMicromapUsageCount> usage_counts;
    };

    struct AccelerationStructureAabbsDesc {
        BufferHandle buffer{};
        u64 offset = 0;
        u64 stride = 0;
    };


    struct AccelerationStructureInstance {
        std::array<f32, 12> transform{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
        };
        u32 custom_index_and_mask = 0xff000000u;
        u32 shader_binding_table_offset_and_flags = 0;
        u64 acceleration_structure_device_address = 0;

        /// Sets the custom index and mask for this `AccelerationStructureInstance`.
        ///
        /// @param custom_index Zero-based index of the target element or entry.
        /// @param mask `mask` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_custom_index_and_mask(u32 custom_index, u8 mask) noexcept;
        /// Sets the shader binding table offset and flags for this `AccelerationStructureInstance`.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @note This function does not throw exceptions.
        void set_shader_binding_table_offset_and_flags(u32 offset, u8 flags) noexcept;
    };
    static_assert(sizeof(AccelerationStructureInstance) == 64);

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
        AccelerationStructureHandle src{};
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


    enum class RayTracingShaderGroupType : u32 {
        General,
        TrianglesHitGroup,
        ProceduralHitGroup,
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
