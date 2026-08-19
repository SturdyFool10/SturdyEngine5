// Shared Hillaire/Bruneton-style precomputed atmospheric scattering math.
//
// Usage from any shader under project_root/Shaders/*:
//     import sturdy_atmosphere;
//
// Dependency-free aside from sturdy_common (no resource bindings of its own), mirroring
// sturdy_tonemap_agx.slang's shape: every function here is a pure function of its explicit
// arguments, including the LUT textures/samplers it reads. That is deliberate — it means the exact
// same integrateScatteredLuminance/sampleTransmittanceLut/etc. calls used to bake the LUTs below and
// to resolve the rasterized sky/aerial-perspective in Shaders/deferred_shadow_lighting.slang are also
// what a future raytracing miss shader would call: nothing here depends on screen-space UVs, a
// G-buffer, or any other rasterization-only state, only a ray origin/direction and bound LUTs.
//
// Reference: Sébastien Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique" (2020), itself building on Eric Bruneton & Fabrice Neyret's original precomputed
// atmospheric scattering model. All positions in this module are in "planet space" — the origin is
// the planet's center, not the camera or world origin; callers translate world-space positions by
// the current frame's planet center (see AtmosphereGpuData::planetCenterWorld) before calling in.

import sturdy_common;

// -------------------------------------------------------------------------------------------------
// Parameters
// -------------------------------------------------------------------------------------------------

// Friendly, individually addressable atmosphere physics constants — what every math function below
// operates on. Not itself a GPU resource type; see AtmosphereGpuData/unpackAtmosphereParams for the
// wire format actually bound as a ConstantBuffer<T>.
struct AtmosphereParams {
    float3 rayleighScattering;      // 1/m at sea level
    float rayleighDensityExpScale;  // = -1/H_R (Rayleigh scale height)
    float3 mieScattering;           // 1/m at sea level
    float mieDensityExpScale;       // = -1/H_M (Mie scale height)
    float3 mieExtinction;           // 1/m at sea level (scattering + absorption)
    float miePhaseG;                // Cornette-Shanks asymmetry factor, (-1, 1)
    float3 ozoneAbsorption;         // 1/m, peak of the ozone tent-density profile
    float ozoneLayerCenterAltitude; // meters above the planet surface
    float ozoneLayerWidth;          // meters; tent half-width (zero density outside +/- this)
    float3 groundAlbedo;
    float planetRadiusMeters;
    float atmosphereRadiusMeters;
};

// GPU constant-buffer layout — field-for-field identical to Renderer::AtmosphereGpuData
// (RendererModule.hpp), pinned there by static_assert, same convention ShadowLightingGpuData already
// follows. Packed into float4s (never a bare float3) so this struct's layout matches its C++ mirror
// byte-for-byte with no implicit vec3-then-scalar padding surprises.
struct AtmosphereGpuData {
    float4 rayleighScatteringExpScale;       // rgb = rayleighScattering, a = rayleighDensityExpScale
    float4 mieScatteringExpScale;            // rgb = mieScattering, a = mieDensityExpScale
    float4 mieExtinctionPhaseG;              // rgb = mieExtinction, a = miePhaseG
    float4 ozoneAbsorptionCenterAltitude;    // rgb = ozoneAbsorption, a = ozoneLayerCenterAltitude
    float4 ozoneWidthPlanetAtmosphereRadius; // x = ozoneLayerWidth, y = planetRadiusMeters, z = atmosphereRadiusMeters
    float4 groundAlbedo;                     // rgb = groundAlbedo, a unused
    float4 planetCenterWorld;                // xyz = this frame's planet center in world space, w unused
    // Frame/view data the LUT bake shaders need but have no other access to (they run before the
    // gbuffer/lighting pass and never see ShadowLightingGpuData) — kept in this same buffer rather
    // than a second one so every atmosphere-related shader has exactly one binding to reason about.
    float4 cameraPositionPlanetSpace; // xyz = camera position relative to the planet center, w unused
    // xyz points FROM the surface TOWARD the sun (already negated on the CPU side relative to
    // DirectionalLightGpuData::directionAngularRadius, which points the opposite way — every phase
    // function/dot product in this module expects a toward-the-light vector). w = angular radius
    // (radians).
    float4 sunDirectionAngularRadius;
    float4 sunIlluminance; // rgb = sun illuminance/radiance (same quantity evaluateDirect() uses), a unused
};

