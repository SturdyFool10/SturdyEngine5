// Shared material-aware ray-query scene ABI. Every spectral camera, visibility, transmission, and
// photon path uses the same vertex pulling, texture evaluation, alpha masking, and normal mapping.

import sturdy_common;
import sturdy_spectral_common;
import sturdy_path_transport;

static const uint SFT_INVALID_MATERIAL_TEXTURE = 0xffffffffu;

// Storage-buffer view of Renderer::GeometryVertex's packed 64-byte CPU arena record. std430 aligns
// a bare float3 member to 16 bytes, so spelling the logical fields directly would produce an 80-byte
// shader stride and read every field after position from the wrong address.
struct PackedGeometryVertex {
    float4 positionNormalX; // xyz position, w normal.x
    float4 normalYZUv;      // xy normal.yz, zw UV0
    float4 color;
    float4 tangent;
};

struct GeometryVertex {
    float3 position;
    float3 normal;
    float2 uv;
    float4 color;
    float4 tangent;
};

GeometryVertex unpackGeometryVertex(PackedGeometryVertex packed) {
    GeometryVertex vertex;
    vertex.position = packed.positionNormalX.xyz;
    vertex.normal = float3(packed.positionNormalX.w, packed.normalYZUv.xy);
    vertex.uv = packed.normalYZUv.zw;
    vertex.color = packed.color;
    vertex.tangent = packed.tangent;
    return vertex;
}

struct SpectralSceneInstance {
    float4x4 model;
    float4x4 previousModel;
    float4x4 normalMatrix;
    uint vertexOffset;
    uint indexOffset;
    uint materialIndex;
    uint visibilityMask;
};

struct SpectralMaterial {
    float4 baseColor;
    float4 emissiveAndStrength;
    // Packed input: roughness factor, metallic factor, occlusion strength, dielectric F0.
    // Evaluated surface: perceptual roughness, metallic, material AO, dielectric F0.
    float4 surface;
    // x transmission, y Cauchy A, z Cauchy B (um^2), w absorption coefficient.
    float4 transmission;
    // Base color, metallic/roughness, normal, and occlusion descriptor indices.
    uint4 textureIndices0;
    // Emissive descriptor index; remaining lanes reserved for material extensions.
    uint4 textureIndices1;
    // x alpha cutoff, y normal scale, zw reserved.
    float4 alphaAndNormal;
};

struct SurfaceSample {
    bool valid;
    bool frontFace;
    float3 position;
    float3 objectPosition;
    float2 uv;
    float3 outwardGeometricNormal;
    float3 outwardShadingNormal;
    float3 shadingNormal;
    uint instanceIndex;
    uint primitiveIndex;
    SpectralMaterial material;
};

// These globals deliberately live in this shared module so both camera and photon programs reflect
// the exact same scene/material descriptor contract.
RaytracingAccelerationStructure sceneAccelerationStructure;
StructuredBuffer<PackedGeometryVertex> geometryVertices;
StructuredBuffer<uint> geometryIndices;
StructuredBuffer<SpectralSceneInstance> sceneInstances;
StructuredBuffer<SpectralMaterial> sceneMaterials;
// Vulkan requires a variable-count binding to be the highest binding in its set. The explicit slot
// keeps Slang's auto-assigned fixed resources below this frame-local combined image/sampler heap.
[[vk::binding(31, 0)]] Sampler2D spectralMaterialTextures[];

float4 sampleSpectralMaterialTexture(uint textureIndex, float2 uv, float4 fallback) {
    if (textureIndex == SFT_INVALID_MATERIAL_TEXTURE) return fallback;
    return spectralMaterialTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(uv, 0.0);
}

