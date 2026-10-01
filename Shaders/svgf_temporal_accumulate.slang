// SVGF pass 1/2 (Schied et al. 2017, "Spatiotemporal Variance-Guided Filtering"): reprojects last
// frame's accumulated color/moment history via the G-buffer motion vector (the same reprojection
// approach restir_gi_temporal_reuse.slang already uses for reservoirs) and blends this frame's raw,
// noisy ReSTIR GI irradiance into it with a dynamically-hastened exponential moving average (fast
// blend while a pixel's history is still short, same hysteresis trick surfel GI used). Tracks the
// first and second raw moments of luminance so variance = moment2 - moment1^2 can drive
// svgf_atrous.slang's edge-stopping weights; pixels with too little history (disoccluded/new) instead
// estimate variance from a spatial 7x7 neighborhood of this frame's raw input, the standard SVGF
// fallback for exactly this case.
//
// Writes the raw blended color + its variance to a *transient* texture (`accumulatedOut`) consumed by
// svgf_atrous.slang's first iteration — NOT to the persistent color-history texture, which instead
// receives that first iteration's spatially-filtered result (see svgf_atrous.slang's header for why).
// Moments and history length, which are never spatially filtered, are written straight to their
// persistent history textures here.
import sturdy_common;
import sturdy_spectral_common;
import sturdy_space;
import sturdy_lens;

struct SvgfFrameConstants {
    float4x4 inverseViewProjection;
    float4x4 previousViewProjection;
    float4 extentHistoryValidFrameIndex;
    float4 temporalPhiParams;
    // xyz = camera world position.
    float4 cameraPosition;
};

ConstantBuffer<SvgfFrameConstants> frame;
Texture2D<float4> rawIrradiance;
Texture2D<float2> gbufferNormal;
Texture2D<float> gbufferDepth;
Texture2D<float2> gbufferMotion;
Texture2D<float4> colorHistoryPrev;
// xy = luminance moments, z = camera distance of the surface this texel showed last frame (0 = none),
// w = its octahedral normal (two half floats packed into the float's bits).
Texture2D<float4> momentsHistoryPrev;
Texture2D<float> historyLengthPrev;
// WGSL's texture_storage_2d requires a format in the type itself; must match each texture's real
// RHI::Format (RGBA16Float / RG32Float / R32Float, see Renderer's SVGF target creation). All
// write-only from this pass.
[format("rgba16f")]
WTexture2D<float4> accumulatedOut;
// No [format(...)] here deliberately: WGSL has no 2-component storage-texture format (confirmed via
// slangc -target wgsl, which silently substitutes rgba32float and warns E31105) -- see
// motion_blur_neighbor_max.slang's dilatedVelocityOut comment for the same issue. Needs the backing
// RHI::Format changed to a WGSL-legal 4-channel one at its creation site; not done here.
[format("rgba32f")]
WTexture2D<float4> momentsHistoryOut;
[format("r32f")]
WTexture2D<float> historyLengthOut;

static const float3 SFT_LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);
// History is rejected when the surface it was accumulated on is not the surface here now.
static const float kHistoryDistanceTolerance = 0.06; // relative to the camera distance
static const float kHistoryNormalCosine = 0.88;

float packedNormalToFloat(float2 encodedNormal) {
    return asfloat(f32tof16(encodedNormal.x) | (f32tof16(encodedNormal.y) << 16u));
}

float2 floatToPackedNormal(float packedNormal) {
    uint bits = asuint(packedNormal);
    return float2(f16tof32(bits & 0xffffu), f16tof32(bits >> 16u));
}

