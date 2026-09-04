// Explicit HDR bloom composite: combines the (possibly custom-effect-modified) scene-linear HDR
// image with the resolved bloom mip chain into one scene-linear HDR result. This is a separate,
// dedicated pass rather than folding bloom into the tonemap shader so that AfterBloomBeforeToneMap
// custom effects see one plain scene-linear HDR texture, and tonemapping never double-applies bloom.

import sturdy_common;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct BloomCompositeConstants {
    float bloomIntensity;
    uint thresholdEnabled;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS

// WebGPU has no push constants at all, so on that target the same block is carried as an

// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of

// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant

// block is packed std430-style, a uniform buffer std140, so a struct whose members would

// pad differently between the two must be laid out to agree under both.

[[vk::binding(0, 3)]] ConstantBuffer<BloomCompositeConstants> composite;

#else

[[push_constant]] ConstantBuffer<BloomCompositeConstants> composite;

#endif
[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = fullscreenTrianglePosition(output.uv);
    return output;
}

Texture2D<float4> sceneTexture;
Texture2D<float4> bloomTexture;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 sceneSample = sceneTexture.Sample(sourceSampler, input.uv);
    float4 bloomSample = bloomTexture.Sample(sourceSampler, input.uv);
    float3 scene = max(sceneSample.rgb, 0.0);
    float3 bloom = max(bloomSample.rgb, 0.0);
    float intensity = saturate(composite.bloomIntensity);
    // Thresholded bloom is an artistic emission layer and remains additive. In the Jimenez
    // no-threshold mode, interpolation conserves a spatially constant HDR image exactly.
    float3 combined = composite.thresholdEnabled != 0
        ? scene + bloom * composite.bloomIntensity
        : lerp(scene, bloom, intensity);
    // Preserves real coverage instead of forcing full opacity: the main scene's own scene texture is
    // already fully opaque everywhere, so this is a no-op there, but a transparent-background source
    // (e.g. one small UI element rendered alone for its own glow) needs its actual — and the bloom's
    // own blurred-outward — alpha to composite correctly instead of painting a solid opaque rect.
    float alpha = saturate(max(sceneSample.a, bloomSample.a));
    return float4(combined, alpha);
}
