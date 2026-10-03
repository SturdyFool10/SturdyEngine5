#include <Audio/Denoise.hpp>
#include <Audio/Effects.hpp>

#include <cmath>
#include <iostream>
#include <numbers>
#include <random>

using namespace SFT::Audio;
using SFT::u32;
using SFT::u64;
using SFT::usize;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }

    constexpr u32 kRate = 48000;

    // Seconds 0-1: hiss only. Seconds 1-2: hiss plus a 1 kHz tone.
    std::shared_ptr<SampleBuffer> noisy(u32 channels = 1) {
        auto b = std::make_shared<SampleBuffer>();
        b->channels = channels;
        b->sample_rate = kRate;
        auto samples = std::make_shared<std::vector<float>>(static_cast<usize>(kRate) * 2 * channels);
        std::mt19937 rng(7);
        std::normal_distribution<float> hiss(0.0f, 0.03f);
        for (usize i = 0; i < kRate * 2; ++i) {
            const float tone = i >= kRate ? 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate)) : 0.0f;
            for (u32 c = 0; c < channels; ++c) (*samples)[i * channels + c] = hiss(rng) + tone;
        }
        b->samples = std::move(samples);
        return b;
    }

    double rms(const SampleBuffer &b, double from, double to, u32 channel = 0) {
        double sum = 0.0;
        usize n = 0;
        for (usize f = static_cast<usize>(from * kRate); f < static_cast<usize>(to * kRate); ++f, ++n) {
            const double v = (*b.samples)[f * b.channels + channel];
            sum += v * v;
        }
        return std::sqrt(sum / static_cast<double>(std::max<usize>(n, 1)));
    }

    // Level of one frequency (Goertzel-free: correlate with a sine and a cosine).
    double tone_level(const SampleBuffer &b, double hz, double from, double to) {
        double s = 0.0, c = 0.0;
        usize n = 0;
        for (usize f = static_cast<usize>(from * kRate); f < static_cast<usize>(to * kRate); ++f, ++n) {
            const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(f) / kRate;
            s += (*b.samples)[f * b.channels] * std::sin(phase);
            c += (*b.samples)[f * b.channels] * std::cos(phase);
        }
        return 2.0 * std::sqrt(s * s + c * c) / static_cast<double>(n);
    }
} // namespace

int main() {
    const auto input = noisy();

    // Zero reduction is a clean delay-free pass-through once the first frame has filled.
    {
        auto passed = reduce_noise(*input, nullptr, 0.0f);
        check(passed.has_value() && (*passed)->frames() == input->frames(), "output has the input's length");
        double worst = 0.0;
        for (usize f = 4096; f < input->frames() - 4096; ++f) worst = std::max(worst, std::fabs(static_cast<double>((*(*passed)->samples)[f] - (*input->samples)[f])));
        check(worst < 1e-3, "with 0 dB reduction the transform is transparent");
    }

    // Auto mode: the hiss drops, the tone stays.
    {
        auto cleaned = reduce_noise(*input, nullptr, 24.0f);
        check(cleaned.has_value(), "auto reduction runs");
        if (cleaned) {
            const double before = rms(*input, 0.5, 1.0), after = rms(**cleaned, 0.5, 1.0);
            check(after < before * 0.5, "tracked noise floor lowers the hiss by at least 6 dB");
            const double tone_before = tone_level(*input, 1000.0, 1.2, 2.0), tone_after = tone_level(**cleaned, 1000.0, 1.2, 2.0);
            check(tone_after > tone_before * 0.85, "the tone survives (within 1.5 dB)");
        }
    }

    // Profile mode: learn from the hiss-only second, remove it everywhere.
    {
        auto profile = learn_noise_profile(*input, 0, kRate);
        check(profile.has_value() && (*profile)->power.size() == 1 && (*profile)->power[0].size() == 1025, "a profile has one spectrum per channel");
        if (profile) {
            auto cleaned = reduce_noise(*input, *profile, 30.0f);
            check(cleaned.has_value(), "profile reduction runs");
            if (cleaned) {
                check(rms(**cleaned, 0.2, 1.0) < rms(*input, 0.2, 1.0) * 0.25, "a learned profile removes the hiss by at least 12 dB");
                check(tone_level(**cleaned, 1000.0, 1.2, 2.0) > tone_level(*input, 1000.0, 1.2, 2.0) * 0.85, "and keeps the tone");
            }
        }
    }

    // As an effect on a stereo signal, built by kind and parameter name.
    {
        EffectSpec spec(EffectKind::NoiseReduction);
        spec.set("reduction", 24.0f);
        auto effect = make_effect(spec);
        check(effect.has_value() && (*effect)->latency_frames() == 2047, "the effect is in the catalogue and reports its latency");
        const auto stereo = noisy(2);
        std::unique_ptr<AudioEffect> chain[] = {std::move(*effect)};
        auto cleaned = apply_effects(*stereo, chain, false);
        check(cleaned.has_value() && (*cleaned)->channels == 2 && rms(**cleaned, 0.5, 1.0, 1) < rms(*stereo, 0.5, 1.0, 1) * 0.5, "both channels are cleaned independently");
    }

    check(!learn_noise_profile(*input, 0, 100).has_value(), "a profile from too little audio is refused");
    return failures == 0 ? 0 : 1;
}
