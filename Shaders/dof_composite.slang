// Depth of field, pass 4 of 4: combine the sharp frame with the far and near fields at full resolution.
//
// The half-resolution fields are filtered with a 3x3 tent (which also hides the gather's fixed per-pixel pattern), then:
//   * far: the sharp colour blends towards the far field as this pixel's own far blur grows past a pixel or two -- so an in-focus
//     foreground pixel stays sharp even where the far field behind it is wide;
//   * near: the near field is laid *over* the result by its coverage, which is how a blurred foreground object bleeds across the
//     sharp surface behind it.
import sturdy_common;
import dof_common;

ConstantBuffer<DofConstants> dof;
Texture2D<float4> sceneColor;
DepthTexture2D sceneDepth;
Texture2D<float4> farField;
Texture2D<float4> nearField;
SamplerState linearSampler;
[format("rgba16f")]
WTexture2D<float4> colorOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void compositeMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= dof.extents.xy)) {
        return;
    }
    int2 pixel = int2(id.xy);
    float4 sharp = sceneColor.Load(int3(pixel, 0));
    float coc = dofSignedCoc(dof, sceneDepth.Load(int3(pixel, 0)));
    float ownFar = max(coc, 0.0);

    float2 halfSize = float2(dof.extents.zw);
    float2 uv = (float2(pixel) + 0.5) / float2(dof.extents.xy);
    float3 farColor = float3(0.0, 0.0, 0.0);
    float farAlpha = 0.0;
    float3 nearColor = float3(0.0, 0.0, 0.0);
    float nearAlpha = 0.0;
    float nearWeight = 0.0;
    float tentWeight = 0.0;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            float weight = (2.0 - abs(float(dx))) * (2.0 - abs(float(dy)));
            float2 tapUv = uv + float2(dx, dy) / halfSize;
            float4 far = farField.SampleLevel(linearSampler, tapUv, 0.0);
            float4 near = nearField.SampleLevel(linearSampler, tapUv, 0.0);
            farColor += far.rgb * far.a * weight;
            farAlpha += far.a * weight;
            // Colour is averaged over the taps that actually hold near-field coverage.
            nearColor += near.rgb * near.a * weight;
            nearAlpha += near.a * weight;
            nearWeight += weight;
            tentWeight += weight;
        }
    }
    farColor = farAlpha > 1.0e-4 ? farColor / farAlpha : sharp.rgb;
    farAlpha /= tentWeight;
    nearColor = nearAlpha > 1.0e-4 ? nearColor / nearAlpha : sharp.rgb;
    nearAlpha /= nearWeight;

    float3 result = sharp.rgb;
    float farBlend = smoothstep(0.5, 3.0, ownFar) * saturate(farAlpha * 4.0);
    result = lerp(result, farColor, farBlend);
    result = lerp(result, nearColor, dof.lens.w > 0.5 ? saturate(nearAlpha) : 0.0);
    colorOut.Store(pixel, float4(result, sharp.a));
}
