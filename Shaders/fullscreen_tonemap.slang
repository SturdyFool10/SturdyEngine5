// Fullscreen tonemap post-process pass.
//
// Draws a single full-screen triangle (no vertex buffer — positions come from SV_VertexID) that
// samples the HDR scene-color target and maps it to the display-referred swapchain. This is the
// first concrete post-process slot of the deferred pipeline. Reuses the reflection-driven binding
// path, while a tiny push-constant block makes the high-level graph's output policy programmable.
//
// Every operator below returns a "reference-domain" float3 where 1.0 means paper/reference white:
// None/Reinhard/Exponential/AgX always stay <= 1.0 (their curves saturate by construction — they
// have no headroom concept), while Hermite/PsychoV can exceed 1.0 up to a defined ceiling when
// `hdrOutput` is set, since only those two are built around an explicit highlight-headroom target.
// The final encode step below is what actually branches on SDR vs HDR: SDR saturates to [0,1] and
// relies on the *Srgb swapchain format's automatic linear->sRGB encode on write; HDR scales by
// paper-white nits and PQ-encodes explicitly, since Vulkan applies no equivalent fixed-function
// curve for an HDR10/ST2084 swapchain (RendererLifecycle.cpp picks that format/color space whenever
// PresentationSettings::hdr_enabled is set).

import sturdy_common;
import sturdy_tonemap_agx;
import sturdy_tonemap_hermite;
import sturdy_tonemap_psychov;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    // Classic attribute-less fullscreen triangle: uv sweeps (0,0),(2,0),(0,2) so the primitive covers
    // the whole [-1,1] clip rect with the screen inside its first quadrant. Vulkan NDC Y and texture V
    // both point down, so map uv straight to clip space (uv*2-1) — negating Y here would flip the image.
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

Texture2D<float4> sceneColor;
SamplerState sceneSampler;

// Field order/types here must match Renderer/RendererPostProcess.cpp's TonemapConstants exactly —
// every field is a plain scalar (no vec3/array members) specifically so this struct's natural size
// matches that C++ struct byte-for-byte; see that file's doc comment on why alignas(16) is avoided.
struct TonemapConstants {
    float exposure;
    float whitePoint;
    float saturation;
    uint operation;

    uint hdrOutput;
    float hdrPaperWhiteNits;
    float hdrPeakNits;

    uint agxLook;

    float hermiteToeStrength;
    float hermiteToeLength;
    float hermiteShoulderStrength;
    float hermiteShoulderLength;
    float hermiteShoulderAngle;

    float psychovHighlights;
    float psychovShadows;
    float psychovContrast;
    float psychovPurityScale;
    float psychovGamutCompression;
    uint psychovGamutCompressionMode;
    float psychovCompression;
    float psychovAdaptedGrayR;
    float psychovAdaptedGrayG;
    float psychovAdaptedGrayB;
    float psychovBackgroundGrayR;
    float psychovBackgroundGrayG;
    float psychovBackgroundGrayB;

    uint antiAliasingMode;
    float antiAliasingSubpixel;
    float antiAliasingEdgeThreshold;
};

[[push_constant]] ConstantBuffer<TonemapConstants> tonemap;

float antiAliasingLuma(float3 hdr) {
    // Compress only for edge classification. Filtering still blends the original scene-linear HDR
    // values, so bright energy is conserved and tonemapping remains a single final operation.
    float3 proxy = max(hdr, 0.0) / (1.0 + max(hdr, 0.0));
    return dot(proxy, float3(0.2126, 0.7152, 0.0722));
}

