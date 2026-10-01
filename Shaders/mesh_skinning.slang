// Linear-blend skinning of one mesh into its slice of the renderer's shared vertex arena.
//
// Bind-pose vertices and per-vertex joint influences live in buffers owned by the skin; the posed
// position/normal/tangent are written over the mesh's own GeometryVertex records in the arena
// (uv and colour are never touched). Vertex records are 64 bytes (Renderer/Geometry.hpp):
// position @0, normal @12, uv @24, colour @32, tangent @48.
//
// Matrices arrive in the glm memory layout the rest of the renderer uses, hence mul(vector, matrix).

struct SkinConstants {
    uint dstFirstVertex; // index of the mesh's first vertex in the arena
    uint vertexCount;
    uint jointBase;      // first matrix of this skin in jointMatrices
    uint morphWeightBase;  // first weight of this skin in morphWeights
    uint morphTargetCount; // 0 = no blend shapes (morphOffsets/morphEntries are dummies)
    uint firstPose;        // 1 = no earlier pose: previous positions become the new ones
    uint padding1;
    uint padding2;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<SkinConstants> skin;
#else
[[push_constant]] ConstantBuffer<SkinConstants> skin;
#endif

struct Influence {
    uint4 joints;
    float4 weights;
};

// Sparse blend shapes in CSR form (Animation/Morph.hpp): the deltas of vertex v are
// morphEntries[morphOffsets[v] .. morphOffsets[v + 1]).
struct MorphEntry {
    float3 positionDelta;
    uint target;
    float3 normalDelta;
    uint padding;
};

StructuredBuffer<Influence> influences;
StructuredBuffer<uint> morphOffsets;
StructuredBuffer<MorphEntry> morphEntries;
StructuredBuffer<float> morphWeights;
StructuredBuffer<float4x4> jointMatrices;
ByteAddressBuffer bindVertices;
RWByteAddressBuffer deformedVertices;
// Previous-frame position of every skinned vertex, parallel to the arena (indexed by absolute vertex index);
// consumed by gbuffer_geometry_history.slang for motion vectors.
RWStructuredBuffer<float4> previousPositions;

[shader("compute")]
[numthreads(64, 1, 1)]
void skinMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint vertex = dispatchThreadId.x;
    if (vertex >= skin.vertexCount) {
        return;
    }
    uint srcByte = vertex * 64u;
    float3 position = asfloat(bindVertices.Load3(srcByte));
    float3 normal = asfloat(bindVertices.Load3(srcByte + 12u));
    float4 tangent = asfloat(bindVertices.Load4(srcByte + 48u));

    if (skin.morphTargetCount != 0u) {
        uint first = morphOffsets[vertex];
        uint last = morphOffsets[vertex + 1u];
        for (uint i = first; i < last; ++i) {
            MorphEntry entry = morphEntries[i];
            float w = morphWeights[skin.morphWeightBase + entry.target];
            position += w * entry.positionDelta;
            normal += w * entry.normalDelta;
        }
        normal = normalize(normal);
    }

    Influence influence = influences[vertex];
    float3 skinnedPosition = float3(0.0, 0.0, 0.0);
    float3 skinnedNormal = float3(0.0, 0.0, 0.0);
    float3 skinnedTangent = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        float w = influence.weights[i];
        if (w > 0.0) {
            float4x4 m = jointMatrices[skin.jointBase + influence.joints[i]];
            skinnedPosition += w * mul(float4(position, 1.0), m).xyz;
            skinnedNormal += w * mul(float4(normal, 0.0), m).xyz;
            skinnedTangent += w * mul(float4(tangent.xyz, 0.0), m).xyz;
            total += w;
        }
    }
    if (total <= 0.0) {
        skinnedPosition = position;
        skinnedNormal = normal;
        skinnedTangent = tangent.xyz;
    } else {
        skinnedPosition /= total;
        skinnedNormal = normalize(skinnedNormal);
        skinnedTangent = normalize(skinnedTangent);
    }

    uint dstByte = (skin.dstFirstVertex + vertex) * 64u;
    float3 previousPosition = asfloat(deformedVertices.Load3(dstByte));
    previousPositions[skin.dstFirstVertex + vertex] = float4(skin.firstPose != 0u ? skinnedPosition : previousPosition, 1.0);
    deformedVertices.Store3(dstByte, asuint(skinnedPosition));
    deformedVertices.Store3(dstByte + 12u, asuint(skinnedNormal));
    deformedVertices.Store4(dstByte + 48u, asuint(float4(skinnedTangent, tangent.w)));
}
