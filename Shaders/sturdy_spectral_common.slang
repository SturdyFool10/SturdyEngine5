// Clean-room spectral transport primitives shared by every ray-query integrator.
// Hero wavelengths are converted through analytic CIE 1931 fits into linear sRGB; all estimators
// include the wavelength PDF so changing the configured interval does not change scene energy.

import sturdy_common;

struct SpectralSample {
    float wavelengthNm;
    float inversePdf;
    float3 linearRgbWeight;
};

uint spectralHash(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float spectralRandom(inout uint state) {
    state = spectralHash(state);
    return (float(state) + 0.5) * (1.0 / 4294967296.0);
}

float spectralGaussian(float wavelengthNm, float meanNm, float sigmaLeftNm, float sigmaRightNm) {
    float sigma = wavelengthNm < meanNm ? sigmaLeftNm : sigmaRightNm;
    float x = (wavelengthNm - meanNm) / sigma;
    return exp(-0.5 * x * x);
}

// Wyman/Sloan/Shirley-style analytic approximation of the CIE 1931 2-degree color matching curves.
float3 cieXyzAtWavelength(float wavelengthNm) {
    float x = 1.056 * spectralGaussian(wavelengthNm, 599.8, 37.9, 31.0)
            + 0.362 * spectralGaussian(wavelengthNm, 442.0, 16.0, 26.7)
            - 0.065 * spectralGaussian(wavelengthNm, 501.1, 20.4, 26.2);
    float y = 0.821 * spectralGaussian(wavelengthNm, 568.8, 46.9, 40.5)
            + 0.286 * spectralGaussian(wavelengthNm, 530.9, 16.3, 31.1);
    float z = 1.217 * spectralGaussian(wavelengthNm, 437.0, 11.8, 36.0)
            + 0.681 * spectralGaussian(wavelengthNm, 459.0, 26.0, 13.8);
    return max(float3(x, y, z), 0.0);
}

float3 xyzToLinearSrgb(float3 xyz) {
    return mul(float3x3(
         3.2404542, -1.5371385, -0.4985314,
        -0.9692660,  1.8760108,  0.0415560,
         0.0556434, -0.2040259,  1.0572252), xyz);
}

float3 wavelengthToLinearRgb(float wavelengthNm) {
    return xyzToLinearSrgb(cieXyzAtWavelength(wavelengthNm)) * (1.0 / 106.856895);
}

SpectralSample sampleHeroWavelength(inout uint rng, float wavelengthMinNm, float wavelengthMaxNm) {
    SpectralSample sample;
    float interval = max(wavelengthMaxNm - wavelengthMinNm, 1.0);
    sample.wavelengthNm = wavelengthMinNm + spectralRandom(rng) * interval;
    sample.inversePdf = interval;
    // 1/106.856895 approximately normalizes the CIE Y integral over the visible interval.
    sample.linearRgbWeight = wavelengthToLinearRgb(sample.wavelengthNm) * sample.inversePdf;
    return sample;
}

// Smooth RGB basis evaluation. This is a bounded reflectance approximation, not an emission SPD.
float rgbReflectanceAtWavelength(float3 linearRgb, float wavelengthNm) {
    float3 basis = float3(
        spectralGaussian(wavelengthNm, 610.0, 45.0, 35.0),
        spectralGaussian(wavelengthNm, 545.0, 35.0, 45.0),
        spectralGaussian(wavelengthNm, 455.0, 25.0, 40.0));
    float normalization = max(dot(basis, float3(1.0, 1.0, 1.0)), 1.0e-4);
    return saturate(dot(saturate(linearRgb), basis) / normalization * 2.25);
}

// Scale-preserving emission/radiance spectrum approximation. Unlike the reflectance helper, this
// retains HDR magnitude and only normalizes the RGB chromaticity before basis evaluation.
float rgbSpectrumAtWavelength(float3 linearRgb, float wavelengthNm) {
    float scale = max3(max(linearRgb, 0.0));
    if (scale <= 0.0) {
        return 0.0;
    }
    return scale * rgbReflectanceAtWavelength(linearRgb / scale, wavelengthNm);
}

float dielectricFresnel(float cosTheta, float etaIncident, float etaTransmitted) {
    cosTheta = saturate(abs(cosTheta));
    float eta = etaIncident / max(etaTransmitted, 1.0e-4);
    float sinThetaT2 = eta * eta * max(0.0, 1.0 - cosTheta * cosTheta);
    if (sinThetaT2 >= 1.0) {
        return 1.0;
    }
    float cosThetaT = sqrt(max(0.0, 1.0 - sinThetaT2));
    float rs = (etaIncident * cosTheta - etaTransmitted * cosThetaT)
             / max(etaIncident * cosTheta + etaTransmitted * cosThetaT, 1.0e-5);
    float rp = (etaTransmitted * cosTheta - etaIncident * cosThetaT)
             / max(etaTransmitted * cosTheta + etaIncident * cosThetaT, 1.0e-5);
    return saturate(0.5 * (rs * rs + rp * rp));
}

// Cauchy dispersion with wavelength in nanometers and B expressed in micrometer squared.
float cauchyIor(float wavelengthNm, float cauchyA, float cauchyB) {
    float wavelengthUm = max(wavelengthNm * 0.001, 0.1);
    return max(1.0, cauchyA + cauchyB / (wavelengthUm * wavelengthUm));
}

float3 cosineHemisphere(float3 normal, inout uint rng) {
    float2 u = float2(spectralRandom(rng), spectralRandom(rng));
    float radius = sqrt(u.x);
    float phi = 2.0 * SFT_PI * u.y;
    float3 local = float3(radius * cos(phi), radius * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
    float3 helper = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(helper, normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

// Light-sampling PDF for treating the sun as a uniform-radiance disk of this angular radius (radians)
// — matches sunDiskLuminance's own solid-angle formula (sturdy_atmosphere.slang) exactly, since both
// need to agree for the MIS weights that combine them (see traceSpectralPath's directSun/environment
// double-counting fix) to be meaningful.
float sunSolidAnglePdf(float angularRadiusRadians) {
    float solidAngle = 2.0 * SFT_PI * (1.0 - cos(angularRadiusRadians));
    return 1.0 / max(solidAngle, 1.0e-6);
}

float russianRoulette(inout float throughput, uint bounce, uint startBounce, inout uint rng) {
    if (bounce < startBounce) {
        return 1.0;
    }
    float survival = clamp(throughput, 0.05, 0.95);
    if (spectralRandom(rng) >= survival) {
        throughput = 0.0;
        return 0.0;
    }
    throughput /= survival;
    return survival;
}
