// Heightfield projection blocks — the per-pixel half of the displacement system.
// See plans/displacement-system.md. The CPU reference in
// Renderer/src/Renderer/Displacement/HeightfieldTrace.cpp implements the same algorithms, and the
// names below deliberately match it; change one, change the other.
//
//   import sturdy_heightfield;
//
// Every block answers one question — "where does this ray meet the heightfield?" — behind the same
// signature, so a material picks a rung of the ladder without changing anything else:
//
//     NormalOnly < Parallax < ParallaxOcclusion < CellExact < HierarchicalCellExact
//
// Blocks are types implementing IHeightfieldProjector and are chosen with a generic parameter (or
// the ActiveProjector alias from SFT_HF_ALGORITHM in a material shader), so the choice is resolved at
// compile time: no dynamic dispatch, and unused blocks cost nothing.
//
// Portability rules this file follows on purpose (see memory of the WGSL-uniformity crash on the
// deferred lighting shader):
//   * Every height/hierarchy read is an explicit-LOD Load(): no implicit derivatives inside the
//     data-dependent loops, so there is nothing for WGSL's uniformity analysis to reject.
//   * Derivative-consuming work (hf_resolve_derivatives) is separate and must be called from
//     uniform control flow, *after* the trace has finished, by the caller.
//   * Loops are bounded by maxSteps and never `while(true)`.
// Height data is a single-channel texture normalized to [0, 1] (R channel). Displacement volume is the
// slab 0 <= z <= 1 in normalized height; the surface is z = H(u, v).

import sturdy_common;

// Height / hierarchy texture type. Every read of these is an explicit-LOD Load(); the sampler half of a
// combined Sampler2D is never used. On the WGSL target Slang (2026.11 and 2026.17.1 alike) miscompiles
// Sampler2D.Load -- it emits textureLoad(tex, sampler.xy, sampler.z), i.e. the *sampler variable* in place of
// the coordinate, which Dawn rejects ("cannot index into expression of type 'sampler'") -- so there the blocks
// take a plain Texture2D, the same pattern the Hi-Z build uses. Vulkan/D3D12 keep the combined Sampler2D so
// existing bindings do not change. Declare the module-scope textures as `HfTexture` (not Sampler2D) so the
// declaration follows the target; SFT_EMULATE_PUSH_CONSTANTS is defined for exactly the WGSL sessions.
#ifdef SFT_EMULATE_PUSH_CONSTANTS
typealias HfTexture = Texture2D<float4>;
#else
typealias HfTexture = Sampler2D;
#endif

static const float HF_INF = 1.0e30;
// Cell-space nudge along the direction of travel so a ray on a cell boundary is owned by the cell it
// enters (matches kBoundaryNudge in HeightfieldTrace.cpp).
static const float HF_BOUNDARY_NUDGE = 1.0e-4;
// Slack on the "ray is above everything in this node" test (kSkipSlack on the CPU).
static const float HF_SKIP_SLACK = 1.0e-6;

static const uint HF_STATUS_MISS = 0;
static const uint HF_STATUS_HIT = 1;
static const uint HF_STATUS_BUDGET = 2;

struct HeightfieldRay {
    // (u, v, normalized height). u/v are in tile units; the field repeats every 1.0 when wrapping.
    float3 origin;
    float3 direction;
    float maxT;
};

struct HeightfieldSettings {
    // Level-0 texel dimensions of the height texture.
    int2 texSize;
    // Loop-iteration ceiling (DisplacementPlan::max_traversal_steps, possibly shrunk per object).
    uint maxSteps;
    // Levels in the min/max hierarchy including the implicit level 0 (HeightfieldHierarchy::level_count).
    // The texture holds levels 1..levels-1 as mips 0..levels-2; 1 means "no hierarchy".
    uint hierarchyLevels;
    // World-space displacement amplitude, and the normalized height the base mesh sits at
    // (1 = depth-map convention, 0 = height-map, 0.5 = centred).
    float heightScale;
    float referenceHeight;
    // World size one UV tile covers.
    float2 tileSize;
    // 1 = repeat addressing; 0 = clamp (rays leaving the texel-centre rectangle miss).
    uint wrap;
    // POM: samples and bisection refinements.
    uint pomSteps;
    uint pomRefinements;
    // Hierarchy level the walk starts at (only read with SFT_HF_START_LEVEL_FROM_FOOTPRINT; otherwise the
    // walk starts at the top level). Purely a step-count hint: it never changes the hit. The CPU side of
    // the choice is select_traversal_start_level(); callers set it per pixel/object, clamped to
    // [0, hierarchyLevels - 1] (the shader clamps again).
    uint startLevel = 64u;
};

