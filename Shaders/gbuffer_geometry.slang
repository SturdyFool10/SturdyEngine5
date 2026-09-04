// Deferred G-buffer geometry shader for GeometryVertex.
// Outputs material attributes into multiple dynamic-rendering color attachments:
//   SV_Target0: albedo + alpha
//   SV_Target1: octahedral-encoded world normal (RG16F — see sturdy_common's
//               encodeOctahedralNormal/decodeOctahedralNormal)
//   SV_Target2: material properties — perceptual roughness, metallic, ambient occlusion,
//               dielectric F0 (see sturdy_common's dielectricF0FromIor; this unifies the
//               metallic-roughness and KHR_materials_specular-style specular workflows onto one
//               material input instead of two — plans/pbr-material-system.md)
//   SV_Target3: HDR emissive color (RGBA16F — glTF's emissive_factor/emissive_texture, scaled by
//               KHR_materials_emissive_strength; unlit, added downstream in the lighting pass
//               after the BRDF/shadow/AO terms, not before)
//   SV_Target4: screen-space motion vector (RG16F), from currentClip/previousClip below — real
//               per-object motion for draws recorded with Renderer::history_pipeline_for
//               (Shaders/gbuffer_geometry_history.slang's vertexMainWithHistory), camera-motion-only
//               (currentClip == previousClip's object term, i.e. previousModel treated as model) for
//               plain vertexMain draws — see vertexMain's own comment.
//
// Material parameter/texture-slot names deliberately match glTF's pbrMetallicRoughness
// convention (plans/material-system.md's "Default PBR template") so a glTF-imported material
// maps onto this template's reflected UBO/texture slots field-for-field.
//
// occlusion_texture packs into the existing material-channel AO slot (no new target needed);
// emissive needed its own target (an HDR RGB value has nowhere to hide in the 8-bit
// albedo/material channels) — see plans/pbr-material-system.md's Phase 2.
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
    // Undivided clip-space position this frame / last frame, for fragmentMain's motion-vector
    // output below — kept undivided (not NDC) since perspective division must happen per-fragment
    // after interpolation, not per-vertex. vertexMain (paired with plain SceneDrawConstants, no
    // per-object history available) sets both to the same value: zero motion, camera movement only.
    // vertexMainWithHistory (Shaders/gbuffer_geometry_history.slang, paired with this same
    // fragmentMain) computes a real previousClip from SceneObjectGpuData::previous_model.
    float4 currentClip : TEXCOORD1;
    float4 previousClip : TEXCOORD2;
};

struct GBufferOutput {
    float4 albedo : SV_Target0;
    float2 normal : SV_Target1;
    float4 material : SV_Target2;
    float4 emissive : SV_Target3;
    float2 motion : SV_Target4;
};

struct SceneDrawConstants {
    float4x4 view_projection;
    float4x4 model;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS

// WebGPU has no push constants at all, so on that target the same block is carried as an

// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of

// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant

// block is packed std430-style, a uniform buffer std140, so a struct whose members would

// pad differently between the two must be laid out to agree under both.

[[vk::binding(0, 3)]] ConstantBuffer<SceneDrawConstants> scene_draw;

#else

[[push_constant]] ConstantBuffer<SceneDrawConstants> scene_draw;

#endif
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
// Ray-query material extensions. Raster G-buffer channels remain unchanged; the spectral scene packer
// reads these reflected CPU values directly for transmission, dispersion, and Beer-Lambert absorption.
float transmission_factor;
float dispersion_cauchy_b;
float absorption_coefficient;
// glTF alpha-mask (cutout) threshold. 0.0 (the zero-initialized default) never discards, matching
// glTF's OPAQUE mode; a glTF MASK material sets this to its real alphaCutoff (spec default 0.5).
// True alpha BLEND is not supported by this deferred G-buffer pass (transparency needs a forward
// pass — plans/pbr-material-system.md's transmission/glass phase notes the same gap) and is treated
// as opaque rather than faked via cutout.
float alpha_cutoff;
// glTF occlusion_texture's strength scale (glTF spec default 1.0; irrelevant when no occlusion
// texture is bound, since sampling the default white texture already yields no darkening).
float occlusion_strength;
// glTF emissive_factor (w unused — see GltfImport.cpp's PendingMaterial for why this is a vec4
// rather than vec3) and KHR_materials_emissive_strength's multiplier (spec default 1.0).
float4 emissive_factor;
float emissive_strength;
// 0.0 (default): metallic_roughness_texture holds glTF's own G=roughness/B=metallic layout. 1.0:
// GltfImport.cpp re-channeled it to R=roughness/G=metallic before compressing as BC5 (half BC7's
// size — see Engine::TextureKind::MetallicRoughness), since BC5 only stores two channels and they
// decode into R/G regardless of which source channels were encoded.
float metallic_roughness_channels_rg;

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
// Packed occlusion data (conventionally the R channel — see GltfImport.cpp's ORM-sharing comment).
// Materials with no occlusion texture bind the renderer's default white texture (no darkening).
Sampler2D occlusion_texture;
// Emissive is display color like base_color_texture (sRGB-decoded on import), not packed data.
// Materials with no emissive texture bind the default white texture, which is harmless since
// emissive_factor defaults to (0,0,0,0) — white * black factor is still black.
Sampler2D emissive_texture;

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
    // No per-object history available on this push-constant-only path — report zero self-motion
    // (camera-movement-only, same as this pipeline's ordinary use before object history existed).
    output.currentClip = output.position;
    output.previousClip = output.position;
    return output;
}

