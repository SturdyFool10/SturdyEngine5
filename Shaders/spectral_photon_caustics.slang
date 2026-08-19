// Directional-sun spectral photon caustics. Emission follows real scene materials and deposits only
// after a path has transmitted through a dielectric and reached an opaque receiver. A second pass
// builds the persistent spatial hash consumed by fullPathTracingMain.

import sturdy_common;
import sturdy_spectral_common;
import sturdy_path_transport;
import sturdy_spectral_scene;
import sturdy_spectral_photons;

ConstantBuffer<SpectralPhotonConstants> photonSettings;
RWStructuredBuffer<SpectralPhoton> causticPhotons;
RWStructuredBuffer<uint> causticPhotonCount;
RWStructuredBuffer<uint> causticPhotonHashHeads;

void photonEmissionRay(uint photonPathIndex, inout uint rng, out float3 origin, out float3 direction) {
    direction = normalize(photonSettings.sunDirectionAndAngularRadius.xyz);
    float3 helper = abs(direction.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(helper, direction));
    float3 bitangent = cross(direction, tangent);
    float2 u = float2(spectralRandom(rng), spectralRandom(rng));
    float diskRadius = sqrt(u.x) * photonSettings.emissionCenterAndRadius.w;
    float phi = 2.0 * SFT_PI * u.y;
    float2 disk = float2(cos(phi), sin(phi)) * diskRadius;
    origin = photonSettings.emissionCenterAndRadius.xyz
           + tangent * disk.x + bitangent * disk.y
           - direction * photonSettings.launchParameters.x;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void emitPhotonsMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint photonPathIndex = dispatchThreadId.x;
    uint photonCapacity = photonSettings.capacitiesAndFrame.x;
    if (photonSettings.limitsAndFlags.z == 0u || photonPathIndex >= photonCapacity) return;

    uint rng = spectralHash(photonPathIndex
                          + photonSettings.capacitiesAndFrame.z * 0x9e3779b9u);
    SpectralSample spectral = sampleHeroWavelength(
        rng, photonSettings.wavelengthRange.x, photonSettings.wavelengthRange.y);
    float3 origin;
    float3 direction;
    photonEmissionRay(photonPathIndex, rng, origin, direction);

    float launchArea = SFT_PI * square(photonSettings.emissionCenterAndRadius.w);
    float throughput = rgbSpectrumAtWavelength(photonSettings.sunRadiance.rgb,
                                               spectral.wavelengthNm)
                     * spectral.inversePdf * launchArea / max(float(photonCapacity), 1.0);
    float activeAbsorption = 0.0;
    bool passedThroughSpecularTransport = false;
    float rayEpsilon = max(photonSettings.launchParameters.y, 1.0e-5);

    for (uint bounce = 0u; bounce < photonSettings.limitsAndFlags.x; ++bounce) {
        SpectralRayHit hit;
        SurfaceSample surface;
        if (!traceSpectralClosestSurface(
                origin, direction, rayEpsilon, 1.0e6,
                photonSettings.capacitiesAndFrame.w, hit, surface)) return;

        throughput *= beerLambert(activeAbsorption, hit.distance);
        if (throughput <= 1.0e-10) return;

        float transmission = saturate(surface.material.transmission.x);
        if (transmission > 0.0) {
            float eta = cauchyIor(spectral.wavelengthNm,
                                  surface.material.transmission.y,
                                  surface.material.transmission.z);
            DielectricScatter scatter = sampleDielectricDirection(
                direction, surface.outwardGeometricNormal, surface.outwardShadingNormal,
                surface.frontFace, 1.0, eta, rng);
            passedThroughSpecularTransport = true;
            if (scatter.transmitted) {
                if (scatter.entering) {
                    throughput *= transmission;
                    activeAbsorption = max(surface.material.transmission.w, 0.0);
                } else {
                    activeAbsorption = 0.0;
                }
            }
            direction = scatter.direction;
            origin = surface.position + direction * rayEpsilon;
            continue;
        }

        bool specularReceiver = surface.material.surface.y > 0.5
                             || surface.material.surface.x < 0.15;
        if (specularReceiver) {
            float reflectance = rgbReflectanceAtWavelength(
                surface.material.baseColor.rgb, spectral.wavelengthNm);
            OpaquePbrScatter scatter = sampleOpaquePbrDirection(
                direction, surface.shadingNormal, reflectance,
                surface.material.surface.x, surface.material.surface.y,
                surface.material.surface.w, rng);
            if (scatter.specular) {
                passedThroughSpecularTransport = true;
                throughput *= scatter.throughputWeight;
                direction = scatter.direction;
                origin = surface.position + direction * rayEpsilon;
                continue;
            }
        }

        if (!passedThroughSpecularTransport || throughput <= 0.0) return;
        uint compactIndex;
        InterlockedAdd(causticPhotonCount[0], 1u, compactIndex);
        if (compactIndex >= photonCapacity) return;

        SpectralPhoton photon;
        photon.positionAndWavelength = float4(surface.position, spectral.wavelengthNm);
        photon.incomingDirectionAndPower = float4(direction, throughput);
        photon.linkAndProvenance = uint4(
            SFT_INVALID_PHOTON_INDEX, hit.instanceId, hit.primitiveId, 0u);
        causticPhotons[compactIndex] = photon;
        return;
    }
}

[shader("compute")]
[numthreads(256, 1, 1)]
void hashPhotonsMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint photonIndex = dispatchThreadId.x;
    uint photonCapacity = photonSettings.capacitiesAndFrame.x;
    uint hashCapacity = photonSettings.capacitiesAndFrame.y;
    uint count = min(causticPhotonCount[0], photonCapacity);
    if (photonSettings.limitsAndFlags.z == 0u || photonIndex >= count || hashCapacity == 0u) return;

    SpectralPhoton photon = causticPhotons[photonIndex];
    float voxelSize = max(photonSettings.launchParameters.z, 1.0e-5);
    int3 cell = int3(floor(photon.positionAndWavelength.xyz / voxelSize));
    uint previousHead;
    InterlockedExchange(
        causticPhotonHashHeads[photonCellHash(cell, hashCapacity)], photonIndex, previousHead);
    causticPhotons[photonIndex].linkAndProvenance.x = previousHead;
    causticPhotons[photonIndex].linkAndProvenance.w = photonCellFingerprint(cell);
}