struct HeightfieldHit {
    uint status;
    float t;
    // Hit position in tile units (not wrapped).
    float2 uv;
    float height;
    // dH/du, dH/dv of the surface at the hit, normalized height per tile unit.
    float2 gradient;
    float residual;
    uint steps;
};

// ------------------------------------------------------------------------------------------------
// Height access
// ------------------------------------------------------------------------------------------------

int2 hf_wrap_texel(int2 p, int2 size) {
    int2 m = p % size;
    return m + select(m < int2(0, 0), size, int2(0, 0));
}

float hf_texel(HfTexture heightTex, int2 p, HeightfieldSettings s) {
    int2 q = (s.wrap != 0) ? hf_wrap_texel(p, s.texSize) : clamp(p, int2(0, 0), s.texSize - int2(1, 1));
    return heightTex.Load(int3(q, 0)).r;
}

// Bilinear patch coefficients a + b*fu + c*fv + d*fu*fv over one cell.
struct HfBilinear {
    float a;
    float b;
    float c;
    float d;
};

HfBilinear hf_make_bilinear(float h00, float h10, float h01, float h11) {
    HfBilinear p;
    p.a = h00;
    p.b = h10 - h00;
    p.c = h01 - h00;
    p.d = h00 - h10 - h01 + h11;
    return p;
}

float hf_eval(HfBilinear p, float fu, float fv) { return p.a + p.b * fu + p.c * fv + p.d * fu * fv; }

HfBilinear hf_cell_loads(HfTexture heightTex, int2 cell, HeightfieldSettings s) {
    return hf_make_bilinear(hf_texel(heightTex, cell, s), hf_texel(heightTex, cell + int2(1, 0), s),
                            hf_texel(heightTex, cell + int2(0, 1), s), hf_texel(heightTex, cell + int2(1, 1), s));
}

// WGSL: HfTexture is a plain Texture2D with no sampler, and textureGather needs one (Slang has no sampler-less
// GatherRed overload), so SFT_HF_USE_GATHER is ignored there and the four-Load() path is used. The planner also
// refuses corner_gather on such backends with a recorded downgrade (DisplacementCapabilities::height_gather).
#if defined(SFT_HF_USE_GATHER) && !defined(SFT_EMULATE_PUSH_CONSTANTS)
// One four-texel gather at the cell's shared corner instead of four Load()s (CPU model:
// HeightfieldGather.hpp, load_cell_via_gather). GatherRed returns (i0,j1), (i1,j1), (i1,j0), (i0,j0) as
// x, y, z, w — j1 being the larger row — so h00 = w, h10 = z, h01 = x, h11 = y.
//
// The gather coordinate is the corner between texel (cx, cy) and (cx+1, cy+1), i.e. (cell + 1) / size: in
// texel-centre space that is cell + 0.5, whose footprint origin floor(.) is unambiguously `cell` (half a texel
// from any rounding boundary). A gather applies the *sampler's* address mode, which this shader cannot know, so
// it is used only where all four texels are in range and addressing is irrelevant; the border row/column
// (where wrap and clamp differ) take the explicit path. No derivatives are involved, so the gather is fine
// inside the data-dependent traversal loop.
HfBilinear hf_cell(HfTexture heightTex, int2 cell, HeightfieldSettings s) {
    int2 last = s.texSize - int2(2, 2);
    if (all(cell >= int2(0, 0)) && all(cell <= last)) {
        float4 g = heightTex.GatherRed((float2(cell) + 1.0) / float2(s.texSize));
        return hf_make_bilinear(g.w, g.z, g.x, g.y);
    }
    return hf_cell_loads(heightTex, cell, s);
}
#else
HfBilinear hf_cell(HfTexture heightTex, int2 cell, HeightfieldSettings s) {
    return hf_cell_loads(heightTex, cell, s);
}
#endif

