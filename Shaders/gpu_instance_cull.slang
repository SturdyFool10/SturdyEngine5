// GPU-driven instance culling for a single (mesh, material) batch of SceneObjectGpuData entries.
//
// Renderer::detect_instanced_batches groups contiguous runs of submission.draws that share both
// mesh and material (already true once submission.draws is sorted — see render_frame_dispatch)
// above a minimum size into one "batch": a [firstObjectIndex, firstObjectIndex + instanceCount)
// range into the frame's SceneObjectGpuData buffer (uploaded in the same order as submission.draws
// by prepare_scene_gpu_data, so the range is contiguous there too). One dispatch of this shader per
// batch frustum-culls every candidate instance and compacts the survivors into that batch's own
// slice of a shared indices buffer, while atomically growing that batch's indirect draw command's
// instanceCount field in place — see Renderer::record_instanced_batches (RendererGpuCulling.cpp)
// for the exact buffer layout and the manual barrier between this pass and the indirect draw that
// consumes its output (RenderGraph's compute-pass builder only tracks texture hazards, not buffer
// ones, so that barrier is hand-rolled there).

import sturdy_common;

struct SceneObjectGpuData {
    float4x4 model;
    float4x4 previousModel;
    float4 idSortVisibilityFlags;
};

// Mirrors Vulkan's VkDrawIndexedIndirectCommand field-for-field (indexCount, instanceCount,
// firstIndex, vertexOffset, firstInstance) — see Renderer::GpuDrawIndexedIndirectCommand
// (RendererModule.hpp). instanceCount starts this frame at 0 (CPU-written before this shader
// runs) and is atomically incremented below as each surviving instance gets compacted in.
struct DrawIndexedIndirectCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

struct InstanceCullConstants {
    float4x4 viewProjection;
    // xyz = mesh-local bounding-sphere center, w = radius — see MeshResource::bounds_center/
    // bounds_radius's doc comment (Resources.hpp). Same value for every instance in this batch;
    // only the per-instance model matrix (below) differs.
    float4 boundsCenterRadius;
    uint firstObjectIndex;
    uint instanceCount;
    // Byte offset of this batch's DrawIndexedIndirectCommand within the shared indirect buffer,
    // and the first slot of this batch's own region of the shared compacted-indices buffer —
    // both buffers are shared across every batch this frame (one allocation, N regions) rather
    // than one small buffer per batch.
    uint indirectCommandByteOffset;
    uint compactedIndicesFirstSlot;
};

[[push_constant]] ConstantBuffer<InstanceCullConstants> cull;

StructuredBuffer<SceneObjectGpuData> sceneObjects;
RWStructuredBuffer<uint> compactedIndices;
RWByteAddressBuffer indirectCommands;

[shader("compute")]
[numthreads(64, 1, 1)]
void cullMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint localIndex = dispatchThreadId.x;
    if (localIndex >= cull.instanceCount) {
        return;
    }
    uint objectIndex = cull.firstObjectIndex + localIndex;
    float4x4 model = sceneObjects[objectIndex].model;

    float3 scale = float3(
        length(float3(model[0][0], model[1][0], model[2][0])),
        length(float3(model[0][1], model[1][1], model[2][1])),
        length(float3(model[0][2], model[1][2], model[2][2])));
    float maxScale = max(scale.x, max(scale.y, scale.z));
    float3 worldCenter = mul(float4(cull.boundsCenterRadius.xyz, 1.0), model).xyz;
    float worldRadius = cull.boundsCenterRadius.w * maxScale;

    if (!frustumIntersectsSphere(cull.viewProjection, worldCenter, worldRadius)) {
        return;
    }

    // Field offset 4 = DrawIndexedIndirectCommand::instanceCount (indexCount is field 0, 4 bytes).
    uint compactedSlot;
    indirectCommands.InterlockedAdd(cull.indirectCommandByteOffset + 4, 1, compactedSlot);
    compactedIndices[cull.compactedIndicesFirstSlot + compactedSlot] = objectIndex;
}
