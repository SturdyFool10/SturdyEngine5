// Scene-linear spatial anti-aliasing pass. Edge classification uses a compressed HDR luminance proxy,
// but filtering blends the original HDR samples so bloom and tonemapping receive preserved light energy.

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

struct AntiAliasingConstants {
    uint mode;
    float subpixel;
    float edgeThreshold;
    // 1 when the frame keeps its alpha (SceneSettings::preserve_alpha).
    uint preserveAlpha;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS

// WebGPU has no push constants at all, so on that target the same block is carried as an

// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of

// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant

// block is packed std430-style, a uniform buffer std140, so a struct whose members would

// pad differently between the two must be laid out to agree under both.

[[vk::binding(0, 3)]] ConstantBuffer<AntiAliasingConstants> antiAliasing;

#else

[[push_constant]] ConstantBuffer<AntiAliasingConstants> antiAliasing;

#endif
// The scene is filtered as RGBA. With `preserveAlpha` (SceneSettings::preserve_alpha) the colour is premultiplied and the
// alpha is a coverage that edges can be detected on too, so a silhouette against a transparent background is smoothed;
// without it alpha is carried through the same filter and ignored by the edge test, exactly as the colour-only filter
// behaved before.
float4 sampleScene(float2 uv) {
    float4 value = sourceTexture.SampleLevel(sourceSampler, uv, 0.0);
    return float4(max(value.rgb, 0.0), value.a);
}

float antiAliasingLuma(float3 hdr) {
    float3 proxy = max(hdr, 0.0) / (1.0 + max(hdr, 0.0));
    return dot(proxy, float3(0.2126, 0.7152, 0.0722));
}

// What edges are detected on: the colour's luma plus half the coverage when alpha is kept.
float edgeLuma(float4 value) {
    return antiAliasingLuma(value.rgb) + (antiAliasing.preserveAlpha != 0u ? 0.5 * saturate(value.a) : 0.0);
}

float4 applyFxaa(float2 uv, float2 texelSize) {
    float4 center = sampleScene(uv);
    float4 northWest = sampleScene(uv + texelSize * float2(-1.0, -1.0));
    float4 northEast = sampleScene(uv + texelSize * float2( 1.0, -1.0));
    float4 southWest = sampleScene(uv + texelSize * float2(-1.0,  1.0));
    float4 southEast = sampleScene(uv + texelSize * float2( 1.0,  1.0));

    float lumaCenter = edgeLuma(center);
    float lumaNorthWest = edgeLuma(northWest);
    float lumaNorthEast = edgeLuma(northEast);
    float lumaSouthWest = edgeLuma(southWest);
    float lumaSouthEast = edgeLuma(southEast);
    float lumaMinimum = min(lumaCenter, min(min(lumaNorthWest, lumaNorthEast),
                                            min(lumaSouthWest, lumaSouthEast)));
    float lumaMaximum = max(lumaCenter, max(max(lumaNorthWest, lumaNorthEast),
                                            max(lumaSouthWest, lumaSouthEast)));
    float lumaRange = lumaMaximum - lumaMinimum;
    // The threshold scales with the colour's brightness only, so keeping alpha does not change how interior edges are
    // judged.
    float colorLumaMaximum = max(antiAliasingLuma(center.rgb), max(max(antiAliasingLuma(northWest.rgb), antiAliasingLuma(northEast.rgb)),
                                                                   max(antiAliasingLuma(southWest.rgb), antiAliasingLuma(southEast.rgb))));
    if (lumaRange < max(1.0 / 32.0, colorLumaMaximum * antiAliasing.edgeThreshold)) {
        return center;
    }

    float2 direction = float2(
        -((lumaNorthWest + lumaNorthEast) - (lumaSouthWest + lumaSouthEast)),
          (lumaNorthWest + lumaSouthWest) - (lumaNorthEast + lumaSouthEast));
    float directionReduce = max(
        (lumaNorthWest + lumaNorthEast + lumaSouthWest + lumaSouthEast) * (0.25 * (1.0 / 8.0)),
        1.0 / 128.0);
    float reciprocalMinimum = 1.0 / (min(abs(direction.x), abs(direction.y)) + directionReduce);
    direction = clamp(direction * reciprocalMinimum, float2(-8.0), float2(8.0)) * texelSize;

    float4 inner = 0.5 * (
        sampleScene(uv + direction * (1.0 / 3.0 - 0.5)) +
        sampleScene(uv + direction * (2.0 / 3.0 - 0.5)));
    float4 outer = inner * 0.5 + 0.25 * (
        sampleScene(uv + direction * -0.5) +
        sampleScene(uv + direction *  0.5));
    float outerLuma = edgeLuma(outer);
    float4 edgeResult = (outerLuma < lumaMinimum || outerLuma > lumaMaximum) ? inner : outer;

    float lumaAverage = 0.25 *
        (lumaNorthWest + lumaNorthEast + lumaSouthWest + lumaSouthEast);
    float subpixel = saturate(abs(lumaAverage - lumaCenter) / max(lumaRange, SFT_EPSILON));
    subpixel = subpixel * subpixel * (3.0 - 2.0 * subpixel);
    return lerp(edgeResult, 0.5 * (center + edgeResult),
                subpixel * antiAliasing.subpixel);
}

float4 applyConservativeMorphologicalAA(float2 uv, float2 texelSize) {
    float4 center = sampleScene(uv);
    float4 north = sampleScene(uv + float2(0.0, -texelSize.y));
    float4 south = sampleScene(uv + float2(0.0,  texelSize.y));
    float4 west = sampleScene(uv + float2(-texelSize.x, 0.0));
    float4 east = sampleScene(uv + float2( texelSize.x, 0.0));
    float lumaCenter = edgeLuma(center);
    float lumaNorth = edgeLuma(north);
    float lumaSouth = edgeLuma(south);
    float lumaWest = edgeLuma(west);
    float lumaEast = edgeLuma(east);
    float lumaMinimum = min(lumaCenter, min(min(lumaNorth, lumaSouth), min(lumaWest, lumaEast)));
    float lumaMaximum = max(lumaCenter, max(max(lumaNorth, lumaSouth), max(lumaWest, lumaEast)));
    float lumaRange = lumaMaximum - lumaMinimum;
    float colorLumaMaximum = max(antiAliasingLuma(center.rgb), max(max(antiAliasingLuma(north.rgb), antiAliasingLuma(south.rgb)),
                                                                   max(antiAliasingLuma(west.rgb), antiAliasingLuma(east.rgb))));
    float edgeThreshold = max(1.0 / 32.0, colorLumaMaximum * antiAliasing.edgeThreshold);
    if (lumaRange < edgeThreshold) {
        return center;
    }

    float horizontalGradient = abs(lumaWest - lumaEast);
    float verticalGradient = abs(lumaNorth - lumaSouth);
    bool crossHorizontal = horizontalGradient >= verticalGradient;
    float4 negativeColor = crossHorizontal ? west : north;
    float4 positiveColor = crossHorizontal ? east : south;
    float negativeLuma = crossHorizontal ? lumaWest : lumaNorth;
    float positiveLuma = crossHorizontal ? lumaEast : lumaSouth;
    float tangentGradient = crossHorizontal
        ? abs(lumaNorth - lumaSouth)
        : abs(lumaWest - lumaEast);
    float cornerGuard = saturate(1.0 - tangentGradient / max(lumaRange, SFT_EPSILON));
    float4 closest = abs(negativeLuma - lumaCenter) < abs(positiveLuma - lumaCenter)
        ? negativeColor : positiveColor;
    float edgeStrength = saturate((lumaRange - edgeThreshold) / max(lumaRange, SFT_EPSILON));
    float blend = min(0.35, 0.35 * edgeStrength * cornerGuard);
    return lerp(center, closest, blend);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    uint sourceWidth;
    uint sourceHeight;
    sourceTexture.GetDimensions(sourceWidth, sourceHeight);
    float2 texelSize = 1.0 / float2(max(sourceWidth, 1u), max(sourceHeight, 1u));

    float4 color = sampleScene(input.uv);
    if (antiAliasing.mode == 1u) {
        color = applyFxaa(input.uv, texelSize);
    } else if (antiAliasing.mode == 2u) {
        color = applyConservativeMorphologicalAA(input.uv, texelSize);
    }
    // Alpha only matters when the frame keeps it; otherwise the scene is opaque.
    return float4(color.rgb, antiAliasing.preserveAlpha != 0u ? color.a : 1.0);
}
