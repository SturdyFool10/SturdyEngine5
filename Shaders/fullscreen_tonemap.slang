// Fullscreen tonemap post-process pass.
//
// Draws a single full-screen triangle (no vertex buffer — positions come from SV_VertexID) that
// samples the HDR scene-color target and maps it to the display-referred swapchain. This is the
// final display-transform slot of the deferred pipeline. Reuses the reflection-driven binding
// path, while a tiny push-constant block makes the high-level graph's output policy programmable.
//
// Every operator below returns a "reference-domain" float3 where 1.0 means paper/reference white:
// None/Reinhard/Exponential/AgX always stay <= 1.0 (their curves saturate by construction — they
// have no headroom concept), while Hermite/PsychoV can exceed 1.0 up to a defined ceiling when
// `hdrOutput` is set, since only those two are built around an explicit highlight-headroom target.
// The final encode step below is what actually branches on SDR vs HDR: SDR saturates to [0,1] and
// relies on the *Srgb swapchain format's automatic linear->sRGB encode on write. HDR scales by
// paper-white nits and then either PQ-encodes explicitly (Hdr10St2084 — Vulkan applies no equivalent
// fixed-function curve the way it does for an *Srgb format) or divides by scRGB's fixed 80-nit
// reference white with no curve at all (ScrgbLinear — the OS/compositor tone-maps the linear float
// target itself). RendererLifecycle.cpp picks the matching swapchain format/color space whenever
// PresentationSettings::hdr_enabled is set, from hdr_color_space (Hdr10St2084 vs ScrgbLinear).

import sturdy_common;
import sturdy_tonemap_agx;
import sturdy_tonemap_hermite;
import sturdy_tonemap_psychov;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Field order/types here must match Renderer/RendererPostProcess.cpp's TonemapConstants exactly —
// every field is a plain scalar (no vec3/array members) specifically so this struct's natural size
// matches that C++ struct byte-for-byte; see that file's doc comment on why alignas(16) is avoided.
struct TonemapConstants {
    float exposure;
    float whitePoint;
    float saturation;
    uint operation;

    uint hdrOutput;
    // Only read when hdrOutput != 0 — 0 = Hdr10St2084 (PQ-encode), 1 = ScrgbLinear (no curve),
    // 2 = Hdr10Hlg (HLG-encode), 3 = DolbyVision (best-effort: falls back to PQ-encode, same as
    // Hdr10St2084 — see RHI::ColorSpace::DolbyVision's own doc comment for why). Must stay
    // numerically in sync with RendererPostProcess.cpp's TonemapConstants and
    // Core::HdrColorSpaceMode's enumerator order (Core/Renderer.hpp).
    uint hdrColorSpace;
    float hdrPaperWhiteNits;
    float hdrPeakNits;
    // When set, source RGB is premultiplied linear light. Tonemap/encode the unassociated color,
    // then premultiply the encoded result again for swapchain compositor alpha.
    uint preserveAlpha;

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
};

[[push_constant]] ConstantBuffer<TonemapConstants> tonemap;

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    // Classic attribute-less fullscreen triangle: uv sweeps (0,0),(2,0),(0,2) so the primitive covers
    // the whole [-1,1] clip rect with the screen inside its first quadrant.
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = fullscreenTrianglePosition(output.uv);
    return output;
}

Texture2D<float4> sceneColor;
SamplerState sceneSampler;

float4 sampleScene(float2 uv) {
    return sceneColor.SampleLevel(sceneSampler, uv, 0.0);
}


[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 source = sampleScene(input.uv);
    float alpha = tonemap.preserveAlpha != 0u ? clamp01(source.a) : 1.0;
    float3 hdr = max(source.rgb, 0.0);
    if (tonemap.preserveAlpha != 0u) {
        hdr = alpha > SFT_EPSILON ? hdr / alpha : float3(0.0);
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
        // Internal scene and UI colors use linear BT.709/sRGB primaries. PQ/HLG swapchain color
        // spaces use BT.2020, while scRGB intentionally keeps linear sRGB primaries.
        float3 outputLinear = tonemap.hdrColorSpace == 1u
            ? max(mapped, 0.0)
            : linearSrgbToLinearBt2020(max(mapped, 0.0));
        float3 nits = outputLinear * tonemap.hdrPaperWhiteNits;
        float3 encoded;
        if (tonemap.hdrColorSpace == 1u) { // ScrgbLinear
            encoded = scrgbEncode(nits);
        } else if (tonemap.hdrColorSpace == 2u) { // Hdr10Hlg
            encoded = hlgEncode(nits);
        } else { // Hdr10St2084, or DolbyVision falling back to the same PQ encode (best-effort — see
                 // RHI::ColorSpace::DolbyVision's own doc comment).
            encoded = pqEncode(nits);
        }
        return float4(encoded * alpha, alpha);
    }
    if (tonemap.preserveAlpha != 0u) {
        // The attachment is *Srgb, so fixed-function encoding happens after this shader. Convert the
        // desired encoded-premultiplied value back to linear first; the attachment encode then stores
        // exactly sRGB(straightColor) * alpha for premultiplied compositor composition.
        float3 associatedSrgb = linearToSrgb(saturate(mapped)) * alpha;
        return float4(srgbToLinear(associatedSrgb), alpha);
    }
    return float4(saturate(mapped), 1.0);
}
