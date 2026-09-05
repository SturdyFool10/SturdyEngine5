// GTAO pass 3: the 5x5 edge-aware spatial denoiser.
//
// This is not an optional polish step - it is half the algorithm. 18 samples per pixel cannot
// resolve the visibility integral on their own; XeGTAO's answer is a small, aggressively
// edge-aware filter that removes the residual sampling structure while leaving contact darkening
// and thin crevices intact. Stock XeGTAO leans on a temporal pass for the rest, which this
// renderer does not have, so the filter here is the last word on noise.
//
// Two things make it edge-aware rather than a blur:
//
//  * The depth-discontinuity edges written by gtao_main.slang. A tap's weight is the strongest
//    monotone path of edge weights from the center pixel to that tap (a tiny 5x5 dynamic program),
//    so AO cannot leak around a silhouette, only along a continuous surface.
//  * Normal agreement, which separates coplanar-in-depth but differently-oriented surfaces - the
//    two faces meeting in a corner are within a texel of each other in depth, and blurring the
//    corner's contact darkening across them is exactly the artifact that makes AO look painted on.
//
// The 8x8 group stages a 12x12 neighbourhood in group-shared memory: the 5x5 footprint means every
// texel is read up to 25 times, and doing that through LDS instead of the texture cache is the
// single biggest bandwidth saving available here.
import sturdy_common;
import gtao_common;

ConstantBuffer<GtaoConstants> gtao;
Texture2D<float> rawAmbientOcclusion;
Texture2D<float4> gtaoEdges;
Texture2D<float2> gbufferNormal;
// WGSL's texture_storage_2d requires a format in the type itself, unlike HLSL/SPIR-V where a UAV's
// format comes from the runtime-bound view -- must match the RHI::Format the AO texture is actually
// created with (RHI::Format::R32Float, see Renderer's GTAO target creation). Write-only since this
// pass only ever writes it.
[format("r32f")]
WTexture2D<float> ambientOcclusionOut;

static const int SFT_GTAO_DENOISE_RADIUS = 2;
static const int SFT_GTAO_TILE_WIDTH = 8 + 2 * SFT_GTAO_DENOISE_RADIUS;
static const int SFT_GTAO_TILE_TEXELS = SFT_GTAO_TILE_WIDTH * SFT_GTAO_TILE_WIDTH;

groupshared float gtaoTileAo[SFT_GTAO_TILE_TEXELS];
groupshared float4 gtaoTileEdges[SFT_GTAO_TILE_TEXELS];
groupshared float3 gtaoTileNormals[SFT_GTAO_TILE_TEXELS];

int tileIndex(int2 tileCoord) {
    return tileCoord.y * SFT_GTAO_TILE_WIDTH + tileCoord.x;
}

