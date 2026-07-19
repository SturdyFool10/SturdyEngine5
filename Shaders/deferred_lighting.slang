// Fullscreen deferred lighting pass.
// Samples the G-buffer and writes HDR scene lighting for tonemapping.

import sturdy_common;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

Texture2D<float4> gbufferAlbedo;
Texture2D<float2> gbufferNormal;
Texture2D<float4> gbufferMaterial;
SamplerState gbufferSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 albedo = gbufferAlbedo.Sample(gbufferSampler, input.uv);
    float3 normal = decodeOctahedralNormal(gbufferNormal.Sample(gbufferSampler, input.uv));
    float4 material = gbufferMaterial.Sample(gbufferSampler, input.uv);

    float perceptualRoughness = saturate(material.x);
    float roughness = perceptualRoughnessToRoughness(perceptualRoughness);
    float metallic = saturate(material.y);
    float ambientOcclusion = material.z;

    float3 ambient = albedo.rgb * 0.025 * ambientOcclusion;
    float3 lightDirection = normalize(float3(0.35, -0.75, 0.55));
    float3 lightVector = -lightDirection;
    float nDotL = saturate(dot(normal, lightVector));
    float3 lightRadiance = float3(4.0, 3.75, 3.35);

    // Bootstrap view vector: the deferred pass does not yet receive depth or camera data. Keeping this
    // approximation independent of the Runtime camera makes the temporary gloss deterministic; replace it
    // with per-pixel world-position reconstruction and the actual camera position when those are available.
    float3 viewVector = float3(0.0, 0.0, 1.0);
    float3 halfVector = safeNormalizeOr(lightVector + viewVector, normal);
    float nDotV = saturate(dot(normal, viewVector));
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewVector, halfVector));

    float3 f0 = pbrF0(albedo.rgb, metallic, 1.5);
    float3 fresnel = fresnelSchlick(vDotH, f0);
    float distribution = distributionGGX(nDotH, roughness);
    float geometry = geometrySmithGGX(nDotV, nDotL, roughness);
    float3 specularBrdf = fresnel * distribution * geometry
        * safeRcp(max(4.0 * nDotV * nDotL, SFT_EPSILON));

    float wrappedDiffuse = lerp(nDotL, 0.5 * nDotL + 0.5, saturate(perceptualRoughness * 0.15));
    float3 diffuse = albedo.rgb * (1.0 - metallic) * lightRadiance * wrappedDiffuse;
    float3 specular = specularBrdf * lightRadiance * nDotL;
    float3 hdr = ambient + diffuse + specular;
    return float4(hdr, albedo.a);
}
