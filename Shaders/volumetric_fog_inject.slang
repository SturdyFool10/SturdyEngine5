// Volumetric fog, pass 1 of 3: the medium and the light it scatters, per froxel.
//
// One thread per froxel (screen tile x depth slice). Evaluates the height/noise density at a point jittered along the
// slice, the sunlight (one cascaded-shadow tap), the ambient term and every clustered local light (one spot/point
// shadow tap each), then blends with last frame's value reprojected through the previous view-projection. The jitter
// plus the blend is what turns one shadow tap per froxel into smooth light shafts.
//
// Output (rgba16f atlas): rgb = in-scattered radiance per unit length (already multiplied by the frame's exposure),
// a = extinction per unit length.
import sturdy_common;
import sturdy_space;
import sturdy_lighting_data;
import volumetric_fog_common;

ConstantBuffer<ShadowLightingGpuData> lightingData;
ConstantBuffer<FogConstants> fog;
StructuredBuffer<LocalLightGpuData> localLights;
StructuredBuffer<uint2> clusterRanges;
StructuredBuffer<uint> clusterLightIndices;
DepthTexture2D directionalShadowAtlas;
DepthTexture2D shadowAtlas;
Texture2D<float4> injectHistory;
SamplerState linearSampler;
[format("rgba16f")]
WTexture2D<float4> injectOut;

// One unfiltered shadow-map tap. `.Load()` rather than a comparison sample: it is valid in any control flow on every
// backend, and the temporal blend does the filtering.
float fogShadowTap(DepthTexture2D atlas, ShadowViewGpuData view, float3 position) {
    float4 clip = sturdySpaceWorldToClip(position, view.viewProjection);
    if (clip.w <= SFT_EPSILON) {
        return 1.0;
    }
    float3 ndc = clip.xyz / clip.w;
    float2 localUv = ndcToUv(ndc.xy);
    if (ndc.z <= 0.0 || ndc.z >= 1.0 || any(localUv <= 0.0) || any(localUv >= 1.0)) {
        return 1.0;
    }
    uint width, height;
    atlas.GetDimensions(width, height);
    float2 atlasUv = localUv * view.atlasScaleBias.xy + view.atlasScaleBias.zw;
    int2 texel = clamp(int2(atlasUv * float2(width, height)), int2(0, 0), int2(int(width), int(height)) - 1);
    float occluder = atlas.Load(int3(texel, 0));
    return occluder + 2.0e-4 < ndc.z ? 0.0 : 1.0;
}

float fogSunShadow(float3 position, float viewDepth) {
    uint cascadeCount = (uint)lightingData.sun.cascadeParams.x;
    if (fog.flags.x == 0u || lightingData.sun.radianceShadow.w < 0.5 || cascadeCount == 0u) {
        return 1.0;
    }
    uint cascade = 0;
    [unroll]
    for (uint i = 0; i < 3; ++i) {
        cascade += (i + 1 < cascadeCount && viewDepth > lightingData.sun.cascadeSplits[i]) ? 1u : 0u;
    }
    if (viewDepth > lightingData.sun.cascadeSplits[cascade]) {
        return 1.0;
    }
    uint viewIndex = (uint)lightingData.sun.cascadeParams.z + cascade;
    if (viewIndex >= (uint)lightingData.counts.z) {
        return 1.0;
    }
    return fogShadowTap(directionalShadowAtlas, lightingData.shadowViews[viewIndex], position);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void injectMain(uint3 id : SV_DispatchThreadID) {
    if (any(id >= fog.grid.xyz)) {
        return;
    }
    float2 uv = min((float2(id.xy) + 0.5) * fog.render.z, fog.render.xy - 0.5) / fog.render.xy;
    float depthPerDistance;
    float3 rayDirection = fogViewRay(lightingData, uv, depthPerDistance);

    // A different point along the slice every frame (golden-ratio sequence, offset per froxel).
    float jitter = frac(float(fog.flags.w % 1024u) * 0.6180340 + fogHash(float3(id) * 0.731));
    float viewDepth = fogSliceDepth(fog, float(id.z) + jitter);
    float3 position = lightingData.cameraPositionNear.xyz + rayDirection * (viewDepth / depthPerDistance);

    float density = fogDensity(fog, position);
    float3 scattering = float3(0.0, 0.0, 0.0);
    if (density > 0.0) {
        float g = clamp(fog.medium.w, -0.99, 0.99);
        // The phase function integrates to one over the sphere, so uniform ambient radiance scatters unchanged.
        float3 light = lightingData.ambientRadianceExposure.rgb * fog.lightScales.y;
        float3 sunVector = -safeNormalizeOr(lightingData.sun.directionAngularRadius.xyz, float3(0.0, -1.0, 0.0));
        light += lightingData.sun.radianceShadow.rgb * fog.lightScales.x * henyeyGreenstein(dot(sunVector, rayDirection), g) *
                 fogSunShadow(position, viewDepth);

        if (fog.render.w > 0.5 && fog.lightScales.z > 0.0) {
            uint cluster = lightClusterIndex(lightingData, position);
            uint2 range = clusterRanges[cluster];
            uint count = min(range.y, (uint)lightingData.clusterFlags.z);
            [loop]
            for (uint k = 0; k < count; ++k) {
                LocalLightGpuData localLight = localLights[clusterLightIndices[range.x + k]];
                float3 lightVector;
                float distanceToLight;
                float3 radiance;
                if (!localLightIncident(localLight, position, lightVector, distanceToLight, radiance)) {
                    continue;
                }
                float visibility = 1.0;
                int shadowView = (int)localLight.shadowRadiusKind.x;
                if (fog.flags.y != 0u && shadowView >= 0) {
                    uint viewIndex = (uint)shadowView;
                    if (localLight.shadowRadiusKind.z > 0.5) {
                        viewIndex += pointShadowFace(position - localLight.positionRange.xyz);
                    }
                    if (viewIndex < (uint)lightingData.counts.z) {
                        visibility = fogShadowTap(shadowAtlas, lightingData.shadowViews[viewIndex], position);
                    }
                }
                light += radiance * fog.lightScales.z * henyeyGreenstein(dot(lightVector, rayDirection), g) * visibility;
            }
        }
        scattering = (light * fog.albedoNoise.rgb + fog.emissiveNoiseScale.rgb) * density;
    }
    float4 current = float4(scattering * fog.lightScales.w, density);

    float4 result = current;
    if (fog.flags.z != 0u) {
        // Where this froxel's centre was last frame. Perspective clip w is the view depth.
        float centreDepth = fogSliceDepth(fog, float(id.z) + 0.5);
        float3 centre = lightingData.cameraPositionNear.xyz + rayDirection * (centreDepth / depthPerDistance);
        float4 previousClip = mul(float4(centre, 1.0), fog.previousViewProjection);
        if (previousClip.w > SFT_EPSILON && previousClip.w < fog.depthParams.y) {
            float2 previousUv = ndcToUv(previousClip.xy / previousClip.w);
            if (all(previousUv >= 0.0) && all(previousUv <= 1.0)) {
                float2 previousGridUv = previousUv * fog.render.xy / (float2(fog.grid.xy) * fog.render.z);
                float4 history = fogSampleVolume(injectHistory, linearSampler, fog, previousGridUv,
                                                 fogSliceCoord(fog, previousClip.w));
                result = lerp(history, current, saturate(fog.depthParams.w));
            }
        }
    }
    injectOut.Store(fogAtlasTexel(fog, id), result);
}
