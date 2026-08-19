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
    // xyz = camera world-space position (for the occlusion test's "nearest point of the sphere"
    // computation below); w = 1.0 if hiZPyramid holds a previous frame's built pyramid, 0.0 on the
    // first frame / right after a resize, when occlusion culling is skipped entirely — see
    // Renderer::ensure_hiz_pyramid's doc comment (RendererHiZ.cpp).
    float4 cameraPositionHiZValid;
    uint firstObjectIndex;
    uint instanceCount;
    // Byte offset of this batch's DrawIndexedIndirectCommand within the shared indirect buffer,
    // and the first slot of this batch's own region of the shared compacted-indices buffer —
    // both buffers are shared across every batch this frame (one allocation, N regions) rather
    // than one small buffer per batch.
    uint indirectCommandByteOffset;
    uint compactedIndicesFirstSlot;
    uint2 hiZExtent;
    uint hiZMipCount;
};

[[push_constant]] ConstantBuffer<InstanceCullConstants> cull;

StructuredBuffer<SceneObjectGpuData> sceneObjects;
RWStructuredBuffer<uint> compactedIndices;
RWByteAddressBuffer indirectCommands;
// Built by Shaders/hiz_build.slang from *last* frame's finished depth (this frame's depth doesn't
// exist yet at the point this compute pass runs — see Renderer::record_instance_cull's doc
// comment) — a max-reduced mip pyramid over device depth (near = 0 / far = 1, Less compare), so a
// texel's stored value is the farthest already-covered depth anywhere under that texel's footprint.
Texture2D<float> hiZPyramid;

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

    if (cull.cameraPositionHiZValid.w > 0.5) {
        float3 towardCamera = cull.cameraPositionHiZValid.xyz - worldCenter;
        float distanceToCamera = length(towardCamera);
        float3 viewDir = distanceToCamera > 0.0001 ? (towardCamera / distanceToCamera) : float3(0.0, 0.0, 1.0);
        // Nearest point of the sphere toward the camera — its device depth is the value the Hi-Z
        // texel must be at least as far (numerically greater, this depth convention) as, or the
        // whole sphere is guaranteed hidden behind whatever was rasterized there last frame.
        float3 nearestPoint = worldCenter + viewDir * min(worldRadius, distanceToCamera);
        float4 nearestClip = mul(float4(nearestPoint, 1.0), cull.viewProjection);
        float4 centerClip = mul(float4(worldCenter, 1.0), cull.viewProjection);
        if (nearestClip.w > 0.0001 && centerClip.w > 0.0001) {
            // A perpendicular-offset point's clip-space distance from the center gives the sphere's
            // screen-space footprint without needing the projection matrix's internals (works for
            // any projection, not just a standard symmetric perspective one).
            float3 upAxis = abs(viewDir.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
            float3 rightAxis = normalize(cross(viewDir, upAxis));
            float4 edgeClip = mul(float4(worldCenter + rightAxis * worldRadius, 1.0), cull.viewProjection);

            float nearestDeviceDepth = nearestClip.z / nearestClip.w;
            float2 centerNdc = centerClip.xy / centerClip.w;
            float radiusNdc = length(edgeClip.xy / max(edgeClip.w, 0.0001) - centerNdc);

            float2 footprintTexels = radiusNdc * float2(cull.hiZExtent);
            float footprintTexelsMax = max(footprintTexels.x, footprintTexels.y) * 2.0;
            uint mip = uint(clamp(ceil(log2(max(footprintTexelsMax, 1.0))), 0.0, float(cull.hiZMipCount - 1)));
            uint2 mipExtent = max(cull.hiZExtent >> mip, uint2(1, 1));

            float2 uv = clamp(ndcToUv(centerNdc), float2(0.0, 0.0), float2(0.9999, 0.9999));
            int2 texel = int2(uv * float2(mipExtent));
            float farthestOccluderDepth = hiZPyramid.Load(int3(texel, int(mip)));

            if (nearestDeviceDepth > farthestOccluderDepth) {
                return;
            }
        }
    }

    // Field offset 4 = DrawIndexedIndirectCommand::instanceCount (indexCount is field 0, 4 bytes).
    uint compactedSlot;
    indirectCommands.InterlockedAdd(cull.indirectCommandByteOffset + 4, 1, compactedSlot);
    compactedIndices[cull.compactedIndicesFirstSlot + compactedSlot] = objectIndex;
}
