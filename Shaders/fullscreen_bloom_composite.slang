// Explicit HDR bloom composite: combines the (possibly custom-effect-modified) scene-linear HDR
// image with the resolved bloom mip chain into one scene-linear HDR result. This is a separate,
// dedicated pass rather than folding bloom into the tonemap shader so that AfterBloomBeforeToneMap
// custom effects see one plain scene-linear HDR texture, and tonemapping never double-applies bloom.

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

Texture2D<float4> sceneTexture;
Texture2D<float4> bloomTexture;
SamplerState sourceSampler;

struct BloomCompositeConstants {
    float bloomIntensity;
};

[[push_constant]] ConstantBuffer<BloomCompositeConstants> composite;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float3 scene = max(sceneTexture.Sample(sourceSampler, input.uv).rgb, 0.0);
    float3 bloom = max(bloomTexture.Sample(sourceSampler, input.uv).rgb, 0.0);
    return float4(scene + bloom * composite.bloomIntensity, 1.0);
}
