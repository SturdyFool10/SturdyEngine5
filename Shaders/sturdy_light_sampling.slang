// Shared stochastic multi-light next-event-estimation (NEE) helper: picks one light — the sun or one
// of up to `kRestirGiMaxLights` colored point/spot lights — per call via uniform selection, so a single
// traced shadow ray per shading event gives an unbiased estimate of the summed direct-lighting
// contribution from every candidate light. This is the piece the RT path was missing:
// `spectral_integrators.slang` only ever samples the sun via NEE, so nothing ray-traced (surfel GI, and
// previously nothing else) could produce colored bounce light from point/spot lights.
//
// Also provides the pdf/MIS building blocks `restir_gi_initial_sample.slang` combines with this
// selector: the sun has real (if tiny) angular extent, so light-sampling and BSDF (cosine-hemisphere)
// sampling are two genuine competing unbiased strategies for it, combinable via the balance heuristic.
// Point/spot lights are true delta lights — a scattered ray can never "hit" a zero-measure point, so no
// competing BSDF strategy exists for them and NEE alone (with no MIS weighting) is already the correct,
// standard treatment, exactly as in offline/path-traced renderers.
//
// Dependency-free (pure functions of explicit arguments only, no resource bindings of its own) —
// mirrors sturdy_atmosphere.slang's shape so it can be imported from any RT shader without pulling in
// unrelated bind-group state.

import sturdy_common;
import sturdy_spectral_common;

static const uint kRestirGiMaxLights = 8;

// Field-for-field mirror of Renderer::RestirGiPackedLight (RestirGi.hpp): xyz = world position,
// w = range; xyz = radiance, w = source radius.
struct SturdyPackedLight {
    float4 positionRange;
    float4 radianceSourceRadius;
};

// Smooth windowed inverse-square falloff — identical shape to deferred_shadow_lighting.slang's
// `punctualAttenuation`, kept in sync so RT and rasterized direct lighting agree on how a light fades
// out near its configured range.
float punctualLightAttenuation(float distanceToLight, float range) {
    float ratio = distanceToLight / max(range, SFT_EPSILON);
    float window = saturate(1.0 - ratio * ratio * ratio * ratio);
    return (window * window) / max(distanceToLight * distanceToLight, 0.01);
}

// Solid angle subtended by the sun's disk, and the corresponding uniform-over-the-disk direction pdf
// (1/solidAngle) — the light-sampling strategy's pdf for a direction toward the sun.
float sunSolidAngle(float sunAngularRadius) {
    float cosThetaMax = cos(clamp(sunAngularRadius, 0.0, SFT_PI * 0.5));
    return 2.0 * SFT_PI * max(1.0 - cosThetaMax, 0.0);
}

float sunDirectionPdf(float sunAngularRadius) {
    float solidAngle = sunSolidAngle(sunAngularRadius);
    return solidAngle > SFT_EPSILON ? 1.0 / solidAngle : 0.0;
}

// Solid-angle pdf of sampling `direction` via cosine-weighted hemisphere sampling around `normal` —
// the BSDF-sampling strategy's pdf, shared by both the primary indirect bounce direction and the
// reciprocal BSDF-sampled sun-MIS ray in restir_gi_initial_sample.slang (both use `cosineHemisphere`).
float diffuseCosinePdf(float3 normal, float3 direction) {
    return max(dot(normal, direction), 0.0) * SFT_INV_PI;
}

// Standard balance-heuristic MIS weight for a sample drawn from the strategy with pdf `pdfSampled`,
// given the other competing strategy's pdf `pdfOther` for that same direction.
float balanceHeuristicWeight(float pdfSampled, float pdfOther) {
    float denom = pdfSampled + pdfOther;
    return denom > SFT_EPSILON ? pdfSampled / denom : 0.0;
}

struct LightSampleResult {
    float3 direction; // shading position -> light
    float3 radiance;  // raw light radiance, NOT pre-divided by pdf
    // Full solid-angle pdf of drawing this direction under the light-sampling strategy (selection
    // probability folded in). For point/spot lights this is only ever used to unbias the NEE estimate
    // (radiance / pdf) — never combined via MIS, since no competing BSDF strategy exists for a delta
    // light (see file header).
    float pdf;
    float distance;
    float sourceRadius;
    bool isSun;
};

// Uniformly selects one candidate among {sun, lightCount point/spot lights}. Dividing the returned
// `radiance` by `pdf` (and multiplying by BSDF * cosine * visibility) gives an unbiased single-sample
// NEE estimate of the full light set's contribution; for the sun specifically, `balanceHeuristicWeight`
// against `diffuseCosinePdf` should also be applied (see restir_gi_initial_sample.slang) since a BSDF-
// sampled ray can legitimately also land on the sun's disk.
LightSampleResult sampleOneLightUniform(
    float3 shadingPosition,
    float3 sunDirectionToward,
    float3 sunRadiance,
    float sunAngularRadius,
    SturdyPackedLight lights[kRestirGiMaxLights],
    uint lightCount,
    inout uint rng)
{
    uint clampedCount = min(lightCount, kRestirGiMaxLights);
    uint candidateCount = 1u + clampedCount;
    float selectionProbability = 1.0 / (float)candidateCount;
    uint selection = min((uint)(spectralRandom(rng) * (float)candidateCount), candidateCount - 1u);

    LightSampleResult result;
    if (selection == 0u) {
        result.direction = sunDirectionToward;
        result.radiance = sunRadiance;
        result.pdf = selectionProbability * sunDirectionPdf(sunAngularRadius);
        result.distance = 1.0e6;
        result.sourceRadius = tan(max(sunAngularRadius, 0.0)) * 1.0e6;
        result.isSun = true;
        return result;
    }

    SturdyPackedLight light = lights[min(selection - 1u, kRestirGiMaxLights - 1u)];
    float3 toLight = light.positionRange.xyz - shadingPosition;
    float dist = length(toLight);
    result.direction = dist > SFT_EPSILON ? toLight / dist : float3(0.0, 1.0, 0.0);
    result.distance = dist;
    result.sourceRadius = light.radianceSourceRadius.w;
    result.radiance = light.radianceSourceRadius.rgb * punctualLightAttenuation(dist, light.positionRange.w);
    result.pdf = selectionProbability;
    result.isSun = false;
    return result;
}
