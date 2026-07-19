#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <type_traits>
#pragma endregion

#include "Flags.hpp"

namespace SFT::RHI {

    // ─── GPU queries ─────────────────────────────────────────────────────────────
    //
    // A query set is a pool of `count` slots the GPU writes results into: occlusion counts, pipeline
    // timestamps, or pipeline-statistics tuples. `QuerySetHandle` (see :Handles) names the pool; the
    // command encoder records into slots (:Command's reset_query_set/write_timestamp/begin_query/...)
    // and either resolves the results into a buffer on the GPU (resolve_query_set) or the caller reads
    // them back on the host (RhiDevice::get_query_set_results). Every real renderer needs at least the
    // timestamp path — it is how per-pass GPU time (the G-buffer, lighting, shadow, and post passes on a
    // frame graph) is measured — and occlusion queries drive predicated/occlusion culling.

    enum class QueryType : u32 {
        // Counts samples that pass the depth/stencil test between begin_occlusion_query/end (on the
        // render-pass encoder). `PreciseOcclusionQueries` graduates this from a boolean-visible result
        // to an exact sample count.
        Occlusion,
        // A single GPU timestamp written by write_timestamp() at a pipeline stage. Two timestamps and
        // the device's timestamp period (a DeviceLimits value) give an elapsed GPU duration.
        Timestamp,
        // A tuple of rasterization-pipeline counters gathered between begin/end (see PipelineStatistic).
        // Requires Feature::PipelineStatisticsQueries; which counters are gathered is fixed at set
        // creation via QuerySetDesc::statistics.
        PipelineStatistics,
    };

    // Which counters a PipelineStatistics query set gathers. A bitmask chosen at set creation; the
    // resolved result contains one u64 per set bit, in ascending bit order. Mirrors
    // VkQueryPipelineStatisticFlagBits / D3D12_QUERY_DATA_PIPELINE_STATISTICS.
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

    // How a query result read/resolve behaves. `Result64Bit` is the safe default (occlusion/timestamp
    // counters routinely exceed 32 bits); leave it set unless you have a reason not to.
    enum class QueryResultFlags : u32 {
        None = 0,
        Result64Bit = 1u << 0,     // 8-byte results rather than 4-byte
        Wait = 1u << 1,            // block until every requested result is available
        WithAvailability = 1u << 2,// append an availability/status integer after each result
        Partial = 1u << 3,         // allow returning a partial (not-yet-final) result without waiting
    };

    struct QuerySetDesc {
        QueryType type = QueryType::Timestamp;
        u32 count = 0;
        // Only meaningful when `type == PipelineStatistics`: the counters to gather. Ignored otherwise.
        PipelineStatistic statistics = PipelineStatistic::None;
        const char *label = nullptr;
    };

    template <>
    struct enable_flag_ops<PipelineStatistic> : std::true_type {};
    template <>
    struct enable_flag_ops<QueryResultFlags> : std::true_type {};

} // namespace SFT::RHI
