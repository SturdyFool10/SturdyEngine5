// Screen-space reflections, pass 2 of 2: one deterministic mirror ray per pixel, glossiness from a prefiltered colour chain.
//
// There is no random sampling anywhere, so there is nothing to denoise and nothing to ghost: the ray goes along the
// specular-dominant reflection direction (the mirror direction leaned towards the normal on rough surfaces, so the visible
// lobe centre is right), and is walked through the min-depth pyramid in screen space (the traversal of AMD FidelityFX
// SSSR). Cells the ray passes in front of are skipped a level coarser, cells it would enter go finer, and reaching below
// level 0 is a hit. The hit is validated against full-resolution depth with a thickness, rejected on back faces and faded
// at screen edges and towards the roughness cutoff.
//
// Roughness is not sampled but filtered (cone tracing, Valient 2014; Uludag 2014): the lit image of the previous frame is
// kept as a chain of progressively blurred half-resolution levels (`reflections_history`), and the level read at the hit is
// chosen from how wide the specular cone has grown by the time it gets there -- sharp up close, soft far away, exactly like
// a real glossy reflection. Rays that miss or leave the screen report zero confidence; the lighting pass fills those from the
// prefiltered sky.
//
// Output (full resolution, rgba16f): rgb = reflected radiance before exposure, a = confidence.
import sturdy_common;
import sturdy_pbr;
import sturdy_lighting_data;
import sturdy_specular;
import ssr_common;

ConstantBuffer<ShadowLightingGpuData> lightingData;
ConstantBuffer<SsrConstants> ssr;
DepthTexture2D sceneDepth;
Texture2D<float2> gbufferNormal;
Texture2D<float4> gbufferMaterial;
Texture2D<float2> gbufferMotion;
Texture2D<float> depthPyramid;
// Last frame's lit colour (exposure-free), level 0 at half resolution and each next level a blur at half the size.
Texture2D<float4> colorLevel0;
Texture2D<float4> colorLevel1;
Texture2D<float4> colorLevel2;
Texture2D<float4> colorLevel3;
Texture2D<float4> colorLevel4;
SamplerState linearSampler;
[format("rgba16f")]
WTexture2D<float4> reflectionOut;

float3 sampleColorLevel(uint level, float2 uv) {
    if (level == 0u) return colorLevel0.SampleLevel(linearSampler, uv, 0.0).rgb;
    if (level == 1u) return colorLevel1.SampleLevel(linearSampler, uv, 0.0).rgb;
    if (level == 2u) return colorLevel2.SampleLevel(linearSampler, uv, 0.0).rgb;
    if (level == 3u) return colorLevel3.SampleLevel(linearSampler, uv, 0.0).rgb;
    return colorLevel4.SampleLevel(linearSampler, uv, 0.0).rgb;
}

// Trilinear between the two blur levels around a continuous level.
float3 sampleColorChain(float level, float2 uv) {
    float lastLevel = float(max(ssr.params.y, 1u) - 1u);
    float l = clamp(level, 0.0, lastLevel);
    uint lower = (uint)floor(l);
    uint upper = min(lower + 1u, (uint)lastLevel);
    return lerp(sampleColorLevel(lower, uv), sampleColorLevel(upper, uv), frac(l));
}

static const float SSR_FLT_MAX = 3.402823466e+38;

float2 pyramidResolution(uint level) {
    return float2(ssrLevelExtent(ssr.extents.zw, level));
}