float3 sampleScene(float2 uv) {
    return max(sceneColor.SampleLevel(sceneSampler, uv, 0.0).rgb, 0.0);
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
    if (lumaRange < max(1.0 / 32.0,
                        lumaMaximum * tonemap.antiAliasingEdgeThreshold)) {
        return center;
    }

    float2 direction = float2(
        -((lumaNorthWest + lumaNorthEast) - (lumaSouthWest + lumaSouthEast)),
          (lumaNorthWest + lumaSouthWest) - (lumaNorthEast + lumaSouthEast));
    float directionReduce = max(
        (lumaNorthWest + lumaNorthEast + lumaSouthWest + lumaSouthEast) *
            (0.25 * (1.0 / 8.0)),
        1.0 / 128.0);
    float reciprocalMinimum = 1.0 /
        (min(abs(direction.x), abs(direction.y)) + directionReduce);
    direction = clamp(direction * reciprocalMinimum, float2(-8.0), float2(8.0)) * texelSize;

    float3 inner = 0.5 * (
        sampleScene(uv + direction * (1.0 / 3.0 - 0.5)) +
        sampleScene(uv + direction * (2.0 / 3.0 - 0.5)));
    float3 outer = inner * 0.5 + 0.25 * (
        sampleScene(uv + direction * -0.5) +
        sampleScene(uv + direction *  0.5));
    float outerLuma = antiAliasingLuma(outer);
    float3 edgeResult = (outerLuma < lumaMinimum || outerLuma > lumaMaximum) ? inner : outer;

    // FXAA's sub-pixel term removes isolated one-pixel shimmer while remaining independently
    // controllable for users who prefer a harder presentation.
    float lumaAverage = 0.25 *
        (lumaNorthWest + lumaNorthEast + lumaSouthWest + lumaSouthEast);
    float subpixel = saturate(abs(lumaAverage - lumaCenter) / max(lumaRange, SFT_EPSILON));
    subpixel = subpixel * subpixel * (3.0 - 2.0 * subpixel);
    return lerp(edgeResult, 0.5 * (center + edgeResult),
                subpixel * tonemap.antiAliasingSubpixel);
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
    if (lumaRange < max(1.0 / 32.0,
                        lumaMaximum * tonemap.antiAliasingEdgeThreshold)) {
        return center;
    }

    // Classify the edge normal from local contrast and blend only across that normal. The
    // perpendicular pair acts as a corner guard, which is why this mode stays sharper than FXAA.
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
        (lumaRange - max(1.0 / 32.0, lumaMaximum * tonemap.antiAliasingEdgeThreshold)) /
        max(lumaRange, SFT_EPSILON));
    float blend = min(0.35, 0.35 * edgeStrength * cornerGuard);
    return lerp(center, closest, blend);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    uint sourceWidth;
    uint sourceHeight;
    sceneColor.GetDimensions(sourceWidth, sourceHeight);
    float2 texelSize = 1.0 / float2(max(sourceWidth, 1u), max(sourceHeight, 1u));
    float3 hdr;
    if (tonemap.antiAliasingMode == 1u) {
        hdr = applyFxaa(input.uv, texelSize);
    } else if (tonemap.antiAliasingMode == 2u) {
        hdr = applyConservativeMorphologicalAA(input.uv, texelSize);
    } else {
        hdr = sampleScene(input.uv);
    }
    float3 exposed = hdr * tonemap.exposure / max(tonemap.whitePoint, SFT_EPSILON);

    // Reference-domain headroom this frame's actual output can use: 1.0 for SDR (no headroom above
    // paper white), the configured peak/paper-white ratio for HDR. Only Hermite/PsychoV read this —
    // see this file's header comment.
    float headroomCeiling = tonemap.hdrOutput != 0u
        ? max(tonemap.hdrPeakNits / max(tonemap.hdrPaperWhiteNits, 1.0), 1.0)
        : 1.0;

    float3 mapped;
    if (tonemap.operation == 0) { // None
        mapped = exposed;
    } else if (tonemap.operation == 1) { // Reinhard
        mapped = reinhardTonemap(exposed);
    } else if (tonemap.operation == 2) { // Exponential
        mapped = exposureTonemap(exposed, 1.0);
    } else if (tonemap.operation == 3) { // AgX
        mapped = agxLook(agxTonemap(exposed), tonemap.agxLook);
    } else if (tonemap.operation == 4) { // HermiteSpline
        mapped = tonemap.hdrOutput != 0u
            ? hermiteTonemapHdr(exposed, tonemap.hermiteToeStrength, tonemap.hermiteToeLength,
                                tonemap.hermiteShoulderStrength, tonemap.hermiteShoulderLength,
                                tonemap.hermiteShoulderAngle, headroomCeiling)
            : hermiteTonemap(exposed, tonemap.hermiteToeStrength, tonemap.hermiteToeLength,
                             tonemap.hermiteShoulderStrength, tonemap.hermiteShoulderLength,
                             tonemap.hermiteShoulderAngle);
    } else { // PsychoV
        mapped = psychovTonemap(exposed, headroomCeiling, tonemap.psychovHighlights, tonemap.psychovShadows,
                                tonemap.psychovContrast, tonemap.psychovPurityScale, tonemap.psychovGamutCompression,
                                tonemap.psychovGamutCompressionMode, tonemap.psychovCompression,
                                float3(tonemap.psychovAdaptedGrayR, tonemap.psychovAdaptedGrayG, tonemap.psychovAdaptedGrayB),
                                float3(tonemap.psychovBackgroundGrayR, tonemap.psychovBackgroundGrayG, tonemap.psychovBackgroundGrayB));
    }

    float gray = luminance(mapped);
    mapped = lerp(float3(gray), mapped, tonemap.saturation);

    if (tonemap.hdrOutput != 0u) {
        return float4(pqEncode(max(mapped, 0.0) * tonemap.hdrPaperWhiteNits), 1.0);
    }
    return float4(saturate(mapped), 1.0);
}
