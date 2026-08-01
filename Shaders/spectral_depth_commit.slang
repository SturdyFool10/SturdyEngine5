// Commits compute-generated primary-hit depth into the canonical hardware depth attachment.

import sturdy_common;

Texture2D<float> primaryDepth;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = fullscreenTriangleUv(vertexId);
    output.position = fullscreenTrianglePosition(output.uv);
    return output;
}

[shader("fragment")]
void fragmentMain(VertexOutput input, out float depth : SV_Depth) {
    uint width;
    uint height;
    primaryDepth.GetDimensions(width, height);
    uint2 pixel = min(uint2(input.uv * float2(width, height)), uint2(width - 1u, height - 1u));
    depth = primaryDepth.Load(int3(pixel, 0));
}