// Bilinear height at (u, v) in tile units — what a filtered fetch returns, computed from Load()s so it
// is safe inside any control flow.
float hf_sample_height(HfTexture heightTex, float2 uv, HeightfieldSettings s) {
    float2 x = uv * float2(s.texSize) - 0.5;
    float2 fl = floor(x);
    HfBilinear p = hf_cell(heightTex, int2(fl), s);
    float2 f = x - fl;
    return hf_eval(p, f.x, f.y);
}

float2 hf_sample_gradient(HfTexture heightTex, float2 uv, HeightfieldSettings s) {
    float2 x = uv * float2(s.texSize) - 0.5;
    float2 fl = floor(x);
    HfBilinear p = hf_cell(heightTex, int2(fl), s);
    float2 f = x - fl;
    return float2((p.b + p.d * f.y) * float(s.texSize.x), (p.c + p.d * f.x) * float(s.texSize.y));
}

// Tangent-space normal of z = heightScale * H(u / tileSize.x, v / tileSize.y).
float3 hf_normal(float2 gradient, HeightfieldSettings s) {
    return normalize(float3(-s.heightScale * gradient.x / s.tileSize.x, -s.heightScale * gradient.y / s.tileSize.y, 1.0));
}

// ------------------------------------------------------------------------------------------------
// Ray setup
// ------------------------------------------------------------------------------------------------

// viewTS points from the surface toward the eye in tangent space (z > 0). t = 0 lies on the top plane
// of the slab; the ray is extended back from the base surface so the whole volume is traversed.
HeightfieldRay hf_make_view_ray(float2 uv, float3 viewTS, HeightfieldSettings s) {
    float vz = max(viewTS.z, 1.0e-3);
    float3 dir = float3(-viewTS.x / s.tileSize.x, -viewTS.y / s.tileSize.y, -vz / s.heightScale);
    float tTop = (1.0 - s.referenceHeight) / dir.z;
    HeightfieldRay ray;
    ray.origin = float3(uv + dir.xy * tTop, 1.0);
    ray.direction = dir;
    ray.maxT = HF_INF;
    return ray;
}

// ------------------------------------------------------------------------------------------------
// Exact cell solve
// ------------------------------------------------------------------------------------------------

// First s in [0, segmentLength] where the ray meets the bilinear patch; returns false if none. `s` and
// `residual` are valid only on true. Mirrors solve_bilinear_cell().
bool hf_solve_cell(HfBilinear patch, float2 f0, float h0, float2 df, float dh, float segmentLength, out float s,
                   out float residual) {
    float c = h0 - hf_eval(patch, f0.x, f0.y);
    float b = dh - patch.b * df.x - patch.c * df.y - patch.d * (f0.x * df.y + f0.y * df.x);
    float a = -patch.d * df.x * df.y;

    s = 0.0;
    residual = abs(c);
    if (c <= 0.0) {
        return true;
    }

    float limit = segmentLength * (1.0 + 1.0e-4) + 1.0e-7;
    float root = HF_INF;
    float scale = max(1.0, max(abs(b), abs(c)));
    if (abs(a) <= 1.0e-7 * scale) {
        if (b < 0.0) {
            root = -c / b;
        }
    } else {
        float disc = b * b - 4.0 * a * c;
        if (disc >= 0.0) {
            float q = -0.5 * (b + (b >= 0.0 ? sqrt(disc) : -sqrt(disc)));
            float r1 = q / a;
            float r2 = (q != 0.0) ? c / q : HF_INF;
            if (r1 >= 0.0 && r1 < root) {
                root = r1;
            }
            if (r2 >= 0.0 && r2 < root) {
                root = r2;
            }
        }
    }

    bool found = false;
    if (root <= limit) {
        s = min(root, segmentLength);
        found = true;
    } else if (hf_eval(patch, f0.x + df.x * segmentLength, f0.y + df.y * segmentLength) >=
               h0 + dh * segmentLength) {
        // Under the surface at the segment end but no root found: rounding. The far boundary is the
        // tightest point we can vouch for.
        s = segmentLength;
        found = true;
    }
    if (found) {
        residual = abs(h0 + dh * s - hf_eval(patch, f0.x + df.x * s, f0.y + df.y * s));
    }
    return found;
}

