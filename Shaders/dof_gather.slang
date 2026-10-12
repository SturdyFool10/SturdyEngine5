// Depth of field, pass 3 of 4: the bokeh gather, at half resolution, far and near fields separately.
//
// "Scatter as gather" (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", 2014; Abadie, "A Life of
// a Bokeh", 2018): instead of splatting each bright pixel as a polygon, every pixel looks around it and accepts a neighbour only
// if that neighbour's own blur disc reaches back to it. Same picture as splatting, but a gather: no atomics, no overdraw, and it
// runs identically on every backend.
//
//   * Samples lie on a Vogel spiral warped into the aperture polygon (`blades` sides), so highlights bloom into the
//     lens's actual bokeh shape. The rotation is fixed per pixel (not per frame): a stable pattern, never temporal noise.
//   * The search radius is the largest blur in the pixel's 3x3 tile neighbourhood (`dof_tile_max`), so a pixel only looks as far as
//     something that could reach it, and entirely sharp neighbourhoods exit at once.
//   * Each sample is weighted by how softly its disc covers this pixel (1 pixel feather) divided by the area of that disc, so a
//     blurred sample spreads its energy instead of adding it (no brightening of defocused areas).
//   * Far field: what lies behind the focus blurs into whatever is behind; the composite keeps an in-focus foreground pixel
//     sharp using its own circle of confusion, so a sharp foreground never leaks into the blur.
//   * Near field: what lies in front of the focus blurs and bleeds *over* the sharp surface behind it. Its alpha is the fraction
//     of the search disc covered by near-blurred samples, which gives the soft translucent edge a real foreground blur has.
//
// Outputs (rgba16f, half resolution): farOut = (colour, 1 if anything reached this pixel), nearOut = (colour, coverage).
import sturdy_common;
import dof_common;

ConstantBuffer<DofConstants> dof;
Texture2D<float4> colorHalf;
Texture2D<float4> cocHalf; // x = far diameter, y = near diameter (full-resolution pixels)
Texture2D<float4> tileMax;
SamplerState linearSampler;
[format("rgba16f")]
WTexture2D<float4> farOut;
[format("rgba16f")]
WTexture2D<float4> nearOut;

static const float kGoldenAngle = 2.39996323;

[shader("compute")]
[numthreads(8, 8, 1)]
void gatherMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= dof.extents.zw)) {
        return;
    }
    int2 pixel = int2(id.xy);
    float2 halfSize = float2(dof.extents.zw);
    float3 ownColor = colorHalf.Load(int3(pixel, 0)).rgb;

    // Search radius, in half-resolution texels: the largest diameter (full-resolution pixels) in the surrounding tiles is
    // four times the radius in half-resolution texels.
    int2 tile = pixel / int(DOF_TILE_SIZE);
    int2 maxTile = int2(dof.gather.yz) - 1;
    float2 widest = float2(0.0, 0.0);
    [unroll]
    for (int ty = -1; ty <= 1; ++ty) {
        [unroll]
        for (int tx = -1; tx <= 1; ++tx) {
            widest = max(widest, tileMax.Load(int3(clamp(tile + int2(tx, ty), int2(0, 0), maxTile), 0)).xy);
        }
    }
    float farRadius = widest.x * 0.25;
    float nearRadius = dof.lens.w > 0.5 ? widest.y * 0.25 : 0.0;
    if (farRadius < 0.5 && nearRadius < 0.5) {
        farOut.Store(pixel, float4(ownColor, 0.0));
        nearOut.Store(pixel, float4(ownColor, 0.0));
        return;
    }

    // Per-pixel rotation of the spiral from a fixed hash of the pixel (stable across frames).
    float rotation = frac(52.9829189 * frac(dot(float2(pixel), float2(0.06711056, 0.00583715)))) * 2.0 * SFT_PI;
    uint count = max(dof.gather.x, 8u);

    float3 farSum = float3(0.0, 0.0, 0.0);
    float farWeight = 0.0;
    float3 nearSum = float3(0.0, 0.0, 0.0);
    float nearWeight = 0.0;
    float nearCoverage = 0.0;
    [loop]
    for (uint i = 0; i < count; ++i) {
        float angle = float(i) * kGoldenAngle + rotation;
        float radius01 = sqrt((float(i) + 0.5) / float(count));
        float2 direction = float2(cos(angle), sin(angle)) * radius01 * dofPolygonRadius(angle, dof.lens.y, dof.lens.z);

        // One texture fetch pair serves both fields; each uses its own radius for the offset.
        if (farRadius >= 0.5) {
            float2 offset = direction * farRadius;
            float2 uv = (float2(pixel) + 0.5 + offset) / halfSize;
            float sampleRadius = cocHalf.SampleLevel(linearSampler, uv, 0.0).x * 0.25;
            float coverage = saturate(sampleRadius - length(offset) + 1.0);
            float weight = coverage / max(sampleRadius * sampleRadius, 1.0);
            farSum += colorHalf.SampleLevel(linearSampler, uv, 0.0).rgb * weight;
            farWeight += weight;
        }
        if (nearRadius >= 0.5) {
            float2 offset = direction * nearRadius;
            float2 uv = (float2(pixel) + 0.5 + offset) / halfSize;
            float sampleRadius = cocHalf.SampleLevel(linearSampler, uv, 0.0).y * 0.25;
            float coverage = saturate(sampleRadius - length(offset) + 1.0);
            float weight = coverage / max(sampleRadius * sampleRadius, 1.0);
            nearSum += colorHalf.SampleLevel(linearSampler, uv, 0.0).rgb * weight;
            nearWeight += weight;
            nearCoverage += coverage;
        }
    }
    farOut.Store(pixel, farWeight > 0.0 ? float4(farSum / farWeight, 1.0) : float4(ownColor, 0.0));
    float nearAlpha = saturate(nearCoverage / float(count) * 1.5);
    nearOut.Store(pixel, nearWeight > 0.0 ? float4(nearSum / nearWeight, nearAlpha) : float4(ownColor, 0.0));
}
