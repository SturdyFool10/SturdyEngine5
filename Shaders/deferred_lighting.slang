// Fullscreen deferred lighting pass.
// Samples the G-buffer and writes HDR scene lighting for tonemapping.

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
Texture2D<float4> gbufferNormal;
Texture2D<float4> gbufferMaterial;
SamplerState gbufferSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 albedo = gbufferAlbedo.Sample(gbufferSampler, input.uv);
    float3 normal = normalize(gbufferNormal.Sample(gbufferSampler, input.uv).xyz * 2.0 - 1.0);
    float4 material = gbufferMaterial.Sample(gbufferSampler, input.uv);

    float roughness = material.x;
    float ambientOcclusion = material.z;

    float3 ambient = albedo.rgb * 0.025 * ambientOcclusion;
    float3 lightDirection = normalize(float3(0.35, -0.75, 0.55));
    float nDotL = saturate(dot(normal, -lightDirection));
    float3 lightRadiance = float3(4.0, 3.75, 3.35);

    // Simple diffuse-only bootstrap. This is intentionally HDR and G-buffer-backed; PBR BRDF, lights,
    // shadows, IBL, and clustered/tiled light lists can replace this without changing the pass topology.
    float diffuseWrap = lerp(nDotL, 0.5 * nDotL + 0.5, saturate(roughness * 0.15));
    float3 hdr = ambient + albedo.rgb * lightRadiance * diffuseWrap;
    return float4(hdr, albedo.a);
}