AtmosphereParams unpackAtmosphereParams(AtmosphereGpuData data) {
    AtmosphereParams p;
    p.rayleighScattering = data.rayleighScatteringExpScale.rgb;
    p.rayleighDensityExpScale = data.rayleighScatteringExpScale.a;
    p.mieScattering = data.mieScatteringExpScale.rgb;
    p.mieDensityExpScale = data.mieScatteringExpScale.a;
    p.mieExtinction = data.mieExtinctionPhaseG.rgb;
    p.miePhaseG = data.mieExtinctionPhaseG.a;
    p.ozoneAbsorption = data.ozoneAbsorptionCenterAltitude.rgb;
    p.ozoneLayerCenterAltitude = data.ozoneAbsorptionCenterAltitude.a;
    p.ozoneLayerWidth = data.ozoneWidthPlanetAtmosphereRadius.x;
    p.planetRadiusMeters = data.ozoneWidthPlanetAtmosphereRadius.y;
    p.atmosphereRadiusMeters = data.ozoneWidthPlanetAtmosphereRadius.z;
    p.groundAlbedo = data.groundAlbedo.rgb;
    return p;
}

// Earth reference constants (Bruneton's commonly used values). Not read by the bake shaders at
// runtime — Renderer::prepare_atmosphere_frame (RendererAtmosphere.cpp) uploads the identical
// literals into AtmosphereGpuData every frame — this exists as inline documentation of the correct
// values right next to the math that consumes them, and as a standalone default for anyone driving
// this module without the engine's own per-frame buffer.
AtmosphereParams defaultEarthAtmosphere() {
    AtmosphereParams p;
    p.rayleighScattering = float3(5.802e-6, 13.558e-6, 33.1e-6);
    p.rayleighDensityExpScale = -1.0 / 8000.0;
    p.mieScattering = float3(3.996e-6, 3.996e-6, 3.996e-6);
    p.mieDensityExpScale = -1.0 / 1200.0;
    p.mieExtinction = float3(4.440e-6, 4.440e-6, 4.440e-6);
    p.miePhaseG = 0.8;
    p.ozoneAbsorption = float3(0.650e-6, 1.881e-6, 0.085e-6);
    p.ozoneLayerCenterAltitude = 25000.0;
    p.ozoneLayerWidth = 15000.0;
    p.groundAlbedo = float3(0.3, 0.3, 0.3);
    p.planetRadiusMeters = 6360000.0;
    p.atmosphereRadiusMeters = 6460000.0;
    return p;
}

// -------------------------------------------------------------------------------------------------
// Density profiles and phase functions
// -------------------------------------------------------------------------------------------------

// `h` is altitude above the planet surface, meters (not distance from planet center).
float rayleighDensity(AtmosphereParams p, float h) {
    return exp(p.rayleighDensityExpScale * h);
}

float mieDensity(AtmosphereParams p, float h) {
    return exp(p.mieDensityExpScale * h);
}

// Symmetric tent profile: zero below (centerAltitude - width) and above (centerAltitude + width),
// linearly ramping to 1 at the center — the standard cheap stand-in for the ozone layer's real
// (asymmetric, ~10-40km) density curve.
float ozoneDensity(AtmosphereParams p, float h) {
    return saturate(1.0 - abs(h - p.ozoneLayerCenterAltitude) * safeRcp(p.ozoneLayerWidth));
}

float rayleighPhase(float cosTheta) {
    return (3.0 / (16.0 * SFT_PI)) * (1.0 + cosTheta * cosTheta);
}

float cornetteShanksMiePhase(float g, float cosTheta) {
    float g2 = g * g;
    float num = 3.0 * (1.0 - g2) * (1.0 + cosTheta * cosTheta);
    float denom = 8.0 * SFT_PI * (2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * cosTheta, SFT_EPSILON), 1.5);
    return num * safeRcp(denom);
}

