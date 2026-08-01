// Shared 48-byte spectral photon ABI, spatial hash, and RGB caustic gather. This module has no
// descriptor globals so both the emission kernels and camera integrator can import it safely.

import sturdy_common;
import sturdy_spectral_common;
import sturdy_spectral_scene;

static const uint SFT_INVALID_PHOTON_INDEX = 0xffffffffu;

struct SpectralPhotonConstants {
    // xyz is the direction sunlight travels toward the scene; w is angular radius in radians.
    float4 sunDirectionAndAngularRadius;
    float4 sunRadiance;
    // xyz is the center of the scene-enclosing emission disk; w is its radius.
    float4 emissionCenterAndRadius;
    // x launch distance, y ray epsilon, z voxel size, w gather radius.
    float4 launchParameters;
    // xy visible wavelength interval in nanometers.
    float4 wavelengthRange;
    // x photon capacity, y hash capacity, z frame index, w ray instance mask.
    uint4 capacitiesAndFrame;
    // x maximum bounces, y maximum hash-chain traversal, z enabled, w reserved.
    uint4 limitsAndFlags;
};

struct SpectralPhoton {
    // xyz deposit position, w wavelength in nanometers.
    float4 positionAndWavelength;
    // xyz propagation direction arriving at the receiver, w scalar spectral flux.
    float4 incomingDirectionAndPower;
    // x next photon, y instance ID, z primitive ID, w cell fingerprint.
    uint4 linkAndProvenance;
};

uint photonCellFingerprint(int3 cell) {
    uint value = asuint(cell.x) * 0x8da6b343u;
    value ^= asuint(cell.y) * 0xd8163841u;
    value ^= asuint(cell.z) * 0xcb1ab31fu;
    return spectralHash(value);
}

uint photonCellHash(int3 cell, uint hashCapacity) {
    return photonCellFingerprint(cell) % max(hashCapacity, 1u);
}

float3 gatherSpectralCausticsRgb(
    StructuredBuffer<SpectralPhoton> photons,
    StructuredBuffer<uint> validPhotonCount,
    StructuredBuffer<uint> photonHashHeads,
    SurfaceSample receiver,
    float radius,
    float voxelSize,
    uint requestedPhotonCapacity,
    uint requestedHashCapacity,
    uint traversalLimit)
{
    float transmission = saturate(receiver.material.transmission.x);
    float metallic = saturate(receiver.material.surface.y);
    if (!receiver.valid || radius <= 0.0 || voxelSize <= 0.0 ||
        requestedPhotonCapacity == 0u || requestedHashCapacity == 0u || traversalLimit == 0u ||
        transmission >= 0.999 || metallic >= 0.999) {
        return float3(0.0);
    }

    uint photonBufferCount = 0u;
    uint photonStride = 0u;
    photons.GetDimensions(photonBufferCount, photonStride);
    uint countBufferCount = 0u;
    uint countStride = 0u;
    validPhotonCount.GetDimensions(countBufferCount, countStride);
    uint hashBufferCount = 0u;
    uint hashStride = 0u;
    photonHashHeads.GetDimensions(hashBufferCount, hashStride);
    if (countBufferCount == 0u || hashBufferCount == 0u) {
        return float3(0.0);
    }

    uint photonCapacity = min(requestedPhotonCapacity, photonBufferCount);
    uint hashCapacity = min(requestedHashCapacity, hashBufferCount);
    uint validCount = min(validPhotonCount[0], photonCapacity);
    if (validCount == 0u || hashCapacity == 0u) {
        return float3(0.0);
    }

    float3 radiance = float3(0.0);
    float safeVoxelSize = max(voxelSize, 1.0e-5);
    float radiusSquared = radius * radius;
    int cellRadius = int(ceil(radius / safeVoxelSize));
    int3 centerCell = int3(floor(receiver.position / safeVoxelSize));
    float diffuseScale = (1.0 - metallic) * (1.0 - transmission) * SFT_INV_PI;

    for (int z = -cellRadius; z <= cellRadius; ++z) {
        for (int y = -cellRadius; y <= cellRadius; ++y) {
            for (int x = -cellRadius; x <= cellRadius; ++x) {
                int3 queryCell = centerCell + int3(x, y, z);
                uint fingerprint = photonCellFingerprint(queryCell);
                uint photonIndex = photonHashHeads[photonCellHash(queryCell, hashCapacity)];
                uint traversed = 0u;
                while (photonIndex != SFT_INVALID_PHOTON_INDEX && traversed++ < traversalLimit) {
                    if (photonIndex >= validCount) {
                        break;
                    }
                    SpectralPhoton photon = photons[photonIndex];
                    uint nextPhoton = photon.linkAndProvenance.x;
                    float3 photonPosition = photon.positionAndWavelength.xyz;
                    int3 photonCell = int3(floor(photonPosition / safeVoxelSize));
                    if (photon.linkAndProvenance.w == fingerprint && all(photonCell == queryCell)) {
                        float3 offset = photonPosition - receiver.position;
                        float distanceSquared = dot(offset, offset);
                        float receiverCosine = dot(receiver.shadingNormal,
                                                   -photon.incomingDirectionAndPower.xyz);
                        if (distanceSquared < radiusSquared && receiverCosine > 0.0) {
                            float normalizedDistance = distanceSquared / max(radiusSquared, 1.0e-8);
                            float kernel = 3.0 * square(1.0 - normalizedDistance)
                                         / max(SFT_PI * radiusSquared, 1.0e-8);
                            float reflectance = rgbReflectanceAtWavelength(
                                receiver.material.baseColor.rgb,
                                photon.positionAndWavelength.w);
                            radiance += wavelengthToLinearRgb(photon.positionAndWavelength.w)
                                      * (photon.incomingDirectionAndPower.w
                                         * kernel * diffuseScale * reflectance);
                        }
                    }
                    photonIndex = nextPhoton;
                }
            }
        }
    }
    return radiance;
}
