// Minimal hardcoded-triangle shader used to exercise shader discovery + SPIR-V compilation.
struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

static const float2 kPositions[3] = {
    float2(0.5, 0.5),
    float2(0.0, -0.5),
    float2(-0.5, 0.5),
};

static const float3 kColors[3] = {
    float3(1.0, 0.0, 0.0),
    float3(0.0, 1.0, 0.0),
    float3(0.0, 0.0, 1.0),
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexID : SV_VertexID) {
    VertexOutput output;
    output.position = float4(kPositions[vertexID], 0.0, 1.0);
    output.color = kColors[vertexID];
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