// -------------------------------------------------------------------------------------------------
// Ray/sphere geometry (planet-space: sphere center at the origin)
// -------------------------------------------------------------------------------------------------

// `dir` must be unit length. Returns false (tNear/tFar zeroed) when the ray never touches the
// sphere; tNear can be negative (origin already inside the sphere).
bool raySphereIntersect(float3 origin, float3 dir, float radius, out float tNear, out float tFar) {
    float b = dot(origin, dir);
    float c = dot(origin, origin) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant < 0.0) {
        tNear = 0.0;
        tFar = 0.0;
        return false;
    }
    float sqrtDiscriminant = sqrt(discriminant);
    tNear = -b - sqrtDiscriminant;
    tFar = -b + sqrtDiscriminant;
    return true;
}

// Distance until the ray exits through the top of the atmosphere. Valid for the common case (origin
// already inside the atmosphere shell, which is every camera position this engine renders from).
bool distanceToAtmosphereTop(AtmosphereParams p, float3 originPlanetSpace, float3 dir, out float tMax) {
    float tNear, tFar;
    bool hit = raySphereIntersect(originPlanetSpace, dir, p.atmosphereRadiusMeters, tNear, tFar);
    tMax = max(tFar, 0.0);
    return hit && tFar >= 0.0;
}

// Distance until the ray hits the planet surface ahead of the origin. False (and tGround
// meaningless) when the ray points away from the planet entirely.
bool distanceToGround(AtmosphereParams p, float3 originPlanetSpace, float3 dir, out float tGround) {
    float tNear, tFar;
    bool hit = raySphereIntersect(originPlanetSpace, dir, p.planetRadiusMeters, tNear, tFar);
    tGround = tNear;
    return hit && tNear >= 0.0;
}

// -------------------------------------------------------------------------------------------------
// LUT texel <-> parameter mappings
// -------------------------------------------------------------------------------------------------

// Bilinear-safe remap so a LUT's edge texels sample exactly at the domain's extremes instead of a
// half-texel short of them (Bruneton/Hillaire's standard fix for visible seams at the poles/horizon).
float fromUnitToSubUvs(float u, float resolution) {
    return (u + 0.5 * safeRcp(resolution)) * (resolution * safeRcp(resolution + 1.0));
}

float fromSubUvsToUnit(float u, float resolution) {
    return (u - 0.5 * safeRcp(resolution)) * (resolution * safeRcp(resolution - 1.0));
}

// Bruneton's non-linear height/cos(zenith) mapping — concentrates transmittance-LUT resolution near
// the horizon (where transmittance changes fastest) instead of spacing texels linearly in angle.
float2 transmittanceLutUv(AtmosphereParams p, float viewHeight, float viewZenithCosAngle) {
    float topRadius = p.atmosphereRadiusMeters;
    float bottomRadius = p.planetRadiusMeters;
    float h = sqrt(max(topRadius * topRadius - bottomRadius * bottomRadius, 0.0));
    float rho = sqrt(max(viewHeight * viewHeight - bottomRadius * bottomRadius, 0.0));

    float discriminant = viewHeight * viewHeight * (viewZenithCosAngle * viewZenithCosAngle - 1.0) + topRadius * topRadius;
    float d = max(0.0, -viewHeight * viewZenithCosAngle + sqrt(max(discriminant, 0.0)));

    float dMin = topRadius - viewHeight;
    float dMax = rho + h;
    float xMu = safeDiv(d - dMin, dMax - dMin);
    float xR = safeDiv(rho, h);
    return float2(saturate(xMu), saturate(xR));
}

void transmittanceLutParams(AtmosphereParams p, float2 uv, out float viewHeight, out float viewZenithCosAngle) {
    float topRadius = p.atmosphereRadiusMeters;
    float bottomRadius = p.planetRadiusMeters;
    float h = sqrt(max(topRadius * topRadius - bottomRadius * bottomRadius, 0.0));
    float rho = h * uv.y;
    viewHeight = sqrt(rho * rho + bottomRadius * bottomRadius);

    float dMin = topRadius - viewHeight;
    float dMax = rho + h;
    float d = dMin + uv.x * (dMax - dMin);
    viewZenithCosAngle = d <= 0.0 ? 1.0 : clamp((h * h - rho * rho - d * d) * safeRcp(2.0 * viewHeight * d), -1.0, 1.0);
}

