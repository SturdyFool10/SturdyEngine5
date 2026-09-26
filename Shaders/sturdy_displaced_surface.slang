// Displaced-surface fragment stage, shared by every geometry path.
// See plans/displacement-system.md. This module owns everything that is the same no matter how the
// triangles arrived: the vertex-to-fragment interface, the glTF material parameters and textures, the
// height/hierarchy resources, and displaced_shade(), which projects the pixel onto the heightfield and
// writes the G-buffer. A geometry path (gbuffer_geometry_displaced.slang's vertex stages, or
// displacement_mesh.slang's task/mesh stages) only has to produce a VertexOutput; it never touches
// shading. That split is what makes the geometry paths interchangeable.
//
// Compile-time configuration (session defines, applied to every module in the compile):
//   SFT_HF_ALGORITHM              0 NormalOnly | 1 Parallax | 2 POM | 3 CellExact | 4 Hierarchical (default 4)
//   SFT_DISPLACEMENT_WRITE_DEPTH  write the displaced depth (SV_Depth); adds the clip-delta varyings
//   SFT_DISPLACEMENT_DEPTH_GE     with the above: declare the depth output conservative (SV_DepthGreaterEqual)
//                                 to keep early-Z. Only valid for depth-map materials (reference height 1).

import sturdy_common;
import sturdy_heightfield;

#ifndef SFT_HF_ALGORITHM
#define SFT_HF_ALGORITHM 4
#endif

// Locations 0-5 match gbuffer_geometry.slang's VertexOutput so a displaced material can share
// pipelines/fragment inputs with the plain one; the tail is displacement-specific.
struct VertexOutput {
    float4 position : SV_Position;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 worldTangent : TANGENT;
    float4 currentClip : TEXCOORD1;
    float4 previousClip : TEXCOORD2;
    float3 worldPosition : TEXCOORD3;
#ifdef SFT_DISPLACEMENT_WRITE_DEPTH
    // Change in clip-space (z, w) per unit step along the surface frame's tangent, bitangent and
    // normal, so the fragment can move its own clip position to the displaced point without needing the
    // view-projection matrix (which lives in a push constant this stage may not be able to read).
    float2 clipZwTangent : TEXCOORD4;
    float2 clipZwBitangent : TEXCOORD5;
    float2 clipZwNormal : TEXCOORD6;
#endif
};

struct GBufferOutput {
    float4 albedo : SV_Target0;
    float2 normal : SV_Target1;
    float4 material : SV_Target2;
    float4 emissive : SV_Target3;
    float2 motion : SV_Target4;
#ifdef SFT_DISPLACEMENT_WRITE_DEPTH
#ifdef SFT_DISPLACEMENT_DEPTH_GE
    // Conservative depth: only valid when the displaced surface is never NEARER than the base one, i.e.
    // displacement_reference_height == 1 (a depth map — the mesh is the top of the volume, so every view
    // ray goes into it). Keeps early-Z rejection alive for the draw. Needs backend support for
    // conservative depth output (Vulkan/D3D12 have it; WebGPU does not — do not define it there).
    float depth : SV_DepthGreaterEqual;
#else
    float depth : SV_Depth;
#endif
#endif
};

// --- glTF material parameters: identical names/meaning to gbuffer_geometry.slang -----------------
float4 base_color_factor;
float metallic_factor;
float roughness_factor;
float specular_factor;
float ior;
float transmission_factor;
float dispersion_cauchy_b;
float absorption_coefficient;
float alpha_cutoff;
float occlusion_strength;
float4 emissive_factor;
float emissive_strength;
float metallic_roughness_channels_rg;

// --- Displacement parameters (all zero-initialised until the application sets them; a zero
// displacement_height_scale is a valid "no displacement" state, see fragmentMain) -----------------
// World-space amplitude of the heightfield.
float displacement_height_scale;
// Normalized height the base mesh sits at: 1 = depth map (mesh is the top of the volume), 0 = height map.
float displacement_reference_height;
// World size one UV tile covers (uv -> metres).
float2 displacement_tile_size;
// Eye position in world space (w unused); set per frame.
float4 displacement_camera_position;
// 0 = use normal_texture for shading detail, 1 = use the normal derived from the height gradient.
float displacement_normal_mix;
// Traversal iteration ceiling and POM sample count (the plan's max_traversal_steps / a fraction of it).
uint displacement_max_steps;
uint displacement_pom_steps;
// Levels in the hierarchy including the implicit level 0; 1 = none bound.
uint displacement_hierarchy_levels;
// 1 = the height texture repeats.
uint displacement_wrap;