// Lightweight sibling of loadSpectralSurface for the two call sites that only need a pass/fail alpha
// decision (the CANDIDATE_NON_OPAQUE_TRIANGLE Proceed() loop below, and the transmittance walk's own
// candidate loop) — skips every field loadSpectralSurface computes past the base-color alpha test
// (geometric/shading normals, tangent frame, normal map, metallic-roughness/occlusion/emissive
// samples), since none of it affects whether this candidate should commit.
bool loadSpectralSurfaceAlphaTest(SpectralRayHit hit, float3 rayOrigin, float3 rayDirection) {
    if (!hit.hit) return false;

    uint instanceCount = 0u;
    uint instanceStride = 0u;
    sceneInstances.GetDimensions(instanceCount, instanceStride);
    if (hit.instanceId >= instanceCount) return false;

    SpectralSceneInstance instance = sceneInstances[hit.instanceId];
    uint indexCount = 0u;
    uint indexStride = 0u;
    geometryIndices.GetDimensions(indexCount, indexStride);
    uint triangleBase = instance.indexOffset + hit.primitiveId * 3u;
    if (triangleBase < instance.indexOffset || triangleBase + 2u >= indexCount) return false;

    uint vertexCount = 0u;
    uint vertexStride = 0u;
    geometryVertices.GetDimensions(vertexCount, vertexStride);
    uint i0 = instance.vertexOffset + geometryIndices[triangleBase + 0u];
    uint i1 = instance.vertexOffset + geometryIndices[triangleBase + 1u];
    uint i2 = instance.vertexOffset + geometryIndices[triangleBase + 2u];
    if (i0 < instance.vertexOffset || i1 < instance.vertexOffset || i2 < instance.vertexOffset ||
        i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) return false;

    uint materialCount = 0u;
    uint materialStride = 0u;
    sceneMaterials.GetDimensions(materialCount, materialStride);
    if (instance.materialIndex >= materialCount) return false;

    GeometryVertex v0 = unpackGeometryVertex(geometryVertices[i0]);
    GeometryVertex v1 = unpackGeometryVertex(geometryVertices[i1]);
    GeometryVertex v2 = unpackGeometryVertex(geometryVertices[i2]);
    float3 barycentrics = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y,
                                 hit.barycentrics.x, hit.barycentrics.y);
    float2 uv = v0.uv * barycentrics.x + v1.uv * barycentrics.y + v2.uv * barycentrics.z;

    SpectralMaterial material = sceneMaterials[instance.materialIndex];
    float4 vertexColor = v0.color * barycentrics.x + v1.color * barycentrics.y + v2.color * barycentrics.z;
    float4 baseColorTexel = sampleSpectralMaterialTexture(material.textureIndices0.x, uv, float4(1.0));
    float alpha = material.baseColor.a * vertexColor.a * baseColorTexel.a;
    return alpha >= material.alphaAndNormal.x;
}

// Lightweight sibling of loadSpectralSurface for traceSpectralTransmittance's committed-hit load: that
// walk only ever reads position/frontFace/outwardShadingNormal/transmission (see its body below), so
// this skips the metallic-roughness, occlusion, and emissive texture samples loadSpectralSurface pays
// for unconditionally — it still needs the tangent frame + normal-map sample, since
// outwardShadingNormal (used for the Fresnel term) is the normal-mapped result, not the raw
// interpolated normal.
struct TransmittanceSurfaceSample {
    bool valid;
    bool frontFace;
    float3 position;
    float3 outwardShadingNormal;
    float4 transmission;
};

