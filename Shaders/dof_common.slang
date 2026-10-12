// Shared by dof_{prefilter,tile_max,gather,composite}.slang. `DofConstants` mirrors `DofConstants` in Renderer/DepthOfField.cpp;
// change both sides together.
//
// The circle of confusion is the thin-lens one: for a lens of focal length f and f-number N focused at distance d, a point at
// view depth z blurs to a disc of diameter  f^2 / (N (d - f)) * |z - d| / z  on the sensor. The host folds everything but the
// depth ratio into one number, `blurScale` (pixels per unit of |z - d| / z), so the shaders only evaluate that ratio.
import sturdy_common;

struct DofConstants {
    // full width, full height, half width, half height
    uint4 extents;
    // depth linearisation a, b (view depth = a / (b - device depth)), focus distance, blur scale (pixels)
    float4 depth;
    // max blur diameter (full-resolution pixels), blade count, blade rotation (radians), near field enabled (1 / 0)
    float4 lens;
    // gather sample count, tile extent x, tile extent y, unused
    uint4 gather;
};

static const uint DOF_TILE_SIZE = 8; // half-resolution texels per tile edge

float dofViewDepth(DofConstants c, float deviceDepth) {
    return c.depth.x / max(c.depth.y - deviceDepth, 1.0e-6);
}

// Signed blur diameter in full-resolution pixels: positive behind the focus plane (far field), negative in front (near field).
// Sky (depth 1) is infinitely far and blurs the full amount a far object can.
float dofSignedCoc(DofConstants c, float deviceDepth) {
    float viewDepth = deviceDepth >= 0.999999 ? 1.0e9 : dofViewDepth(c, deviceDepth);
    float coc = c.depth.w * (viewDepth - c.depth.z) / max(viewDepth, 1.0e-4);
    return clamp(coc, -c.lens.x, c.lens.x);
}

// Radius of a regular polygon with circumradius 1 in direction `angle`, for an aperture of `blades` blades: the bokeh shape.
// Scaled so the polygon has the area of the unit disc, which keeps the blur size comparable between blade counts.
float dofPolygonRadius(float angle, float blades, float rotation) {
    float sector = 2.0 * SFT_PI / blades;
    float local = angle - rotation;
    local = local - floor(local / sector) * sector - 0.5 * sector;
    float radius = cos(0.5 * sector) / cos(local);
    float areaScale = sqrt(SFT_PI / (0.5 * blades * sin(sector)));
    return radius * areaScale;
}
