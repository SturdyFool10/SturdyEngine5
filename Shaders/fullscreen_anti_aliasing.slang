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
};

[[push_constant]] ConstantBuffer<AntiAliasingConstants> antiAliasing;

float3 sampleScene(float2 uv) {
    return max(sourceTexture.SampleLevel(sourceSampler, uv, 0.0).rgb, 0.0);
}

float antiAliasingLuma(float3 hdr) {
    float3 proxy = max(hdr, 0.0) / (1.0 + max(hdr, 0.0));
    return dot(proxy, float3(0.2126, 0.7152, 0.0722));
}

float3 applyFxaa(float2 uv, float2 texelSize) {
    float3 center = sampleScene(uv);
    float3 northWest = sampleScene(uv + texelSize * float2(-1.0, -1.0));
    float3 northEast = sampleScene(uv + texelSize * float2( 1.0, -1.0));
    float3 southWest = sampleScene(uv + texelSize * float2(-1.0,  1.0));
    float3 southEast = sampleScene(uv + texelSize * float2( 1.0,  1.0));

    float lumaCenter = antiAliasingLuma(center);
    float lumaNorthWest = antiAliasingLuma(northWest);
    float lumaNorthEast = antiAliasingLuma(northEast);
    float lumaSouthWest = antiAliasingLuma(southWest);
    float lumaSouthEast = antiAliasingLuma(southEast);
    float lumaMinimum = min(lumaCenter, min(min(lumaNorthWest, lumaNorthEast),
                                            min(lumaSouthWest, lumaSouthEast)));
    float lumaMaximum = max(lumaCenter, max(max(lumaNorthWest, lumaNorthEast),
                                            max(lumaSouthWest, lumaSouthEast)));
    float lumaRange = lumaMaximum - lumaMinimum;
    if (lumaRange < max(1.0 / 32.0, lumaMaximum * antiAliasing.edgeThreshold)) {
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

    float3 inner = 0.5 * (
        sampleScene(uv + direction * (1.0 / 3.0 - 0.5)) +
        sampleScene(uv + direction * (2.0 / 3.0 - 0.5)));
    float3 outer = inner * 0.5 + 0.25 * (
        sampleScene(uv + direction * -0.5) +
        sampleScene(uv + direction *  0.5));
    float outerLuma = antiAliasingLuma(outer);
    float3 edgeResult = (outerLuma < lumaMinimum || outerLuma > lumaMaximum) ? inner : outer;

    float lumaAverage = 0.25 *
        (lumaNorthWest + lumaNorthEast + lumaSouthWest + lumaSouthEast);
    float subpixel = saturate(abs(lumaAverage - lumaCenter) / max(lumaRange, SFT_EPSILON));
    subpixel = subpixel * subpixel * (3.0 - 2.0 * subpixel);
    return lerp(edgeResult, 0.5 * (center + edgeResult),
                subpixel * antiAliasing.subpixel);
}

float3 applyConservativeMorphologicalAA(float2 uv, float2 texelSize) {
    float3 center = sampleScene(uv);
    float3 north = sampleScene(uv + float2(0.0, -texelSize.y));
    float3 south = sampleScene(uv + float2(0.0,  texelSize.y));
    float3 west = sampleScene(uv + float2(-texelSize.x, 0.0));
    float3 east = sampleScene(uv + float2( texelSize.x, 0.0));
    float lumaCenter = antiAliasingLuma(center);
    float lumaNorth = antiAliasingLuma(north);
    float lumaSouth = antiAliasingLuma(south);
    float lumaWest = antiAliasingLuma(west);
    float lumaEast = antiAliasingLuma(east);
    float lumaMinimum = min(lumaCenter, min(min(lumaNorth, lumaSouth), min(lumaWest, lumaEast)));
    float lumaMaximum = max(lumaCenter, max(max(lumaNorth, lumaSouth), max(lumaWest, lumaEast)));
    float lumaRange = lumaMaximum - lumaMinimum;
    if (lumaRange < max(1.0 / 32.0, lumaMaximum * antiAliasing.edgeThreshold)) {
        return center;
    }

    float horizontalGradient = abs(lumaWest - lumaEast);
    float verticalGradient = abs(lumaNorth - lumaSouth);
    bool crossHorizontal = horizontalGradient >= verticalGradient;
    float3 negativeColor = crossHorizontal ? west : north;
    float3 positiveColor = crossHorizontal ? east : south;
    float negativeLuma = crossHorizontal ? lumaWest : lumaNorth;
    float positiveLuma = crossHorizontal ? lumaEast : lumaSouth;
    float tangentGradient = crossHorizontal
        ? abs(lumaNorth - lumaSouth)
        : abs(lumaWest - lumaEast);
    float cornerGuard = saturate(1.0 - tangentGradient / max(lumaRange, SFT_EPSILON));
    float3 closest = abs(negativeLuma - lumaCenter) < abs(positiveLuma - lumaCenter)
        ? negativeColor : positiveColor;
    float edgeStrength = saturate(
        (lumaRange - max(1.0 / 32.0, lumaMaximum * antiAliasing.edgeThreshold)) /
        max(lumaRange, SFT_EPSILON));
    float blend = min(0.35, 0.35 * edgeStrength * cornerGuard);
    return lerp(center, closest, blend);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    uint sourceWidth;
    uint sourceHeight;
    sourceTexture.GetDimensions(sourceWidth, sourceHeight);
    float2 texelSize = 1.0 / float2(max(sourceWidth, 1u), max(sourceHeight, 1u));

    float3 color = sampleScene(input.uv);
    if (antiAliasing.mode == 1u) {
        color = applyFxaa(input.uv, texelSize);
    } else if (antiAliasing.mode == 2u) {
        color = applyConservativeMorphologicalAA(input.uv, texelSize);
    }
    return float4(color, 1.0);
}