float3 sampleTransmittanceLut(Texture2D<float4> lut, SamplerState s, AtmosphereParams p,
                              float viewHeight, float viewZenithCosAngle) {
    float2 uv = transmittanceLutUv(p, viewHeight, viewZenithCosAngle);
    return lut.SampleLevel(s, uv, 0.0).rgb;
}

// Extinction-only raymarch (no in-scattering) from a point at `originPlanetSpace` out to `tMax` —
// exactly what the transmittance LUT bake needs, and reused by nothing else (every other consumer
// samples the baked LUT instead of raymarching transmittance itself).
float3 computeTransmittanceToAtmosphereTop(AtmosphereParams p, float3 originPlanetSpace, float3 dir,
                                           float tMax, uint sampleCount) {
    float3 opticalDepth = float3(0.0);
    float dt = tMax * safeRcp(float(max(sampleCount, 1u)));
    [loop]
    for (uint i = 0; i < sampleCount; ++i) {
        float t = (float(i) + 0.5) * dt;
        float3 samplePos = originPlanetSpace + dir * t;
        float h = max(length(samplePos) - p.planetRadiusMeters, 0.0);
        float3 extinction = p.rayleighScattering * rayleighDensity(p, h) +
                            p.mieExtinction * mieDensity(p, h) +
                            p.ozoneAbsorption * ozoneDensity(p, h);
        opticalDepth += extinction * dt;
    }
    return exp(-opticalDepth);
}

// Multi-scattering LUT is parameterized linearly (unlike the transmittance LUT above) — its content
// is already smooth enough that a non-linear remap buys nothing.
float2 multiScatteringLutUv(AtmosphereParams p, float viewHeight, float sunZenithCosAngle) {
    float heightFraction = saturate(safeDiv(viewHeight - p.planetRadiusMeters, p.atmosphereRadiusMeters - p.planetRadiusMeters));
    float sunCosFraction = saturate(sunZenithCosAngle * 0.5 + 0.5);
    return float2(sunCosFraction, heightFraction);
}

float3 sampleMultiScatteringLut(Texture2D<float4> lut, SamplerState s, AtmosphereParams p,
                                float viewHeight, float sunZenithCosAngle) {
    float2 uv = multiScatteringLutUv(p, viewHeight, sunZenithCosAngle);
    return lut.SampleLevel(s, uv, 0.0).rgb;
}

// Shared azimuth basis for the sky-view LUT: `forwardVector` is the sun's direction projected onto
// the local horizontal plane (azimuth zero pointing at the sun). Built identically by both
// skyViewLutUv and skyViewLutDirection so encode/decode are always exact inverses of each other.
void skyViewAzimuthBasis(float3 upVector, float3 sunDir, out float3 forwardVector, out float3 sideVector) {
    float3 sunHorizontal = sunDir - upVector * dot(sunDir, upVector);
    float3 fallbackForward = safeNormalizeOr(cross(upVector, float3(1.0, 0.0, 0.0)), float3(0.0, 0.0, 1.0));
    forwardVector = safeNormalizeOr(sunHorizontal, fallbackForward);
    sideVector = safeNormalizeOr(cross(upVector, forwardVector), float3(1.0, 0.0, 0.0));
}

float skyViewHorizonZenithAngle(AtmosphereParams p, float viewHeight) {
    float safeHeight = max(viewHeight, p.planetRadiusMeters + 1.0);
    float horizonCos = sqrt(max(safeHeight * safeHeight - p.planetRadiusMeters * p.planetRadiusMeters, 0.0)) * safeRcp(safeHeight);
    return SFT_PI - acos(clamp(horizonCos, -1.0, 1.0));
}

