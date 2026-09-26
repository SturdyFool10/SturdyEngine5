// Ray-tracing displacement slot: prism entry/exit + ray-vs-displaced-triangle intersection.
// See plans/displacement-system.md section 9 and Renderer/src/Renderer/Displacement/RayTracingReference.hpp,
// whose functions this file mirrors one for one (names are the same modulo the hf_rt_ prefix). Change one,
// change the other.
//
//   import heightfield_rt_prism;
//
// A hardware ray tracer intersects the *base* triangle, so secondary rays (reflection, refraction, GI,
// shadow) see a flat surface where the raster path shows relief. This module lets a ray see the displaced
// surface of one triangle:
//
//   1. PRISM  the triangle extruded along its per-vertex normals over the material's height range. Point
//             (barycentric b, normalized height h) maps to  P(b, h) = sum_i b_i (p_i + s n_i),
//             s = (h - referenceHeight) * heightScale.
//   2. CHORD  the ray's entry/exit of the prism are found exactly (two cap triangles, three bilinear side
//             patches). Between them the ray is taken as the straight line in (b, h) space — exact for a
//             right prism (equal vertex normals), the standard projective-displacement approximation otherwise.
//   3. TRACE  b is linear in uv, so that line is a straight ray through heightfield space (u, v, h): exactly
//             what hf_trace_grid from sturdy_heightfield solves. Nothing in that module is duplicated.
//
// Two ways to reach hf_rt_intersect_triangle, both provided:
//   * hardware  each prism is a procedural (AABB) primitive of a BLAS; the inline ray query's
//               CANDIDATE_PROCEDURAL_PRIMITIVE case calls it and commits with CommitProceduralPrimitiveHit.
//               (heightfield_rt_query_probe.slang shows the loop.) Needs RayQuery + AccelerationStructures.
//   * software  heightfield_rt_software.slang runs it against a triangle list in a plain compute shader —
//               no acceleration structure, works on every device that runs compute, WebGPU included.
//
// The material contract (what a displaced material exposes to ray tracing) is HfRtDisplacementRecord
// below. In the engine's ray-query scene ABI (sturdy_spectral_scene.slang) a displaced material is
// identified by SpectralMaterial.textureIndices1.y (a lane that ABI reserves for material extensions):
// hfRtRecordIndex. It indexes a StructuredBuffer<HfRtDisplacementRecord>; HF_RT_NO_RECORD (0xffffffff) means
// "not displaced, use the base triangle". The record's heightTexture/hierarchyTexture are indices into the
// same bindless material-texture heap the material's other textures use.
//
// Portability follows sturdy_heightfield: only explicit-LOD Load()s, bounded loops, no derivatives.

import sturdy_common;
import sturdy_heightfield;

static const uint HF_RT_NO_RECORD = 0xffffffffu;
static const uint HF_RT_FLAG_WRAP = 1u;

// ------------------------------------------------------------------------------------------------
// Material contract
// ------------------------------------------------------------------------------------------------

// One record per displaced material, 80 bytes (std430; every member is a 16-byte vector so no packing
// surprises). HfRtDisplacementRecordGpu in RayTracingReference.hpp is the CPU twin and static_asserts the layout.
struct HfRtDisplacementRecord {
    // x height texture index, y hierarchy texture index (0xffffffff = none), z flags (bit 0 = wrap),
    // w traversal step budget inside one prism.
    uint4 textures;
    // xy height texture size in texels, z hierarchy level count including implicit level 0 (1 = none), w reserved.
    int4 sizes;
    // x heightScale, y referenceHeight, zw tileSize.
    float4 scale;
    // x heightMin, y heightMax (normalized bounds of the heights present: [0,1] always valid, a hierarchy
    // root's min/max gives tighter prisms), zw uvScale.
    float4 heightRange;
    // xy uvOffset (heightfield uv = meshUv * uvScale + uvOffset), zw reserved.
    float4 uvTransform;
};

struct HfRtParams {
    HeightfieldSettings hf;
    float heightMin;
    float heightMax;
    float2 uvScale;
    float2 uvOffset;
};

HfRtParams hf_rt_params_from_record(HfRtDisplacementRecord r) {
    HfRtParams p;
    p.hf.texSize = int2(r.sizes.xy);
    p.hf.maxSteps = r.textures.w;
    p.hf.hierarchyLevels = uint(r.sizes.z);
    p.hf.heightScale = r.scale.x;
    p.hf.referenceHeight = r.scale.y;
    p.hf.tileSize = r.scale.zw;
    p.hf.wrap = (r.textures.z & HF_RT_FLAG_WRAP) != 0u ? 1u : 0u;
    p.hf.pomSteps = 0u;
    p.hf.pomRefinements = 0u;
    p.hf.startLevel = 64u;
    p.heightMin = r.heightRange.x;
    p.heightMax = r.heightRange.y;
    p.uvScale = r.heightRange.zw;
    p.uvOffset = r.uvTransform.xy;
    return p;
}

