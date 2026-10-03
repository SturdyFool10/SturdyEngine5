#include <Audio/Analysis.hpp>

#include <Audio/Fft.hpp>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace SFT::Audio {

    void compute_spectrum(std::span<const f32> samples, std::span<f32> magnitudes) {
        const usize n = samples.size();
        if (n < 4 || (n & (n - 1)) != 0 || magnitudes.size() < n / 2) {
            std::fill(magnitudes.begin(), magnitudes.end(), 0.0f);
            return;
        }
        // One cached transform per size per thread (building twiddles is the expensive part, not running them).
        thread_local std::vector<std::unique_ptr<Fft>> cache;
        Fft *fft = nullptr;
        for (const auto &candidate : cache) {
            if (candidate->size() == n) {
                fft = candidate.get();
                break;
            }
        }
        if (fft == nullptr) {
            cache.push_back(std::make_unique<Fft>(static_cast<u32>(n)));
            fft = cache.back().get();
        }
        thread_local std::vector<f32> windowed, re, im, scratch;
        windowed.resize(n);
        re.resize(n / 2 + 1);
        im.resize(n / 2 + 1);
        scratch.resize(fft->real_scratch_size());
        f32 window_sum = 0.0f;
        for (usize i = 0; i < n; ++i) {
            const f32 window = 0.5f - 0.5f * std::cos(glm::two_pi<f32>() * static_cast<f32>(i) / static_cast<f32>(n - 1));
            window_sum += window;
            windowed[i] = samples[i] * window;
        }
        fft->forward_real(windowed.data(), re.data(), im.data(), scratch.data());
        const f32 scale = 2.0f / window_sum; // a full-scale sine of amplitude 1 reads 1
        for (usize k = 0; k < n / 2; ++k) {
            magnitudes[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]) * scale;
        }
    }

    SpectrumAnalyzer::SpectrumAnalyzer(u32 fft_size) {
        u32 size = 64;
        while (size < fft_size && size < 65536) {
            size <<= 1;
        }
        size_ = std::min(size, 8192u); // the engine keeps the last 8192 samples of each output
        samples_.assign(size_, 0.0f);
        magnitudes_.assign(size_ / 2, 0.0f);
    }

    void SpectrumAnalyzer::analyze(const AudioEngine &engine, OutputId output) {
        engine.copy_output_tap(output, samples_.data(), size_);
        compute_spectrum(samples_, magnitudes_);
    }

    std::vector<f32> SpectrumAnalyzer::bands(u32 count, u32 sample_rate, f32 low_hz, f32 high_hz) const {
        std::vector<f32> out(count, 0.0f);
        if (count == 0 || high_hz <= low_hz) {
            return out;
        }
        const f32 log_low = std::log(low_hz), log_high = std::log(high_hz);
        for (u32 b = 0; b < count; ++b) {
            const f32 f0 = std::exp(log_low + (log_high - log_low) * static_cast<f32>(b) / static_cast<f32>(count));
            const f32 f1 = std::exp(log_low + (log_high - log_low) * static_cast<f32>(b + 1) / static_cast<f32>(count));
            const u32 k0 = std::max<u32>(1, static_cast<u32>(f0 * static_cast<f32>(size_) / static_cast<f32>(sample_rate)));
            const u32 k1 = std::max<u32>(k0 + 1, static_cast<u32>(f1 * static_cast<f32>(size_) / static_cast<f32>(sample_rate)));
            f32 peak = 0.0f;
            for (u32 k = k0; k < k1 && k < magnitudes_.size(); ++k) {
                peak = std::max(peak, magnitudes_[k]);
            }
            const f32 db = 20.0f * std::log10(std::max(peak, 1e-6f));
            out[b] = std::clamp((db + 80.0f) / 80.0f, 0.0f, 1.0f);
        }
        return out;
    }

    f32 SpectrumAnalyzer::centroid(u32 sample_rate) const {
        f64 weighted = 0.0, total = 0.0;
        for (u32 k = 1; k < magnitudes_.size(); ++k) {
            weighted += static_cast<f64>(bin_frequency(k, size_, sample_rate)) * magnitudes_[k];
            total += magnitudes_[k];
        }
        return total > 1e-9 ? static_cast<f32>(weighted / total) : 0.0f;
    }

} // namespace SFT::Audio