// Hillaire section 5.3's sky-view LUT parameterization: v biases resolution toward the horizon (the
// most detailed part of the sky) and splits above/below it; u stores azimuth relative to the sun,
// mirrored across the sun's vertical plane (clear-sky scattering is bilaterally symmetric about it,
// so only 0..180 degrees of relative azimuth need to be stored).
float2 skyViewLutUv(AtmosphereParams p, float3 rayDir, float3 upVector, float3 sunDir,
                    float viewHeight, bool rayIntersectsGround, uint2 lutSize) {
    float horizonZenithAngle = skyViewHorizonZenithAngle(p, viewHeight);
    float viewZenithAngle = acos(clamp(dot(rayDir, upVector), -1.0, 1.0));

    float vCoord;
    if (!rayIntersectsGround) {
        float coord = 1.0 - sqrt(saturate(1.0 - safeDiv(viewZenithAngle, horizonZenithAngle)));
        vCoord = coord * 0.5;
    } else {
        float coord = sqrt(saturate(safeDiv(viewZenithAngle - horizonZenithAngle, SFT_PI - horizonZenithAngle)));
        vCoord = coord * 0.5 + 0.5;
    }

    float3 forwardVector, sideVector;
    skyViewAzimuthBasis(upVector, sunDir, forwardVector, sideVector);
    float3 rayHorizontal = safeNormalizeOr(rayDir - upVector * dot(rayDir, upVector), forwardVector);
    float cosRelativeAzimuth = dot(rayHorizontal, forwardVector);
    float uCoord = sqrt(saturate(cosRelativeAzimuth * 0.5 + 0.5));

    return float2(fromUnitToSubUvs(uCoord, float(lutSize.x)), fromUnitToSubUvs(vCoord, float(lutSize.y)));
}

// Inverse of skyViewLutUv — texel uv (already texel-centered, i.e. straight from
// dispatchThreadId/lutSize) to a world-planet-space ray direction. Used only by the sky-view LUT
// bake shader.
float3 skyViewLutDirection(AtmosphereParams p, float2 uv, float3 upVector, float3 sunDir,
                           float viewHeight, uint2 lutSize, out bool rayIntersectsGround) {
    float u = fromSubUvsToUnit(uv.x, float(lutSize.x));
    float v = fromSubUvsToUnit(uv.y, float(lutSize.y));
    float horizonZenithAngle = skyViewHorizonZenithAngle(p, viewHeight);

    float viewZenithAngle;
    if (v < 0.5) {
        float coord = 1.0 - 2.0 * v;
        coord = 1.0 - coord * coord;
        viewZenithAngle = coord * horizonZenithAngle;
        rayIntersectsGround = false;
    } else {
        float coord = 2.0 * v - 1.0;
        coord = coord * coord;
        viewZenithAngle = horizonZenithAngle + coord * (SFT_PI - horizonZenithAngle);
        rayIntersectsGround = true;
    }

    float uCoordSquared = u * u;
    float cosRelativeAzimuth = uCoordSquared * 2.0 - 1.0;
    float sinRelativeAzimuth = sqrt(saturate(1.0 - cosRelativeAzimuth * cosRelativeAzimuth));

    float3 forwardVector, sideVector;
    skyViewAzimuthBasis(upVector, sunDir, forwardVector, sideVector);
    float viewZenithCosAngle = cos(viewZenithAngle);
    float viewZenithSinAngle = sqrt(saturate(1.0 - viewZenithCosAngle * viewZenithCosAngle));
    float3 horizontalDir = forwardVector * cosRelativeAzimuth + sideVector * sinRelativeAzimuth;
    return safeNormalize(upVector * viewZenithCosAngle + horizontalDir * viewZenithSinAngle);
}

// -------------------------------------------------------------------------------------------------
// Scattering integration
// -------------------------------------------------------------------------------------------------

struct ScatteringResult {
    float3 luminance;
    float3 transmittance;
};

