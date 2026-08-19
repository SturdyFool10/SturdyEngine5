// Bakes the sky-view LUT: a 2D slice of the sky dome as seen from the camera's *current* height,
// one texel per (zenith, azimuth-relative-to-sun) direction (Hillaire section 5.3's parameterization
// — see skyViewLutUv/skyViewLutDirection in Shaders/sturdy_atmosphere.slang). Unlike the
// transmittance/multi-scattering LUTs above, this one genuinely depends on the current frame's
// camera height and sun direction, which is exactly why it's cheap to rebake every frame instead of
// needing a full 3D LUT over every possible height.
//
// World "up" is a fixed (0, 1, 0) throughout this engine's atmosphere model (the planet center is
// always placed directly below the camera along -Y — see Renderer::prepare_atmosphere_frame), so the
// canonical local frame used here (origin at (0, viewHeight, 0), up = +Y) matches the real world
// frame exactly rather than approximating it.

import sturdy_common;
import sturdy_atmosphere;

static const uint2 LUT_SIZE = uint2(192, 108);
static const uint SAMPLE_COUNT = 32;

ConstantBuffer<AtmosphereGpuData> atmosphereData;
Texture2D<float4> transmittanceLut;
Texture2D<float4> multiScatteringLut;
SamplerState atmosphereSampler;
RWTexture2D<float4> outputLut;

[shader("compute")]
[numthreads(8, 8, 1)]
void mainCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (any(dispatchThreadId.xy >= LUT_SIZE)) {
        return;
    }
    AtmosphereParams p = unpackAtmosphereParams(atmosphereData);
    float3 upVector = float3(0.0, 1.0, 0.0);
    float viewHeight = max(length(atmosphereData.cameraPositionPlanetSpace.xyz), p.planetRadiusMeters + 1.0);
    float3 sunDir = safeNormalize(atmosphereData.sunDirectionAngularRadius.xyz);

    float2 texelUv = (float2(dispatchThreadId.xy) + 0.5) / float2(LUT_SIZE);
    bool rayIntersectsGround;
    float3 rayDir = skyViewLutDirection(p, texelUv, upVector, sunDir, viewHeight, LUT_SIZE, rayIntersectsGround);

    float3 origin = float3(0.0, viewHeight, 0.0);
    float tMax;
    if (rayIntersectsGround) {
        distanceToGround(p, origin, rayDir, tMax);
    } else {
        distanceToAtmosphereTop(p, origin, rayDir, tMax);
    }

    ScatteringResult result = integrateScatteredLuminance(
        p, origin, rayDir, sunDir, atmosphereData.sunIlluminance.rgb, tMax, SAMPLE_COUNT,
        transmittanceLut, atmosphereSampler, multiScatteringLut, atmosphereSampler,
        /*useMultiScattering*/ true, /*includeGroundBounce*/ true);
    outputLut[dispatchThreadId.xy] = float4(result.luminance, 1.0);
}