float spatialVarianceFallback(uint2 pixel, uint2 extent) {
    float sum = 0.0;
    float sumSquared = 0.0;
    float count = 0.0;
    for (int dy = -3; dy <= 3; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
            int2 tap = int2(pixel) + int2(dx, dy);
            if (any(tap < int2(0, 0)) || any(tap >= int2(extent))) continue;
            float luminance = dot(rawIrradiance.Load(int3(tap, 0)).rgb, SFT_LUMINANCE_WEIGHTS);
            sum += luminance;
            sumSquared += luminance * luminance;
            count += 1.0;
        }
    }
    if (count < 1.0) return 0.0;
    float mean = sum / count;
    return max(sumSquared / count - mean * mean, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void temporalAccumulateMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 extent = (uint2)frame.extentHistoryValidFrameIndex.xy;
    if (any(dispatchThreadId.xy >= extent)) return;

    float3 rawColor = rawIrradiance.Load(int3(dispatchThreadId.xy, 0)).rgb;
    float rawLuminance = dot(rawColor, SFT_LUMINANCE_WEIGHTS);

    // The surface this pixel shows now, to validate reprojected history against.
    float depth = gbufferDepth.Load(int3(dispatchThreadId.xy, 0));
    bool isSurface = depth < 0.999999;
    float2 encodedNormal = gbufferNormal.Load(int3(dispatchThreadId.xy, 0));
    float currentDistance = 0.0;
    if (isSurface) {
        float2 pixelUv = (float2(dispatchThreadId.xy) + 0.5) / float2(extent);
        float3 worldPosition = sturdySpaceCameraClipToWorld(float4(uvToNdc(pixelUv), depth, 1.0), frame.inverseViewProjection, frame.cameraPosition.w);
        currentDistance = length(worldPosition - frame.cameraPosition.xyz);
    }

    bool historyValid = frame.extentHistoryValidFrameIndex.z > 0.5;
    float3 previousColor = float3(0.0, 0.0, 0.0);
    float2 previousMoments = float2(0.0, 0.0);
    float previousHistoryLength = 0.0;
    bool reprojected = false;

    if (historyValid && isSurface) {
        float2 uv = (float2(dispatchThreadId.xy) + 0.5) / float2(extent);
        float2 motion = gbufferMotion.Load(int3(dispatchThreadId.xy, 0));
        float2 previousUv = uv - motionToUvDelta(motion);
        if (all(previousUv >= 0.0) && all(previousUv <= 1.0)) {
            int2 previousPixel = clamp(int2(previousUv * float2(extent)), int2(0, 0), int2(extent) - int2(1, 1));
            float4 previousMomentsAndGeometry = momentsHistoryPrev.Load(int3(previousPixel, 0));
            float previousDistance = previousMomentsAndGeometry.z;
            float3 previousNormal = decodeOctahedralNormal(floatToPackedNormal(previousMomentsAndGeometry.w));
            float3 currentNormal = decodeOctahedralNormal(encodedNormal);
            bool sameSurface = previousDistance > 0.0 &&
                               abs(previousDistance - currentDistance) <= kHistoryDistanceTolerance * currentDistance + 0.02 &&
                               dot(previousNormal, currentNormal) >= kHistoryNormalCosine;
            if (sameSurface) {
                previousColor = colorHistoryPrev.Load(int3(previousPixel, 0)).rgb;
                previousMoments = previousMomentsAndGeometry.xy;
                previousHistoryLength = historyLengthPrev.Load(int3(previousPixel, 0));
                reprojected = previousHistoryLength > 0.0;
            }
        }
    }

    float historyLength = reprojected ? min(previousHistoryLength + 1.0, 32.0) : 1.0;
    float alpha = max(frame.temporalPhiParams.x, 1.0 / historyLength);

    float3 blendedColor = reprojected ? lerp(previousColor, rawColor, alpha) : rawColor;
    float2 blendedMoments = reprojected
        ? lerp(previousMoments, float2(rawLuminance, rawLuminance * rawLuminance), alpha)
        : float2(rawLuminance, rawLuminance * rawLuminance);

    float variance;
    if (historyLength >= 4.0) {
        variance = max(blendedMoments.y - blendedMoments.x * blendedMoments.x, 0.0);
    } else {
        variance = spatialVarianceFallback(dispatchThreadId.xy, extent);
    }

    accumulatedOut.Store(dispatchThreadId.xy, float4(blendedColor, variance));
    momentsHistoryOut.Store(dispatchThreadId.xy, float4(blendedMoments, currentDistance, packedNormalToFloat(encodedNormal)));
    historyLengthOut.Store(dispatchThreadId.xy, historyLength);
}