// Raymarches single scattering (phase-weighted) plus, optionally, the precomputed multi-scattering
// LUT's isotropic contribution, from `originPlanetSpace` along `dir` out to `tMax` — the caller is
// responsible for clipping tMax to whatever is nearer of the atmosphere top, the ground, or (for the
// rasterized aerial-perspective case) opaque scene geometry. Pure function of its explicit
// arguments: this is both what bakes the sky-view LUT and what a future raytracing miss shader would
// call directly with a traced ray, with zero rasterization-specific state anywhere in it.
ScatteringResult integrateScatteredLuminance(
    AtmosphereParams p, float3 originPlanetSpace, float3 dir, float3 sunDir, float3 sunIlluminance,
    float tMax, uint sampleCount,
    Texture2D<float4> transmittanceLut, SamplerState transmittanceSampler,
    Texture2D<float4> multiScatteringLut, SamplerState multiScatteringSampler,
    bool useMultiScattering, bool includeGroundBounce) {
    float3 luminance = float3(0.0);
    float3 opticalDepth = float3(0.0);
    float dt = max(tMax, 0.0) * safeRcp(float(max(sampleCount, 1u)));
    float cosViewSun = dot(dir, sunDir);
    float rayleighPhaseValue = rayleighPhase(cosViewSun);
    float miePhaseValue = cornetteShanksMiePhase(p.miePhaseG, cosViewSun);

    [loop]
    for (uint i = 0; i < sampleCount; ++i) {
        float t = (float(i) + 0.5) * dt;
        float3 samplePos = originPlanetSpace + dir * t;
        float sampleHeight = length(samplePos);
        float h = max(sampleHeight - p.planetRadiusMeters, 0.0);

        float rayleighD = rayleighDensity(p, h);
        float mieD = mieDensity(p, h);
        float ozoneD = ozoneDensity(p, h);
        float3 sampleExtinction = p.rayleighScattering * rayleighD + p.mieExtinction * mieD + p.ozoneAbsorption * ozoneD;

        // Transmittance from the ray origin to this sample's midpoint = exp(-accumulated optical
        // depth including half of this sample's own contribution) — a standard midpoint-rule
        // correction so a single-sample raymarch isn't systematically biased dark.
        float3 transmittanceToSample = exp(-(opticalDepth + sampleExtinction * (dt * 0.5)));

        float3 upVector = samplePos * safeRcp(max(sampleHeight, SFT_EPSILON));
        float sunZenithCos = dot(upVector, sunDir);
        float3 transmittanceToSun = sampleTransmittanceLut(transmittanceLut, transmittanceSampler, p, sampleHeight, sunZenithCos);

        float tGroundToSun;
        bool sampleInPlanetShadow = distanceToGround(p, samplePos, sunDir, tGroundToSun);
        float3 sunVisibility = sampleInPlanetShadow ? float3(0.0) : float3(1.0);

        float3 singleScattering =
            (p.rayleighScattering * rayleighD * rayleighPhaseValue + p.mieScattering * mieD * miePhaseValue) *
            transmittanceToSun * sunVisibility;

        float3 multiScattering = float3(0.0);
        if (useMultiScattering) {
            float3 ms = sampleMultiScatteringLut(multiScatteringLut, multiScatteringSampler, p, sampleHeight, sunZenithCos);
            multiScattering = ms * (p.rayleighScattering * rayleighD + p.mieScattering * mieD);
        }

        luminance += transmittanceToSample * (singleScattering + multiScattering) * sunIlluminance * dt;
        opticalDepth += sampleExtinction * dt;
    }

    // Ground bounce: only for rays with no real rendered geometry to hit (the sky-view LUT's
    // below-horizon/background case) — `includeGroundBounce` must be false for the rasterized
    // aerial-perspective call site, whose tMax is clipped to actual on-screen geometry. That
    // geometry is already fully lit by the main BRDF pass; the mathematical "ground" plane sits at
    // very nearly the same height as a real floor/table for any downward-looking ray, so leaving
    // this enabled there was double-adding a flat grey ground-albedo term on top of already-lit
    // surfaces almost everywhere — the reported "fog" this fixes.
    float tGround;
    if (includeGroundBounce && distanceToGround(p, originPlanetSpace, dir, tGround) && tGround <= tMax + 1.0) {
        float3 groundPos = originPlanetSpace + dir * tGround;
        float3 groundUp = safeNormalizeOr(groundPos, float3(0.0, 1.0, 0.0));
        float sunZenithCosGround = dot(groundUp, sunDir);
        if (sunZenithCosGround > 0.0) {
            float3 transmittanceToGround = exp(-opticalDepth);
            float3 transmittanceGroundToSun =
                sampleTransmittanceLut(transmittanceLut, transmittanceSampler, p, p.planetRadiusMeters, sunZenithCosGround);
            luminance += transmittanceToGround * p.groundAlbedo * SFT_INV_PI * sunZenithCosGround *
                        transmittanceGroundToSun * sunIlluminance;
        }
    }

    ScatteringResult result;
    result.luminance = luminance;
    result.transmittance = exp(-opticalDepth);
    return result;
}