// ------------------------------------------------------------------------------------------------
// Triangle
// ------------------------------------------------------------------------------------------------

// One base triangle in the ray's space. Normals are the interpolated-normal inputs (lengths scale the
// displacement); uv are already heightfield coordinates in tile units (see HfRtParams.uvScale/uvOffset).
struct HfRtTriangle {
    float3 p0;
    float3 p1;
    float3 p2;
    float3 n0;
    float3 n1;
    float3 n2;
    float2 uv0;
    float2 uv1;
    float2 uv2;
};

// Buffer form used by the probes: 96 bytes, vertex i = (position.xyz, uv.x), (normal.xyz, uv.y).
struct HfRtPackedTriangle {
    float4 v0a;
    float4 v0b;
    float4 v1a;
    float4 v1b;
    float4 v2a;
    float4 v2b;
};

HfRtTriangle hf_rt_unpack_triangle(HfRtPackedTriangle t) {
    HfRtTriangle o;
    o.p0 = t.v0a.xyz;
    o.p1 = t.v1a.xyz;
    o.p2 = t.v2a.xyz;
    o.n0 = t.v0b.xyz;
    o.n1 = t.v1b.xyz;
    o.n2 = t.v2b.xyz;
    o.uv0 = float2(t.v0a.w, t.v0b.w);
    o.uv1 = float2(t.v1a.w, t.v1b.w);
    o.uv2 = float2(t.v2a.w, t.v2b.w);
    return o;
}

// Maps a triangle's mesh UVs to heightfield tile units (KHR_texture_transform style). Scene-ABI callers
// load the triangle from the vertex arena, run this, then intersect; the probes upload pre-mapped UVs.
HfRtTriangle hf_rt_apply_uv_transform(HfRtTriangle t, HfRtParams p) {
    t.uv0 = t.uv0 * p.uvScale + p.uvOffset;
    t.uv1 = t.uv1 * p.uvScale + p.uvOffset;
    t.uv2 = t.uv2 * p.uvScale + p.uvOffset;
    return t;
}

float hf_rt_displacement(HfRtParams p, float height) { return (height - p.hf.referenceHeight) * p.hf.heightScale; }

float3 hf_rt_prism_position(HfRtTriangle t, HfRtParams p, float3 b, float height) {
    float s = hf_rt_displacement(p, height);
    return b.x * (t.p0 + s * t.n0) + b.y * (t.p1 + s * t.n1) + b.z * (t.p2 + s * t.n2);
}

float2 hf_rt_uv_at(HfRtTriangle t, float3 b) { return t.uv0 * b.x + t.uv1 * b.y + t.uv2 * b.z; }

// ------------------------------------------------------------------------------------------------
// Prism
// ------------------------------------------------------------------------------------------------

struct HfRtPrism {
    // [0..2] bottom cap (heightMin), [3..5] top cap (heightMax).
    float3 c0;
    float3 c1;
    float3 c2;
    float3 c3;
    float3 c4;
    float3 c5;
};

HfRtPrism hf_rt_make_prism(HfRtTriangle t, HfRtParams p) {
    float sMin = hf_rt_displacement(p, p.heightMin);
    float sMax = hf_rt_displacement(p, p.heightMax);
    HfRtPrism prism;
    prism.c0 = t.p0 + sMin * t.n0;
    prism.c1 = t.p1 + sMin * t.n1;
    prism.c2 = t.p2 + sMin * t.n2;
    prism.c3 = t.p0 + sMax * t.n0;
    prism.c4 = t.p1 + sMax * t.n1;
    prism.c5 = t.p2 + sMax * t.n2;
    return prism;
}

// Box the BVH is built over. The bilinear sides lie inside the convex hull of their corners, so the
// corner box is conservative. (Host code builds these on the CPU with prism_bounds(); this is for
// compute-built BLAS inputs and for tests.)
void hf_rt_prism_bounds(HfRtPrism prism, out float3 boundsMin, out float3 boundsMax) {
    boundsMin = min(min(min(prism.c0, prism.c1), min(prism.c2, prism.c3)), min(prism.c4, prism.c5));
    boundsMax = max(max(max(prism.c0, prism.c1), max(prism.c2, prism.c3)), max(prism.c4, prism.c5));
}