// Edge weight for a single step between two adjacent tile texels.
//
// Edges are stored per pixel as (left, right, top, bottom) and are not perfectly reciprocal - a
// foreground pixel and the background pixel behind it can disagree about how strong the step is.
// Taking the minimum makes the barrier symmetric, so the filter never leaks in one direction only
// (which shows up as a one-sided bright or dark fringe along a silhouette).
float stepEdgeWeight(int fromIndex, int toIndex, int2 direction) {
    float4 fromEdges = gtaoTileEdges[fromIndex];
    float4 toEdges = gtaoTileEdges[toIndex];
    if (direction.x > 0) return min(fromEdges.y, toEdges.x);
    if (direction.x < 0) return min(fromEdges.x, toEdges.y);
    if (direction.y > 0) return min(fromEdges.w, toEdges.z);
    return min(fromEdges.z, toEdges.w);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void gtaoDenoiseMain(uint3 dispatchThreadId : SV_DispatchThreadID,
                     uint3 groupThreadId : SV_GroupThreadID,
                     uint3 groupId : SV_GroupID) {
    int2 extent = int2(gtao.viewportSizePixelSize.xy);
    int2 maxPixel = extent - int2(1, 1);
    int2 tileOrigin = int2(groupId.xy) * 8 - SFT_GTAO_DENOISE_RADIUS;

    // Cooperative staging: 64 threads fill 144 texels in three strided passes.
    uint linearThread = groupThreadId.y * 8 + groupThreadId.x;
    for (uint slot = linearThread; slot < uint(SFT_GTAO_TILE_TEXELS); slot += 64) {
        int2 tileCoord = int2(int(slot) % SFT_GTAO_TILE_WIDTH, int(slot) / SFT_GTAO_TILE_WIDTH);
        int2 pixel = clamp(tileOrigin + tileCoord, int2(0, 0), maxPixel);
        gtaoTileAo[slot] = rawAmbientOcclusion.Load(int3(pixel, 0));
        gtaoTileEdges[slot] = gtaoEdges.Load(int3(pixel, 0));
        gtaoTileNormals[slot] = decodeOctahedralNormal(gbufferNormal.Load(int3(pixel, 0)));
    }
    GroupMemoryBarrierWithGroupSync();

    if (any(dispatchThreadId.xy >= uint2(extent))) return;
    int2 pixel = int2(dispatchThreadId.xy);
    int2 center = int2(groupThreadId.xy) + int2(SFT_GTAO_DENOISE_RADIUS, SFT_GTAO_DENOISE_RADIUS);

    // pathWeight[x][y] = strongest product of edge weights along any monotone axis-aligned path
    // from the center to that offset. Filled outward so every predecessor is already final.
    float pathWeight[5][5];
    const int c = SFT_GTAO_DENOISE_RADIUS;
    pathWeight[c][c] = 1.0;
    for (int px = c + 1; px < 5; ++px) {
        pathWeight[px][c] = pathWeight[px - 1][c] *
            stepEdgeWeight(tileIndex(center + int2(px - 1 - c, 0)), tileIndex(center + int2(px - c, 0)), int2(1, 0));
    }
    for (int nx = c - 1; nx >= 0; --nx) {
        pathWeight[nx][c] = pathWeight[nx + 1][c] *
            stepEdgeWeight(tileIndex(center + int2(nx + 1 - c, 0)), tileIndex(center + int2(nx - c, 0)), int2(-1, 0));
    }
    for (int py = c + 1; py < 5; ++py) {
        pathWeight[c][py] = pathWeight[c][py - 1] *
            stepEdgeWeight(tileIndex(center + int2(0, py - 1 - c)), tileIndex(center + int2(0, py - c)), int2(0, 1));
    }
    for (int ny = c - 1; ny >= 0; --ny) {
        pathWeight[c][ny] = pathWeight[c][ny + 1] *
            stepEdgeWeight(tileIndex(center + int2(0, ny + 1 - c)), tileIndex(center + int2(0, ny - c)), int2(0, -1));
    }
    // Quadrants: each texel inherits the better of "arrive horizontally" and "arrive vertically".
    for (int qx = 0; qx < 2; ++qx) {
        for (int qy = 0; qy < 2; ++qy) {
            int2 stepDir = int2(qx == 0 ? 1 : -1, qy == 0 ? 1 : -1);
            for (int i = 1; i <= c; ++i) {
                for (int j = 1; j <= c; ++j) {
                    int2 offset = int2(i, j) * stepDir;
                    int2 slotIndex = offset + int2(c, c);
                    int here = tileIndex(center + offset);
                    float fromX = pathWeight[slotIndex.x - stepDir.x][slotIndex.y] *
                        stepEdgeWeight(tileIndex(center + offset - int2(stepDir.x, 0)), here, int2(stepDir.x, 0));
                    float fromY = pathWeight[slotIndex.x][slotIndex.y - stepDir.y] *
                        stepEdgeWeight(tileIndex(center + offset - int2(0, stepDir.y)), here, int2(0, stepDir.y));
                    pathWeight[slotIndex.x][slotIndex.y] = max(fromX, fromY);
                }
            }
        }
    }

    // Mild spatial taper. Deliberately gentle: the edge term is what stops the filter, and a
    // steeper kernel would just under-use the 5x5 footprint the sample count was chosen for.
    const float spatialWeights[3] = {1.0, 0.75, 0.35};
    float3 centerNormal = gtaoTileNormals[tileIndex(center)];

    float sum = 0.0;
    float sumWeight = 0.0;
    for (int dy = -c; dy <= c; ++dy) {
        for (int dx = -c; dx <= c; ++dx) {
            int tap = tileIndex(center + int2(dx, dy));
            float weight = spatialWeights[abs(dx)] * spatialWeights[abs(dy)];
            weight *= pathWeight[dx + c][dy + c];
            // Normal agreement, raised to a modest power: coplanar taps pass unattenuated, a
            // 90-degree corner is cut to nothing, and the transition is smooth enough that
            // gently curved surfaces do not develop a visible seam.
            float normalAgreement = saturate(dot(gtaoTileNormals[tap], centerNormal));
            weight *= normalAgreement * normalAgreement * normalAgreement * normalAgreement;
            sum += gtaoTileAo[tap] * weight;
            sumWeight += weight;
        }
    }

    // Intensity was already folded in by gtao_main.slang; a normalized weighted average of
    // lerp(1, ao, k) equals lerp(1, average, k), so nothing more is needed here.
    float denoised = sumWeight > SFT_EPSILON ? sum / sumWeight : gtaoTileAo[tileIndex(center)];
    ambientOcclusionOut.Store(pixel, saturate(denoised));
}