// -------------------------------------------------------------------------------------------------
// Multi-scattering LUT bake support (Hillaire section 5.2)
// -------------------------------------------------------------------------------------------------

struct MultiScatterSample {
    float3 luminance;          // second-order (isotropic, no phase function) in-scattered radiance
    float3 multiScatterFactor; // transmittance-weighted scattering-coefficient integral along the ray
};

// Single-direction raymarch feeding computeMultipleScattering below. Deliberately omits the phase
// function (Hillaire's multi-scatter approximation treats in-scattered light as isotropic, which is
// what makes the geometric-series closed form in computeMultipleScattering valid) and is not
// normalized by sun illuminance — the multi-scattering LUT stores a per-unit-illuminance response;
// integrateScatteredLuminance above multiplies by the real sunIlluminance when sampling it.
MultiScatterSample integrateSingleScatteringForMultiScatter(
    AtmosphereParams p, float3 originPlanetSpace, float3 dir, float3 sunDir,
    float tMax, uint sampleCount,
    Texture2D<float4> transmittanceLut, SamplerState transmittanceSampler) {
    float3 luminance = float3(0.0);
    float3 multiScatterFactor = float3(0.0);
    float3 opticalDepth = float3(0.0);
    float dt = max(tMax, 0.0) * safeRcp(float(max(sampleCount, 1u)));

    [loop]
    for (uint i = 0; i < sampleCount; ++i) {
        float t = (float(i) + 0.5) * dt;
        float3 samplePos = originPlanetSpace + dir * t;
        float sampleHeight = length(samplePos);
        float h = max(sampleHeight - p.planetRadiusMeters, 0.0);

        float rayleighD = rayleighDensity(p, h);
        float mieD = mieDensity(p, h);
        float ozoneD = ozoneDensity(p, h);
        float3 sampleExtinction = p.rayleighScattering * rayleighD + p.mieExtinction * mieD + p.ozoneAbsorption * ozoneD;
        float3 sampleScattering = p.rayleighScattering * rayleighD + p.mieScattering * mieD;

        float3 transmittanceToSample = exp(-(opticalDepth + sampleExtinction * (dt * 0.5)));

        float3 upVector = samplePos * safeRcp(max(sampleHeight, SFT_EPSILON));
        float sunZenithCos = dot(upVector, sunDir);
        float3 transmittanceToSun = sampleTransmittanceLut(transmittanceLut, transmittanceSampler, p, sampleHeight, sunZenithCos);
        float tGroundToSun;
        bool sampleInPlanetShadow = distanceToGround(p, samplePos, sunDir, tGroundToSun);
        float3 sunVisibility = sampleInPlanetShadow ? float3(0.0) : float3(1.0);

        luminance += transmittanceToSample * sampleScattering * transmittanceToSun * sunVisibility * dt;
        multiScatterFactor += transmittanceToSample * sampleScattering * dt;
        opticalDepth += sampleExtinction * dt;
    }

    float tGround;
    if (distanceToGround(p, originPlanetSpace, dir, tGround) && tGround <= tMax + 1.0) {
        float3 groundPos = originPlanetSpace + dir * tGround;
        float3 groundUp = safeNormalizeOr(groundPos, float3(0.0, 1.0, 0.0));
        float sunZenithCosGround = dot(groundUp, sunDir);
        if (sunZenithCosGround > 0.0) {
            float3 transmittanceToGround = exp(-opticalDepth);
            float3 transmittanceGroundToSun =
                sampleTransmittanceLut(transmittanceLut, transmittanceSampler, p, p.planetRadiusMeters, sunZenithCosGround);
            luminance += transmittanceToGround * p.groundAlbedo * SFT_INV_PI * sunZenithCosGround * transmittanceGroundToSun;
        }
    }

    MultiScatterSample result;
    result.luminance = luminance;
    result.multiScatterFactor = multiScatterFactor;
    return result;
}

