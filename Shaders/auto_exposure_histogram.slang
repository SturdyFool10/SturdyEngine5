// Histogram auto-exposure, pass 1 of 3: per-tile luminance histograms.
//
// Every 16x16-thread group meters one 32x32-pixel tile (each thread takes the top-left sample of a 2x2
// block, so the metering reads a quarter of the pixels) and bins the samples' log2 luminance into a
// 64-bin histogram held in group-shared memory, which it writes as one 64-texel row segment of
// `tileHistogramOut`. Bin 0 collects (near-)black pixels and is ignored by the resolve pass; bins 1..63
// span [minLog2Luminance, minLog2Luminance + log2Range]. Counts are weighted (centre-weighted metering),
// stored as fixed point (weight * 256) in the group-shared bins.
//
// Deliberately avoids global atomics: WebGPU has no atomics on storage textures, so the per-tile results
// are reduced in pass 2 instead.
import sturdy_common;

static const uint kHistogramBins = 64;
static const uint kTileThreads = 16;
static const uint kTilesPerRow = 64; // tiles packed 64 per texture row: width = 64 bins * 64 tiles

struct HistogramConstants {
    uint2 extent;
    uint tilesX;
    uint pad0;

    float minLog2Luminance;
    float invLog2Range;
    float centerWeight;
    float pad1;
};
#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<HistogramConstants> histogram;
#else
[[push_constant]] ConstantBuffer<HistogramConstants> histogram;
#endif

Texture2D<float4> sceneColor;
[format("r32f")]
WTexture2D<float> tileHistogramOut;

groupshared Atomic<uint> sharedBins[kHistogramBins];

[shader("compute")]
[numthreads(kTileThreads, kTileThreads, 1)]
void histogramMain(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID, uint localIndex : SV_GroupIndex) {
    if (localIndex < kHistogramBins) sharedBins[localIndex].store(0u);
    GroupMemoryBarrierWithGroupSync();

    uint2 pixel = groupId.xy * (kTileThreads * 2u) + localId.xy * 2u;
    if (all(pixel < histogram.extent)) {
        float3 color = max(sceneColor.Load(int3(pixel, 0)).rgb, float3(0.0));
        float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
        uint bin = 0u;
        if (luminance > 1.0e-5) {
            float normalized = saturate((log2(luminance) - histogram.minLog2Luminance) * histogram.invLog2Range);
            bin = 1u + (uint)(normalized * (float)(kHistogramBins - 2u) + 0.5);
        }
        float2 uv = (float2(pixel) + 1.0) / float2(histogram.extent);
        float centerDistance = saturate(length(uv - 0.5) * 1.41421356);
        float weight = 1.0 - histogram.centerWeight * centerDistance;
        sharedBins[bin].add((uint)(weight * 256.0 + 0.5));
    }
    GroupMemoryBarrierWithGroupSync();

    if (localIndex < kHistogramBins) {
        uint tileIndex = groupId.y * histogram.tilesX + groupId.x;
        uint2 texel = uint2((tileIndex % kTilesPerRow) * kHistogramBins + localIndex, tileIndex / kTilesPerRow);
        tileHistogramOut.Store(texel, (float)sharedBins[localIndex].load() * (1.0 / 256.0));
    }
}