Sampler2D base_color_texture;
Sampler2D metallic_roughness_texture;
Sampler2D normal_texture;
Sampler2D occlusion_texture;
Sampler2D emissive_texture;
// Single-channel height in [0, 1] (R). Read with Load() only (plus SampleLevel through displaced_vertex_height()
// on targets where HfTexture is a combined sampler). HfTexture is Sampler2D except on WGSL, where it is a plain
// Texture2D because Slang miscompiles Sampler2D.Load there -- see sturdy_heightfield.slang.
HfTexture height_texture;
// Levels 1.. of the max hierarchy (HeightfieldHierarchy::pack), read with Load(). Materials that do
// not use the hierarchical block bind any 1x1 texture here.
HfTexture height_hierarchy_texture;

// Vertex-stage height fetch at explicit LOD 0 (there are no derivatives in a vertex shader). Hardware-filtered
// where a combined sampler exists; on WGSL the height texture has no sampler, so the same bilinear value is
// rebuilt from four Load()s (hf_sample_height reads only texSize and wrap).
float displaced_vertex_height(float2 uv) {
#ifdef SFT_EMULATE_PUSH_CONSTANTS
    HeightfieldSettings hs;
    uint texW;
    uint texH;
    height_texture.GetDimensions(texW, texH);
    hs.texSize = int2(int(texW), int(texH));
    hs.maxSteps = 0;
    hs.hierarchyLevels = 1;
    hs.heightScale = 1.0;
    hs.referenceHeight = 0.0;
    hs.tileSize = float2(1.0, 1.0);
    hs.wrap = displacement_wrap;
    hs.pomSteps = 0;
    hs.pomRefinements = 0;
    return hf_sample_height(height_texture, uv, hs);
#else
    return height_texture.SampleLevel(uv, 0.0).r;
#endif
}

// ------------------------------------------------------------------------------------------------
// Vertex-side helpers shared by the geometry paths
// ------------------------------------------------------------------------------------------------

struct DisplacedVertexAttributes {
    float3 position;
    float3 normal;
    float2 uv;
    float4 color;
    float4 tangent;
};

// Displacement, in world units along the surface normal, for a normalized height h.
float displaced_height_offset(float h) { return (h - displacement_reference_height) * displacement_height_scale; }

// Builds the vertex-to-fragment record for one (possibly displaced) vertex. Displacement is applied in
// WORLD space, along the world normal, by `displacement` world units — the same units the fragment stage
// uses for its projection, so the geometry paths and the per-pixel blocks agree under any model scale.
VertexOutput displaced_build_vertex(DisplacedVertexAttributes a, float4x4 model, float4x4 viewProjection,
                                    float displacement) {
    VertexOutput output;
    float3 worldNormal = normalize(mul(a.normal, (float3x3)model));
    float4 worldPosition = mul(float4(a.position, 1.0), model);
    worldPosition.xyz += worldNormal * displacement;
    output.position = mul(worldPosition, viewProjection);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = worldNormal;
    output.uv = a.uv;
    output.color = a.color;
    output.worldTangent = float4(normalize(mul(a.tangent.xyz, (float3x3)model)), a.tangent.w);
    output.currentClip = output.position;
    output.previousClip = output.position; // no per-object history on these paths
#ifdef SFT_DISPLACEMENT_WRITE_DEPTH
    float3 tangent = output.worldTangent.xyz;
    float3 bitangent = cross(worldNormal, tangent) * a.tangent.w;
    float4 dt = mul(float4(tangent, 0.0), viewProjection);
    float4 db = mul(float4(bitangent, 0.0), viewProjection);
    float4 dn = mul(float4(worldNormal, 0.0), viewProjection);
    output.clipZwTangent = float2(dt.z, dt.w);
    output.clipZwBitangent = float2(db.z, db.w);
    output.clipZwNormal = float2(dn.z, dn.w);
#endif
    return output;
}

// ------------------------------------------------------------------------------------------------
// Fragment stage
// ------------------------------------------------------------------------------------------------