// Running min/max of the boundary crossings. Heights are prism-local (0 = bottom cap, 1 = top cap).
struct HfRtRange {
    bool hit;
    float tEnter;
    float tExit;
    float3 baryEnter;
    float3 baryExit;
    float heightEnter;
    float heightExit;
};

static const float HF_RT_BARY_TOLERANCE = 1.0e-5f;

void hf_rt_add_crossing(inout HfRtRange r, float t, float3 bary, float height) {
    if (!r.hit) {
        r.hit = true;
        r.tEnter = t;
        r.tExit = t;
        r.baryEnter = bary;
        r.baryExit = bary;
        r.heightEnter = height;
        r.heightExit = height;
        return;
    }
    if (t < r.tEnter) {
        r.tEnter = t;
        r.baryEnter = bary;
        r.heightEnter = height;
    }
    if (t > r.tExit) {
        r.tExit = t;
        r.baryExit = bary;
        r.heightExit = height;
    }
}

// Moller-Trumbore without culling. (1-u-v, u, v) are the prism barycentrics directly.
void hf_rt_cross_cap(float3 a, float3 b, float3 c, float height, float3 o, float3 d, inout HfRtRange r) {
    float3 e1 = b - a;
    float3 e2 = c - a;
    float3 pv = cross(d, e2);
    float det = dot(e1, pv);
    if (abs(det) < 1.0e-20f) {
        return;
    }
    float inv = 1.0f / det;
    float3 tv = o - a;
    float u = dot(tv, pv) * inv;
    float3 qv = cross(tv, e1);
    float v = dot(d, qv) * inv;
    if (u < -HF_RT_BARY_TOLERANCE || v < -HF_RT_BARY_TOLERANCE || u + v > 1.0f + HF_RT_BARY_TOLERANCE) {
        return;
    }
    hf_rt_add_crossing(r, dot(e2, qv) * inv, float3(1.0f - u - v, u, v), height);
}

// Barycentrics of the point at parameter `a` along side k's edge (the edge opposite vertex k).
float3 hf_rt_side_bary(uint k, float a) {
    if (k == 0u) {
        return float3(0.0f, 1.0f - a, a);
    }
    if (k == 1u) {
        return float3(a, 0.0f, 1.0f - a);
    }
    return float3(1.0f - a, a, 0.0f);
}

// Ray vs the bilinear patch S(a, w) = c00 + a e1 + w e2 + a w e3. Eliminates t by projecting onto the plane
// perpendicular to the ray, leaving two bilinear equations and a quadratic in a.
void hf_rt_cross_side(float3 c00, float3 c10, float3 c01, float3 c11, uint side, float3 o, float3 d,
                      inout HfRtRange r) {
    float dLen2 = dot(d, d);
    if (dLen2 <= 0.0f) {
        return;
    }
    float3 dn = d * rsqrt(dLen2);
    float3 helper = abs(dn.x) < 0.9f ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
    float3 u1 = normalize(cross(dn, helper));
    float3 u2 = cross(dn, u1);

    float3 e1 = c10 - c00;
    float3 e2 = c01 - c00;
    float3 e3 = c00 - c10 - c01 + c11;
    float3 rr = c00 - o;
    float A1 = dot(u1, rr);
    float A2 = dot(u2, rr);
    float B1 = dot(u1, e1);
    float B2 = dot(u2, e1);
    float C1 = dot(u1, e2);
    float C2 = dot(u2, e2);
    float D1 = dot(u1, e3);
    float D2 = dot(u2, e3);

    float qa = B1 * D2 - B2 * D1;
    float qb = B1 * C2 + A1 * D2 - B2 * C1 - A2 * D1;
    float qc = A1 * C2 - A2 * C1;

    float roots[2] = { 0.0f, 0.0f };
    uint rootCount = 0u;
    float scale = max(max(abs(qb), abs(qc)), 1.0e-30f);
    if (abs(qa) <= 1.0e-6f * scale) {
        if (abs(qb) > 1.0e-30f) {
            roots[0] = -qc / qb;
            rootCount = 1u;
        }
    } else {
        float disc = qb * qb - 4.0f * qa * qc;
        if (disc >= 0.0f) {
            float sq = sqrt(disc);
            float q = -0.5f * (qb + (qb >= 0.0f ? sq : -sq));
            roots[0] = q / qa;
            rootCount = 1u;
            if (q != 0.0f) {
                roots[1] = qc / q;
                rootCount = 2u;
            }
        }
    }

    for (uint i = 0u; i < rootCount; ++i) {
        float a = roots[i];
        if (a < -HF_RT_BARY_TOLERANCE || a > 1.0f + HF_RT_BARY_TOLERANCE) {
            continue;
        }
        float den1 = C1 + a * D1;
        float den2 = C2 + a * D2;
        float w;
        if (abs(den1) >= abs(den2)) {
            if (abs(den1) < 1.0e-30f) {
                continue;
            }
            w = -(A1 + a * B1) / den1;
        } else {
            w = -(A2 + a * B2) / den2;
        }
        if (w < -HF_RT_BARY_TOLERANCE || w > 1.0f + HF_RT_BARY_TOLERANCE) {
            continue;
        }
        float3 point = c00 + e1 * a + e2 * w + e3 * (a * w);
        hf_rt_add_crossing(r, dot(point - o, d) / dLen2, hf_rt_side_bary(side, a), w);
    }
}