// Hierarchical min-depth traversal. `origin`/`direction` are in (uv, device depth). Returns true and the hit position
// when the ray converged below level 0 inside the screen.
bool hierarchicalTrace(float3 origin, float3 direction, out float3 position) {
    uint2 halfExtent = ssr.extents.zw;
    float3 invDirection = float3(direction.x != 0.0 ? 1.0 / direction.x : SSR_FLT_MAX,
                                 direction.y != 0.0 ? 1.0 / direction.y : SSR_FLT_MAX,
                                 direction.z != 0.0 ? 1.0 / direction.z : SSR_FLT_MAX);
    float2 floorOffset = float2(direction.x < 0.0 ? 0.0 : 1.0, direction.y < 0.0 ? 0.0 : 1.0);
    float2 resolution = pyramidResolution(0u);
    // Nudges each step just past the cell boundary it lands on, so the next cell is the one looked up.
    float2 uvOffset = 0.005 / resolution;
    uvOffset = float2(direction.x < 0.0 ? -uvOffset.x : uvOffset.x, direction.y < 0.0 ? -uvOffset.y : uvOffset.y);

    // Step out of the starting cell first: the surface the ray leaves is in it.
    float2 plane = (floor(origin.xy * resolution) + floorOffset) / resolution + uvOffset;
    float2 tStart = (plane - origin.xy) * invDirection.xy;
    float t = min(tStart.x, tStart.y);
    position = origin + t * direction;
    // The last ray parameter known to be in front of everything in its cell: the refinement below searches from here.
    float tSafe = t;

    int level = 0;
    uint steps = 0;
    [loop]
    while (steps < ssr.params.x && level >= 0) {
        if (any(position.xy < 0.0) || any(position.xy > 1.0) || position.z >= 1.0 || position.z <= 0.0) {
            return false;
        }
        float2 cellPosition = position.xy * resolution;
        float surfaceZ = ssrLoadPyramid(depthPyramid, halfExtent, (uint)level, int2(floor(cellPosition)));
        float2 xyPlane = (floor(cellPosition) + floorOffset) / resolution + uvOffset;
        float3 tPlanes = (float3(xyPlane, surfaceZ) - origin) * invDirection;
        // The depth plane only stops a ray moving away from the camera.
        tPlanes.z = direction.z > 0.0 ? tPlanes.z : SSR_FLT_MAX;
        float tMin = min(min(tPlanes.x, tPlanes.y), tPlanes.z);
        bool aboveSurface = surfaceZ > position.z;
        bool skippedCell = tMin != tPlanes.z && aboveSurface;
        if (aboveSurface) {
            tSafe = t;
        }
        t = aboveSurface ? tMin : t;
        position = origin + t * direction;
        level = skippedCell ? min(level + 1, int(SSR_PYRAMID_LEVELS) - 1) : level - 1;
        resolution = pyramidResolution((uint)max(level, 0));
        ++steps;
    }
    if (level >= 0 || any(position.xy < 0.0) || any(position.xy > 1.0)) {
        return false;
    }
    // The traversal lands on a half-resolution cell edge, which would quantise every hit to a 2-pixel grid (stair-stepped
    // reflection boundaries). Bisect between the last clear point and that edge against the full-resolution depth to find the
    // surface crossing to sub-pixel accuracy.
    float tLow = tSafe;
    float tHigh = t;
    [unroll]
    for (uint i = 0; i < 6; ++i) {
        float tMid = 0.5 * (tLow + tHigh);
        float3 probe = origin + tMid * direction;
        int2 probePixel = clamp(int2(probe.xy * float2(ssr.extents.xy)), int2(0, 0), int2(ssr.extents.xy) - 1);
        if (probe.z >= sceneDepth.Load(int3(probePixel, 0))) {
            tHigh = tMid;
        } else {
            tLow = tMid;
        }
    }
    position = origin + tHigh * direction;
    return all(position.xy >= 0.0) && all(position.xy <= 1.0);
}

