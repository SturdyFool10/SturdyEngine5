// ReSTIR GI pass 4/4: evaluates this pixel's own diffuse BRDF toward each final reservoir's kept
// sample direction, scales by the reservoir's unbiased contribution weight `W`, and writes the result
// into the same-shaped RGBA16Float irradiance texture surfel GI used to produce — the deferred shadow
// lighting pass's consumption side (`Shaders/deferred_shadow_lighting.slang`'s `surfelIrradiance`
// binding) is unchanged; only the producer differs.
import sturdy_common;
import sturdy_pbr;

struct ReservoirGpuData {
    float4 samplePositionPdf;
    float4 sampleRadianceNormalX;
    float4 normalYValid;
    float4 weightM;
};

struct RestirGiPackedLight {
    float4 positionRange;
    float4 radianceSourceRadius;
};

struct RestirGiFrameConstants {
    float4x4 inverseViewProjection;
    float4x4 previousViewProjection;
    float4 cameraPositionFrameIndex;
    float4 extentMaxRayDistance;
    float4 sunDirectionAngularRadius;
    float4 sunRadiance;
    float4 temporalSpatialParams;
    float4 lightCountParams;
    RestirGiPackedLight lights[8];
};

ConstantBuffer<RestirGiFrameConstants> frame;
Texture2D<float2> gbufferNormal;
Texture2D<float> gbufferDepth;
Texture2D<float4> gbufferAlbedo;
Texture2D<float4> gbufferMaterial;
StructuredBuffer<ReservoirGpuData> reservoirSpatial;
// WGSL's texture_storage_2d requires a format in the type itself; must match the RHI::Format the
// raw-irradiance target is actually created with (RHI::Format::RGBA16Float). Write-only.
[format("rgba16f")]
WTexture2D<float4> irradianceOut;

float3 reconstructWorldPosition(uint2 pixel, uint2 extent, float depth) {
    float2 uv = (float2(pixel) + 0.5) / float2(extent);
    float4 clip = float4(uvToNdc(uv), depth, 1.0);
    float4 world = mul(clip, frame.inverseViewProjection);
    return world.xyz / max(world.w, 1.0e-6);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void shadeResolveMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 extent = (uint2)frame.extentMaxRayDistance.xy;
    if (any(dispatchThreadId.xy >= extent)) return;

    float depth = gbufferDepth.Load(int3(dispatchThreadId.xy, 0));
    if (depth >= 0.999999) {
        irradianceOut.Store(dispatchThreadId.xy, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    uint pixelIndex = dispatchThreadId.y * extent.x + dispatchThreadId.x;
    ReservoirGpuData reservoir = reservoirSpatial[pixelIndex];
    if (reservoir.normalYValid.y < 0.5 || reservoir.weightM.y <= 0.0) {
        irradianceOut.Store(dispatchThreadId.xy, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    float3 worldPosition = reconstructWorldPosition(dispatchThreadId.xy, extent, depth);
    float3 normal = decodeOctahedralNormal(gbufferNormal.Load(int3(dispatchThreadId.xy, 0)));
    float4 albedoSample = gbufferAlbedo.Load(int3(dispatchThreadId.xy, 0));
    float4 materialSample = gbufferMaterial.Load(int3(dispatchThreadId.xy, 0));
    float perceptualRoughness = saturate(materialSample.x);
    float metallic = saturate(materialSample.y);

    float3 toSample = reservoir.samplePositionPdf.xyz - worldPosition;
    float sampleDistance = length(toSample);
    if (sampleDistance < 1.0e-4) {
        irradianceOut.Store(dispatchThreadId.xy, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }
    float3 direction = toSample / sampleDistance;
    float cosine = dot(normal, direction);
    if (cosine <= 0.0) {
        irradianceOut.Store(dispatchThreadId.xy, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    float3 viewDirection = safeNormalizeOr(frame.cameraPositionFrameIndex.xyz - worldPosition, normal);
    float3 diffuseBrdf = evaluateEonDiffuse(albedoSample.rgb, perceptualRoughness, normal, direction, viewDirection);
    float3 indirect = diffuseBrdf * (1.0 - metallic) * cosine
                     * reservoir.sampleRadianceNormalX.xyz * reservoir.weightM.y
                     * max(frame.temporalSpatialParams.w, 0.0);

    irradianceOut.Store(dispatchThreadId.xy, float4(max(indirect, float3(0.0, 0.0, 0.0)), 1.0));
}
