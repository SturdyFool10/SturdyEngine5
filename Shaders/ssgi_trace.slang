// Screen-space indirect lighting with a visibility bitmask (Therrien, Levesque & Gilet 2023, "Screen Space
// Indirect Lighting with Visibility Bitmask"), pass 1 of 3: the trace.
//
// Runs on a half-resolution grid; each cell shades one full-resolution pixel of its 2x2 block, rotating every
// frame so the temporal pass (ssgi_resolve.slang) sees the whole block. For a handful of screen-space slices it
// marches the depth buffer outward in both directions. Every tap is treated as a surface of finite `thickness`
// (front face from the depth buffer, back face `thickness` further along its view ray), which occupies a range of
// angles in the slice; those angles set bits in a 32-sector mask. Consecutive taps along a direction that lie on
// one continuous surface (no depth jump between them) are connected like a height field -- the span back to the
// previous tap's angle is filled too -- because a sparse march would otherwise leave most of a floor or wall
// uncovered between taps and under-count its light several times over. A depth jump breaks the link, which keeps
// the bitmask's point: light still passes behind thin objects. Sectors are spaced by the *cosine-weighted*
// measure of the slice (the same weight GTAO's arc integral uses), so each bit is an equal share of irradiance
// and a tap's contribution is simply (newly covered bits / 32) * its radiance. Radiance comes from last frame's
// lit scene colour, reprojected through the motion vectors -- which also makes bounces accumulate over frames.
//
// Output (rgba16f, half resolution): rgb = irradiance / pi (demodulated: the lighting pass multiplies by the
// diffuse albedo, exactly like the ray-traced GI it stands in for), a = the shaded pixel's linear view depth.
//
// No ray tracing, no atomics, no wave ops: this is the GI tier that runs on every backend, including WebGPU.
import sturdy_common;
import sturdy_space;
import sturdy_lens;

// Mirrors ScreenSpaceGiTraceConstants in Renderer/ScreenSpaceGi.cpp (plain 4-byte scalars and float4s only).
struct TraceConstants {
    // Screen UV -> view ray: view.xy = (mul * uv + add) * viewZ.
    float4 ndcToViewMulAdd;
    // Rows of the world -> view rotation (view.x = dot(worldDir, row0.xyz), ...).
    float4 viewRow0;
    float4 viewRow1;
    float4 viewRow2;
    // x, y = depth linearisation (viewZ = x / (y - deviceDepth)), z = camera lens strength, w = radius (world units).
    float4 depthLensRadius;
    // x = thickness (world units), y = slice count, z = steps per slice, w = frame index.
    float4 thicknessSlicesStepsFrame;
    uint2 traceExtent;
    uint2 fullExtent;
    // 0 when there is no radiance history yet (first frame, resize): the trace then only reports depth.
    uint radianceValid;
    uint pad0;
    uint pad1;
    uint pad2;
};
#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<TraceConstants> trace;
#else
[[push_constant]] ConstantBuffer<TraceConstants> trace;
#endif

DepthTexture2D sceneDepth;
Texture2D<float2> gbufferNormal;
Texture2D<float2> gbufferMotion;
// Last frame's lit scene colour, half resolution, exposure removed (ssgi_history.slang).
Texture2D<float4> radianceHistory;
SamplerState radianceSampler;
[format("rgba16f")]
WTexture2D<float4> rawIrradianceOut;

static const uint kSectorCount = 32u;

float linearViewZ(float deviceDepth) {
    return trace.depthLensRadius.x / max(trace.depthLensRadius.y - deviceDepth, 1.0e-6);
}

// View-space position (X right, Y up, Z *into* the screen, like gtao_common.slang) of a screen UV at a depth.
float3 viewPositionAt(float2 uv, float viewZ) {
    if (trace.depthLensRadius.z > 0.0) {
        uv = ndcToUv(sturdyLensToWide(uvToNdc(uv), trace.depthLensRadius.z));
    }
    return float3((trace.ndcToViewMulAdd.xy * uv + trace.ndcToViewMulAdd.zw) * viewZ, viewZ);
}

float3 viewNormalAt(int2 pixel) {
    float3 worldNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(pixel, 0)));
    float3 viewNormal = float3(dot(worldNormal, trace.viewRow0.xyz), dot(worldNormal, trace.viewRow1.xyz),
                               dot(worldNormal, trace.viewRow2.xyz));
    return safeNormalizeOr(float3(viewNormal.x, viewNormal.y, -viewNormal.z), float3(0.0, 0.0, -1.0));
}

