// Instanced anti-aliased annular-sector ("pie slice") rendering for Sturdy.UI (UI/UiSectorPipeline.hpp).
//
// Vertex-pulled, same idiom as Shaders/ui_quad.slang/ui_stroke.slang: no vertex buffer, six vertices
// per instance built from SV_VertexID/SV_InstanceID, batched by (scissor, active paint group) on the
// CPU side (UI::UiRenderer::prepare()). One instance draws one sector via the analytic sectorSDF()
// helper in sturdy_common.slang — see its own doc comment for the angle convention and math.

import sturdy_common;

// Field order must byte-match UI/UiSector.hpp's UiSectorInstance exactly (see the comment there).
struct UiSectorInstance {
    float2 center;
    float innerRadius;
    float outerRadius;
    float startAngle;
    float endAngle;
    float featherPx;
    float _pad0;
    float4 color;
};

struct UiViewConstants {
    float2 viewportSize;
    // Explicit base into `instances` for the current batch — see ui_quad.slang's identical
    // `instanceIndexBase` field for why this isn't left to SV_InstanceID/first_instance alone.
    uint instanceIndexBase;
    uint padding;
};

[[push_constant]] ConstantBuffer<UiViewConstants> viewConstants;
StructuredBuffer<UiSectorInstance> instances;

struct VertexOutput {
    float4 position : SV_Position;
    float2 localPos : TEXCOORD0; // pixel offset from the sector's own center
    float innerRadius : TEXCOORD1;
    float outerRadius : TEXCOORD2;
    float startAngle : TEXCOORD3;
    float endAngle : TEXCOORD4;
    float4 color : COLOR0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    UiSectorInstance inst = instances[viewConstants.instanceIndexBase + instanceId];

    float radius = inst.outerRadius + max(inst.featherPx, 0.0);
    float2 corner = uiQuadCorner(vertexId);
    float2 localPos = lerp(float2(-radius, -radius), float2(radius, radius), corner);
    float2 pixelPos = inst.center + localPos;
    float2 pixelUv = pixelPos * safeRcp(viewConstants.viewportSize);

    VertexOutput output;
    output.position = float4(uvToNdc(pixelUv), 0.0, 1.0);
    output.localPos = localPos;
    output.innerRadius = inst.innerRadius;
    output.outerRadius = inst.outerRadius;
    output.startAngle = inst.startAngle;
    output.endAngle = inst.endAngle;
    output.color = inst.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float dist = sectorSDF(input.localPos, input.innerRadius, input.outerRadius, input.startAngle, input.endAngle);
    float edgeSoftness = max(fwidth(dist), SFT_EPSILON);
    float alpha = clamp01(0.5 - dist / edgeSoftness);
    return float4(input.color.rgb, input.color.a * alpha);
}