// ------------------------------------------------------------------------------------------------
// Traversal (cell-exact, optionally hierarchical)
// ------------------------------------------------------------------------------------------------

HeightfieldHit hf_finish_at(HfTexture heightTex, HeightfieldSettings s, HeightfieldRay ray, uint status, float t,
                            uint steps) {
    HeightfieldHit hit;
    hit.status = status;
    hit.t = t;
    hit.uv = ray.origin.xy + ray.direction.xy * t;
    hit.height = hf_sample_height(heightTex, hit.uv, s);
    hit.gradient = hf_sample_gradient(heightTex, hit.uv, s);
    hit.residual = abs(ray.origin.z + ray.direction.z * t - hit.height);
    hit.steps = steps;
    return hit;
}

HeightfieldHit hf_miss() {
    HeightfieldHit hit;
    hit.status = HF_STATUS_MISS;
    hit.t = 0.0;
    hit.uv = float2(0.0, 0.0);
    hit.height = 0.0;
    hit.gradient = float2(0.0, 0.0);
    hit.residual = 0.0;
    hit.steps = 0;
    return hit;
}

int hf_floor_div(int a, int b) { return (a >= 0) ? (a / b) : -((-a + b - 1) / b); }

// Shared traversal. `hierarchyTex` is only read when useHierarchy is true and s.hierarchyLevels > 1.
// Mirrors trace_grid() step for step.
HeightfieldHit hf_trace_grid(HfTexture heightTex, HfTexture hierarchyTex, bool useHierarchy, HeightfieldSettings s,
                             HeightfieldRay ray) {
    int2 size = s.texSize;
    float2 sizeF = float2(size);
    // Cell space: one unit = one texel; texel centres on integers.
    float3 o = float3(ray.origin.xy * sizeF - 0.5, ray.origin.z);
    float3 d = float3(ray.direction.xy * sizeF, ray.direction.z);

    // Slab and (for non-wrapping fields) rectangle clipping.
    float tEnter = 0.0;
    float tExit = ray.maxT;
    bool bottomLimited = false;
    bool empty = false;
    if (o.z < 0.0 || (o.z > 1.0 && d.z >= 0.0)) {
        empty = true;
    } else {
        if (o.z > 1.0) {
            tEnter = (1.0 - o.z) / d.z;
        }
        if (d.z < 0.0) {
            float bottom = -o.z / d.z;
            if (bottom <= tExit) {
                tExit = bottom;
                bottomLimited = true;
            }
        } else if (d.z > 0.0) {
            tExit = min(tExit, (1.0 - o.z) / d.z);
        }
        if (s.wrap == 0) {
            float2 hi = sizeF - 1.0;
            float2 oc = o.xy;
            float2 dc = d.xy;
            for (int axis = 0; axis < 2; ++axis) {
                if (dc[axis] == 0.0) {
                    if (oc[axis] < 0.0 || oc[axis] > hi[axis]) {
                        empty = true;
                    }
                } else {
                    float t0 = (0.0 - oc[axis]) / dc[axis];
                    float t1 = (hi[axis] - oc[axis]) / dc[axis];
                    float lo = min(t0, t1);
                    float up = max(t0, t1);
                    tEnter = max(tEnter, lo);
                    if (up < tExit) {
                        tExit = up;
                        bottomLimited = false;
                    }
                }
            }
        }
        empty = empty || !(tEnter < tExit);
    }
    if (empty) {
        return hf_miss();
    }

    bool hier = useHierarchy && s.hierarchyLevels > 1;
    int topLevel = hier ? int(s.hierarchyLevels) - 1 : 0;
#ifdef SFT_HF_START_LEVEL_FROM_FOOTPRINT
    int level = min(int(s.startLevel), topLevel);
#else
    int level = topLevel;
#endif
    float t = tEnter;
    uint steps = 0;

    for (; steps < s.maxSteps && t < tExit; ++steps) {
        float2 probe = o.xy + d.xy * t + sign(d.xy) * HF_BOUNDARY_NUDGE;
        int2 wi = int2(floor(probe));
        int2 baseCell = int2(0, 0);
        if (s.wrap != 0) {
            baseCell = int2(hf_floor_div(wi.x, size.x), hf_floor_div(wi.y, size.y));
        } else {
            wi = clamp(wi, int2(0, 0), size - int2(1, 1));
        }
        int2 cell = wi - baseCell * size;

        int2 k = cell >> level;
        int2 loI = baseCell * size + (k << level);
        int2 hiI = baseCell * size + min((k + int2(1, 1)) << level, size);
        float2 lo = float2(loI);
        float2 hi = float2(hiI);

        float tx = (d.x > 0.0) ? (hi.x - o.x) / d.x : ((d.x < 0.0) ? (lo.x - o.x) / d.x : HF_INF);
        float ty = (d.y > 0.0) ? (hi.y - o.y) / d.y : ((d.y < 0.0) ? (lo.y - o.y) / d.y : HF_INF);
        float tNext = min(min(tx, ty), tExit);
        if (!(tNext > t)) {
            tNext = t + max(abs(t) * 1.2e-7, 1.0e-30);
        }

        float hA = o.z + d.z * t;
        float hB = o.z + d.z * tNext;
        float rayLow = min(hA, hB);

        if (level > 0) {
            float nodeMax = hierarchyTex.Load(int3(k, level - 1)).r;
            if (rayLow > nodeMax + HF_SKIP_SLACK) {
                t = tNext;
                level = min(level + 1, topLevel);
            } else {
                level -= 1;
            }
            continue;
        }

        HfBilinear patch = hf_cell(heightTex, cell, s);
        float cellMax = max(max(patch.a, patch.a + patch.b), max(patch.a + patch.c, patch.a + patch.b + patch.c + patch.d));
        if (rayLow > cellMax + HF_SKIP_SLACK) {
            t = tNext;
            level = min(level + 1, topLevel);
            continue;
        }

        float2 f0 = (o.xy + d.xy * t) - float2(wi);
        float sHit;
        float residual;
        if (hf_solve_cell(patch, f0, hA, d.xy, d.z, tNext - t, sHit, residual)) {
            float hitT = t + sHit;
            float2 f = clamp((o.xy + d.xy * hitT) - float2(wi), float2(0.0, 0.0), float2(1.0, 1.0));
            HeightfieldHit hit;
            hit.status = HF_STATUS_HIT;
            hit.t = hitT;
            hit.uv = (o.xy + d.xy * hitT + 0.5) / sizeF;
            hit.height = hf_eval(patch, f.x, f.y);
            hit.gradient = float2((patch.b + patch.d * f.y) * sizeF.x, (patch.c + patch.d * f.x) * sizeF.y);
            hit.residual = residual;
            hit.steps = steps + 1;
            return hit;
        }
        t = tNext;
    }

    if (t < tExit) {
        return hf_finish_at(heightTex, s, ray, HF_STATUS_BUDGET, t, steps);
    }
    if (bottomLimited) {
        // A descending ray that reached the slab floor must be at/under the surface; report the floor
        // crossing rather than a spurious miss if per-cell rounding disagreed.
        return hf_finish_at(heightTex, s, ray, HF_STATUS_HIT, tExit, steps);
    }
    return hf_miss();
}