float fastAcos(float x) {
    float ax = abs(x);
    float result = (-0.156583 * ax + SFT_PI * 0.5) * sqrt(max(1.0 - ax, 0.0));
    return x >= 0.0 ? result : SFT_PI - result;
}

// Antiderivative of |sin t| cos(t - n) on t >= 0 (GTAO's arc integrand); the t < 0 half is its negation.
float arcPrimitive(float t, float n) {
    return (2.0 * t * sin(n) - cos(2.0 * t - n)) * 0.25;
}

// Cosine-weighted position of angle h within the slice's visible half-circle [n - pi/2, n + pi/2], in [0, 1].
float cosineSectorCoordinate(float h, float n) {
    float lower = n - SFT_PI * 0.5;
    float upper = n + SFT_PI * 0.5;
    h = clamp(h, lower, upper);
    float atZero = arcPrimitive(0.0, n);
    float negativeHalf = arcPrimitive(lower, n) - atZero;             // integral from lower to 0 (>= 0)
    float total = negativeHalf + arcPrimitive(upper, n) - atZero;
    float covered = h <= 0.0 ? arcPrimitive(lower, n) - arcPrimitive(h, n)
                             : negativeHalf + arcPrimitive(h, n) - atZero;
    return saturate(covered / max(total, 1.0e-6));
}

uint sectorMask(float from, float to) {
    uint first = (uint)clamp(floor(from * (float)kSectorCount), 0.0, (float)kSectorCount);
    uint last = (uint)clamp(ceil(to * (float)kSectorCount), 0.0, (float)kSectorCount);
    if (last <= first) return 0u;
    uint count = last - first;
    uint bits = count >= kSectorCount ? 0xffffffffu : ((1u << count) - 1u);
    return bits << first;
}

