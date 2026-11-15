// CSS `clip-path` for UI effect groups (UI::Effects::clip_path in UI/UiEffectLibrary.hpp): everything the element draws is cut to a
// shape, with a one pixel antialiased edge.
//
// Parameters (80 bytes):
//   header = (mode, point count, corner radius in pixels, unused)   mode: 0 inset, 1 circle, 2 ellipse, 3 polygon
//   a      = inset: (top, right, bottom, left) pixels;  circle: (centre x, centre y as box fractions, radius px, unused);
//            ellipse: (centre x, centre y fractions, radius x px, radius y px)
//   points = polygon: up to six (x, y) points as box fractions, two per row (x0 y0 x1 y1 / x2 y2 x3 y3 / x4 y4 x5 y5)
//
// The content is premultiplied, so scaling the whole colour by the coverage keeps it premultiplied.

import sturdy_common;

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

struct ClipParameters {
    UiEffectConstants frame;
    float4 header;
    float4 a;
    float4 points0;
    float4 points1;
    float4 points2;
};

[[push_constant]] ConstantBuffer<ClipParameters> parameters;

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

float roundedRectSdf(float2 p, float2 center, float2 halfSize, float radius) {
    float r = min(radius, min(halfSize.x, halfSize.y));
    float2 q = abs(p - center) - (halfSize - float2(r, r));
    return length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - r;
}

float ellipseSdf(float2 p, float2 radii) {
    // First-order distance estimate to an axis-aligned ellipse centred at the origin.
    float2 r = max(radii, float2(0.001, 0.001));
    float k0 = length(p / r);
    float k1 = length(p / (r * r));
    return k1 > 0.0 ? k0 * (k0 - 1.0) / k1 : -min(r.x, r.y);
}

float2 polygonPoint(uint i) {
    float4 row = i < 2u ? parameters.points0 : (i < 4u ? parameters.points1 : parameters.points2);
    return (i & 1u) == 0u ? row.xy : row.zw;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 content = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
    float2 pixel = input.uv * parameters.frame.contentSize;
    float2 boxSize = max(parameters.frame.groupSize, float2(1.0, 1.0));
    float2 local = pixel - parameters.frame.groupOrigin; // pixels from the element's top-left
    int mode = int(parameters.header.x + 0.5);
    float sd = 0.0;
    if (mode == 0) {
        float top = parameters.a.x, right = parameters.a.y, bottom = parameters.a.z, left = parameters.a.w;
        float2 lo = float2(left, top);
        float2 hi = boxSize - float2(right, bottom);
        sd = roundedRectSdf(local, (lo + hi) * 0.5, max((hi - lo) * 0.5, float2(0.0, 0.0)), parameters.header.z);
    } else if (mode == 1) {
        float2 center = parameters.a.xy * boxSize;
        sd = length(local - center) - parameters.a.z;
    } else if (mode == 2) {
        float2 center = parameters.a.xy * boxSize;
        sd = ellipseSdf(local - center, parameters.a.zw);
    } else {
        int count = clamp(int(parameters.header.y + 0.5), 3, 6);
        float minDist = 1.0e9;
        bool inside = false;
        float2 previous = polygonPoint(uint(count - 1)) * boxSize;
        for (int i = 0; i < 6; ++i) {
            if (i >= count) break;
            float2 current = polygonPoint(uint(i)) * boxSize;
            float2 edge = current - previous;
            float2 toPoint = local - previous;
            float t = clamp(dot(toPoint, edge) / max(dot(edge, edge), 1.0e-6), 0.0, 1.0);
            minDist = min(minDist, length(toPoint - edge * t));
            // Even-odd crossing test.
            if (((previous.y > local.y) != (current.y > local.y)) &&
                (local.x < (current.x - previous.x) * (local.y - previous.y) / (current.y - previous.y) + previous.x)) {
                inside = !inside;
            }
            previous = current;
        }
        sd = inside ? -minDist : minDist;
    }
    float coverage = saturate(0.5 - sd);
    return content * coverage;
}
