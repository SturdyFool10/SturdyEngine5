// Depth-only Subpixel Reconstruction Antialiasing (SRAA) for deferred shading.
//
// Based on NVIDIA Research's "Subpixel Reconstruction Antialiasing for Deferred
// Shading" (I3D 2011). Geometry visibility is sampled with an MSAA depth prepass,
// while the expensive G-buffer and lighting remain single-sampled. For each MSAA
// depth sample at an edge, a joint bilateral filter reconstructs a shading sample
// from the 3x3 neighborhood of the 1x shaded color/depth buffers. The reconstructed
// subpixel colors are then box-filtered into one output pixel.
//
// The paper identifies depth-only reconstruction as the best bandwidth/quality
// tradeoff for games. A fast interior path returns the original shaded pixel when
// every geometric sample belongs to the same surface, avoiding both blur and the
// 3x3 gather over most of the image.

import sturdy_common;

struct VertexOutput {
    float4 position : SV_Position;
};

struct ReconstructionConstants {
    uint width;
    uint height;
    uint sampleCount;
    float nearPlane;
    float farPlane;
    float depthFalloff;
    float spatialFalloff;
    float sameSurfaceThreshold;
};

[[push_constant]] ConstantBuffer<ReconstructionConstants> reconstruction;

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = fullscreenTrianglePosition(uv);
    return output;
}

Texture2D<float4> shadedColor;
Texture2D<float> shadedDepth;
Texture2DMS<float> geometryDepth;

float viewDepth(float deviceDepth) {
    // Conventional Vulkan/D3D 0..1 perspective depth used by this renderer (near=0,
    // far=1). Treat the clear value explicitly to keep sky samples numerically stable.
    if (deviceDepth >= 0.999999) {
        return reconstruction.farPlane;
    }
    float nearPlane = max(reconstruction.nearPlane, 1.0e-4);
    float farPlane = max(reconstruction.farPlane, nearPlane + 1.0e-3);
    return (nearPlane * farPlane) /
        max(farPlane - deviceDepth * (farPlane - nearPlane), 1.0e-6);
}

bool sameSurface(float a, float b) {
    bool aIsSky = a >= 0.999999;
    bool bIsSky = b >= 0.999999;
    if (aIsSky || bIsSky) {
        return aIsSky && bIsSky;
    }
    float aView = viewDepth(a);
    float bView = viewDepth(b);
    float relativeDelta = abs(aView - bView) / max(min(aView, bView), reconstruction.nearPlane);
    return relativeDelta <= reconstruction.sameSurfaceThreshold;
}

float bilateralDepthWeight(float targetDeviceDepth, float tapDeviceDepth) {
    bool targetIsSky = targetDeviceDepth >= 0.999999;
    bool tapIsSky = tapDeviceDepth >= 0.999999;
    if (targetIsSky || tapIsSky) {
        return targetIsSky && tapIsSky ? 1.0 : 0.0;
    }

    float targetView = viewDepth(targetDeviceDepth);
    float tapView = viewDepth(tapDeviceDepth);
    float relativeDelta = abs(targetView - tapView) /
        max(min(targetView, tapView), reconstruction.nearPlane);
    return exp(-min(reconstruction.depthFalloff * relativeDelta, 80.0));
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    int2 maximumPixel = int2(max(reconstruction.width, 1u), max(reconstruction.height, 1u)) - int2(1, 1);
    int2 pixel = clamp(int2(input.position.xy), int2(0, 0), maximumPixel);
    float4 centerColor = shadedColor.Load(int3(pixel, 0));
    float centerDepth = shadedDepth.Load(int3(pixel, 0));

    // Most pixels are not geometric edges. Test the MSAA samples first and return the
    // unfiltered 1x result for interiors, both preserving sharp texture detail and making
    // the expensive bilateral gather proportional to edge pixels rather than resolution.
    bool interior = true;
    uint sampleCount = min(max(reconstruction.sampleCount, 1u), 8u);
    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        interior = interior && sameSurface(geometryDepth.Load(pixel, sampleIndex), centerDepth);
    }
    if (interior) {
        return centerColor;
    }

    float4 reconstructed = float4(0.0, 0.0, 0.0, 0.0);
    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        float targetDepth = geometryDepth.Load(pixel, sampleIndex);
        float4 subpixelColor = float4(0.0, 0.0, 0.0, 0.0);
        float weightSum = 0.0;
        // Avoid dynamically indexed local arrays here. They can spill into per-wave scratch
        // on some drivers, reserving far more memory than the MSAA depth image itself. Edge
        // pixels issue more texture loads, but the texture cache still shares the 3x3 taps.
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                int2 tapPixel = clamp(pixel + int2(x, y), int2(0, 0), maximumPixel);
                float tapDepth = shadedDepth.Load(int3(tapPixel, 0));
                float spatialWeight = exp(-reconstruction.spatialFalloff * float(x * x + y * y));
                float weight = spatialWeight * bilateralDepthWeight(targetDepth, tapDepth);
                subpixelColor += shadedColor.Load(int3(tapPixel, 0)) * weight;
                weightSum += weight;
            }
        }
        // Extremely thin geometry may have no matching 1x shaded sample in the 3x3
        // support. The paper notes that undersampled shading cannot be reconstructed;
        // preserving the center is safer than emitting black or NaN in that case.
        reconstructed += weightSum > 1.0e-6 ? subpixelColor / weightSum : centerColor;
    }

    reconstructed /= float(sampleCount);
    reconstructed.a = centerColor.a;
    return reconstructed;
}
