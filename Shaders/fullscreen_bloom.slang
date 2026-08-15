// Call of Duty: Advanced Warfare-style bloom: a progressive fractional 13-tap downsample pyramid, a
// renormalized Karis-weighted first downsample for HDR firefly suppression, and progressive 3x3 tent
// reconstruction. Filtering happens while moving down and back up the hierarchy rather than in
// standalone blur passes. Intermediates are packed RG11B10Float.

import sturdy_common;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = fullscreenTrianglePosition(output.uv);
    return output;
}

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

struct BloomConstants {
    float2 sourceTexelSize;
    float threshold;
    float softKnee;
    float2 filterScale;
};

[[push_constant]] ConstantBuffer<BloomConstants> bloom;

float3 sampleSource(float2 uv, float2 texelOffset) {
    return sourceTexture.SampleLevel(
        sourceSampler,
        uv + texelOffset * bloom.sourceTexelSize * bloom.filterScale,
        0.0).rgb;
}

float3 softThreshold(float3 color) {
    // Threshold zero is the physically based Jimenez mode: bloom the complete HDR image and control
    // visibility with a low, energy-preserving final mix.
    if (bloom.threshold <= 0.0) {
        return color;
    }

    float brightness = max(color.r, max(color.g, color.b));
    if (brightness <= 0.0) {
        return 0.0;
    }

    float knee = bloom.threshold * bloom.softKnee;
    if (knee <= 0.0) {
        return color * (max(brightness - bloom.threshold, 0.0) / brightness);
    }

    float soft = clamp(brightness - bloom.threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    return color * (max(brightness - bloom.threshold, soft) / brightness);
}

// CoD:AW's 13-tap downsample kernel: four overlapping 2x2 quads (weight 1/32 per sample) plus an
// inner 2x2 quad (weight 1/8 per sample), each averaged with hardware bilinear filtering. filterScale
// adapts the original 2x footprint to the configured fractional reduction ratio.
float3 sample13Tap(float2 uv) {
    float3 a = sampleSource(uv, float2(-2.0, -2.0));
    float3 b = sampleSource(uv, float2( 0.0, -2.0));
    float3 c = sampleSource(uv, float2( 2.0, -2.0));
    float3 d = sampleSource(uv, float2(-2.0,  0.0));
    float3 e = sampleSource(uv, float2( 0.0,  0.0));
    float3 f = sampleSource(uv, float2( 2.0,  0.0));
    float3 g = sampleSource(uv, float2(-2.0,  2.0));
    float3 h = sampleSource(uv, float2( 0.0,  2.0));
    float3 i = sampleSource(uv, float2( 2.0,  2.0));
    float3 j = sampleSource(uv, float2(-1.0, -1.0));
    float3 k = sampleSource(uv, float2( 1.0, -1.0));
    float3 l = sampleSource(uv, float2(-1.0,  1.0));
    float3 m = sampleSource(uv, float2( 1.0,  1.0));

    float3 color = (j + k + l + m) * 0.125;
    color += (a + b + d + e) * 0.03125;
    color += (b + c + e + f) * 0.03125;
    color += (d + e + g + h) * 0.03125;
    color += (e + f + h + i) * 0.03125;
    return max(color, 0.0);
}

float karisWeight(float3 color) {
    float luma = dot(max(color, 0.0), float3(0.2126, 0.7152, 0.0722));
    return rcp(1.0 + luma);
}

// The first full-resolution -> half-resolution step uses the five overlapping groups from the same
// 13-tap kernel. Karis weights suppress a disproportionately bright subpixel, and division by the
// effective weight sum preserves spatially constant colors exactly.
float3 sample13TapKaris(float2 uv) {
    float3 a = sampleSource(uv, float2(-2.0, -2.0));
    float3 b = sampleSource(uv, float2( 0.0, -2.0));
    float3 c = sampleSource(uv, float2( 2.0, -2.0));
    float3 d = sampleSource(uv, float2(-2.0,  0.0));
    float3 e = sampleSource(uv, float2( 0.0,  0.0));
    float3 f = sampleSource(uv, float2( 2.0,  0.0));
    float3 g = sampleSource(uv, float2(-2.0,  2.0));
    float3 h = sampleSource(uv, float2( 0.0,  2.0));
    float3 i = sampleSource(uv, float2( 2.0,  2.0));
    float3 j = sampleSource(uv, float2(-1.0, -1.0));
    float3 k = sampleSource(uv, float2( 1.0, -1.0));
    float3 l = sampleSource(uv, float2(-1.0,  1.0));
    float3 m = sampleSource(uv, float2( 1.0,  1.0));

    float3 group0 = (a + b + d + e) * 0.25;
    float3 group1 = (b + c + e + f) * 0.25;
    float3 group2 = (d + e + g + h) * 0.25;
    float3 group3 = (e + f + h + i) * 0.25;
    float3 group4 = (j + k + l + m) * 0.25;

    float weight0 = 0.125 * karisWeight(group0);
    float weight1 = 0.125 * karisWeight(group1);
    float weight2 = 0.125 * karisWeight(group2);
    float weight3 = 0.125 * karisWeight(group3);
    float weight4 = 0.5 * karisWeight(group4);
    float3 numerator = group0 * weight0 + group1 * weight1 + group2 * weight2 +
                       group3 * weight3 + group4 * weight4;
    float denominator = weight0 + weight1 + weight2 + weight3 + weight4;
    return max(numerator / max(denominator, 1.0e-6), 0.0);
}

[shader("fragment")]
float4 prefilterMain(VertexOutput input) : SV_Target {
    return float4(softThreshold(sample13TapKaris(input.uv)), 1.0);
}

[shader("fragment")]
float4 downsampleMain(VertexOutput input) : SV_Target {
    // Full 13-tap kernel repeated at every pyramid level, not just the prefilter, for maximum
    // quality: it stays firefly/flicker-resistant all the way down the chain. The mip chain (each
    // level is half the resolution of the last) is what keeps this affordable despite the extra taps.
    return float4(sample13Tap(input.uv), 1.0);
}

// Four bilinear samples at +/-0.5 source texels expand exactly to the normalized 3x3 tent kernel:
// center=4/16, cardinal neighbors=2/16, corners=1/16. Radii above 0.5 suppress the center and turn
// this into a four-lobed kernel rather than a wider tent.
static const float BLOOM_UPSAMPLE_FILTER_RADIUS = 0.5;

[shader("fragment")]
float4 upsampleMain(VertexOutput input) : SV_Target {
    float3 color = sampleSource(input.uv, float2(-BLOOM_UPSAMPLE_FILTER_RADIUS, -BLOOM_UPSAMPLE_FILTER_RADIUS));
    color += sampleSource(input.uv, float2( BLOOM_UPSAMPLE_FILTER_RADIUS, -BLOOM_UPSAMPLE_FILTER_RADIUS));
    color += sampleSource(input.uv, float2(-BLOOM_UPSAMPLE_FILTER_RADIUS,  BLOOM_UPSAMPLE_FILTER_RADIUS));
    color += sampleSource(input.uv, float2( BLOOM_UPSAMPLE_FILTER_RADIUS,  BLOOM_UPSAMPLE_FILTER_RADIUS));
    // Scatter is applied by normalized constant-color blending: coarse * scatter + fine * (1-scatter).
    return float4(color * 0.25, 1.0);
}
