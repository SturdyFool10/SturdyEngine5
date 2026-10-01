// Histogram auto-exposure, pass 3 of 3: multiplies the scene-linear colour by the metered exposure.
// A plain custom post-process fullscreen effect (sourceTexture + extraTexture0 = the 1x1 exposure
// texture), so it can be swapped or reused like any other; a replacement metering feature only has to
// publish a scene colour that is already exposed.
import sturdy_common;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = fullscreenTrianglePosition(output.uv);
    return output;
}

Texture2D<float4> sourceTexture;
Texture2D<float> extraTexture0;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 source = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
    float exposure = extraTexture0.Load(int3(0, 0, 0));
    return float4(source.rgb * exposure, source.a);
}
