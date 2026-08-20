// FrostBite-style motion blur, pass 3 of 3: per-pixel line-integral gather.
//
// Reconstructs each pixel's blur by averaging bilinear sceneColor taps along the dilated velocity
// line centered on that pixel (DICE's "Next Generation Post Processing in Call of Duty: Advanced
// Warfare" reconstruction filter) — one line per pixel rather than a full disc, since the dominant
// blur direction at any point on screen is well approximated by a single vector once
// motion_blur_neighbor_max.slang has already dilated it across tile boundaries.
//
// Per-pixel motion (gbufferMotion) and the dilated per-tile motion (dilatedVelocity) are compared
// by magnitude and the larger one wins as this pixel's blurVelocity: a fast object thinner than one
// tile still needs its own per-pixel motion to dominate the (possibly smaller) tile-averaged value
// around it, while a pixel deep inside a uniformly fast-moving tile is better served by the
// dilated/neighborhood value bleeding in from a moving neighbor.
//
// gbuffer_geometry.slang's motion output convention: (currentNdc - previousNdc) * 0.5, i.e. half the
// raw clip-space NDC delta, in NDC's Y-up convention. Converting that directly to a pixel-space
// vector needs two things ndcToUv (sturdy_common) already documents: the *2 to undo the 0.5 scale
// cancels exactly against NDC's half-width-per-pixel-extent scale (NDC spans width 2 over
// renderExtent pixels, i.e. pixels-per-NDC-unit = renderExtent / 2), leaving a plain
// `motion * renderExtent` magnitude — but the Y axis still needs sign flip (NDC Y-up vs this
// engine's Y-down pixel/UV space), so the full conversion is `motion * renderExtent * float2(1,-1)`.
//
// Depth test: this engine's convention is depth_compare = Less, near = 0 / far = 1 (see
// hiz_build.slang's own doc comment), so a *smaller* stored depth is *closer* to the camera. A tap
// closer than the center pixel is treated as foreground (full weight); a tap farther away is
// background and gets settings.background_foreground_weight_bias's reduced weight — approximates
// depth-aware blur without a full scatter-based reconstruction (DICE's own approach on this budget).

import sturdy_common;

struct GatherConstants {
    float2 renderExtent;
    float intensity;
    float shutterFraction;
    float maxBlurRadiusPx;
    float fgBgWeightBias;
    uint sampleCount;
    uint tileSize;
    // Renderer::MotionBlurSettings::camera_motion_only (Scene.hpp): when nonzero, always take the
    // tile-dilated velocity rather than letting a fast individual object's own per-pixel motion win
    // the max-magnitude comparison below — the dilated field is tile-smoothed and screen-coherent,
    // which reads as pure camera-pan blur instead of picking out any one moving object's silhouette.
    uint cameraMotionOnly;
};
[[push_constant]] ConstantBuffer<GatherConstants> gather;

Texture2D<float4> sceneColor;
SamplerState sceneColorSampler;
Texture2D<float2> gbufferMotion;
Texture2D<float> gbufferDepth;
Texture2D<float2> dilatedVelocity;
RWTexture2D<float4> sceneColorOut;

static const uint kMaxGatherTaps = 32;

[shader("compute")]
[numthreads(8, 8, 1)]
void gatherMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= uint(gather.renderExtent.x) || pixel.y >= uint(gather.renderExtent.y)) {
        return;
    }

    float4 centerColor = sceneColor.Load(int3(int2(pixel), 0));
    float2 pixelMotion = gbufferMotion.Load(int3(int2(pixel), 0));
    uint2 tile = pixel / max(gather.tileSize, 1u);
    float2 tileMotion = dilatedVelocity.Load(int3(int2(tile), 0));

    float2 blurVelocityNdcHalf = gather.cameraMotionOnly != 0
        ? tileMotion
        : (dot(tileMotion, tileMotion) > dot(pixelMotion, pixelMotion) ? tileMotion : pixelMotion);
    float2 blurVelocityPx = blurVelocityNdcHalf * gather.renderExtent * float2(1.0, -1.0);
    blurVelocityPx *= gather.intensity * gather.shutterFraction;

    float blurLength = length(blurVelocityPx);
    if (blurLength > gather.maxBlurRadiusPx) {
        blurVelocityPx *= gather.maxBlurRadiusPx * safeRcp(blurLength);
        blurLength = gather.maxBlurRadiusPx;
    }

    if (blurLength < 0.5) {
        sceneColorOut[pixel] = centerColor;
        return;
    }

    float centerDepth = gbufferDepth.Load(int3(int2(pixel), 0));
    uint taps = clamp(gather.sampleCount, 1u, kMaxGatherTaps);
    float2 maxTexel = gather.renderExtent - float2(1.0, 1.0);

    float3 accumulatedColor = float3(0.0, 0.0, 0.0);
    float accumulatedWeight = 0.0;
    for (uint i = 0; i < taps; ++i) {
        float t = taps > 1 ? (float(i) / float(taps - 1)) - 0.5 : 0.0;
        float2 tapPixel = clamp(float2(pixel) + 0.5 + blurVelocityPx * t, float2(0.0, 0.0), maxTexel);

        float tapDepth = gbufferDepth.Load(int3(int2(tapPixel), 0));
        float weight = tapDepth <= centerDepth ? 1.0 : gather.fgBgWeightBias;

        float2 tapUv = tapPixel * safeRcp(gather.renderExtent);
        float3 tapColor = sceneColor.SampleLevel(sceneColorSampler, tapUv, 0.0).rgb;

        accumulatedColor += tapColor * weight;
        accumulatedWeight += weight;
    }

    float3 resolvedColor = accumulatedWeight > 0.0 ? accumulatedColor * safeRcp(accumulatedWeight) : centerColor.rgb;
    sceneColorOut[pixel] = float4(resolvedColor, centerColor.a);
}
