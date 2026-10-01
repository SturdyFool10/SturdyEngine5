// Shared ReSTIR GI reservoir storage and low-resolution-grid helpers, imported by every restir_gi_*.slang
// pass so the packed layout has exactly one definition (mirrored by Renderer::ReservoirGpuData in
// RestirGi.hpp — 8 bytes).
//
// A reservoir only has to carry what later passes actually read: the kept sample's outgoing radiance, the
// unbiased contribution weight W and the (clamped) sample count M. The sample's world position and hit
// normal used to be stored too (64 bytes per pixel) but nothing after the initial-sample pass consumes
// them — the resolve pass works on demodulated radiance*W — so they are gone. The running weight sum only
// lives inside a single pass' registers. Validity is encoded as M > 0.
//
// Layout: x = RGB9E5 shared-exponent radiance, y = half(W) | half(M) << 16.
import sturdy_common;

struct ReservoirGpuData {
    uint radianceRgb9e5;
    uint weightMHalf;
};

static const float3 SFT_RESTIR_LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);

uint restirPackRgb9e5(float3 color) {
    // 9-bit mantissas, 5-bit shared exponent, bias 15. Max representable value is 65408.
    float3 clamped = clamp(color, float3(0.0, 0.0, 0.0), float3(65408.0, 65408.0, 65408.0));
    float maxChannel = max(max(clamped.r, clamped.g), clamped.b);
    int sharedExponent = max(-16, (int)floor(log2(max(maxChannel, 1.0e-20)))) + 16;
    float denominator = exp2((float)(sharedExponent - 24));
    int maxMantissa = (int)floor(maxChannel / denominator + 0.5);
    if (maxMantissa == 512) {
        denominator *= 2.0;
        sharedExponent += 1;
    }
    uint3 mantissa = (uint3)floor(clamped / denominator + 0.5);
    return ((uint)sharedExponent << 27) | (mantissa.b << 18) | (mantissa.g << 9) | mantissa.r;
}

float3 restirUnpackRgb9e5(uint packedColor) {
    float scale = exp2((float)((int)(packedColor >> 27) - 24));
    return float3((float)(packedColor & 511u), (float)((packedColor >> 9) & 511u),
                  (float)((packedColor >> 18) & 511u)) * scale;
}

ReservoirGpuData restirMakeReservoir(float3 radiance, float contributionWeight, float sampleCount) {
    ReservoirGpuData reservoir;
    reservoir.radianceRgb9e5 = restirPackRgb9e5(radiance);
    reservoir.weightMHalf = f32tof16(contributionWeight) | (f32tof16(sampleCount) << 16);
    return reservoir;
}

ReservoirGpuData restirEmptyReservoir() {
    ReservoirGpuData reservoir;
    reservoir.radianceRgb9e5 = 0u;
    reservoir.weightMHalf = 0u;
    return reservoir;
}

float3 restirRadiance(ReservoirGpuData reservoir) { return restirUnpackRgb9e5(reservoir.radianceRgb9e5); }
float restirContributionWeight(ReservoirGpuData reservoir) { return f16tof32(reservoir.weightMHalf & 0xffffu); }
float restirSampleCount(ReservoirGpuData reservoir) { return f16tof32(reservoir.weightMHalf >> 16); }
bool restirIsValid(ReservoirGpuData reservoir) { return restirSampleCount(reservoir) > 0.5; }

float restirTargetFunction(float3 radiance) {
    return max(dot(radiance, SFT_RESTIR_LUMINANCE_WEIGHTS), 0.0);
}

// The GI grid is `scale`x smaller than the G-buffer (scale = 1 traces every pixel). Each grid cell
// represents one G-buffer pixel inside its scale x scale block, and which one rotates every frame so the
// temporal passes see the whole block over time instead of the same corner forever.
uint2 restirRepresentativePixel(uint2 gridPixel, uint scale, uint2 fullExtent, uint frameIndex) {
    if (scale <= 1u) return gridPixel;
    uint blockSize = scale * scale;
    uint slot = (frameIndex * 5u + 1u) % blockSize; // 5 is coprime with 1..4 blocks, visits every slot
    uint2 jitter = uint2(slot % scale, slot / scale);
    return min(gridPixel * scale + jitter, fullExtent - uint2(1u, 1u));
}
