// Shared ray-query transport. Hybrid effects, photon emission, and full path tracing call these
// exact routines.

import sturdy_common;
import sturdy_spectral_common;

struct SpectralRayHit {
    bool hit;
    float distance;
    uint instanceId;
    uint primitiveId;
    float2 barycentrics;
    bool frontFace;
};

struct DielectricScatter {
    float3 direction;
    float etaRatio;
    bool transmitted;
    bool entering;
};

struct OpaquePbrScatter {
    float3 direction;
    float throughputWeight;
    bool specular;
};

SpectralRayHit traceClosestHit(
    RaytracingAccelerationStructure scene,
    float3 origin,
    float3 direction,
    float tMin,
    float tMax,
    uint instanceMask)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = tMin;
    ray.TMax = tMax;

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, instanceMask, ray);
    while (query.Proceed()) {}

    SpectralRayHit result;
    result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    result.distance = result.hit ? query.CommittedRayT() : tMax;
    result.instanceId = result.hit ? query.CommittedInstanceID() : 0u;
    result.primitiveId = result.hit ? query.CommittedPrimitiveIndex() : 0u;
    result.barycentrics = result.hit ? query.CommittedTriangleBarycentrics() : float2(0.0);
    result.frontFace = result.hit ? query.CommittedTriangleFrontFace() : true;
    return result;
}

float traceVisibility(
    RaytracingAccelerationStructure scene,
    float3 origin,
    float3 direction,
    float maxDistance,
    uint instanceMask)
{
    SpectralRayHit hit = traceClosestHit(scene, origin, direction, 0.001, maxDistance, instanceMask);
    return hit.hit ? 0.0 : 1.0;
}

float traceAmbientOcclusion(
    RaytracingAccelerationStructure scene,
    float3 position,
    float3 normal,
    float radius,
    uint rayCount,
    uint instanceMask,
    inout uint rng)
{
    float visible = 0.0;
    uint samples = max(rayCount, 1u);
    for (uint sampleIndex = 0; sampleIndex < samples; ++sampleIndex) {
        float3 direction = cosineHemisphere(normal, rng);
        visible += traceVisibility(scene, position + normal * 0.001, direction, radius, instanceMask);
    }
    return visible / float(samples);
}

float3 reflectedDirection(float3 incident, float3 normal, float roughness, inout uint rng) {
    float3 ideal = reflect(incident, normal);
    float3 diffuseLobe = cosineHemisphere(ideal, rng);
    return normalize(lerp(ideal, diffuseLobe, saturate(roughness * roughness)));
}

float schlickFresnel(float cosineTheta, float f0) {
    float oneMinusCosine = 1.0 - saturate(cosineTheta);
    return saturate(f0 + (1.0 - f0) * oneMinusCosine * oneMinusCosine
                  * oneMinusCosine * oneMinusCosine * oneMinusCosine);
}

float evaluateOpaquePbr(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float baseReflectance,
    float perceptualRoughness,
    float metallic,
    float dielectricF0)
{
    float nDotL = saturate(dot(normal, lightDirection));
    float nDotV = saturate(dot(normal, viewDirection));
    if (nDotL <= 0.0 || nDotV <= 0.0) return 0.0;

    float3 halfVector = normalize(viewDirection + lightDirection);
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));
    float alpha = max(perceptualRoughness * perceptualRoughness, 0.002);
    float alphaSquared = alpha * alpha;
    float denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
    float distribution = alphaSquared / max(SFT_PI * denominator * denominator, 1.0e-6);
    float k = square(alpha + 1.0) * 0.125;
    float geometryV = nDotV / max(nDotV * (1.0 - k) + k, 1.0e-6);
    float geometryL = nDotL / max(nDotL * (1.0 - k) + k, 1.0e-6);
    float f0 = lerp(saturate(dielectricF0), saturate(baseReflectance), saturate(metallic));
    float fresnel = schlickFresnel(vDotH, f0);
    float specular = distribution * geometryV * geometryL * fresnel
                   / max(4.0 * nDotV * nDotL, 1.0e-6);
    float diffuse = (1.0 - fresnel) * (1.0 - saturate(metallic))
                  * max(baseReflectance, 0.0) * SFT_INV_PI;
    return max(diffuse + specular, 0.0) * nDotL;
}