float3 projectToScreen(float3 world) {
    float4 clip = mul(float4(world, 1.0), lightingData.viewProjection);
    float3 ndc = clip.xyz / (abs(clip.w) > SFT_EPSILON ? clip.w : SFT_EPSILON);
    return float3(ndcToUv(ndc.xy), ndc.z);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void traceMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= ssr.extents.xy)) {
        return;
    }
    int2 pixel = int2(id.xy);
    const float4 none = float4(0.0, 0.0, 0.0, 0.0);
    float deviceDepth = sceneDepth.Load(int3(pixel, 0));
    float perceptualRoughness = saturate(gbufferMaterial.Load(int3(pixel, 0)).x);
    float maxRoughness = ssr.tuning.x;
    if (deviceDepth >= 0.999999 || perceptualRoughness > maxRoughness || ssr.params.z == 0u) {
        reflectionOut.Store(pixel, none);
        return;
    }

    float2 uv = (float2(pixel) + 0.5) / float2(ssr.extents.xy);
    float3 worldPosition = ssrWorldFromDepth(lightingData, uv, deviceDepth);
    float3 normal = decodeOctahedralNormal(gbufferNormal.Load(int3(pixel, 0)));
    float3 viewVector = safeNormalizeOr(lightingData.cameraPositionNear.xyz - worldPosition, normal);
    float alphaRoughness = perceptualRoughness * perceptualRoughness;
    float3 reflected = safeNormalizeOr(reflect(-viewVector, normal), normal);
    // Rough lobes lean towards the normal: the direction the visible reflection is centred on.
    reflected = specularDominantDirection(normal, reflected, alphaRoughness);

    // Start a pixel and a half off the surface (in world units at this depth) so the ray does not hit its own texel.
    float viewDepth = ssrViewDepth(lightingData, worldPosition);
    float worldPerPixel = viewDepth * 2.0 * lightingData.viewportParams.y / max(lightingData.viewportParams.z, SFT_EPSILON);
    float3 start = worldPosition + normal * (worldPerPixel * 1.5);
    // March a length of one view depth, cut short before the near plane for rays coming back towards the camera.
    float nearPlane = lightingData.cameraPositionNear.w;
    float startViewZ = mul(float4(start, 1.0), lightingData.view).z;
    float directionViewZ = mul(float4(reflected, 0.0), lightingData.view).z;
    float rayLength = viewDepth;
    if (directionViewZ > 1.0e-4) {
        rayLength = min(rayLength, max((-nearPlane - startViewZ) / directionViewZ * 0.9, 0.0));
    }
    float3 origin = projectToScreen(start);
    float3 direction = projectToScreen(start + reflected * max(rayLength, 1.0e-3)) - origin;

    float3 hit;
    if (!hierarchicalTrace(origin, direction, hit)) {
        reflectionOut.Store(pixel, none);
        return;
    }

    int2 hitPixel = clamp(int2(hit.xy * float2(ssr.extents.xy)), int2(0, 0), int2(ssr.extents.xy) - 1);
    float surfaceDepth = sceneDepth.Load(int3(hitPixel, 0));
    float confidence = surfaceDepth < 0.999999 ? 1.0 : 0.0;
    float3 hitWorld = ssrWorldFromDepth(lightingData, hit.xy, surfaceDepth);
    // Thickness: the ray must end close behind (or, against the half-resolution pyramid, slightly in front of) the surface.
    float surfaceViewDepth = ssrViewDepth(lightingData, hitWorld);
    float rayViewDepth = ssrViewDepth(lightingData, ssrWorldFromDepth(lightingData, hit.xy, hit.z));
    float tolerance = max(ssr.tuning.y * surfaceViewDepth, worldPerPixel);
    confidence *= 1.0 - smoothstep(0.5 * tolerance, tolerance, abs(rayViewDepth - surfaceViewDepth));
    // The hit must face the ray: a surface turned away is a back side, and one lying along the ray (dot near zero) is, at
    // grazing angles, the surface the ray started on -- reflecting yourself.
    float3 hitNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(hitPixel, 0)));
    confidence *= saturate(-dot(hitNormal, reflected) * 6.0);
    // A reflection can only come from above the starting surface's plane; a hit on that plane is the surface itself.
    confidence *= saturate((dot(hitWorld - worldPosition, normal) - worldPerPixel) / max(2.0 * worldPerPixel, 1.0e-6));
    // Fade near the screen edge (where the reflection runs out of image) and towards the roughness cutoff.
    float2 edge = min(hit.xy, 1.0 - hit.xy);
    confidence *= saturate(min(edge.x, edge.y) / 0.05);
    confidence *= 1.0 - smoothstep(maxRoughness * 0.8, maxRoughness, perceptualRoughness);
    if (confidence <= 0.0) {
        reflectionOut.Store(pixel, none);
        return;
    }

    // Cone footprint at the hit: the lobe's half-angle grows with roughness, the cone with distance travelled. Level 0
    // texels are 2 full-resolution pixels wide and each level doubles that, so the level is log2 of the footprint radius
    // in full-resolution pixels at the hit's depth.
    float travel = length(hitWorld - worldPosition);
    float hitWorldPerPixel = surfaceViewDepth * 2.0 * lightingData.viewportParams.y / max(lightingData.viewportParams.z, SFT_EPSILON);
    float radiusPixels = travel * alphaRoughness * ssr.tuning.z / max(hitWorldPerPixel, 1.0e-5);
    float level = log2(max(radiusPixels, 1.0));

    // Cull the reflection unless the hit surface was on screen where last frame's colour for it is stored. Reproject the hit
    // through its motion vector; it must land inside the image (with a fade at the border), and the nearest-surface depth
    // `reflections_history` kept there (level 0's alpha) must match the depth of what we hit. A mismatch means last frame
    // showed something else at that spot -- the surface was hidden or off-screen, so its colour is not available.
    float2 previousUv = hit.xy - motionToUvDelta(gbufferMotion.Load(int3(hitPixel, 0)));
    if (any(previousUv < 0.0) || any(previousUv > 1.0)) {
        reflectionOut.Store(pixel, none);
        return;
    }
    float2 previousEdge = min(previousUv, 1.0 - previousUv);
    confidence *= saturate(min(previousEdge.x, previousEdge.y) / 0.02);
    // Validate against the four history texels the bilinear footprint covers, each weighted by how well its stored depth matches
    // the hit's depth. A single-texel test flips whole 2-pixel blocks on and off along silhouettes (stair steps); weighting by
    // the bilinear footprint makes the cull a smooth gradient, and the same weights pick the sharp colour so a texel from a
    // different surface never bleeds in.
    float depthTolerance = max(0.08 * surfaceViewDepth, 4.0 * hitWorldPerPixel);
    float2 footprint = previousUv * float2(ssr.extents.zw) - 0.5;
    int2 footprintBase = int2(floor(footprint));
    float2 footprintFrac = frac(footprint);
    float matchWeight = 0.0;
    float3 sharpColor = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int k = 0; k < 4; ++k) {
        int2 offset = int2(k & 1, k >> 1);
        int2 texel = clamp(footprintBase + offset, int2(0, 0), int2(ssr.extents.zw) - 1);
        float4 stored = colorLevel0.Load(int3(texel, 0));
        float bilinear = (offset.x == 0 ? 1.0 - footprintFrac.x : footprintFrac.x) * (offset.y == 0 ? 1.0 - footprintFrac.y : footprintFrac.y);
        float match = stored.a > 0.0 ? 1.0 - smoothstep(0.5 * depthTolerance, depthTolerance, abs(stored.a - surfaceViewDepth)) : 0.0;
        matchWeight += bilinear * match;
        sharpColor += bilinear * match * stored.rgb;
    }
    confidence *= matchWeight;
    if (confidence <= 0.0) {
        reflectionOut.Store(pixel, none);
        return;
    }
    sharpColor /= max(matchWeight, 1.0e-4);
    // Sharp (depth-aware) at level 0, the blurred chain from level 1 on.
    float3 color = lerp(sharpColor, sampleColorChain(level, previousUv), saturate(level));
    reflectionOut.Store(pixel, float4(color, confidence));
}
