// Volumetric fog, pass 2 of 3: front-to-back integration along every froxel column.
//
// Each slice is integrated analytically assuming constant scattering and extinction across it (Hillaire 2015), which
// stays energy-conserving however thick the slice is.
//
// Output (rgba16f atlas, same layout as the input): for each slice, rgb = scattered light accumulated from the camera
// to the slice's far boundary, a = transmittance over the same range.
import sturdy_common;
import sturdy_lighting_data;
import volumetric_fog_common;

ConstantBuffer<ShadowLightingGpuData> lightingData;
ConstantBuffer<FogConstants> fog;
Texture2D<float4> injected;
[format("rgba16f")]
WTexture2D<float4> integratedOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void integrateMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= fog.grid.xy)) {
        return;
    }
    float2 uv = min((float2(id.xy) + 0.5) * fog.render.z, fog.render.xy - 0.5) / fog.render.xy;
    float depthPerDistance;
    fogViewRay(lightingData, uv, depthPerDistance);

    float3 accumulated = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;
    [loop]
    for (uint z = 0; z < fog.grid.z; ++z) {
        int2 texel = fogAtlasTexel(fog, uint3(id.xy, z));
        float4 froxel = injected.Load(int3(texel, 0));
        float stepLength = (fogSliceDepth(fog, float(z + 1u)) - fogSliceDepth(fog, float(z))) / depthPerDistance;
        float extinction = max(froxel.a, 1.0e-6);
        float sliceTransmittance = exp(-extinction * stepLength);
        accumulated += transmittance * froxel.rgb * (1.0 - sliceTransmittance) / extinction;
        transmittance *= sliceTransmittance;
        integratedOut.Store(texel, float4(accumulated, transmittance));
    }
}
