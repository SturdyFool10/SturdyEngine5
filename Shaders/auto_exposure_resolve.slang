// Histogram auto-exposure, pass 2 of 3: reduces the per-tile histograms, meters the clipped histogram
// and adapts the exposure over time.
//
// One 64-thread group. Thread b sums bin b over every tile; thread 0 then walks the 63 luminance bins,
// discarding the darkest `lowPercent` and brightest `highPercent` of the weighted pixel mass (so tiny
// specular highlights and pure-black regions do not steer the exposure), takes the mean log2 luminance
// of what remains, and converts it to the exposure that maps that luminance to `keyValue` (middle
// grey). The result is blended toward the previous exposure in log space with separate speeds for
// brightening (adapting to a darker scene) and darkening, and stored in the persistent 1x1
// `exposureState` texture that pass 3 (and any other feature) reads.
import sturdy_common;

static const uint kHistogramBins = 64;
static const uint kTilesPerRow = 64;

struct ResolveConstants {
    uint tileCount;
    uint reset; // 1 = no valid history: jump straight to the target exposure
    float minLog2Luminance;
    float log2Range;

    float lowPercent;
    float highPercent;
    float keyValue;
    float compensationScale; // 2^EV compensation

    float minExposure;
    float maxExposure;
    float adaptUp;   // 1/seconds
    float adaptDown; // 1/seconds

    float deltaSeconds;
    float pad0;
    float pad1;
    float pad2;
};
#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<ResolveConstants> resolve;
#else
[[push_constant]] ConstantBuffer<ResolveConstants> resolve;
#endif

Texture2D<float> tileHistogram;
[format("r32f")]
RWTexture2D<float> exposureState;

groupshared float sharedHistogram[kHistogramBins];

[shader("compute")]
[numthreads(kHistogramBins, 1, 1)]
void resolveMain(uint3 localId : SV_GroupThreadID) {
    float sum = 0.0;
    for (uint tile = 0u; tile < resolve.tileCount; ++tile) {
        uint2 texel = uint2((tile % kTilesPerRow) * kHistogramBins + localId.x, tile / kTilesPerRow);
        sum += tileHistogram.Load(int3(texel, 0));
    }
    sharedHistogram[localId.x] = sum;
    GroupMemoryBarrierWithGroupSync();

    if (localId.x != 0u) return;

    float total = 0.0;
    for (uint bin = 1u; bin < kHistogramBins; ++bin) total += sharedHistogram[bin];

    float previous = exposureState[uint2(0, 0)];
    bool hasPrevious = resolve.reset == 0u && previous > 0.0;
    if (total < 1.0) {
        // Nothing meterable (a black frame): hold the current exposure.
        if (!hasPrevious) exposureState[uint2(0, 0)] = 1.0;
        return;
    }

    float lowCut = total * resolve.lowPercent;
    float highCut = total * resolve.highPercent;
    float accumulated = 0.0;
    float weightedLog = 0.0;
    float keptWeight = 0.0;
    for (uint bin = 1u; bin < kHistogramBins; ++bin) {
        float count = sharedHistogram[bin];
        float binStart = accumulated;
        float binEnd = accumulated + count;
        accumulated = binEnd;
        // Portion of this bin inside [lowCut, highCut].
        float kept = max(min(binEnd, highCut) - max(binStart, lowCut), 0.0);
        float log2Luminance = resolve.minLog2Luminance +
                              ((float)(bin - 1u) / (float)(kHistogramBins - 2u)) * resolve.log2Range;
        weightedLog += kept * log2Luminance;
        keptWeight += kept;
    }
    float meanLog2 = keptWeight > 0.0 ? weightedLog / keptWeight : resolve.minLog2Luminance;
    float averageLuminance = exp2(meanLog2);
    float target = clamp(resolve.keyValue / max(averageLuminance, 1.0e-5) * resolve.compensationScale,
                         resolve.minExposure, resolve.maxExposure);

    float result = target;
    if (hasPrevious) {
        float speed = target > previous ? resolve.adaptUp : resolve.adaptDown;
        float blend = 1.0 - exp(-max(speed, 0.0) * resolve.deltaSeconds);
        result = exp2(lerp(log2(previous), log2(target), blend));
    }
    exposureState[uint2(0, 0)] = result;
}
