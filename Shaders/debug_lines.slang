// Debug line drawing (Renderer/DebugDraw.hpp): thick, anti-aliased 3D line segments with a per-pixel depth test against the scene.
//
// Vertex-pulled like ui_stroke.slang: no vertex buffer, six vertices per instance, one instance per segment, so a frame's
// whole debug drawing is a single draw call. The segment is projected to the screen (clipped against the near plane first, so a
// line passing behind the camera stays correct), expanded into a capsule `widthPx` wide, and shaded with the same capsule
// distance field the UI strokes use.
//
// Depth: debug lines are drawn after tone mapping, onto the display target, which has no depth attachment. Occlusion is
// therefore decided here, by comparing the line's own depth with the scene's depth buffer (`SFT_DEBUG_LINES_DEPTH` = 1). A
// hidden stretch is not removed but dimmed by `xrayAlpha` (0 = hidden lines vanish), the usual "see through walls, faintly"
// behaviour. Frames without a scene have no depth, so the renderer compiles the variant with SFT_DEBUG_LINES_DEPTH = 0.

import sturdy_common;

#ifndef SFT_DEBUG_LINES_DEPTH
#define SFT_DEBUG_LINES_DEPTH 0
#endif

// Must match `DebugLineGpu` in Renderer/DebugDraw.cpp.
struct DebugLineInstance {
    float3 a;
    float widthPx;
    float3 b;
    uint color; // rgba, 8 bits per channel, r in the low byte
};

struct DebugLineConstants {
    float4x4 viewProjection;
    float2 viewportSize;
    // Alpha multiplier for the parts of a line behind scene geometry.
    float xrayAlpha;
    // Clip-space w below which geometry is considered behind the near plane.
    float nearW;
};
#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<DebugLineConstants> lineConstants;
#else
[[push_constant]] ConstantBuffer<DebugLineConstants> lineConstants;
#endif

StructuredBuffer<DebugLineInstance> lines;
#if SFT_DEBUG_LINES_DEPTH
DepthTexture2D sceneDepth;
#endif

struct VertexOutput {
    float4 position : SV_Position;
    float2 localPos : TEXCOORD0; // pixel offset from the projected start point
    float2 segEnd : TEXCOORD1;   // projected end point in that local space
    float radius : TEXCOORD2;    // half width in pixels
    float2 depthRange : TEXCOORD3; // NDC depth at the projected start / end
    float4 color : COLOR0;
};

float4 unpackColor(uint packed) {
    return float4(float(packed & 0xffu), float((packed >> 8u) & 0xffu), float((packed >> 16u) & 0xffu),
                  float((packed >> 24u) & 0xffu)) * (1.0 / 255.0);
}

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    DebugLineInstance line = lines[instanceId];
    float4 clipA = mul(float4(line.a, 1.0), lineConstants.viewProjection);
    float4 clipB = mul(float4(line.b, 1.0), lineConstants.viewProjection);

    // Clip to the near plane in homogeneous space (a line crossing it would otherwise project to the wrong side).
    float nearW = lineConstants.nearW;
    bool culled = clipA.w < nearW && clipB.w < nearW;
    if (clipA.w < nearW && !culled) {
        clipA = lerp(clipA, clipB, (nearW - clipA.w) / (clipB.w - clipA.w));
    } else if (clipB.w < nearW && !culled) {
        clipB = lerp(clipB, clipA, (nearW - clipB.w) / (clipA.w - clipB.w));
    }

    VertexOutput output;
    output.localPos = float2(0.0, 0.0);
    output.segEnd = float2(0.0, 0.0);
    output.radius = 0.0;
    output.depthRange = float2(0.0, 0.0);
    output.color = float4(0.0, 0.0, 0.0, 0.0);
    if (culled) {
        output.position = float4(2.0, 2.0, 2.0, 1.0); // outside the clip volume: the quad is discarded
        return output;
    }

    float3 ndcA = clipA.xyz / clipA.w;
    float3 ndcB = clipB.xyz / clipB.w;
    float2 p0 = ndcToUv(ndcA.xy) * lineConstants.viewportSize;
    float2 p1 = ndcToUv(ndcB.xy) * lineConstants.viewportSize;

    float segLength = length(p1 - p0);
    float2 segDir = segLength > 1.0e-4 ? (p1 - p0) / segLength : float2(1.0, 0.0);
    float2 segNormal = float2(-segDir.y, segDir.x);
    // One pixel of padding for the anti-aliased edge.
    float radius = 0.5 * max(line.widthPx, 1.0);
    float pad = radius + 1.0;

    float2 corner = uiQuadCorner(vertexId);
    float alongLocal = lerp(-pad, segLength + pad, corner.x);
    float acrossLocal = lerp(-pad, pad, corner.y);
    float2 localPos = segDir * alongLocal + segNormal * acrossLocal;
    float2 pixelPos = p0 + localPos;

    output.position = float4(uvToNdc(pixelPos / lineConstants.viewportSize), 0.0, 1.0);
    output.localPos = localPos;
    output.segEnd = segDir * segLength;
    output.radius = radius;
    output.depthRange = float2(ndcA.z, ndcB.z);
    output.color = unpackColor(line.color);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float dist = capsuleSDF(input.localPos, float2(0.0, 0.0), input.segEnd, input.radius);
    // Taken before any branch: derivatives must come from uniform control flow.
    float edgeSoftness = max(fwidth(dist), SFT_EPSILON);
    float alpha = clamp01(0.5 - dist / edgeSoftness);

#if SFT_DEBUG_LINES_DEPTH
    // NDC depth is affine in screen space, so it interpolates linearly along the projected segment.
    float segLengthSquared = max(dot(input.segEnd, input.segEnd), 1.0e-6);
    float along = clamp01(dot(input.localPos, input.segEnd) / segLengthSquared);
    float lineDepth = lerp(input.depthRange.x, input.depthRange.y, along);

    uint depthWidth, depthHeight;
    sceneDepth.GetDimensions(depthWidth, depthHeight);
    float2 uv = input.position.xy / lineConstants.viewportSize;
    int2 texel = clamp(int2(uv * float2(depthWidth, depthHeight)), int2(0, 0), int2(int(depthWidth) - 1, int(depthHeight) - 1));
    float sceneZ = sceneDepth.Load(int3(texel, 0));
    // Small relative bias so a line lying on a surface (a drawn bounding box face) does not z-fight with it.
    bool visible = lineDepth <= sceneZ * 1.0005 + 2.0e-5;
    alpha *= visible ? 1.0 : lineConstants.xrayAlpha;
#endif

    return float4(input.color.rgb, input.color.a * alpha);
}
