// Shared constants, view-space reconstruction, and the deterministic sampling sequence used by the
// three XeGTAO-style ambient-occlusion passes (prefilter -> main -> denoise).
//
// Algorithm reference: Jimenez et al., "Practical Realtime Strategies for Accurate Indirect
// Occlusion" (2016), as productized by Intel's XeGTAO. Everything temporal in XeGTAO is
// deliberately absent here: this renderer has no TAA/temporal reconstruction to clean up after a
// frame-varying sampling pattern, so the pattern is a pure function of the pixel coordinate and the
// 5x5 spatial denoiser is the only noise-removal stage. See Shaders/gtao_main.slang.
import sturdy_common;

// XeGTAO's depth hierarchy depth. Mip 0 is full resolution linear view depth; mips 1..4 exist so
// far-away taps read a coarser (and much more cache-friendly) representation.
static const uint SFT_GTAO_DEPTH_MIP_LEVELS = 5;

struct GtaoConstants {
    // World -> view. Only used to rotate the G-buffer's world-space normal into view space; the
    // pass never reconstructs position from it (that comes from linear depth, which is cheaper and
    // avoids a second matrix multiply per tap).
    float4x4 view;

    // xy = viewport size in pixels, zw = 1 / viewport size.
    float4 viewportSizePixelSize;

    // Screen UV -> view-space ray scale: viewPos.xy = (mul * uv + add) * viewZ.
    // xy = mul, zw = add. Derived on the CPU from the projection matrix so this shader stays
    // agnostic to the projection convention (and never has to invert a matrix per pixel).
    float4 ndcToViewMulAdd;

    // x = effect radius (world units), y = falloff range as a fraction of the radius,
    // z = final value power, w = thin-occluder compensation.
    float4 radiusFalloffPowerThin;

    // x = sample distance distribution power (2 => x^2 bias toward the shaded pixel),
    // y = depth-mip sampling offset, z = slice count, w = steps per slice.
    float4 sampleParams;

    // x, y = depth linearization (viewZ = x / (y - deviceDepth)),
    // z = screen-border fade width in pixels, w = AO intensity (0 => no occlusion, 1 => full).
    float4 depthLinearizeEdgeIntensity;
};

// Device depth -> positive view-space distance from the camera plane.
//
// Matches glm::perspectiveRH_ZO (depth 0 at the near plane, 1 at the far plane): with
// A = near*far/(far-near) and B = far/(far-near), viewZ = A / (B - deviceDepth).
float gtaoLinearizeDepth(float deviceDepth, GtaoConstants gtao) {
    return gtao.depthLinearizeEdgeIntensity.x /
           max(gtao.depthLinearizeEdgeIntensity.y - deviceDepth, SFT_EPSILON);
}

// View-space position for a screen UV at a given linear view depth.
//
// The returned space is XeGTAO's: X right, Y up, Z *positive* into the screen. That is the engine's
// right-handed view space with Z negated, which is why gtaoViewSpaceNormal() negates z as well —
// mixing the two conventions silently mirrors every horizon and is the single easiest way to get a
// plausible-looking but wrong AO term.
float3 gtaoViewSpacePosition(float2 uv, float viewZ, GtaoConstants gtao) {
    float2 xy = (gtao.ndcToViewMulAdd.xy * uv + gtao.ndcToViewMulAdd.zw) * viewZ;
    return float3(xy, viewZ);
}

// World-space G-buffer normal -> the view space described above.
float3 gtaoViewSpaceNormal(float3 worldNormal, GtaoConstants gtao) {
    float3 viewNormal = mul(float4(worldNormal, 0.0), gtao.view).xyz;
    return safeNormalizeOr(float3(viewNormal.x, viewNormal.y, -viewNormal.z), float3(0.0, 0.0, -1.0));
}

// Cheap acos approximation from XeGTAO. Accurate to well under a degree over [-1, 1], which is far
// below the angular resolution the horizon search itself resolves.
float gtaoFastAcos(float x) {
    float ax = abs(x);
    float res = (-0.156583 * ax + SFT_PI * 0.5) * sqrt(1.0 - ax);
    return x >= 0.0 ? res : SFT_PI - res;
}

// Depth-similarity weighted 2x2 reduction used to build the depth mip chain.
//
// A plain average would smear foreground depth into background across every silhouette, and the
// main pass would then read a horizon that exists nowhere in the scene. Weighting each of the four
// texels by how close it is to the *farthest* of them keeps a mip texel anchored to one surface.
float gtaoDepthMipFilter(float d0, float d1, float d2, float d3, GtaoConstants gtao) {
    float maxDepth = max(max(d0, d1), max(d2, d3));

    // 0.75 matches XeGTAO: the mip chain is only consulted for taps well inside the effect radius,
    // so the discontinuity threshold is deliberately tighter than the radius itself.
    float effectRadius = 0.75 * gtao.radiusFalloffPowerThin.x;
    float falloffRange = max(gtao.radiusFalloffPowerThin.y * effectRadius, SFT_EPSILON);
    float falloffFrom = effectRadius * (1.0 - gtao.radiusFalloffPowerThin.y);
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = falloffFrom / falloffRange + 1.0;

    float w0 = saturate((maxDepth - d0) * falloffMul + falloffAdd);
    float w1 = saturate((maxDepth - d1) * falloffMul + falloffAdd);
    float w2 = saturate((maxDepth - d2) * falloffMul + falloffAdd);
    float w3 = saturate((maxDepth - d3) * falloffMul + falloffAdd);

    float weightSum = w0 + w1 + w2 + w3;
    if (weightSum < SFT_EPSILON) {
        return 0.25 * (d0 + d1 + d2 + d3);
    }
    return (w0 * d0 + w1 * d1 + w2 * d2 + w3 * d3) / weightSum;
}

// Hilbert curve index of a pixel inside a 64x64 tile.
//
// XeGTAO feeds this into an R2 low-discrepancy sequence to pick each pixel's slice rotation and
// sample offset. The Hilbert ordering is what makes the result well distributed *spatially*: any
// small neighbourhood of pixels receives a well-spread set of sequence indices, so the residual
// error the denoiser has to remove is high frequency rather than a large-scale blotch. A 64x64 tile
// is large enough that the repeat is not visible as structure, unlike a 4x4 dither.
uint gtaoHilbertIndex(uint2 position) {
    const uint width = 64;
    uint index = 0;
    for (uint level = width / 2; level > 0; level /= 2) {
        uint regionX = (position.x & level) > 0 ? 1 : 0;
        uint regionY = (position.y & level) > 0 ? 1 : 0;
        index += level * level * ((3 * regionX) ^ regionY);
        if (regionY == 0) {
            if (regionX == 1) {
                position.x = (width - 1) - position.x;
                position.y = (width - 1) - position.y;
            }
            uint temp = position.x;
            position.x = position.y;
            position.y = temp;
        }
    }
    return index;
}

// Per-pixel (slice rotation, sample offset) pair.
//
// This is XeGTAO's SpatioTemporalNoise with the temporal term removed: no frame index participates,
// so a static camera produces a bit-identical AO buffer every frame and there is nothing for the
// eye to read as crawl or shimmer. The two multipliers are the R2 sequence's irrational constants.
float2 gtaoSpatialNoise(uint2 pixel) {
    uint index = gtaoHilbertIndex(pixel % 64);
    return frac(0.5 + float(index) * float2(0.75487766624669276005, 0.5698402909980532659114));
}
