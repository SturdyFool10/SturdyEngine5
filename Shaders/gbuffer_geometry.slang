// Deferred G-buffer geometry shader for GeometryVertex.
// Outputs material attributes into multiple dynamic-rendering color attachments:
//   SV_Target0: albedo + alpha
//   SV_Target1: octahedral-encoded world normal (RG16F — see sturdy_common's
//               encodeOctahedralNormal/decodeOctahedralNormal)
//   SV_Target2: material properties — perceptual roughness, metallic, ambient occlusion,
//               dielectric F0 (see sturdy_common's dielectricF0FromIor; this unifies the
//               metallic-roughness and KHR_materials_specular-style specular workflows onto one
//               material input instead of two — plans/pbr-material-system.md)
//
// Material parameter/texture-slot names deliberately match glTF's pbrMetallicRoughness
// convention (plans/material-system.md's "Default PBR template") so a glTF-imported material
// maps onto this template's reflected UBO/texture slots field-for-field.
//
// occlusion_texture/emissive are not exposed here yet: emissive has nowhere to go until the
// G-buffer gains its own target (today's 3 targets — albedo/normal/material — have no spare
// channel wide enough for an HDR RGB emissive value), and no occlusion texture has shown up in
// real content routed through this template yet — see plans/pbr-material-system.md's Phase 2 for
// the G-buffer-expansion follow-up work.
//
// normal_texture IS exposed: GeometryVertex now carries a per-vertex tangent (xyz direction, w
// bitangent-handedness sign — glTF's own TANGENT accessor convention), so a real tangent-space
// basis can be built per-fragment instead of makeSurfaceBasis()'s arbitrary one (which doesn't
// align with a normal map's UV-space orientation).

import sturdy_common;

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 tangent : TANGENT;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 worldTangent : TANGENT;
};

struct GBufferOutput {
    float4 albedo : SV_Target0;
    float2 normal : SV_Target1;
    float4 material : SV_Target2;
};

struct SceneDrawConstants {
    float4x4 view_projection;
    float4x4 model;
};

[[push_constant]] ConstantBuffer<SceneDrawConstants> scene_draw;

// glTF-convention material parameters. These are deliberately loose module-scope uniforms, not an
// explicit ConstantBuffer<T>: Renderer::collect_uniform_fields (ReflectionBinding.hpp) walks
// Slang's implicit default constant buffer, which is exactly what bare global uniforms become —
// wrapping them in a named ConstantBuffer<T> instead makes Slang reflect `material` as one opaque
// resource parameter, invisible to collect_uniform_fields (this shipped broken that way once
// already; see the git history for the fix).
//
// Every MaterialInstance parameter is zero-initialized by the renderer until a consumer calls
// set_material_*() (Renderer::initialize_material_instance_state zero-fills default_bytes; Slang
// has no reflected-initializer mechanism this system reads yet), so a consumer must explicitly set
// base_color_factor/specular_factor/ior to their glTF spec defaults (1,1,1,1 / 1.0 / 1.5) —
// leaving them at the zero default is a valid (if unusual) fully-black, zero-F0 material, not an
// error.
float4 base_color_factor;
float metallic_factor;
float roughness_factor;
float specular_factor;
float ior;
// glTF alpha-mask (cutout) threshold. 0.0 (the zero-initialized default) never discards, matching
// glTF's OPAQUE mode; a glTF MASK material sets this to its real alphaCutoff (spec default 0.5).
// True alpha BLEND is not supported by this deferred G-buffer pass (transparency needs a forward
// pass — plans/pbr-material-system.md's transmission/glass phase notes the same gap) and is treated
// as opaque rather than faked via cutout.
float alpha_cutoff;

