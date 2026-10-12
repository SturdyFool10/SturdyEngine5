// Colour grading on the scene-linear image, just before tone mapping (Renderer/ColorGrade.cpp).
//
// Everything runs in scene-linear light with no clamping above 1, so the tone mapper's curve still shapes the result:
//   1. white balance and exposure offset (per-channel gains, computed on the CPU, luminance-preserving),
//   2. contrast, hue-preserving (the colour is scaled by a power of its luminance around middle grey 0.18),
//   3. lift / gamma / gain per channel (lift fades out towards highlights; gamma pivots at middle grey, so it is HDR-safe),
//   4. split toning (shadow and highlight tints weighted by luminance in stops),
//   5. saturation,
//   6. an optional 3D LUT in a *log* domain (the film-grading convention): the input is log2(colour / 0.18) mapped from
//      -8..+8 stops to 0..1 per channel, and the output is decoded the same way, so the LUT covers the whole dynamic range.
//      Channels outside the LUT's range pass through unchanged (an identity LUT is then exactly the identity).
//
// The LUT is a 2D atlas (N*N wide, N tall, one N x N slice per blue value, blue slices left to right) rather than a 3D
// texture, so it binds the same way on every backend (WebGPU bind-group layouts here carry no view dimension).
import sturdy_common;

struct GradeConstants {
    // width, height, LUT size (0 = no LUT), unused
    uint4 extent;
    // white balance gain rgb, exposure gain (2^EV)
    float4 whiteBalance;
    // contrast, saturation, split-toning strength, LUT intensity
    float4 look;
    float4 lift;
    // 1 / gamma per channel
    float4 inverseGamma;
    float4 gain;
    float4 shadowTint;
    float4 highlightTint;
};

ConstantBuffer<GradeConstants> grade;
Texture2D<float4> sceneColor;
// Bound to the scene colour itself when there is no LUT (never sampled then).
Texture2D<float4> gradeLut;
SamplerState linearSampler;
[format("rgba16f")]
WTexture2D<float4> colorOut;

static const float kMidGrey = 0.18;
static const float kLutStops = 8.0; // the LUT covers -8..+8 stops around middle grey

float3 sampleLut(float3 uvw, uint size) {
    float n = float(size);
    float z = uvw.z * (n - 1.0);
    float z0 = floor(z);
    float z1 = min(z0 + 1.0, n - 1.0);
    float blend = z - z0;
    // Texel centres: slice texel i sits at (i + 0.5) / N inside its slice.
    float2 inSlice = uvw.xy * (n - 1.0) + 0.5;
    float2 uv0 = float2((z0 * n + inSlice.x) / (n * n), inSlice.y / n);
    float2 uv1 = float2((z1 * n + inSlice.x) / (n * n), inSlice.y / n);
    return lerp(gradeLut.SampleLevel(linearSampler, uv0, 0.0).rgb, gradeLut.SampleLevel(linearSampler, uv1, 0.0).rgb, blend);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void gradeMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= grade.extent.xy)) {
        return;
    }
    int2 pixel = int2(id.xy);
    float4 source = sceneColor.Load(int3(pixel, 0));
    float3 c = all(source.rgb == source.rgb) && all(abs(source.rgb) < 65000.0) ? max(source.rgb, float3(0.0, 0.0, 0.0)) : float3(0.0, 0.0, 0.0);

    // 1. White balance and exposure.
    c *= grade.whiteBalance.rgb * grade.whiteBalance.w;

    // 2. Contrast around middle grey, scaling the colour by its luminance so hue and saturation hold.
    float luma = max(luminance(c), 1.0e-5);
    c *= pow(luma / kMidGrey, grade.look.x - 1.0);

    // 3. Lift / gamma / gain.
    c = c * grade.gain.rgb + grade.lift.rgb * (1.0 - saturate(c / 0.5));
    c = kMidGrey * pow(max(c, float3(0.0, 0.0, 0.0)) / kMidGrey, grade.inverseGamma.rgb);

    // 4. Split toning: shadows weighted towards -4 stops, highlights towards +4, around middle grey.
    float stops = log2(max(luminance(c), 1.0e-5) / kMidGrey);
    float highlightWeight = saturate((stops + 4.0) / 8.0);
    c *= lerp(float3(1.0, 1.0, 1.0), grade.shadowTint.rgb, (1.0 - highlightWeight) * grade.look.z);
    c *= lerp(float3(1.0, 1.0, 1.0), grade.highlightTint.rgb, highlightWeight * grade.look.z);

    // 5. Saturation.
    float gray = luminance(c);
    c = max(lerp(float3(gray, gray, gray), c, grade.look.y), float3(0.0, 0.0, 0.0));

    // 6. 3D LUT in the log domain.
    if (grade.extent.z > 0u && grade.look.w > 0.0) {
        float3 logColor = log2(max(c, float3(1.0e-6, 1.0e-6, 1.0e-6)) / kMidGrey);
        float3 uvw = saturate((logColor + kLutStops) / (2.0 * kLutStops));
        float3 graded = kMidGrey * exp2(sampleLut(uvw, grade.extent.z) * (2.0 * kLutStops) - kLutStops);
        float3 inRange = float3(abs(logColor.x) < kLutStops ? 1.0 : 0.0, abs(logColor.y) < kLutStops ? 1.0 : 0.0,
                                abs(logColor.z) < kLutStops ? 1.0 : 0.0);
        c = lerp(c, graded, grade.look.w * inRange);
    }
    colorOut.Store(pixel, float4(c, source.a));
}
