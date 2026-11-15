// Clips an element and everything in it to the element's own box with its rounded, bevelled, scooped or notched corners (UI::Effects::clip_box, used
// for `ClipConfig::follow_corners`: CSS `overflow: hidden` with `border-radius`, including `corner-shape`), with a one pixel antialiased edge.
//
// Parameters (32 bytes):
//   radii = (top left, top right, bottom left, bottom right) corner radii in surface pixels
//   codes = the four corners' CornerShape bytes as floats, same order (see uiCornerShapeK)

import sturdy_common;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = fullscreenTriangleUv(vertexId);
    output.position = fullscreenTrianglePosition(output.uv);
    return output;
}

struct ClipBoxParameters {
    UiEffectConstants frame;
    float4 radii;
    float4 codes;
};

[[push_constant]] ConstantBuffer<ClipBoxParameters> parameters;

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 content = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
    float2 pixel = input.uv * parameters.frame.contentSize;
    float2 halfSize = parameters.frame.groupSize * 0.5;
    float2 centerPos = pixel - parameters.frame.groupOrigin - halfSize;
    float4 k = float4(uiCornerShapeK(uint(parameters.codes.x + 0.5)), uiCornerShapeK(uint(parameters.codes.y + 0.5)),
                      uiCornerShapeK(uint(parameters.codes.z + 0.5)), uiCornerShapeK(uint(parameters.codes.w + 0.5)));
    float sd = shapedBoxSDF(centerPos, halfSize, parameters.radii, k);
    return content * saturate(0.5 - sd);
}