// Combined texture+sampler resources (Slang's Sampler2D, not a split Texture2D+SamplerState pair):
// Renderer::prepare_material_frame (RendererMaterial.cpp) binds a texture slot's *own* sampler
// alongside its view at one binding index — the split model's separate sampler binding is reflected
// into the bind-group layout but never actually written to, which the validation layer catches as
// an unbound descriptor. Every material texture must be Sampler2D until the generic binder grows
// real split-sampler support (see plans/pbr-material-system.md).
Sampler2D base_color_texture;
Sampler2D metallic_roughness_texture;
// Tangent-space normal map: RGB in [0,1] unpacks to a [-1,1] tangent-space direction
// (X = +tangent, Y = +bitangent, Z = +normal — glTF's own normalTexture convention). Materials
// without one bind a flat (0.5, 0.5, 1.0) placeholder (GltfImport.cpp), which unpacks to
// tangent-space (0, 0, 1) — a no-op perturbation, so this sampling is safe unconditionally.
Sampler2D normal_texture;

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    float4 world_position = mul(float4(input.position, 1.0), scene_draw.model);
    output.position = mul(world_position, scene_draw.view_projection);
    output.worldNormal = normalize(mul(input.normal, (float3x3)scene_draw.model));
    output.uv = input.uv;
    output.color = input.color;
    // Same (float3x3)model convention as worldNormal above (no inverse-transpose) — consistent
    // under the same uniform-scale assumption, and tangents specifically transform correctly with
    // the plain model matrix even under non-uniform scale (unlike normals).
    output.worldTangent = float4(normalize(mul(input.tangent.xyz, (float3x3)scene_draw.model)), input.tangent.w);
    return output;
}

[shader("fragment")]
GBufferOutput fragmentMain(VertexOutput input) {
    GBufferOutput output;
    float3 normal = normalize(input.worldNormal);
    // Gram-Schmidt re-orthogonalize the interpolated tangent against the interpolated normal (they
    // drift apart across a triangle) before building the TBN basis.
    float3 tangent = normalize(input.worldTangent.xyz - normal * dot(normal, input.worldTangent.xyz));
    float3 bitangent = cross(normal, tangent) * input.worldTangent.w;
    float3 normalSample = normal_texture.Sample(input.uv).rgb * 2.0 - 1.0;
    float3x3 tbn = float3x3(tangent, bitangent, normal);
    normal = normalize(mul(normalSample, tbn));
    output.normal = encodeOctahedralNormal(normal);

    float4 baseColorSample = base_color_texture.Sample(input.uv);
    float4 baseColor = input.color * base_color_factor * baseColorSample;
    if (baseColor.a < alpha_cutoff) {
        discard;
    }
    output.albedo = baseColor;

    // glTF packing convention: G = roughness, B = metallic (R/A unused by this template).
    float4 metallicRoughnessSample = metallic_roughness_texture.Sample(input.uv);
    float perceptualRoughness = saturate(roughness_factor * metallicRoughnessSample.g);
    float metallic = saturate(metallic_factor * metallicRoughnessSample.b);
    // No occlusion_texture yet (see file header): occlusion is constant 1.0 (no darkening) until
    // Phase 2 wires a real one in.
    float ambientOcclusion = 1.0;
    float dielectricF0Scalar = dielectricF0FromIor(ior) * saturate(specular_factor);

    output.material = float4(perceptualRoughness, metallic, ambientOcclusion, dielectricF0Scalar);
    return output;
}

// Z prepass entry point (Renderer::depth_only_pipeline_for) — same vertex stage and globals as
// fragmentMain, but writes no color output: only the alpha-cutout discard test needs to run before
// rasterization commits real depth, so the later "deferred gbuffer geometry" pass can require an
// exact depth match instead of shading fragments that would just be overdrawn.
[shader("fragment")]
void depthOnlyMain(VertexOutput input) {
    float4 baseColorSample = base_color_texture.Sample(input.uv);
    float alpha = input.color.a * base_color_factor.a * baseColorSample.a;
    if (alpha < alpha_cutoff) {
        discard;
    }
}
