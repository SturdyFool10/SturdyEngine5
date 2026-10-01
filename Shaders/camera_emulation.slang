// Body-camera / camcorder look, one fullscreen pass on the scene-linear HDR image (after bloom and
// exposure, before tone mapping -- the sensor sees light before any display curve): barrel distortion,
// radial chromatic aberration, local-contrast sharpening, contrast about middle grey, saturation and a
// white-balance tint, shot/read-noise style sensor noise, vignette and an optional rounded-rectangle
// "housing" frame. Everything is per-pixel ALU plus a handful of texture taps, so the whole stack costs a
// fraction of a millisecond.
//
// Pixel-perfect fisheye: the frame is rendered *overscanned* -- the engine widens the projection by
// `overscan` and renders `overscan` times more pixels per axis -- and this pass maps the output back
// through the barrel curve. With src = centered * (1 + k r^2) / overscan, the source resolves exactly
// one texel per output pixel at the centre of the image and only ever *more* than one toward the edges
// (the image is minified, never stretched), and the corners land exactly on the source edge, so there is
// no crop and no empty border. Where the source has more than one texel per pixel the pass integrates
// them along the radial direction instead of point-sampling, so the compressed edges do not alias.
// Fields are documented in Engine::CameraEmulationSettings.
import sturdy_common;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Every member is a plain 4-byte scalar so the push-constant (std430) and emulated-uniform (std140)
// layouts agree; must match Renderer/CameraEmulation.cpp's CameraEmulationConstants byte for byte.
struct CameraEmulationConstants {
    float fisheye;
    float chromaticAberration;
    float vignette;
    float sensorNoise;

    float sharpen;
    float saturation;
    float contrast;
    float housing;

    float tintR;
    float tintG;
    float tintB;
    uint frameIndex;

    float aspect;
    // How much wider (and how many more pixels per axis) than the output the source was rendered.
    float overscan;
    float pad1;
    float pad2;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<CameraEmulationConstants> camera;
#else
[[push_constant]] ConstantBuffer<CameraEmulationConstants> camera;
#endif

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = fullscreenTrianglePosition(output.uv);
    return output;
}

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

float3 sampleSource(float2 uv) {
    return sourceTexture.SampleLevel(sourceSampler, clamp(uv, float2(0.0), float2(1.0)), 0.0).rgb;
}

// Averages the source along `radialStep` (source-uv per texel, pointing away from the centre) over
// `footprintTexels` texels, so a region the barrel curve compresses is integrated instead of skipped.
float3 sampleSourceFiltered(float2 uv, float2 radialStep, float footprintTexels) {
    if (footprintTexels <= 1.05) return sampleSource(uv);
    float extent = footprintTexels - 1.0;
    float3 sum = float3(0.0);
    [unroll]
    for (int i = 0; i < 5; ++i) {
        sum += sampleSource(uv + radialStep * (((float)i - 2.0) * 0.25 * extent));
    }
    return sum * 0.2;
}

uint hashPixel(uint2 pixel, uint frame) {
    uint h = pixel.x * 374761393u + pixel.y * 668265263u + frame * 2246822519u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    h ^= h >> 16u;
    h *= 2654435761u;
    return h ^ (h >> 15u);
}

float hashToUnit(uint h) { return (float)(h & 0xffffffu) * (1.0 / 16777216.0); }

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float2 uv = input.uv;
    float2 centered = (uv - 0.5) * 2.0;
    float2 aspectScaled = float2(centered.x * camera.aspect, centered.y);
    float cornerRadiusSquared = camera.aspect * camera.aspect + 1.0;
    // Normalised so the corner is r^2 = 1 whatever the aspect ratio.
    float radiusSquared = dot(aspectScaled, aspectScaled) / cornerRadiusSquared;

    // Barrel distortion: an output point at r samples the source at r * (1 + k r^2) / overscan.
    float overscan = max(camera.overscan, 1.0);
    float distortion = (1.0 + camera.fisheye * radiusSquared) / overscan;
    float2 sourceCentered = centered * distortion;

    // Source texels covered by one output pixel: radially d/dr[r (1 + k r^2)] = 1 + 3 k r^2 (times the
    // overscan that turns output pixels into source pixels, over the 1/overscan of the mapping = 1).
    float footprint = 1.0 + 3.0 * camera.fisheye * radiusSquared;

    uint sourceWidth, sourceHeight;
    sourceTexture.GetDimensions(sourceWidth, sourceHeight);
    float2 texel = 1.0 / float2(sourceWidth, sourceHeight);
    float2 radialDirection = length(centered) > 1.0e-5 ? normalize(centered) : float2(1.0, 0.0);
    float2 radialStep = radialDirection * texel;

    // Chromatic aberration: red and blue are pushed in opposite radial directions, growing with r^2.
    float2 caOffset = centered * (camera.chromaticAberration * radiusSquared);
    float2 uvG = sourceCentered * 0.5 + 0.5;
    float3 color;
    color.r = sampleSourceFiltered(uvG + caOffset * 0.5, radialStep, footprint).r;
    color.g = sampleSourceFiltered(uvG, radialStep, footprint).g;
    color.b = sampleSourceFiltered(uvG - caOffset * 0.5, radialStep, footprint).b;

    if (camera.sharpen > 0.0) {
        float3 blur = 0.25 * (sampleSource(uvG + float2(texel.x, 0.0)) + sampleSource(uvG - float2(texel.x, 0.0)) +
                              sampleSource(uvG + float2(0.0, texel.y)) + sampleSource(uvG - float2(0.0, texel.y)));
        // Clamped so a bright highlight cannot ring into a halo that then blooms through the tone curve.
        color += clamp(color - blur, float3(-0.5), float3(0.5)) * camera.sharpen;
    }

    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = lerp(float3(luma), color, camera.saturation);
    // Contrast about middle grey (0.18) as a power curve, which keeps black at black and never goes negative.
    color = 0.18 * pow(max(color, float3(0.0)) / 0.18, float3(camera.contrast));
    color *= float3(camera.tintR, camera.tintG, camera.tintB);

    if (camera.sensorNoise > 0.0) {
        uint2 pixel = uint2(input.position.xy);
        float noise = hashToUnit(hashPixel(pixel, camera.frameIndex)) - 0.5;
        float chromaNoise = hashToUnit(hashPixel(pixel + uint2(7919u, 104729u), camera.frameIndex)) - 0.5;
        // Shot noise grows with the square root of the signal (relative noise falls with brightness) and
        // a small read-noise floor keeps the shadows alive.
        float signal = max(luma, 0.0);
        float amplitude = camera.sensorNoise * 2.0 * (0.02 + sqrt(signal) * 0.25);
        color += (noise + float3(chromaNoise, -chromaNoise, 0.0) * 0.35) * amplitude;
    }

    float vignette = 1.0 - camera.vignette * saturate(radiusSquared);
    color *= vignette;

    if (camera.housing > 0.0) {
        // Signed distance to a rounded rectangle inset from the frame; darkens outside of it.
        float2 halfSize = float2(camera.aspect, 1.0) - 0.06;
        float2 q = abs(aspectScaled) - (halfSize - 0.12);
        float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - 0.12;
        color *= lerp(1.0, saturate(1.0 - distance * 12.0), camera.housing);
    }

    return float4(max(color, float3(0.0)), sourceTexture.SampleLevel(sourceSampler, uv, 0.0).a);
}