float interleavedGradientNoise(float2 pixel, uint frame) {
    pixel += (float)(frame % 64u) * float2(5.588238, 5.588238);
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void traceMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 cell = dispatchThreadId.xy;
    if (any(cell >= trace.traceExtent)) return;

    uint frame = (uint)trace.thicknessSlicesStepsFrame.w;
    uint2 fullExtent = trace.fullExtent;
    uint jitterIndex = (frame * 3u + 1u) & 3u;
    int2 pixel = int2(min(cell * 2u + uint2(jitterIndex & 1u, jitterIndex >> 1u), fullExtent - uint2(1u, 1u)));
    float2 texel = 1.0 / float2(fullExtent);
    float2 uv = (float2(pixel) + 0.5) * texel;

    float deviceDepth = sceneDepth.Load(int3(pixel, 0));
    if (deviceDepth >= 0.999999) {
        rawIrradianceOut.Store(cell, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }
    float viewZ = linearViewZ(deviceDepth);
    // Nudged toward the camera so a tap on the same texel never occludes the pixel itself.
    float3 position = viewPositionAt(uv, viewZ * 0.9999);
    float3 viewVector = safeNormalizeOr(-position, float3(0.0, 0.0, -1.0));
    float3 normal = viewNormalAt(pixel);

    float radius = trace.depthLensRadius.w;
    float thickness = trace.thicknessSlicesStepsFrame.x;
    uint sliceCount = max((uint)trace.thicknessSlicesStepsFrame.y, 1u);
    uint stepCount = max((uint)trace.thicknessSlicesStepsFrame.z, 1u);

    // Search radius in pixels: the world radius projected at this depth.
    float pixelViewSize = viewZ * abs(trace.ndcToViewMulAdd.x) * texel.x / (1.0 + trace.depthLensRadius.z);
    float screenRadius = min(radius / max(pixelViewSize, 1.0e-6), 0.25 * (float)max(fullExtent.x, fullExtent.y));
    if (screenRadius < 1.5 || trace.radianceValid == 0u) {
        rawIrradianceOut.Store(cell, float4(0.0, 0.0, 0.0, viewZ));
        return;
    }

    float sliceNoise = interleavedGradientNoise(float2(pixel), frame);
    float stepNoise = interleavedGradientNoise(float2(pixel) + float2(17.0, 43.0), frame);

    float3 irradiance = float3(0.0, 0.0, 0.0);
    for (uint slice = 0u; slice < sliceCount; ++slice) {
        float phi = ((float)slice + sliceNoise) / (float)sliceCount * SFT_PI;
        float cosPhi = cos(phi);
        float sinPhi = sin(phi);
        float2 omega = float2(cosPhi, -sinPhi) * screenRadius;   // pixels; uv y runs down, view y up

        float3 direction = float3(cosPhi, sinPhi, 0.0);
        float3 orthoDirection = direction - dot(direction, viewVector) * viewVector;
        float3 axis = safeNormalizeOr(cross(orthoDirection, viewVector), float3(0.0, 1.0, 0.0));
        float3 projectedNormal = normal - axis * dot(normal, axis);
        float projectedLength = length(projectedNormal);
        float cosN = saturate(dot(projectedNormal, viewVector) / max(projectedLength, 1.0e-6));
        float n = sign(dot(orthoDirection, projectedNormal)) * fastAcos(cosN);

        uint occluded = 0u;
        float3 sliceLight = float3(0.0, 0.0, 0.0);
        // Per side: the previous tap's front angle and position (w = 0 until there is one).
        float4 previousTap[2] = {float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0)};
        float previousAngle[2] = {0.0, 0.0};
        for (uint step = 0u; step < stepCount; ++step) {
            float s = ((float)step + frac(stepNoise + (float)slice * 0.618034)) / (float)stepCount;
            s = s * s;                                             // denser near the pixel, like GTAO
            float2 offset = max(s * length(omega), 1.5) * normalize(omega);
            [unroll]
            for (int side = 0; side < 2; ++side) {
                float sideSign = side == 0 ? 1.0 : -1.0;
                int2 samplePixel = pixel + int2(round(offset * sideSign));
                if (any(samplePixel < int2(0, 0)) || any(samplePixel >= int2(fullExtent))) {
                    previousTap[side].w = 0.0;
                    continue;
                }
                float sampleDeviceDepth = sceneDepth.Load(int3(samplePixel, 0));
                if (sampleDeviceDepth >= 0.999999) {
                    previousTap[side].w = 0.0;
                    continue;
                }
                float2 sampleUv = (float2(samplePixel) + 0.5) * texel;
                float3 samplePosition = viewPositionAt(sampleUv, linearViewZ(sampleDeviceDepth));

                float3 frontDelta = samplePosition - position;
                float frontDistance = length(frontDelta);
                if (frontDistance > radius || frontDistance < 1.0e-5) {
                    previousTap[side].w = 0.0;
                    continue;
                }
                float3 backPosition = samplePosition + normalize(samplePosition) * thickness;
                float3 backDelta = backPosition - position;
                float3 toSample = frontDelta / frontDistance;

                float frontAngle = sideSign * fastAcos(dot(toSample, viewVector));
                float backAngle = sideSign * fastAcos(dot(normalize(backDelta), viewVector));
                float spanMin = min(frontAngle, backAngle);
                float spanMax = max(frontAngle, backAngle);
                // Continuous with the previous tap on this side: fill the gap between them.
                if (previousTap[side].w > 0.0) {
                    float3 between = samplePosition - previousTap[side].xyz;
                    float stepLength = length(between);
                    bool continuous = abs(between.z) <= max(thickness, 0.5 * length(between.xy)) &&
                                      stepLength <= 0.5 * radius;
                    if (continuous) {
                        spanMin = min(spanMin, previousAngle[side]);
                        spanMax = max(spanMax, previousAngle[side]);
                    }
                }
                previousTap[side] = float4(samplePosition, 1.0);
                previousAngle[side] = frontAngle;
                float u0 = cosineSectorCoordinate(spanMin, n);
                float u1 = cosineSectorCoordinate(spanMax, n);
                uint mask = sectorMask(u0, u1);
                uint newlyCovered = mask & ~occluded;
                occluded |= mask;
                if (newlyCovered == 0u) continue;

                // Only the side of the tap that faces us emits toward the pixel.
                float3 sampleNormal = viewNormalAt(samplePixel);
                float facing = saturate(-dot(sampleNormal, toSample) * 4.0);
                if (facing <= 0.0) continue;

                float2 previousUv = sampleUv - motionToUvDelta(gbufferMotion.Load(int3(samplePixel, 0)));
                if (any(previousUv < 0.0) || any(previousUv > 1.0)) continue;
                float3 radiance = radianceHistory.SampleLevel(radianceSampler, previousUv, 0.0).rgb;
                sliceLight += radiance * (facing * (float)countbits(newlyCovered) / (float)kSectorCount);
            }
        }
        irradiance += sliceLight * projectedLength;
    }
    irradiance /= (float)sliceCount;
    rawIrradianceOut.Store(cell, float4(max(irradiance, float3(0.0, 0.0, 0.0)), viewZ));
}
