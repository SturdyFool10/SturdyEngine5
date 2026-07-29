// Bakes the multiple-scattering LUT: for each (view height, sun zenith angle) texel, an
// isotropic-approximation radiance response representing the infinite sum of second-order-and-beyond
// scattering bounces (Hillaire section 5.2's closed-form geometric series). Stored per unit sun
// illuminance — Shaders/sturdy_atmosphere.slang's integrateScatteredLuminance multiplies by the real
// sun illuminance when sampling this. See computeMultipleScattering there for the actual math; this
// file only maps texel <-> (height, sun angle) and drives the dispatch.

import sturdy_common;
import sturdy_atmosphere;

static const uint2 LUT_SIZE = uint2(32, 32);
static const uint SQRT_SAMPLE_COUNT = 8;

ConstantBuffer<AtmosphereGpuData> atmosphereData;
Texture2D<float4> transmittanceLut;
SamplerState atmosphereSampler;
RWTexture2D<float4> outputLut;

[shader("compute")]
[numthreads(8, 8, 1)]
void mainCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (any(dispatchThreadId.xy >= LUT_SIZE)) {
        return;
    }
    AtmosphereParams p = unpackAtmosphereParams(atmosphereData);
    // Inverse of multiScatteringLutUv (Shaders/sturdy_atmosphere.slang) — that mapping is linear, so
    // the inverse is a direct texel->parameter remap rather than a dedicated "Params" out-param
    // function like the non-linear transmittance LUT needs.
    float2 uv = (float2(dispatchThreadId.xy) + 0.5) / float2(LUT_SIZE);
    float sunCosFraction = uv.x;
    float heightFraction = uv.y;
    float viewHeight = p.planetRadiusMeters + heightFraction * (p.atmosphereRadiusMeters - p.planetRadiusMeters);
    float sunZenithCosAngle = sunCosFraction * 2.0 - 1.0;

    float3 origin = float3(0.0, viewHeight, 0.0);
    float sunZenithSinAngle = sqrt(saturate(1.0 - sunZenithCosAngle * sunZenithCosAngle));
    float3 sunDir = float3(sunZenithSinAngle, sunZenithCosAngle, 0.0);
    float3 multiScattering = computeMultipleScattering(p, origin, sunDir, SQRT_SAMPLE_COUNT, transmittanceLut, atmosphereSampler);
    outputLut[dispatchThreadId.xy] = float4(multiScattering, 1.0);
}