// ------------------------------------------------------------------------------------------------
// POM and parallax
// ------------------------------------------------------------------------------------------------

HeightfieldHit hf_trace_pom(HfTexture heightTex, HeightfieldSettings s, HeightfieldRay ray) {
    float3 o = ray.origin;
    float3 d = ray.direction;
    float tEnter = 0.0;
    float tExit = ray.maxT;
    if (o.z < 0.0 || (o.z > 1.0 && d.z >= 0.0)) {
        return hf_miss();
    }
    if (o.z > 1.0) {
        tEnter = (1.0 - o.z) / d.z;
    }
    if (d.z < 0.0) {
        tExit = min(tExit, -o.z / d.z);
    } else if (d.z > 0.0) {
        tExit = min(tExit, (1.0 - o.z) / d.z);
    }
    if (!(tEnter < tExit)) {
        return hf_miss();
    }

    uint samples = max(s.pomSteps, 1u);
    float dt = (tExit - tEnter) / float(samples);
    float prevT = tEnter;
    float prevGap = (o.z + d.z * prevT) - hf_sample_height(heightTex, o.xy + d.xy * prevT, s);
    if (prevGap <= 0.0) {
        return hf_finish_at(heightTex, s, ray, HF_STATUS_HIT, prevT, 0);
    }
    // A descending ray that runs to the slab floor is under the surface there by definition (mirrors the
    // CPU march_and_bisect): the last sample of a floor-touching ray is otherwise decided by rounding noise.
    bool floorLimited = d.z < 0.0 && (-o.z / d.z) <= ray.maxT;
    for (uint i = 1; i <= samples; ++i) {
        float t = tEnter + dt * float(i);
        float gap = (o.z + d.z * t) - hf_sample_height(heightTex, o.xy + d.xy * t, s);
        if ((floorLimited && i == samples) || gap <= 0.0) {
            float lo = prevT;
            float hi = t;
            for (uint r = 0; r < s.pomRefinements; ++r) {
                float mid = 0.5 * (lo + hi);
                float midGap = (o.z + d.z * mid) - hf_sample_height(heightTex, o.xy + d.xy * mid, s);
                if (midGap <= 0.0) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            return hf_finish_at(heightTex, s, ray, HF_STATUS_HIT, 0.5 * (lo + hi), i);
        }
        prevT = t;
    }
    return hf_miss();
}

// Single-offset parallax: one height fetch at the base uv shifts the uv along the view direction.
// Always "hits"; there is no visibility, only a plausible shift.
HeightfieldHit hf_parallax(HfTexture heightTex, HeightfieldSettings s, float2 uv, float3 viewTS) {
    float vz = max(viewTS.z, 0.25); // offset limiting: bound the shift at grazing angles
    float h = hf_sample_height(heightTex, uv, s);
    // Depth below the base surface, in normalized units: zero at the reference height.
    float depth = s.referenceHeight - h;
    float2 shift = -viewTS.xy / vz * (depth * s.heightScale) / s.tileSize;
    HeightfieldHit hit;
    hit.status = HF_STATUS_HIT;
    hit.t = 0.0;
    hit.uv = uv + shift;
    hit.height = hf_sample_height(heightTex, hit.uv, s);
    hit.gradient = hf_sample_gradient(heightTex, hit.uv, s);
    hit.residual = 0.0;
    hit.steps = 1;
    return hit;
}

// ------------------------------------------------------------------------------------------------
// Interchangeable projector blocks
// ------------------------------------------------------------------------------------------------

// A projector resolves one shaded point. `uv` is the base-surface coordinate and `viewTS` the
// tangent-space direction to the eye; blocks that trace build their own ray from them.
interface IHeightfieldProjector {
    static HeightfieldHit project(HfTexture heightTex, HfTexture hierarchyTex, HeightfieldSettings s, float2 uv,
                                  float3 viewTS);
}

// Rung 0: no visibility change; the "hit" is the base point with the true surface gradient.
struct NormalOnlyProjector : IHeightfieldProjector {
    static HeightfieldHit project(HfTexture heightTex, HfTexture hierarchyTex, HeightfieldSettings s, float2 uv,
                                  float3 viewTS) {
        HeightfieldHit hit;
        hit.status = HF_STATUS_HIT;
        hit.t = 0.0;
        hit.uv = uv;
        hit.height = hf_sample_height(heightTex, uv, s);
        hit.gradient = hf_sample_gradient(heightTex, uv, s);
        hit.residual = 0.0;
        hit.steps = 0;
        return hit;
    }
}

struct ParallaxProjector : IHeightfieldProjector {
    static HeightfieldHit project(HfTexture heightTex, HfTexture hierarchyTex, HeightfieldSettings s, float2 uv,
                                  float3 viewTS) {
        return hf_parallax(heightTex, s, uv, viewTS);
    }
}

struct PomProjector : IHeightfieldProjector {
    static HeightfieldHit project(HfTexture heightTex, HfTexture hierarchyTex, HeightfieldSettings s, float2 uv,
                                  float3 viewTS) {
        return hf_trace_pom(heightTex, s, hf_make_view_ray(uv, viewTS, s));
    }
}

struct CellExactProjector : IHeightfieldProjector {
    static HeightfieldHit project(HfTexture heightTex, HfTexture hierarchyTex, HeightfieldSettings s, float2 uv,
                                  float3 viewTS) {
        return hf_trace_grid(heightTex, hierarchyTex, false, s, hf_make_view_ray(uv, viewTS, s));
    }
}

struct HierarchicalProjector : IHeightfieldProjector {
    static HeightfieldHit project(HfTexture heightTex, HfTexture hierarchyTex, HeightfieldSettings s, float2 uv,
                                  float3 viewTS) {
        return hf_trace_grid(heightTex, hierarchyTex, true, s, hf_make_view_ray(uv, viewTS, s));
    }
}

// Compile-time selection for material shaders. Values match HeightfieldAlgorithm's enumerators.
#define SFT_HF_NORMAL_ONLY 0
#define SFT_HF_PARALLAX 1
#define SFT_HF_POM 2
#define SFT_HF_CELL_EXACT 3
#define SFT_HF_HIERARCHICAL 4

#ifndef SFT_HF_ALGORITHM
#define SFT_HF_ALGORITHM SFT_HF_HIERARCHICAL
#endif

#if SFT_HF_ALGORITHM == SFT_HF_NORMAL_ONLY
typealias ActiveProjector = NormalOnlyProjector;
#elif SFT_HF_ALGORITHM == SFT_HF_PARALLAX
typealias ActiveProjector = ParallaxProjector;
#elif SFT_HF_ALGORITHM == SFT_HF_POM
typealias ActiveProjector = PomProjector;
#elif SFT_HF_ALGORITHM == SFT_HF_CELL_EXACT
typealias ActiveProjector = CellExactProjector;
#else
typealias ActiveProjector = HierarchicalProjector;
#endif

// ------------------------------------------------------------------------------------------------
// Self-shadowing
// ------------------------------------------------------------------------------------------------

// True when the surface blocks the path from the shaded point toward the light. Uses the same
// traversal as the view ray, so a cheaper quality tier is just a smaller maxSteps (and a coarser start).
// Settings for a shadow ray under a SelfShadowBudget: caller passes self_shadow_max_steps() and
// self_shadow_start_level() (Renderer/Displacement/DisplacementPlanner.hpp) for the plan's quality.
HeightfieldSettings hf_shadow_settings(HeightfieldSettings s, uint shadowMaxSteps, uint shadowStartLevel) {
    s.maxSteps = shadowMaxSteps;
    s.startLevel = shadowStartLevel;
    return s;
}

bool hf_trace_shadow(HfTexture heightTex, HfTexture hierarchyTex, bool useHierarchy, HeightfieldSettings s,
                     float2 surfaceUv, float surfaceHeight, float3 lightTS, float bias) {
    if (lightTS.z <= 0.0) {
        return true;
    }
    HeightfieldRay ray;
    ray.origin = float3(surfaceUv, surfaceHeight + bias);
    ray.direction = float3(lightTS.xy / s.tileSize, lightTS.z / s.heightScale);
    ray.maxT = HF_INF;
    return hf_trace_grid(heightTex, hierarchyTex, useHierarchy, s, ray).status == HF_STATUS_HIT;
}

// ------------------------------------------------------------------------------------------------
// Depth and derivatives
// ------------------------------------------------------------------------------------------------

// Displaced tangent-space position of a hit, relative to the base surface point: xy in world units
// across the surface, z along the normal.
float3 hf_hit_offset_ts(HeightfieldHit hit, float2 baseUv, HeightfieldSettings s) {
    float2 xy = (hit.uv - baseUv) * s.tileSize;
    float z = (hit.height - s.referenceHeight) * s.heightScale;
    return float3(xy, z);
}

// Warped derivatives (plans/displacement.md section 13). Call from UNIFORM control flow after the
// projector has returned — ddx/ddy of the hit uv, clamped against the base uv's own derivatives so a
// discontinuity between neighbouring lanes' hits (a cliff, a miss) cannot blow the footprint up.
void hf_resolve_derivatives(float2 hitUv, float2 baseDdx, float2 baseDdy, out float2 dUVdx, out float2 dUVdy) {
    float2 dx = ddx(hitUv);
    float2 dy = ddy(hitUv);
    float baseX = max(length(baseDdx), 1.0e-9);
    float baseY = max(length(baseDdy), 1.0e-9);
    float sx = clamp(length(dx) / baseX, 0.5, 8.0);
    float sy = clamp(length(dy) / baseY, 0.5, 8.0);
    dUVdx = baseDdx * sx;
    dUVdy = baseDdy * sy;
}