GBufferOutput displaced_shade(VertexOutput input, bool frontFacing) {
    GBufferOutput output;
    float faceSign = frontFacing ? 1.0 : -1.0;
    float3 baseNormal = normalize(input.worldNormal);
    float3 tangent = normalize(input.worldTangent.xyz - baseNormal * dot(baseNormal, input.worldTangent.xyz));
    float3 bitangent = cross(baseNormal, tangent) * input.worldTangent.w;

    // ---- Displacement projection -------------------------------------------------------------------
    // Everything in this block is data-dependent control flow, so it only Load()s. The derivatives and
    // every filtered material fetch come after it, in uniform control flow (WGSL requires this).
    HeightfieldSettings hs;
    uint texW;
    uint texH;
    height_texture.GetDimensions(texW, texH);
    hs.texSize = int2(int(texW), int(texH));
    hs.maxSteps = displacement_max_steps;
    hs.hierarchyLevels = displacement_hierarchy_levels;
    hs.heightScale = max(displacement_height_scale, 1.0e-6);
    hs.referenceHeight = displacement_reference_height;
    hs.tileSize = max(displacement_tile_size, float2(1.0e-6, 1.0e-6));
    hs.wrap = displacement_wrap;
    hs.pomSteps = displacement_pom_steps;
    hs.pomRefinements = 5;

    float3 viewWorld = normalize(displacement_camera_position.xyz - input.worldPosition);
    float3 viewTS = float3(dot(viewWorld, tangent), dot(viewWorld, bitangent), dot(viewWorld, baseNormal));

    HeightfieldHit hit;
    if (displacement_height_scale > 0.0 && viewTS.z > 1.0e-3) {
        hit = ActiveProjector.project(height_texture, height_hierarchy_texture, hs, input.uv, viewTS);
        // A miss or an exhausted budget on a clamped field falls back to the base point rather than
        // shading garbage; an exhausted budget on a tracing block keeps its best-effort position.
        if (hit.status == HF_STATUS_MISS) {
            hit = NormalOnlyProjector.project(height_texture, height_hierarchy_texture, hs, input.uv, viewTS);
        }
    } else {
        // No displacement configured, or a back-facing/edge-on view: base surface, height gradient only.
        hit = NormalOnlyProjector.project(height_texture, height_hierarchy_texture, hs, input.uv, viewTS);
    }

    float2 dUVdx;
    float2 dUVdy;
    hf_resolve_derivatives(hit.uv, ddx(input.uv), ddy(input.uv), dUVdx, dUVdy);
    float2 uv = hit.uv;

    // ---- Shading normal ----------------------------------------------------------------------------
    float2 normalXY = normal_texture.SampleGrad(uv, dUVdx, dUVdy).rg * 2.0 - 1.0;
    float normalZ = sqrt(saturate(1.0 - dot(normalXY, normalXY)));
    float3 mapNormalTS = float3(normalXY, normalZ);
    float3 heightNormalTS = hf_normal(hit.gradient, hs);
    float3 normalTS = normalize(lerp(mapNormalTS, heightNormalTS, saturate(displacement_normal_mix)));
    float3x3 tbn = float3x3(tangent, bitangent, baseNormal);
    float3 normal = normalize(mul(normalTS, tbn)) * faceSign;
    output.normal = encodeOctahedralNormal(normal);

    // ---- Material fetches at the displaced uv, with warped derivatives -----------------------------------
    float4 baseColor = input.color * base_color_factor * base_color_texture.SampleGrad(uv, dUVdx, dUVdy);
    if (baseColor.a < alpha_cutoff) {
        discard;
    }
    output.albedo = baseColor;

    float4 mr = metallic_roughness_texture.SampleGrad(uv, dUVdx, dUVdy);
    float roughnessRaw = lerp(mr.g, mr.r, metallic_roughness_channels_rg);
    float metallicRaw = lerp(mr.b, mr.g, metallic_roughness_channels_rg);
    float perceptualRoughness = saturate(roughness_factor * roughnessRaw);
    float metallic = saturate(metallic_factor * metallicRaw);
    float ambientOcclusion = saturate(1.0 + occlusion_strength * (occlusion_texture.SampleGrad(uv, dUVdx, dUVdy).r - 1.0));
    float dielectricF0Scalar = dielectricF0FromIor(ior) * saturate(specular_factor);
    output.material = float4(perceptualRoughness, metallic, ambientOcclusion, dielectricF0Scalar);
    output.emissive = float4(emissive_texture.SampleGrad(uv, dUVdx, dUVdy).rgb * emissive_factor.rgb * emissive_strength, 1.0);

    // ---- Motion ----------------------------------------------------------------------------------------
    // Camera-motion-only, as in gbuffer_geometry.slang. A displaced point's own motion (previous
    // transform applied to the displaced position) needs the history path — a pre-planned follow-up.
    float2 currentNdc = input.currentClip.xy * safeRcp(input.currentClip.w);
    float2 previousNdc = input.previousClip.xy * safeRcp(input.previousClip.w);
    output.motion = (currentNdc - previousNdc) * 0.5;

#ifdef SFT_DISPLACEMENT_WRITE_DEPTH
    // Move the clip position from the base point to the displaced one along the surface frame.
    float3 offsetTS = hf_hit_offset_ts(hit, input.uv, hs);
    float clipZ = input.currentClip.z + offsetTS.x * input.clipZwTangent.x + offsetTS.y * input.clipZwBitangent.x +
                  offsetTS.z * input.clipZwNormal.x;
    float clipW = input.currentClip.w + offsetTS.x * input.clipZwTangent.y + offsetTS.y * input.clipZwBitangent.y +
                  offsetTS.z * input.clipZwNormal.y;
    output.depth = clipZ * safeRcp(clipW);
#endif
    return output;
}


// Alpha-cutout test for the z prepass: only the base surface is known there.
bool displaced_alpha_passes(VertexOutput input, float alpha) { return alpha >= alpha_cutoff; }