TransmittanceSurfaceSample loadSpectralSurfaceForTransmittance(
    SpectralRayHit hit, float3 rayOrigin, float3 rayDirection)
{
    TransmittanceSurfaceSample surface = (TransmittanceSurfaceSample)0;
    surface.position = rayOrigin + rayDirection * hit.distance;
    surface.frontFace = hit.frontFace;
    if (!hit.hit) return surface;

    uint instanceCount = 0u;
    uint instanceStride = 0u;
    sceneInstances.GetDimensions(instanceCount, instanceStride);
    if (hit.instanceId >= instanceCount) return surface;

    SpectralSceneInstance instance = sceneInstances[hit.instanceId];
    uint indexCount = 0u;
    uint indexStride = 0u;
    geometryIndices.GetDimensions(indexCount, indexStride);
    uint triangleBase = instance.indexOffset + hit.primitiveId * 3u;
    if (triangleBase < instance.indexOffset || triangleBase + 2u >= indexCount) return surface;

    uint vertexCount = 0u;
    uint vertexStride = 0u;
    geometryVertices.GetDimensions(vertexCount, vertexStride);
    uint i0 = instance.vertexOffset + geometryIndices[triangleBase + 0u];
    uint i1 = instance.vertexOffset + geometryIndices[triangleBase + 1u];
    uint i2 = instance.vertexOffset + geometryIndices[triangleBase + 2u];
    if (i0 < instance.vertexOffset || i1 < instance.vertexOffset || i2 < instance.vertexOffset ||
        i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) return surface;

    uint materialCount = 0u;
    uint materialStride = 0u;
    sceneMaterials.GetDimensions(materialCount, materialStride);
    if (instance.materialIndex >= materialCount) return surface;

    GeometryVertex v0 = unpackGeometryVertex(geometryVertices[i0]);
    GeometryVertex v1 = unpackGeometryVertex(geometryVertices[i1]);
    GeometryVertex v2 = unpackGeometryVertex(geometryVertices[i2]);
    float3 barycentrics = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y,
                                 hit.barycentrics.x, hit.barycentrics.y);
    float2 uv = v0.uv * barycentrics.x + v1.uv * barycentrics.y + v2.uv * barycentrics.z;

    float3 p0 = mul(float4(v0.position, 1.0), instance.model).xyz;
    float3 p1 = mul(float4(v1.position, 1.0), instance.model).xyz;
    float3 p2 = mul(float4(v2.position, 1.0), instance.model).xyz;
    float3 geometricNormal = normalize(cross(p1 - p0, p2 - p0));
    bool geometricFrontFace = dot(geometricNormal, rayDirection) < 0.0;
    if (geometricFrontFace != hit.frontFace) geometricNormal = -geometricNormal;

    float3 objectNormal = normalize(v0.normal * barycentrics.x
                                  + v1.normal * barycentrics.y
                                  + v2.normal * barycentrics.z);
    float3 interpolatedNormal = normalize(mul(float4(objectNormal, 0.0), instance.normalMatrix).xyz);
    if (dot(interpolatedNormal, geometricNormal) < 0.0) interpolatedNormal = -interpolatedNormal;

    SpectralMaterial material = sceneMaterials[instance.materialIndex];
    float4 vertexColor = v0.color * barycentrics.x + v1.color * barycentrics.y + v2.color * barycentrics.z;
    float4 baseColorTexel = sampleSpectralMaterialTexture(material.textureIndices0.x, uv, float4(1.0));
    float alpha = material.baseColor.a * vertexColor.a * baseColorTexel.a;
    if (alpha < material.alphaAndNormal.x) return surface;

    float3 objectTangent = v0.tangent.xyz * barycentrics.x
                         + v1.tangent.xyz * barycentrics.y
                         + v2.tangent.xyz * barycentrics.z;
    float handedness = v0.tangent.w * barycentrics.x
                     + v1.tangent.w * barycentrics.y
                     + v2.tangent.w * barycentrics.z;
    float3 tangent = mul(float4(objectTangent, 0.0), instance.model).xyz;
    tangent -= interpolatedNormal * dot(tangent, interpolatedNormal);
    float tangentLengthSquared = dot(tangent, tangent);
    SurfaceBasis fallbackBasis = makeSurfaceBasis(interpolatedNormal);
    tangent = tangentLengthSquared > 1.0e-10
        ? tangent * rsqrt(tangentLengthSquared) : fallbackBasis.tangent;
    float3 bitangent = normalize(cross(interpolatedNormal, tangent)) * (handedness < 0.0 ? -1.0 : 1.0);
    float3 normalTexel = sampleSpectralMaterialTexture(
        material.textureIndices0.z, uv, float4(0.5, 0.5, 1.0, 1.0)).rgb * 2.0 - 1.0;
    normalTexel.xy *= material.alphaAndNormal.y;
    float3 mappedNormal = normalize(tangent * normalTexel.x
                                  + bitangent * normalTexel.y
                                  + interpolatedNormal * normalTexel.z);
    if (dot(mappedNormal, geometricNormal) <= 1.0e-5) mappedNormal = interpolatedNormal;

    surface.outwardShadingNormal = mappedNormal;
    surface.transmission = material.transmission;
    surface.valid = true;
    return surface;
}