[shader("fragment")]
GBufferOutput fragmentMain(VertexOutput input, bool frontFacing : SV_IsFrontFace) {
    GBufferOutput output;
    float faceSign = frontFacing ? 1.0 : -1.0;
    float3 normal = normalize(input.worldNormal);
    // Gram-Schmidt re-orthogonalize the interpolated tangent against the interpolated normal (they
    // drift apart across a triangle) before building the TBN basis.
    float3 tangent = normalize(input.worldTangent.xyz - normal * dot(normal, input.worldTangent.xyz));
    float3 bitangent = cross(normal, tangent) * input.worldTangent.w;
    // Reconstruct Z from the sampled X/Y rather than trusting a stored B channel: mathematically
    // equivalent for any valid tangent-space normal map (a unit vector's Z is determined by its
    // X/Y up to the always-true positive-hemisphere assumption here), and it's what makes BC5
    // (2-channel, X/Y only) a safe drop-in compressed format for this slot — see
    // Engine::TextureKind::NormalMap.
    float2 normalXY = normal_texture.Sample(input.uv).rg * 2.0 - 1.0;
    float normalZ = sqrt(saturate(1.0 - dot(normalXY, normalXY)));
    float3 normalSample = float3(normalXY, normalZ);
    float3x3 tbn = float3x3(tangent, bitangent, normal);
    normal = normalize(mul(normalSample, tbn)) * faceSign;
    output.normal = encodeOctahedralNormal(normal);

    float4 baseColorSample = base_color_texture.Sample(input.uv);
    float4 baseColor = input.color * base_color_factor * baseColorSample;
    if (baseColor.a < alpha_cutoff) {
        discard;
    }
    output.albedo = baseColor;

    // glTF packing convention: G = roughness, B = metallic (R/A unused by this template) — unless
    // metallic_roughness_channels_rg selects the BC5-friendly R = roughness, G = metallic layout
    // instead (see that uniform's own doc comment).
    float4 metallicRoughnessSample = metallic_roughness_texture.Sample(input.uv);
    float roughnessRaw = lerp(metallicRoughnessSample.g, metallicRoughnessSample.r, metallic_roughness_channels_rg);
    float metallicRaw = lerp(metallicRoughnessSample.b, metallicRoughnessSample.g, metallic_roughness_channels_rg);
    float perceptualRoughness = saturate(roughness_factor * roughnessRaw);
    float metallic = saturate(metallic_factor * metallicRaw);
    float ambientOcclusion = saturate(1.0 + occlusion_strength * (occlusion_texture.Sample(input.uv).r - 1.0));
    float dielectricF0Scalar = dielectricF0FromIor(ior) * saturate(specular_factor);

    output.material = float4(perceptualRoughness, metallic, ambientOcclusion, dielectricF0Scalar);
    output.emissive = float4(emissive_texture.Sample(input.uv).rgb * emissive_factor.rgb * emissive_strength, 1.0);

    // NDC-space delta (halved for UV-space) — perspective divide happens here, per-fragment, after
    // interpolation of the undivided clip positions (dividing per-vertex then interpolating would be
    // wrong: NDC doesn't interpolate linearly in screen space the way clip-space does).
    float2 currentNdc = input.currentClip.xy * safeRcp(input.currentClip.w);
    float2 previousNdc = input.previousClip.xy * safeRcp(input.previousClip.w);
    output.motion = (currentNdc - previousNdc) * 0.5;
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