// Exact prism crossings; the interval is the hull of all of them (valid: the (b, h) domain is convex).
// Returns hit = false when the ray misses the prism or the hull lies outside [tMin, tMax].
HfRtRange hf_rt_intersect_prism(HfRtPrism prism, float3 o, float3 d, float tMin, float tMax) {
    HfRtRange r;
    r.hit = false;
    r.tEnter = 0.0f;
    r.tExit = 0.0f;
    r.baryEnter = float3(0.0f, 0.0f, 0.0f);
    r.baryExit = float3(0.0f, 0.0f, 0.0f);
    r.heightEnter = 0.0f;
    r.heightExit = 0.0f;
    hf_rt_cross_cap(prism.c0, prism.c1, prism.c2, 0.0f, o, d, r);
    hf_rt_cross_cap(prism.c3, prism.c4, prism.c5, 1.0f, o, d, r);
    hf_rt_cross_side(prism.c1, prism.c2, prism.c4, prism.c5, 0u, o, d, r);
    hf_rt_cross_side(prism.c2, prism.c0, prism.c5, prism.c3, 1u, o, d, r);
    hf_rt_cross_side(prism.c0, prism.c1, prism.c3, prism.c4, 2u, o, d, r);
    if (r.hit && (r.tExit < tMin || r.tEnter > tMax || !(r.tExit > r.tEnter))) {
        r.hit = false;
    }
    return r;
}

// ------------------------------------------------------------------------------------------------
// Displaced triangle
// ------------------------------------------------------------------------------------------------

struct HfRtHit {
    uint status;      // HF_STATUS_MISS / HIT / BUDGET (same values as HeightfieldHit)
    float t;
    float3 position;
    float3 normal;
    float2 uv;
    float3 barycentrics;
    float height;
    uint backFace;
    uint steps;
};

HfRtHit hf_rt_miss() {
    HfRtHit h;
    h.status = HF_STATUS_MISS;
    h.t = 0.0f;
    h.position = float3(0.0f, 0.0f, 0.0f);
    h.normal = float3(0.0f, 0.0f, 1.0f);
    h.uv = float2(0.0f, 0.0f);
    h.barycentrics = float3(0.0f, 0.0f, 0.0f);
    h.height = 0.0f;
    h.backFace = 0u;
    h.steps = 0u;
    return h;
}

