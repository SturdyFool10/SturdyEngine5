import sturdy_common;

// Floors alpha-roughness above zero everywhere a GGX NDF/Smith term is evaluated (raster and path
// tracer alike), avoiding a singular mirror NDF at roughness=0.
static const float SFT_MIN_PBR_ROUGHNESS = 0.045;

static const float SFT_EON_FON_C1 = 0.5 - 2.0 / (3.0 * SFT_PI);
static const float SFT_EON_FON_C2 = 2.0 / 3.0 - 28.0 / (15.0 * SFT_PI);

float eonFujiiDirectionalAlbedoApprox(float cosineTheta, float roughness) {
    float mu = saturate(cosineTheta);
    float r = saturate(roughness);
    float x = 1.0 - mu;
    float gOverPi = x * (0.0571085289
                       + x * (0.491881867
                       + x * (-0.332181442
                       + x * 0.0714429953)));
    return (1.0 + r * gOverPi) / (1.0 + SFT_EON_FON_C1 * r);
}

float3 evaluateEonDiffuse(
    float3 diffuseAlbedo,
    float roughness,
    float3 normal,
    float3 lightDirection,
    float3 viewDirection)
{
    float r = saturate(roughness);
    float muI = saturate(dot(normal, lightDirection));
    float muO = saturate(dot(normal, viewDirection));
    if (muI <= 0.0 || muO <= 0.0) return float3(0.0);

    float3 color = saturate(diffuseAlbedo);
    if (r <= 1.0e-5) return color * SFT_INV_PI;

    float a = 1.0 / (1.0 + SFT_EON_FON_C1 * r);
    float averageAlbedo = a * (1.0 + SFT_EON_FON_C2 * r);
    float3 rho = color / max(
        float3(SFT_EPSILON), color + averageAlbedo * (1.0 - color));

    float tangentCorrelation = dot(lightDirection, viewDirection) - muI * muO;
    float correlationTerm = tangentCorrelation > 0.0
        ? tangentCorrelation / max(max(muI, muO), SFT_EPSILON)
        : tangentCorrelation;

    float3 singleScatter = rho * SFT_INV_PI * a * (1.0 + r * correlationTerm);

    float incomingAlbedo = eonFujiiDirectionalAlbedoApprox(muI, r);
    float outgoingAlbedo = eonFujiiDirectionalAlbedoApprox(muO, r);

    float3 multipleScatterAlbedo = rho * rho * averageAlbedo
        / max(float3(SFT_EPSILON), 1.0 - rho * (1.0 - averageAlbedo));
    float missingIncoming = max(SFT_EPSILON, 1.0 - incomingAlbedo);
    float missingOutgoing = max(SFT_EPSILON, 1.0 - outgoingAlbedo);
    float missingAverage = max(SFT_EPSILON, 1.0 - averageAlbedo);
    float3 multipleScatter = multipleScatterAlbedo * SFT_INV_PI
        * missingIncoming * missingOutgoing / missingAverage;

    return max(singleScatter + multipleScatter, float3(0.0));
}

float evaluateEonDiffuse(
    float diffuseAlbedo,
    float roughness,
    float3 normal,
    float3 lightDirection,
    float3 viewDirection)
{
    return evaluateEonDiffuse(
        float3(diffuseAlbedo), roughness, normal, lightDirection, viewDirection).x;
}

// Shared GGX/Smith specular terms. Used by both the raster deferred lighting pass and the
// spectral/path-traced integrator so the same material shades identically in both.
float distributionGGXLocal(float nDotH, float alphaRoughness) {
    float a2 = alphaRoughness * alphaRoughness;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 * SFT_INV_PI / max(d * d, SFT_EPSILON);
}

// Height-correlated Smith visibility (G already divided by 4*NdotL*NdotV — callers must not divide
// again).
float geometrySmithCorrelated(float nDotV, float nDotL, float alphaRoughness) {
    float a2 = alphaRoughness * alphaRoughness;
    float gv = nDotL * sqrt(max(nDotV * nDotV * (1.0 - a2) + a2, SFT_EPSILON));
    float gl = nDotV * sqrt(max(nDotL * nDotL * (1.0 - a2) + a2, SFT_EPSILON));
    return 0.5 / max(gv + gl, SFT_EPSILON);
}

// Karis, "Real Shading in Unreal Engine 4" (2013) sec 3.4: widen the GGX roughness so the specular
// lobe integrates to roughly the same energy as an infinitesimal light convolved with a sphere of
// the given angular size, producing a physically-sized rather than pinpoint highlight.
// `sphereAngleTangent` is tan(light angular half-radius) for the sun, or source_radius/distance for
// point/spot lights; 0 leaves the lobe unwidened.
float sphereNormalizedAlpha(float alpha, float sphereAngleTangent) {
    float sphereAngle = saturate(sphereAngleTangent);
    return saturate(alpha + 0.5 * sphereAngle);
}

// UE4-course energy renormalization for the alpha widening above, so rough surfaces under large
// area lights don't over-brighten just because the lobe was widened.
float sphereNormalizedAlphaEnergyScale(float alpha, float widenedAlpha) {
    float normalizationFactor = alpha / clamp(widenedAlpha, alpha, 1.0);
    return normalizationFactor * normalizationFactor;
}

// Moves the light direction toward the mirror-reflection ray by an amount bounded by the light's
// angular size, so the highlight is correctly positioned on physically sized lights rather than
// merely blurred in place. Operates on unit directions (light treated as a sphere of angular radius
// `sphereAngleTangent` at unit distance along `lightVector`).
float3 representativePointDirection(float3 lightVector, float3 reflectionVector, float sphereAngleTangent) {
    float3 centerToRay = dot(lightVector, reflectionVector) * reflectionVector - lightVector;
    float centerToRayLenSq = max(dot(centerToRay, centerToRay), SFT_EPSILON);
    float3 closestPoint = lightVector + centerToRay * saturate(sphereAngleTangent * rsqrt(centerToRayLenSq));
    return normalize(closestPoint);
}
