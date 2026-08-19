import sturdy_common;

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