// Ray vs one displaced triangle. `useHierarchy` selects the hierarchical block (needs hierarchyTex and
// p.hf.hierarchyLevels > 1) over the plain cell-exact DDA. t is in units of `d`. Same semantics as the
// heightfield blocks: the volume below the surface is solid, and a ray starting inside it hits at its origin.
HfRtHit hf_rt_intersect_triangle(HfTexture heightTex, HfTexture hierarchyTex, bool useHierarchy, HfRtParams p,
                                 HfRtTriangle tri, float3 o, float3 d, float tMin, float tMax) {
    if (!(p.heightMax > p.heightMin)) {
        return hf_rt_miss();
    }
    HfRtPrism prism = hf_rt_make_prism(tri, p);
    HfRtRange range = hf_rt_intersect_prism(prism, o, d, tMin, tMax);
    if (!range.hit) {
        return hf_rt_miss();
    }

    float hSpan = p.heightMax - p.heightMin;
    float hEnter = p.heightMin + range.heightEnter * hSpan;
    float hExit = p.heightMin + range.heightExit * hSpan;

    // Clip to [tMin, tMax]; (b, h) is linear in t along the chord so this is a lerp.
    float span = range.tExit - range.tEnter;
    float tau0 = clamp((tMin - range.tEnter) / span, 0.0f, 1.0f);
    float tau1 = clamp((tMax - range.tEnter) / span, 0.0f, 1.0f);
    if (!(tau1 > tau0)) {
        return hf_rt_miss();
    }
    float3 bA = lerp(range.baryEnter, range.baryExit, tau0);
    float3 bB = lerp(range.baryEnter, range.baryExit, tau1);
    float hA = hEnter + (hExit - hEnter) * tau0;
    float hB = hEnter + (hExit - hEnter) * tau1;
    float tA = range.tEnter + span * tau0;
    float tB = range.tEnter + span * tau1;

    float2 uvA = hf_rt_uv_at(tri, bA);
    float2 uvB = hf_rt_uv_at(tri, bB);
    HeightfieldRay ray;
    ray.origin = float3(uvA, hA);
    ray.direction = float3(uvB - uvA, hB - hA);
    ray.maxT = 1.0f;

    HeightfieldHit hit = hf_trace_grid(heightTex, hierarchyTex, useHierarchy, p.hf, ray);
    HfRtHit result = hf_rt_miss();
    result.status = hit.status;
    result.steps = hit.steps;
    if (hit.status == HF_STATUS_MISS) {
        return result;
    }

    float tau = clamp(hit.t, 0.0f, 1.0f);
    float3 b = lerp(bA, bB, tau);
    result.barycentrics = b;
    result.uv = hit.uv;
    result.height = hit.height;
    // The chord approximation can put the point marginally off the ray; keep t inside the caller's interval
    // so it is always a legal value for CommitProceduralPrimitiveHit.
    result.t = clamp(tA + (tB - tA) * tau, tMin, tMax);
    result.position = hf_rt_prism_position(tri, p, b, hit.height);

    // Exact derivative of P(b1, b2) = sum_i b_i (p_i + s n_i), s = heightScale (h(uv(b)) - reference).
    float s = hf_rt_displacement(p, hit.height);
    float3 nInterp = b.x * tri.n0 + b.y * tri.n1 + b.z * tri.n2;
    float3 q0 = tri.p0 + s * tri.n0;
    float3 q1 = tri.p1 + s * tri.n1;
    float3 q2 = tri.p2 + s * tri.n2;
    float ds1 = p.hf.heightScale * dot(hit.gradient, tri.uv1 - tri.uv0);
    float ds2 = p.hf.heightScale * dot(hit.gradient, tri.uv2 - tri.uv0);
    float3 dp1 = (q1 - q0) + nInterp * ds1;
    float3 dp2 = (q2 - q0) + nInterp * ds2;
    float3 normal = cross(dp1, dp2);
    float len2 = dot(normal, normal);
    float nLen2 = dot(nInterp, nInterp);
    if (len2 > 1.0e-30f) {
        normal = normal * rsqrt(len2);
        if (dot(normal, nInterp) < 0.0f) {
            normal = -normal;
        }
    } else {
        normal = nLen2 > 0.0f ? nInterp * rsqrt(nLen2) : float3(0.0f, 0.0f, 1.0f);
    }
    result.normal = normal;
    result.backFace = dot(d, normal) > 0.0f ? 1u : 0u;
    return result;
}

// ------------------------------------------------------------------------------------------------
// Probe output record (shared by the software and hardware probes; HfRtProbeHitGpu on the CPU side)
// ------------------------------------------------------------------------------------------------

struct HfRtProbeRay {
    float4 originTMin;    // xyz origin, w tMin
    float4 directionTMax; // xyz direction, w tMax
};

struct HfRtProbeHit {
    uint status;
    uint triangle;
    float t;
    uint steps;
    float4 positionU;   // xyz position, w uv.x
    float4 normalV;     // xyz normal, w uv.y
    float4 baryHeight;  // xyz barycentrics, w surface height
};

HfRtProbeHit hf_rt_pack_probe_hit(HfRtHit h, uint triangle) {
    HfRtProbeHit o;
    o.status = h.status;
    o.triangle = triangle;
    o.t = h.t;
    o.steps = h.steps;
    o.positionU = float4(h.position, h.uv.x);
    o.normalV = float4(h.normal, h.uv.y);
    o.baryHeight = float4(h.barycentrics, h.height);
    return o;
}

HfRtProbeHit hf_rt_empty_probe_hit() {
    HfRtProbeHit o;
    o.status = HF_STATUS_MISS;
    o.triangle = 0xffffffffu;
    o.t = 0.0f;
    o.steps = 0u;
    o.positionU = float4(0.0f, 0.0f, 0.0f, 0.0f);
    o.normalV = float4(0.0f, 0.0f, 1.0f, 0.0f);
    o.baryHeight = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return o;
}
