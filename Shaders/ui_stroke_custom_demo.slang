// Sample UI::Context::stroke_custom() shader (see UI/CustomElement.hpp's CustomStrokeElementData
// doc comment) — an animated rainbow gradient along the line, to smoke-test the whole custom-
// stroke-shader path end to end. Not part of the engine's own draw primitives; this is a
// demonstration a caller's own shader would follow the same pattern as.
//
// vertexMain MUST build its geometry via sturdy_common.slang's uiStrokeSegmentClipPosition(), given
// a push-constant struct whose first nine fields match CustomStrokeElementConstants exactly.

import sturdy_common;

struct DemoStrokeConstants {
    float2 p0;
    float2 p1;
    float halfWidth;
    float featherPx;
    float2 viewportSize;
    float arcLengthSoFar;
    float _pad0;
    float _pad1;
    float _pad2;
    // Trailing fields, appended by this demo's own CustomShaderRef::push_constants.
    float time;
    float _pad3;
    float _pad4;
    float _pad5;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS

// WebGPU has no push constants at all, so on that target the same block is carried as an

// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of

// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant

// block is packed std430-style, a uniform buffer std140, so a struct whose members would

// pad differently between the two must be laid out to agree under both.

[[vk::binding(0, 3)]] ConstantBuffer<DemoStrokeConstants> constants;

#else

[[push_constant]] ConstantBuffer<DemoStrokeConstants> constants;

#endif
struct VertexOutput {
    float4 position : SV_Position;
    float2 localPos : TEXCOORD0;
    float2 segEnd : TEXCOORD1;
    float radius : TEXCOORD2;
    float time : TEXCOORD3;
    float arcLengthSoFar : TEXCOORD4;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.position = uiStrokeSegmentClipPosition(vertexId, constants.p0, constants.p1, constants.halfWidth,
                                                  constants.featherPx, constants.viewportSize,
                                                  output.localPos, output.segEnd);
    output.radius = constants.halfWidth;
    output.time = constants.time;
    output.arcLengthSoFar = constants.arcLengthSoFar;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    // Reuse the same AA edge test the built-in ui_stroke.slang uses -- a custom shader is free to
    // ignore this and do something entirely different with localPos/segEnd instead.
    float dist = capsuleSDF(input.localPos, float2(0.0, 0.0), input.segEnd, input.radius);
    float edgeSoftness = max(fwidth(dist), SFT_EPSILON);
    float alpha = clamp01(0.5 - dist / edgeSoftness);

    // Distance along the *whole* polyline, not just this segment -- arcLengthSoFar (the segment's
    // own start) plus how far this pixel sits along the segment's own direction. Using a purely
    // per-segment-local parametrization here instead produces a visibly discontinuous "gradient
    // restarts every segment" look, since adjacent segments' local axes point in different
    // directions (see CustomStrokeElementConstants::arc_length_so_far's own doc comment).
    float segDist = length(input.segEnd);
    float alongSegment = segDist > 0.0001 ? dot(input.localPos, input.segEnd / segDist) : 0.0;
    float totalDistance = input.arcLengthSoFar + alongSegment;

    float hue = fmod((totalDistance * 0.6 + input.time * 60.0), 360.0);
    float3 rgb = hsvToRgb(hue, 0.8, 1.0);
    return float4(rgb, alpha);
}
