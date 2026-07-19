// Bandwidth-optimized mip-pyramid bloom inspired by Call of Duty: Advanced Warfare.
// One high-quality 13-tap prefilter extracts stable highlights at half resolution. Lower levels use
// four hardware-bilinear taps, and reconstruction uses four half-texel bilinear taps equivalent to
// the original 3x3 tent weights. Intermediates are rendered to packed RG11B10Float.

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

struct BloomConstants {
    float2 sourceTexelSize;
    float threshold;
    float softKnee;
    float scatter;
};

[[push_constant]] ConstantBuffer<BloomConstants> bloom;

float3 sampleSource(float2 uv, float2 texelOffset) {
    return sourceTexture.SampleLevel(sourceSampler, uv + texelOffset * bloom.sourceTexelSize, 0.0).rgb;
}

float3 softThreshold(float3 color) {
    float brightness = max(color.r, max(color.g, color.b));
    float knee = max(bloom.threshold * bloom.softKnee, 1.0e-5);
    float soft = clamp(brightness - bloom.threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1.0e-5);
    return color * (max(brightness - bloom.threshold, soft) / max(brightness, 1.0e-5));
}

[shader("fragment")]
float4 prefilterMain(VertexOutput input) : SV_Target {
    float3 a = sampleSource(input.uv, float2(-2.0, -2.0));
    float3 b = sampleSource(input.uv, float2( 0.0, -2.0));
    float3 c = sampleSource(input.uv, float2( 2.0, -2.0));
    float3 d = sampleSource(input.uv, float2(-2.0,  0.0));
    float3 e = sampleSource(input.uv, float2( 0.0,  0.0));
    float3 f = sampleSource(input.uv, float2( 2.0,  0.0));
    float3 g = sampleSource(input.uv, float2(-2.0,  2.0));
    float3 h = sampleSource(input.uv, float2( 0.0,  2.0));
    float3 i = sampleSource(input.uv, float2( 2.0,  2.0));
    float3 j = sampleSource(input.uv, float2(-1.0, -1.0));
    float3 k = sampleSource(input.uv, float2( 1.0, -1.0));
    float3 l = sampleSource(input.uv, float2(-1.0,  1.0));
    float3 m = sampleSource(input.uv, float2( 1.0,  1.0));

    float3 color = (j + k + l + m) * 0.125;
    color += (a + b + d + e) * 0.03125;
    color += (b + c + e + f) * 0.03125;
    color += (d + e + g + h) * 0.03125;
    color += (e + f + h + i) * 0.03125;
    return float4(softThreshold(max(color, 0.0)), 1.0);
}

[shader("fragment")]
float4 downsampleMain(VertexOutput input) : SV_Target {
    // Four bilinear taps cover a stable 4x4 source footprint. Lower pyramid levels are already
    // band-limited by the prefilter, so repeating the 13-tap kernel only burns bandwidth.
    float3 color = sampleSource(input.uv, float2(-1.0, -1.0));
    color += sampleSource(input.uv, float2( 1.0, -1.0));
    color += sampleSource(input.uv, float2(-1.0,  1.0));
    color += sampleSource(input.uv, float2( 1.0,  1.0));
    return float4(color * 0.25, 1.0);
}

[shader("fragment")]
float4 upsampleMain(VertexOutput input) : SV_Target {
    // Four half-texel bilinear taps produce the same normalized [1 2 1]x[1 2 1] tent footprint as
    // nine point samples: center=4/16, edges=2/16, corners=1/16.
    float3 color = sampleSource(input.uv, float2(-0.5, -0.5));
    color += sampleSource(input.uv, float2( 0.5, -0.5));
    color += sampleSource(input.uv, float2(-0.5,  0.5));
    color += sampleSource(input.uv, float2( 0.5,  0.5));
    return float4(color * (bloom.scatter * 0.25), 1.0);
}