SurfaceSample loadSpectralSurface(SpectralRayHit hit, float3 rayOrigin, float3 rayDirection) {
    SurfaceSample surface = (SurfaceSample)0;
    surface.position = rayOrigin + rayDirection * hit.distance;
    surface.frontFace = hit.frontFace;
    surface.instanceIndex = hit.instanceId;
    surface.primitiveIndex = hit.primitiveId;

    uint instanceCount = 0u;
    uint instanceStride = 0u;
    sceneInstances.GetDimensions(instanceCount, instanceStride);
    if (!hit.hit || hit.instanceId >= instanceCount) return surface;

    SpectralSceneInstance instance = sceneInstances[hit.instanceId];
    uint indexCount = 0u;
    uint indexStride = 0u;
    geometryIndices.GetDimensions(indexCount, indexStride);
    uint triangleBase = instance.indexOffset + hit.primitiveId * 3u;
    if (triangleBase < instance.indexOffset || triangleBase + 2u >= indexCount) return surface;

    uint vertexCount = 0u;
    uint vertexStride = 0u;
    geometryVertices.GetDimensions(vertexCount, vertexStride);
    uint i0 = instance.vertexOffset + geometryIndices[triangleBase + 0u];
    uint i1 = instance.vertexOffset + geometryIndices[triangleBase + 1u];
    uint i2 = instance.vertexOffset + geometryIndices[triangleBase + 2u];
    if (i0 < instance.vertexOffset || i1 < instance.vertexOffset || i2 < instance.vertexOffset ||
        i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) return surface;

    uint materialCount = 0u;
    uint materialStride = 0u;
    sceneMaterials.GetDimensions(materialCount, materialStride);
    if (instance.materialIndex >= materialCount) return surface;

    GeometryVertex v0 = unpackGeometryVertex(geometryVertices[i0]);
    GeometryVertex v1 = unpackGeometryVertex(geometryVertices[i1]);
    GeometryVertex v2 = unpackGeometryVertex(geometryVertices[i2]);
    float3 barycentrics = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y,
                                 hit.barycentrics.x, hit.barycentrics.y);
    surface.objectPosition = v0.position * barycentrics.x
                           + v1.position * barycentrics.y
                           + v2.position * barycentrics.z;
    surface.uv = v0.uv * barycentrics.x + v1.uv * barycentrics.y + v2.uv * barycentrics.z;

    float3 p0 = mul(float4(v0.position, 1.0), instance.model).xyz;
    float3 p1 = mul(float4(v1.position, 1.0), instance.model).xyz;
    float3 p2 = mul(float4(v2.position, 1.0), instance.model).xyz;
    float3 geometricNormal = normalize(cross(p1 - p0, p2 - p0));
    bool geometricFrontFace = dot(geometricNormal, rayDirection) < 0.0;
    if (geometricFrontFace != hit.frontFace) geometricNormal = -geometricNormal;

    float3 objectNormal = normalize(v0.normal * barycentrics.x
                                  + v1.normal * barycentrics.y
                                  + v2.normal * barycentrics.z);
    float3 interpolatedNormal = normalize(mul(float4(objectNormal, 0.0), instance.normalMatrix).xyz);
    if (dot(interpolatedNormal, geometricNormal) < 0.0) interpolatedNormal = -interpolatedNormal;

    SpectralMaterial material = sceneMaterials[instance.materialIndex];
    float4 vertexColor = v0.color * barycentrics.x + v1.color * barycentrics.y + v2.color * barycentrics.z;
    float4 baseColorTexel = sampleSpectralMaterialTexture(
        material.textureIndices0.x, surface.uv, float4(1.0));
    material.baseColor *= vertexColor * baseColorTexel;
    if (material.baseColor.a < material.alphaAndNormal.x) return surface;

    float4 metallicRoughness = sampleSpectralMaterialTexture(
        material.textureIndices0.y, surface.uv, float4(1.0));
    material.surface.x = saturate(material.surface.x * metallicRoughness.g);
    material.surface.y = saturate(material.surface.y * metallicRoughness.b);
    float occlusion = sampleSpectralMaterialTexture(
        material.textureIndices0.w, surface.uv, float4(1.0)).r;
    material.surface.z = saturate(1.0 + material.surface.z * (occlusion - 1.0));
    material.emissiveAndStrength.rgb *= sampleSpectralMaterialTexture(
        material.textureIndices1.x, surface.uv, float4(1.0)).rgb;

    float3 objectTangent = v0.tangent.xyz * barycentrics.x
                         + v1.tangent.xyz * barycentrics.y
                         + v2.tangent.xyz * barycentrics.z;
    float handedness = v0.tangent.w * barycentrics.x
                     + v1.tangent.w * barycentrics.y
                     + v2.tangent.w * barycentrics.z;
    float3 tangent = mul(float4(objectTangent, 0.0), instance.model).xyz;
    tangent -= interpolatedNormal * dot(tangent, interpolatedNormal);
    float tangentLengthSquared = dot(tangent, tangent);
    SurfaceBasis fallbackBasis = makeSurfaceBasis(interpolatedNormal);
    tangent = tangentLengthSquared > 1.0e-10
        ? tangent * rsqrt(tangentLengthSquared) : fallbackBasis.tangent;
    float3 bitangent = normalize(cross(interpolatedNormal, tangent)) * (handedness < 0.0 ? -1.0 : 1.0);
    float3 normalTexel = sampleSpectralMaterialTexture(
        material.textureIndices0.z, surface.uv, float4(0.5, 0.5, 1.0, 1.0)).rgb * 2.0 - 1.0;
    normalTexel.xy *= material.alphaAndNormal.y;
    float3 mappedNormal = normalize(tangent * normalTexel.x
                                  + bitangent * normalTexel.y
                                  + interpolatedNormal * normalTexel.z);
    if (dot(mappedNormal, geometricNormal) <= 1.0e-5) mappedNormal = interpolatedNormal;

    surface.outwardGeometricNormal = geometricNormal;
    surface.outwardShadingNormal = mappedNormal;
    surface.shadingNormal = hit.frontFace ? mappedNormal : -mappedNormal;
    surface.material = material;
    surface.valid = true;
    return surface;
}

