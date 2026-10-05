// Screen-space reflections: the edge-aware clean-up of the trace, run on the receiving surface.
//
// The trace makes one binary-ish decision per pixel (hit, or fall back to the sky), and where a reflection ends -- the
// horizon of a grazing floor, a thin silhouette, the thickness limit -- neighbouring pixels flip between the two, which reads
// as speckle and 2-4 pixel stair steps. A 5x5 bilateral filter turns those flips into a smooth gradient:
//   * weights are spatial (Gaussian) x same-plane (distance of the neighbour's surface from this pixel's tangent plane, so it
//     works at grazing angles where raw depth differences are huge) x same-orientation (normals),
//   * colour is filtered *premultiplied by confidence*, so a missing neighbour lowers the confidence instead of darkening the
//     colour, and confidence is the plain weighted mean.
// Deterministic and stateless: nothing here depends on history, so it cannot ghost.
//
// Output (full resolution, rgba16f): rgb = reflected radiance before exposure, a = confidence.
import sturdy_common;
import sturdy_lighting_data;
import ssr_common;

ConstantBuffer<ShadowLightingGpuData> lightingData;
ConstantBuffer<SsrConstants> ssr;
DepthTexture2D sceneDepth;
Texture2D<float2> gbufferNormal;
Texture2D<float4> gbufferMaterial;
Texture2D<float4> traced;
[format("rgba16f")]
WTexture2D<float4> filteredOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void filterMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= ssr.extents.xy)) {
        return;
    }
    int2 pixel = int2(id.xy);
    int2 maxPixel = int2(ssr.extents.xy) - 1;
    float4 center = traced.Load(int3(pixel, 0));
    float centerDepth = sceneDepth.Load(int3(pixel, 0));
    float perceptualRoughness = saturate(gbufferMaterial.Load(int3(pixel, 0)).x);
    if (centerDepth >= 0.999999 || perceptualRoughness > ssr.tuning.x) {
        filteredOut.Store(pixel, center);
        return;
    }
    float2 uv = (float2(pixel) + 0.5) / float2(ssr.extents.xy);
    float3 centerWorld = ssrWorldFromDepth(lightingData, uv, centerDepth);
    float3 centerNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(pixel, 0)));
    float viewDepth = ssrViewDepth(lightingData, centerWorld);
    // Plane tolerance: about two pixels' worth of world distance at this depth.
    float worldPerPixel = viewDepth * 2.0 * lightingData.viewportParams.y / max(lightingData.viewportParams.z, SFT_EPSILON);
    float planeTolerance = max(2.0 * worldPerPixel, 1.0e-4);

    float3 sumColor = float3(0.0, 0.0, 0.0);
    float sumConfidence = 0.0;
    float sumWeight = 0.0;
    [loop]
    for (int dy = -2; dy <= 2; ++dy) {
        [loop]
        for (int dx = -2; dx <= 2; ++dx) {
            int2 tap = clamp(pixel + int2(dx, dy), int2(0, 0), maxPixel);
            float spatial = exp(-float(dx * dx + dy * dy) / (2.0 * 1.2 * 1.2));
            float tapDepth = sceneDepth.Load(int3(tap, 0));
            float weight = 0.0;
            float4 sample = float4(0.0, 0.0, 0.0, 0.0);
            if (tapDepth < 0.999999) {
                float3 tapWorld = ssrWorldFromDepth(lightingData, (float2(tap) + 0.5) / float2(ssr.extents.xy), tapDepth);
                float planeDistance = abs(dot(tapWorld - centerWorld, centerNormal));
                float3 tapNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(tap, 0)));
                float samePlane = exp(-planeDistance / planeTolerance);
                float sameFacing = pow(saturate(dot(centerNormal, tapNormal)), 16.0);
                weight = spatial * samePlane * sameFacing;
                sample = traced.Load(int3(tap, 0));
            }
            sumWeight += weight;
            sumConfidence += weight * sample.a;
            sumColor += weight * sample.a * sample.rgb;
        }
    }
    if (sumWeight <= 1.0e-6) {
        filteredOut.Store(pixel, center);
        return;
    }
    float confidence = sumConfidence / sumWeight;
    float3 color = sumColor / max(sumConfidence, 1.0e-5);
    filteredOut.Store(pixel, float4(color, confidence));
}
