// GPU build of the min/max heightfield hierarchy (HierarchyBuild::Compute) — the compute counterpart of
// HeightfieldHierarchy::build/update_region + pack() in Renderer/Displacement/HeightfieldHierarchy.cpp.
// See plans/displacement-system.md section 7.
//
// One dispatch writes ONE rectangle of ONE hierarchy level (level L >= 1; level 0 is never stored, see
// HeightfieldHierarchy::pack). A full build is one whole-level rectangle per level, bottom-up; a dirty-tile
// update is the (small, possibly wrap-split) rectangles HeightfieldHierarchy's dirty-range logic produces
// (plan_hierarchy_build in HeightfieldHierarchy.hpp generates both). Dispatches for level L+1 read what level
// L wrote, so the host must put a compute -> compute buffer barrier between levels.
//
// The output is a plain storage BUFFER laid out like PackedHierarchy (mip m = level m + 1), with a per-mip
// row pitch: universally supported (no storage-image format capability needed, which the RHI cannot query),
// and then a per-mip buffer-to-texture copy — row pitch 256-byte-aligned for D3D12/WebGPU — makes the
// hierarchy texture. The bytes are IDENTICAL to HeightfieldHierarchy::pack for the same precision/channels:
//   * Unorm16 rounds conservatively (max up, min down) exactly like pack(); levels >= 2 reduce in the
//     quantized integer domain, which equals quantizing the float reduction because clamp/ceil/floor are
//     monotone (max(ceil(a), ceil(b)) == ceil(max(a, b))).
//   * Non-power-of-two clipping: a parent with fewer than four children uses only the existing ones (the
//     `cx >= childWidth` skips), and level-1 leaf cells read their four texels with the field's wrap/clamp
//     addressing, exactly like compute_leaf().
//   * texels beyond the real level extent (the power-of-two padding) are written as zero.
//
// Threads are per output *word* (32-bit): a Unorm16 single-channel texel is 16 bits, so one thread writes two
// adjacent texels — no two threads ever share a word, so no atomics are needed. Height reads are Load() only
// and there are no derivatives, so this is WGSL-uniformity-safe anywhere.

import sturdy_common;