bool traceSpectralClosestSurface(
    float3 origin,
    float3 direction,
    float tMin,
    float tMax,
    uint instanceMask,
    out SpectralRayHit hit,
    out SurfaceSample surface)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = tMin;
    ray.TMax = tMax;

    // Every BLAS geometry is built Opaque (ensure_spectral_mesh_acceleration_structures) and every
    // TLAS instance whose material doesn't alpha-test is left at that default (see
    // prepare_spectral_scene_acceleration_structure's kForceNoOpaqueInstanceFlag). Forcing non-opaque
    // scene-wide here used to make every single candidate triangle — including plain opaque
    // walls/floors — pay for a full vertex-pull + texture-sample candidate evaluation instead of the
    // hardware's opaque auto-commit fast path. Only alpha-cutout instances now pay that cost, via
    // their own per-instance FORCE_NO_OPAQUE flag.
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(sceneAccelerationStructure, RAY_FLAG_NONE, instanceMask, ray);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        SpectralRayHit candidate;
        candidate.hit = true;
        candidate.distance = query.CandidateTriangleRayT();
        candidate.instanceId = query.CandidateInstanceID();
        candidate.primitiveId = query.CandidatePrimitiveIndex();
        candidate.barycentrics = query.CandidateTriangleBarycentrics();
        candidate.frontFace = query.CandidateTriangleFrontFace();
        if (loadSpectralSurfaceAlphaTest(candidate, origin, direction)) query.CommitNonOpaqueTriangleHit();
    }

    hit.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    hit.distance = hit.hit ? query.CommittedRayT() : tMax;
    hit.instanceId = hit.hit ? query.CommittedInstanceID() : 0u;
    hit.primitiveId = hit.hit ? query.CommittedPrimitiveIndex() : 0u;
    hit.barycentrics = hit.hit ? query.CommittedTriangleBarycentrics() : float2(0.0);
    hit.frontFace = hit.hit ? query.CommittedTriangleFrontFace() : true;
    surface = hit.hit ? loadSpectralSurface(hit, origin, direction) : (SurfaceSample)0;
    return hit.hit && surface.valid;
}

// Owns its own RayQuery (rather than delegating to traceSpectralClosestSurface) so it can request
// RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH — a pure boolean occluded/unoccluded test has no use for
// the closest hit, so traversal can stop at the first committed hit instead of continuing to find the
// nearest one. Do NOT reuse this flag for transmittance: that walk needs hits in nearest-first order
// to accumulate Beer-Lambert absorption per interface correctly.
float traceSpectralVisibility(
    float3 origin, float3 direction, float maxDistance, uint instanceMask)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = maxDistance;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(sceneAccelerationStructure, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, instanceMask, ray);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        SpectralRayHit candidate;
        candidate.hit = true;
        candidate.distance = query.CandidateTriangleRayT();
        candidate.instanceId = query.CandidateInstanceID();
        candidate.primitiveId = query.CandidatePrimitiveIndex();
        candidate.barycentrics = query.CandidateTriangleBarycentrics();
        candidate.frontFace = query.CandidateTriangleFrontFace();
        if (loadSpectralSurfaceAlphaTest(candidate, origin, direction)) query.CommitNonOpaqueTriangleHit();
    }

    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