// Spherically integrates integrateSingleScatteringForMultiScatter over `sqrtSampleCount^2` fixed,
// deterministic directions (a simple stratified equal-area sphere mapping — no RNG/blue-noise
// dependency), then closes the infinite-bounce geometric series `L + L*f + L*f^2 + ... = L / (1 - f)`
// to get the multiply-scattered radiance response for one (height, sun zenith angle) LUT texel.
float3 computeMultipleScattering(AtmosphereParams p, float3 originPlanetSpace, float3 sunDir,
                                 uint sqrtSampleCount,
                                 Texture2D<float4> transmittanceLut, SamplerState transmittanceSampler) {
    float3 luminanceSum = float3(0.0);
    float3 factorSum = float3(0.0);
    uint totalDirections = sqrtSampleCount * sqrtSampleCount;
    float invSqrtSampleCount = safeRcp(float(sqrtSampleCount));

    [loop]
    for (uint i = 0; i < totalDirections; ++i) {
        float u = (float(i % sqrtSampleCount) + 0.5) * invSqrtSampleCount;
        float v = (float(i / sqrtSampleCount) + 0.5) * invSqrtSampleCount;
        float theta = 2.0 * SFT_PI * u;
        float phi = acos(1.0 - 2.0 * v);
        float sinPhi = sin(phi);
        float3 dir = float3(sinPhi * cos(theta), cos(phi), sinPhi * sin(theta));

        float tMax;
        float tGround;
        bool hitsGround = distanceToGround(p, originPlanetSpace, dir, tGround);
        if (!distanceToAtmosphereTop(p, originPlanetSpace, dir, tMax)) {
            continue;
        }
        if (hitsGround) {
            tMax = min(tMax, tGround);
        }

        MultiScatterSample sampleResult = integrateSingleScatteringForMultiScatter(
            p, originPlanetSpace, dir, sunDir, tMax, 32u, transmittanceLut, transmittanceSampler);
        luminanceSum += sampleResult.luminance;
        factorSum += sampleResult.multiScatterFactor;
    }

    float sphereSolidAngle = 4.0 * SFT_PI * safeRcp(float(totalDirections));
    float3 luminanceAvg = luminanceSum * sphereSolidAngle * SFT_INV_PI * 0.25;
    float3 factorAvg = factorSum * sphereSolidAngle * SFT_INV_PI * 0.25;
    float3 multiScatterDenominator = max(float3(1.0) - factorAvg, float3(0.001));
    return luminanceAvg / multiScatterDenominator;
}

// -------------------------------------------------------------------------------------------------
// Sun disk
// -------------------------------------------------------------------------------------------------

// Flat angular disk (uniform radiance across the sun's solid angle) attenuated by transmittance to
// space along the view ray — visually indistinguishable from a limb-darkened profile at this LUT's
// resolution and viewing scale, so kept simple. `sunIlluminance` is the same quantity
// evaluateDirect() in deferred_shadow_lighting.slang already treats as the directional light's
// radiance (lightingData.sun.radianceShadow.rgb) — converting it to a disk radiance here by dividing
// by the disk's solid angle is what makes a physically sized sun read at the correct brightness.
float3 sunDiskLuminance(float3 dir, float3 sunDir, float sunAngularRadiusRadians,
                        float3 sunIlluminance, float3 transmittanceToSun) {
    float cosHalfAngular = cos(sunAngularRadiusRadians);
    float cosAngle = dot(dir, sunDir);
    if (cosAngle < cosHalfAngular) {
        return float3(0.0);
    }
    float solidAngle = 2.0 * SFT_PI * (1.0 - cosHalfAngular);
    float3 sunRadiance = sunIlluminance * safeRcp(max(solidAngle, SFT_EPSILON));
    return sunRadiance * transmittanceToSun;
}
