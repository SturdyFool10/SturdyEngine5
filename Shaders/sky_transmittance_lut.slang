// Bakes the atmosphere transmittance LUT: for each (view height, view zenith angle) texel, the
// fraction of light surviving Rayleigh/Mie/ozone extinction along a ray from that height straight
// out to the top of the atmosphere. Every other atmosphere shader samples this instead of
// raymarching extinction itself — see Shaders/sturdy_atmosphere.slang for the shared math and the
// non-linear LUT parameterization (transmittanceLutParams) that this bake inverts per texel.
//
// Rebaked every frame (cheap at this resolution/sample count — see Renderer::record_atmosphere_lut_bakes)
// rather than cached/invalidated, since atmosphere parameters can change frame to frame and this has
// no per-pixel scene dependency to gate on.

import sturdy_common;
import sturdy_atmosphere;

static const uint2 LUT_SIZE = uint2(256, 64);
static const uint SAMPLE_COUNT = 40;

ConstantBuffer<AtmosphereGpuData> atmosphereData;
RWTexture2D<float4> outputLut;

[shader("compute")]
[numthreads(8, 8, 1)]
void mainCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (any(dispatchThreadId.xy >= LUT_SIZE)) {
        return;
    }
    AtmosphereParams p = unpackAtmosphereParams(atmosphereData);
    float2 uv = (float2(dispatchThreadId.xy) + 0.5) / float2(LUT_SIZE);
    float viewHeight;
    float viewZenithCosAngle;
    transmittanceLutParams(p, uv, viewHeight, viewZenithCosAngle);

    float3 origin = float3(0.0, viewHeight, 0.0);
    float viewZenithSinAngle = sqrt(saturate(1.0 - viewZenithCosAngle * viewZenithCosAngle));
    float3 dir = float3(viewZenithSinAngle, viewZenithCosAngle, 0.0);
    float tMax;
    distanceToAtmosphereTop(p, origin, dir, tMax);
    float3 transmittance = computeTransmittanceToAtmosphereTop(p, origin, dir, tMax, SAMPLE_COUNT);
    outputLut[dispatchThreadId.xy] = float4(transmittance, 1.0);
}