// Own inline RayQuery loop (rather than delegating to traceSpectralClosestSurface) so both the
// candidate alpha test and the committed-hit surface load use the lightweight variants above —
// traceSpectralTransmittance never needs the full PBR surface (metallic/roughness/occlusion/emissive),
// only geometry + transmission fields, and this is the hottest closest-hit call in the integrator
// (fired every bounce via directSun).
bool traceSpectralClosestSurfaceForTransmittance(
    float3 origin,
    float3 direction,
    float tMin,
    float tMax,
    uint instanceMask,
    out SpectralRayHit hit,
    out TransmittanceSurfaceSample surface)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = tMin;
    ray.TMax = tMax;

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(sceneAccelerationStructure, RAY_FLAG_NONE, instanceMask, ray);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        SpectralRayHit candidate;
        candidate.hit = true;
        candidate.distance = query.CandidateTriangleRayT();
        candidate.instanceId = query.CandidateInstanceID();
        candidate.primitiveId = query.CandidatePrimitiveIndex();
        candidate.barycentrics = query.CandidateTriangleBarycentrics();
        candidate.frontFace = query.CandidateTriangleFrontFace();
        if (loadSpectralSurfaceAlphaTest(candidate, origin, direction)) query.CommitNonOpaqueTriangleHit();
    }

    hit.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    hit.distance = hit.hit ? query.CommittedRayT() : tMax;
    hit.instanceId = hit.hit ? query.CommittedInstanceID() : 0u;
    hit.primitiveId = hit.hit ? query.CommittedPrimitiveIndex() : 0u;
    hit.barycentrics = hit.hit ? query.CommittedTriangleBarycentrics() : float2(0.0);
    hit.frontFace = hit.hit ? query.CommittedTriangleFrontFace() : true;
    surface = hit.hit ? loadSpectralSurfaceForTransmittance(hit, origin, direction) : (TransmittanceSurfaceSample)0;
    return hit.hit && surface.valid;
}

float traceSpectralTransmittance(
    float3 origin,
    float3 direction,
    float maxDistance,
    uint instanceMask,
    float wavelengthNm,
    uint maximumLayers)
{
    float transmittance = 1.0;
    float activeAbsorption = 0.0;
    float remainingDistance = maxDistance;
    uint layers = max(maximumLayers, 1u);
    for (uint layer = 0u; layer < layers; ++layer) {
        SpectralRayHit hit;
        TransmittanceSurfaceSample blocker;
        if (!traceSpectralClosestSurfaceForTransmittance(
                origin, direction, 0.001, remainingDistance, instanceMask, hit, blocker)) {
            return transmittance;
        }
        transmittance *= beerLambert(activeAbsorption, hit.distance);
        float transmission = saturate(blocker.transmission.x);
        if (transmission <= 0.0) return 0.0;

        float etaInside = cauchyIor(
            wavelengthNm, blocker.transmission.y, blocker.transmission.z);
        float etaIncident = blocker.frontFace ? 1.0 : etaInside;
        float etaTransmitted = blocker.frontFace ? etaInside : 1.0;
        float3 boundaryNormal = blocker.frontFace
            ? blocker.outwardShadingNormal : -blocker.outwardShadingNormal;
        float interfaceTransmission = 1.0 - dielectricFresnel(
            dot(-direction, boundaryNormal), etaIncident, etaTransmitted);
        transmittance *= interfaceTransmission;
        if (blocker.frontFace) {
            transmittance *= transmission;
            activeAbsorption = max(blocker.transmission.w, 0.0);
        } else {
            activeAbsorption = 0.0;
        }
        if (transmittance <= 1.0e-5) return 0.0;
        remainingDistance = max(remainingDistance - hit.distance, 0.001);
        origin = blocker.position + direction * 0.001;
    }
    // Exhausting the interface budget means the light path never reached an unoccluded segment.
    return 0.0;
}

float traceSpectralAmbientOcclusion(
    float3 position,
    float3 normal,
    float radius,
    uint rayCount,
    uint instanceMask,
    inout uint rng)
{
    float visible = 0.0;
    uint samples = max(rayCount, 1u);
    for (uint sampleIndex = 0; sampleIndex < samples; ++sampleIndex) {
        float3 direction = cosineHemisphere(normal, rng);
        visible += traceSpectralVisibility(position + normal * 0.001, direction, radius, instanceMask);
    }
    return visible / float(samples);
}
