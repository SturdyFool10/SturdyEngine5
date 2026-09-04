// Instanced anti-aliased line-segment ("stroke") rendering for Sturdy.UI (UI/UiStrokePipeline.hpp).
//
// Vertex-pulled, same idiom as Shaders/ui_quad.slang: no vertex buffer, six vertices per instance
// built from SV_VertexID/SV_InstanceID, batched by (scissor, active paint group) on the CPU side
// (UI::UiRenderer::prepare()). One instance draws ONE polyline segment as a capsule (a rounded-end
// thick line, via capsuleSDF() in sturdy_common.slang) — a capsule's own round ends double as both the
// segment's line caps AND, because consecutive segments of one polyline share an endpoint, its round
// joins: two capsules meeting at a shared point already overlap into a seamless round join with no
// extra geometry. Miter/bevel joins and butt/square caps are not implemented (StrokeJoinKind/
// StrokeCapKind exist for a future stage); every stroke today is drawn round-capped and round-joined
// regardless of the requested kind.

import sturdy_common;

// Field order must byte-match UI/UiStroke.hpp's UiStrokeInstance exactly (see the comment there).
struct UiStrokeInstance {
    float2 p0;
    float2 p1;
    float4 color;
    float halfWidth;
    float featherPx;   // 0 = derive AA purely from fwidth(), matching ui_quad.slang's own convention
    float dashLength;  // 0 = solid, no dashing
    float dashGap;
    float dashPhase;       // arc-length offset of this segment's start along the whole polyline
    float snapToPixelGrid; // >0.5 = true; only meaningful for axis-aligned segments (axis/gridlines)
    float _pad0;
    float _pad1;
};

struct UiViewConstants {
    float2 viewportSize;
    // Explicit base into `instances` for the current batch — see ui_quad.slang's identical
    // `instanceIndexBase` field for why this isn't left to SV_InstanceID/first_instance alone.
    uint instanceIndexBase;
    uint padding;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS

// WebGPU has no push constants at all, so on that target the same block is carried as an

// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of

// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant

// block is packed std430-style, a uniform buffer std140, so a struct whose members would

// pad differently between the two must be laid out to agree under both.

[[vk::binding(0, 3)]] ConstantBuffer<UiViewConstants> viewConstants;

#else

[[push_constant]] ConstantBuffer<UiViewConstants> viewConstants;

#endif
StructuredBuffer<UiStrokeInstance> instances;

struct VertexOutput {
    float4 position : SV_Position;
    float2 localPos : TEXCOORD0; // pixel offset from (possibly pixel-snapped) p0
    float2 segEnd : TEXCOORD1;   // p1 - p0 in that same local space
    float radius : TEXCOORD2;    // geometric half-width (unfeathered) — the true capsule edge
    float4 color : COLOR0;
    float3 dash : TEXCOORD3; // x = dashLength, y = dashGap, z = dashPhase
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    UiStrokeInstance inst = instances[viewConstants.instanceIndexBase + instanceId];

    float2 p0 = inst.p0;
    float2 p1 = inst.p1;
    if (inst.snapToPixelGrid > 0.5) {
        // Snap axis-aligned hairlines to the pixel grid: a crisp 1px line instead of a fwidth-blurred
        // half-pixel-offset one. Only meaningful when the segment is exactly axis-aligned — a diagonal
        // "pixel-perfect" snap has no single well-defined grid to snap to, so only the axis the
        // segment is constant along gets adjusted.
        if (abs(p0.y - p1.y) < 0.001) {
            float snappedY = floor(p0.y) + 0.5;
            p0.y = snappedY;
            p1.y = snappedY;
        } else if (abs(p0.x - p1.x) < 0.001) {
            float snappedX = floor(p0.x) + 0.5;
            p0.x = snappedX;
            p1.x = snappedX;
        }
    }

    float segLength = length(p1 - p0);
    // A degenerate (zero-length) segment still renders as a filled circle of `radius` — used by
    // point-marker draws (scatter charts) that emit a single-point "segment" rather than a real line.
    float2 segDir = segLength > 0.0001 ? (p1 - p0) / segLength : float2(1.0, 0.0);
    float2 segNormal = float2(-segDir.y, segDir.x);
    float radius = inst.halfWidth + max(inst.featherPx, 0.0);

    float2 corner = uiQuadCorner(vertexId);
    // Expand corner.x across [-radius, segLength + radius] (covers both round caps) and corner.y
    // across [-radius, radius] (covers the stroke width plus any AA/feather padding).
    float alongLocal = lerp(-radius, segLength + radius, corner.x);
    float acrossLocal = lerp(-radius, radius, corner.y);
    float2 localPos = segDir * alongLocal + segNormal * acrossLocal;
    float2 pixelPos = p0 + localPos;
    float2 pixelUv = pixelPos * safeRcp(viewConstants.viewportSize);

    VertexOutput output;
    output.position = float4(uvToNdc(pixelUv), 0.0, 1.0);
    output.localPos = localPos;
    output.segEnd = segDir * segLength;
    output.radius = inst.halfWidth;
    output.color = inst.color;
    output.dash = float3(inst.dashLength, inst.dashGap, inst.dashPhase);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float dist = capsuleSDF(input.localPos, float2(0.0, 0.0), input.segEnd, input.radius);
    float edgeSoftness = max(fwidth(dist), SFT_EPSILON);
    float alpha = clamp01(0.5 - dist / edgeSoftness);

    if (input.dash.x > 0.0) {
        float2 segDir = safeNormalize(input.segEnd);
        float along = dot(input.localPos, segDir) + input.dash.z;
        float period = max(input.dash.x + input.dash.y, SFT_EPSILON);
        float phase = along - floor(along / period) * period; // positive-only fmod
        if (phase >= input.dash.x) {
            alpha = 0.0;
        }
    }

    return float4(input.color.rgb, input.color.a * alpha);
}
