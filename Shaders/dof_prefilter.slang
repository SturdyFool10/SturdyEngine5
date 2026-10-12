// Depth of field, pass 1 of 4: half-resolution colour and circle of confusion.
//
// Each half-resolution texel takes its 2x2 full-resolution block: the colour is a firefly-resistant average (weighted by
// 1 / (1 + luminance), so one very bright pixel cannot dominate the bokeh), and the circle of confusion keeps the *largest*
// far and near diameters separately, so a thin in-focus object in front of a blurred background still blurs its surroundings.
//
// Outputs: colour (rgb), and (x = far diameter, y = near diameter) in full-resolution pixels, both >= 0.
import sturdy_common;
import dof_common;

ConstantBuffer<DofConstants> dof;
Texture2D<float4> sceneColor;
DepthTexture2D sceneDepth;
[format("rgba16f")]
WTexture2D<float4> colorOut;
// rgba16f rather than rg16f: WGSL has no rg16f storage format. Only .xy is used.
[format("rgba16f")]
WTexture2D<float4> cocOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void prefilterMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= dof.extents.zw)) {
        return;
    }
    int2 maxPixel = int2(dof.extents.xy) - 1;
    int2 base = int2(id.xy) * 2;
    float3 colorSum = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;
    float far = 0.0;
    float near = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        int2 pixel = min(base + int2(i & 1, i >> 1), maxPixel);
        float3 color = sceneColor.Load(int3(pixel, 0)).rgb;
        color = all(color == color) && all(abs(color) < 65000.0) ? max(color, float3(0.0, 0.0, 0.0)) : float3(0.0, 0.0, 0.0);
        float weight = 1.0 / (1.0 + dot(color, float3(0.2126, 0.7152, 0.0722)));
        colorSum += color * weight;
        weightSum += weight;
        float coc = dofSignedCoc(dof, sceneDepth.Load(int3(pixel, 0)));
        far = max(far, coc);
        near = max(near, -coc);
    }
    colorOut.Store(int2(id.xy), float4(colorSum / max(weightSum, 1.0e-6), 1.0));
    cocOut.Store(int2(id.xy), float4(far, near, 0.0, 0.0));
}