// Companion to sampleOpaquePbrDirection: the density that function's mixture (specular-lobe vs.
// cosine-weighted diffuse-lobe, chosen with probability specularProbability) would assign to sampling
// `lightDirection` specifically, rather than the direction it actually drew. Needed to MIS-weight
// directSun's NEE contribution against a future bounce's BSDF-sampled ray landing on the same light
// (see traceSpectralPath in spectral_integrators.slang) — this is the standard mixture-PDF formula
// p(w) = sum_i weight_i * p_i(w), NOT a resampling of sampleOpaquePbrDirection itself.
// specularPdf uses the standard GGX half-vector-sampling density (D * nDotH / (4 * vDotH)); this is an
// approximation of the actual specular lobe's density since sampleOpaquePbrDirection's specular branch
// samples via an ad-hoc lerp toward a cosine lobe (reflectedDirection) rather than true GGX importance
// sampling — close enough for a stable MIS weight, since a small PDF mismatch only shifts weight
// slightly between the two strategies rather than reintroducing bias in the un-weighted sum either
// term is drawn from.
float opaquePbrPdf(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float baseReflectance,
    float perceptualRoughness,
    float metallic,
    float dielectricF0)
{
    float nDotL = saturate(dot(normal, lightDirection));
    float nDotV = saturate(dot(normal, viewDirection));
    if (nDotL <= 0.0 || nDotV <= 0.0) return 0.0;

    float3 halfVector = normalize(viewDirection + lightDirection);
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));
    float alpha = max(perceptualRoughness * perceptualRoughness, 0.002);
    float alphaSquared = alpha * alpha;
    float denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
    float distribution = alphaSquared / max(SFT_PI * denominator * denominator, 1.0e-6);
    float specularPdf = distribution * nDotH / max(4.0 * vDotH, 1.0e-6);
    float diffusePdf = nDotL * SFT_INV_PI;

    float f0 = lerp(saturate(dielectricF0), saturate(baseReflectance), saturate(metallic));
    float specularProbability = clamp(f0, 0.05, 0.95);
    return lerp(diffusePdf, specularPdf, specularProbability);
}

OpaquePbrScatter sampleOpaquePbrDirection(
    float3 incident,
    float3 normal,
    float baseReflectance,
    float perceptualRoughness,
    float metallic,
    float dielectricF0,
    inout uint rng)
{
    OpaquePbrScatter scatter;
    float f0 = lerp(saturate(dielectricF0), saturate(baseReflectance), saturate(metallic));
    float specularProbability = clamp(f0, 0.05, 0.95);
    scatter.specular = spectralRandom(rng) < specularProbability;
    if (scatter.specular) {
        scatter.direction = reflectedDirection(incident, normal, perceptualRoughness, rng);
        if (dot(scatter.direction, normal) <= 0.0) {
            scatter.direction = normalize(reflect(incident, normal));
        }
        scatter.throughputWeight = f0 / specularProbability;
    } else {
        scatter.direction = cosineHemisphere(normal, rng);
        scatter.throughputWeight = max(baseReflectance, 0.0) * (1.0 - saturate(metallic))
                                / max(1.0 - specularProbability, 1.0e-4);
    }
    return scatter;
}

DielectricScatter sampleDielectricDirection(
    float3 incident,
    float3 outwardGeometricNormal,
    float3 outwardShadingNormal,
    bool frontFace,
    float etaOutside,
    float etaInside,
    inout uint rng)
{
    DielectricScatter result;
    result.entering = frontFace;
    // Geometry determines which medium the ray is in. The smooth interpolated normal determines the
    // optical interface direction; using the triangle normal here turns every triangle into a flat
    // prism and makes smooth glass visibly blocky.
    float3 geometricBoundaryNormal = frontFace ? outwardGeometricNormal : -outwardGeometricNormal;
    float3 boundaryNormal = frontFace ? outwardShadingNormal : -outwardShadingNormal;
    // Only distrust the shading normal on genuine hemisphere disagreement with the true geometric
    // surface (normal-mapped materials pushed past a right angle from their base normal — rare). A
    // ray merely grazing the *shading* normal is completely normal at a curved silhouette/rim — most
    // of a sphere's visible refraction-bent rim band is grazing relative to it — and blanket-replacing
    // the boundary normal there was exactly what made refined glass spheres show a faceted scale
    // pattern around the rim despite plenty of tessellation: it silently swapped in the flat
    // per-triangle normal for the whole fresnel/refraction calculation, not just the rare pathological
    // ray. The refract/reflect branches below already re-check their own output direction against
    // geometricBoundaryNormal and fall back per-ray, which is the correct place for that safety net.
    if (dot(boundaryNormal, geometricBoundaryNormal) <= 0.0) {
        boundaryNormal = geometricBoundaryNormal;
    }

    float etaIncident = frontFace ? etaOutside : etaInside;
    float etaTransmitted = frontFace ? etaInside : etaOutside;
    result.etaRatio = etaIncident / max(etaTransmitted, 1.0e-4);

    float fresnel = dielectricFresnel(dot(-incident, boundaryNormal), etaIncident, etaTransmitted);
    float3 refracted = refract(incident, boundaryNormal, result.etaRatio);
    // A strongly perturbed shading normal can produce a direction on the wrong geometric side. Fall
    // back to the true boundary in that rare case rather than leaking a path into the wrong medium.
    if (dot(refracted, refracted) >= 1.0e-8 && dot(refracted, geometricBoundaryNormal) >= 0.0) {
        refracted = refract(incident, geometricBoundaryNormal, result.etaRatio);
    }
    bool totalInternalReflection = dot(refracted, refracted) < 1.0e-8;
    bool reflectEvent = totalInternalReflection || spectralRandom(rng) < fresnel;
    if (reflectEvent) {
        float3 reflected = normalize(reflect(incident, boundaryNormal));
        if (dot(reflected, geometricBoundaryNormal) <= 0.0) {
            reflected = normalize(reflect(incident, geometricBoundaryNormal));
        }
        result.direction = reflected;
        result.transmitted = false;
    } else {
        result.direction = normalize(refracted);
        result.transmitted = true;
    }
    return result;
}

float beerLambert(float absorptionCoefficient, float distance) {
    return exp(-max(absorptionCoefficient, 0.0) * max(distance, 0.0));
}