struct HierarchyBuildConstants {
    // Level-0 (height texture) size and addressing.
    uint srcWidth;
    uint srcHeight;
    uint wrap;
    // bit 0: Float32 (else Unorm16); bit 1: two channels (min, max) (else max only).
    uint format;
    // Target level >= 1, its real size, and the size of the level below it (level 1: the cell grid = srcWidth/Height).
    uint level;
    uint levelWidth;
    uint levelHeight;
    uint childWidth;
    uint childHeight;
    // Rectangle of the target mip to (re)write, in texels, half-open.
    uint rectX0;
    uint rectY0;
    uint rectX1;
    uint rectY1;
    // Target mip and (for level >= 2) the child mip: word offset inside the buffer and row pitch in words.
    uint outOffsetWords;
    uint outPitchWords;
    uint inOffsetWords;
    uint inPitchWords;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<HierarchyBuildConstants> params;
#else
[[push_constant]] ConstantBuffer<HierarchyBuildConstants> params;
#endif

// Plain Texture2D on WGSL (Slang miscompiles Sampler2D.Load there), combined Sampler2D elsewhere so Vulkan/D3D12
// bindings stay unchanged -- same rule as HfTexture in sturdy_heightfield.slang.
#ifdef SFT_EMULATE_PUSH_CONSTANTS
Texture2D<float4> heightTexture;
#else
Sampler2D heightTexture;
#endif
RWStructuredBuffer<uint> hierarchy;

static const uint FMT_FLOAT32 = 1;
static const uint FMT_TWO_CHANNELS = 2;

float loadHeight(int2 p) {
    int2 size = int2(int(params.srcWidth), int(params.srcHeight));
    int2 q;
    if (params.wrap != 0) {
        int2 m = p % size;
        q = m + select(m < int2(0, 0), size, int2(0, 0));
    } else {
        q = clamp(p, int2(0, 0), size - int2(1, 1));
    }
    return heightTexture.Load(int3(q, 0)).r;
}

uint quantUp(float v) { return uint(min(ceil(clamp(v, 0.0, 1.0) * 65535.0), 65535.0)); }
uint quantDown(float v) { return uint(min(floor(clamp(v, 0.0, 1.0) * 65535.0), 65535.0)); }

struct Bounds {
    float minF;
    float maxF;
    uint minQ;
    uint maxQ;
};

// Bounds of child node (x, y) of the level below: level 1 reads leaf cell (x, y) from the height texture
// (floats); deeper levels read the previous mip of the output buffer.
Bounds readChild(uint x, uint y) {
    Bounds b;
    b.minF = 0.0;
    b.maxF = 0.0;
    b.minQ = 0;
    b.maxQ = 0;
    if (params.level == 1) {
        int2 c = int2(int(x), int(y));
        float h00 = loadHeight(c);
        float h10 = loadHeight(c + int2(1, 0));
        float h01 = loadHeight(c + int2(0, 1));
        float h11 = loadHeight(c + int2(1, 1));
        b.minF = min(min(h00, h10), min(h01, h11));
        b.maxF = max(max(h00, h10), max(h01, h11));
        return b;
    }
    uint row = params.inOffsetWords + y * params.inPitchWords;
    bool twoCh = (params.format & FMT_TWO_CHANNELS) != 0;
    if ((params.format & FMT_FLOAT32) != 0) {
        if (twoCh) {
            b.minF = asfloat(hierarchy[row + 2 * x]);
            b.maxF = asfloat(hierarchy[row + 2 * x + 1]);
        } else {
            b.maxF = asfloat(hierarchy[row + x]);
        }
    } else {
        if (twoCh) {
            uint w = hierarchy[row + x];
            b.minQ = w & 0xFFFF;
            b.maxQ = w >> 16;
        } else {
            uint w = hierarchy[row + (x >> 1)];
            b.maxQ = (w >> ((x & 1) * 16)) & 0xFFFF;
        }
    }
    return b;
}

// Bounds of node (x, y) of the target level, in the target format's own domain (floats for Float32, quantized
// integers for Unorm16). Assumes (x, y) is inside the real level.
Bounds computeNode(uint x, uint y) {
    Bounds acc;
    acc.minF = 1.0e30;
    acc.maxF = -1.0e30;
    acc.minQ = 0xFFFF;
    acc.maxQ = 0;
    for (uint dy = 0; dy < 2; ++dy) {
        for (uint dx = 0; dx < 2; ++dx) {
            uint cx = x * 2 + dx;
            uint cy = y * 2 + dy;
            if (cx >= params.childWidth || cy >= params.childHeight) {
                continue;
            }
            Bounds c = readChild(cx, cy);
            acc.minF = min(acc.minF, c.minF);
            acc.maxF = max(acc.maxF, c.maxF);
            acc.minQ = min(acc.minQ, c.minQ);
            acc.maxQ = max(acc.maxQ, c.maxQ);
        }
    }
    // Level 1 reduced floats; Unorm16 quantizes them now (levels >= 2 already reduced integers).
    if (params.level == 1 && (params.format & FMT_FLOAT32) == 0) {
        acc.minQ = quantDown(acc.minF);
        acc.maxQ = quantUp(acc.maxF);
    }
    return acc;
}

uint texelWord16(uint x, uint y, bool twoCh) {
    // Word for output texel column x in a Unorm16 layout; for single channel this is the pair (x, x + 1).
    if (x >= params.levelWidth || y >= params.levelHeight) {
        return 0;
    }
    Bounds b = computeNode(x, y);
    return twoCh ? (b.minQ | (b.maxQ << 16)) : b.maxQ;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void hierarchyBuildMain(uint3 id : SV_DispatchThreadID) {
    bool isFloat = (params.format & FMT_FLOAT32) != 0;
    bool twoCh = (params.format & FMT_TWO_CHANNELS) != 0;

    uint wordBegin;
    uint wordEnd;
    if (!isFloat && !twoCh) {
        wordBegin = params.rectX0 >> 1;
        wordEnd = (params.rectX1 + 1) >> 1;
    } else if (isFloat && twoCh) {
        wordBegin = params.rectX0 * 2;
        wordEnd = params.rectX1 * 2;
    } else {
        wordBegin = params.rectX0;
        wordEnd = params.rectX1;
    }
    uint wx = wordBegin + id.x;
    uint y = params.rectY0 + id.y;
    if (wx >= wordEnd || y >= params.rectY1) {
        return;
    }

    uint word = 0;
    if (!isFloat) {
        if (twoCh) {
            word = texelWord16(wx, y, true);
        } else {
            uint lo = texelWord16(wx * 2, y, false);
            uint hi = texelWord16(wx * 2 + 1, y, false);
            word = lo | (hi << 16);
        }
    } else {
        uint tx = twoCh ? (wx >> 1) : wx;
        if (tx < params.levelWidth && y < params.levelHeight) {
            Bounds b = computeNode(tx, y);
            float v = (twoCh && (wx & 1) == 0) ? b.minF : b.maxF;
            word = asuint(v);
        }
    }
    hierarchy[params.outOffsetWords + y * params.outPitchWords + wx] = word;
}
