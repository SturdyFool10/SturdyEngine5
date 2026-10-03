#include <Audio/Compute.hpp>
#include <Audio/Dsp.hpp>
#include <Audio/Mixer.hpp>
#include <Audio/Source.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>

using namespace SFT::Audio;
using SFT::u32;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }

    std::shared_ptr<const SampleBuffer> tone(u32 channels, double frequency, double seconds, float amplitude, u32 rate) {
        auto b = std::make_shared<SampleBuffer>();
        b->channels = channels;
        b->sample_rate = rate;
        const size_t frames = static_cast<size_t>(rate * seconds);
        auto samples = std::make_shared<std::vector<float>>(frames * channels);
        for (size_t i = 0; i < frames; ++i) {
            for (u32 c = 0; c < channels; ++c) {
                (*samples)[i * channels + c] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * (1.0 + 0.37 * c) * static_cast<double>(i) / rate));
            }
        }
        b->samples = std::move(samples);
        return b;
    }

    // Plays a mix of voices (2D and positioned, mono and stereo, looping and one-shot, assorted pitches and rates) and returns
    // every block of the 5.1 bed concatenated.
    struct Result {
        std::vector<float> samples;
        AudioStats stats{};
    };
    Result render(std::shared_ptr<ComputeMixBackend> backend, u32 count, u32 blocks) {
        AudioEngineConfig config;
        config.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::surround_5_1()}};
        config.max_voices = 4096;
        config.max_physical_voices = 4096;
        config.mix_threads = 0;
        config.compute_backend = std::move(backend);
        config.compute_min_voices = 1;
        AudioEngine engine(config);
        const auto mono = tone(1, 330.0, 0.40, 0.05f, 48000);
        const auto stereo = tone(2, 220.0, 0.25, 0.05f, 44100);
        const auto shortclip = tone(1, 700.0, 0.03, 0.05f, 22050);
        for (u32 i = 0; i < count; ++i) {
            PlayParams p;
            const auto &sound = i % 3 == 0 ? stereo : (i % 3 == 1 ? mono : shortclip);
            p.source = std::make_shared<BufferSource>(sound, 48000, i % 4 == 0);
            p.spatial = i % 5 != 0;
            p.pitch = 0.6f + 0.13f * static_cast<float>(i % 11);
            p.volume = 0.3f + 0.05f * static_cast<float>(i % 7);
            p.position = {static_cast<float>(i % 17) - 8.0f, static_cast<float>(i % 3) - 1.0f, -2.0f - static_cast<float>(i % 5)};
            engine.play(std::move(p));
        }
        Result result;
        u32 peak_compute = 0;
        for (u32 b = 0; b < blocks; ++b) {
            engine.render_block();
            peak_compute = std::max(peak_compute, engine.stats().compute_voices);
            for (u32 c = 0; c < engine.bed(0).channels(); ++c) {
                result.samples.insert(result.samples.end(), engine.bed(0).data(c), engine.bed(0).data(c) + engine.config().block_frames);
            }
        }
        result.stats = engine.stats();
        result.stats.compute_voices = peak_compute;
        return result;
    }

    double max_difference(const Result &a, const Result &b) {
        double worst = 0.0;
        if (a.samples.size() != b.samples.size()) {
            return 1e9;
        }
        for (size_t i = 0; i < a.samples.size(); ++i) {
            worst = std::max(worst, static_cast<double>(std::fabs(a.samples[i] - b.samples[i])));
        }
        return worst;
    }
} // namespace

int main() {
    // The backend's arithmetic against the source's own: a pitched, looping read, then the filter against Biquad.
    {
        const auto sound = tone(2, 440.0, 0.1, 0.5f, 48000);
        BufferSource reference(sound, 48000, true);
        reference.set_rate(1.37f);
        BufferSource planned(sound, 48000, true);
        planned.set_rate(1.37f);
        AudioBuffer out(2, 256);
        Biquad biquad;
        biquad.set(FilterType::LowPass, 48000.0f, 3000.0f);
        double worst = 0.0;
        float state[2] = {0.0f, 0.0f};
        for (int block = 0; block < 40; ++block) {
            reference.read(out, 256);
            const auto plan = planned.plan_block(256);
            std::vector<float> signals(256, 0.0f);
            ComputeVoice voice{1, 1, 0, plan.valid_frames, plan.base, plan.frac, plan.step};
            voice.mix_down = 1;
            voice.filter = 1;
            const auto c = biquad.coefficients();
            voice.b0 = c.b0, voice.b1 = c.b1, voice.b2 = c.b2, voice.a1 = c.a1, voice.a2 = c.a2;
            voice.z1 = state[0], voice.z2 = state[1];
            reference_voice_signals(voice, sound->samples->data(), 2, sound->frames(), 256, signals.data(), state);
            for (u32 i = 0; i < 256; ++i) {
                const float mono = 0.5f * (out.data(0)[i] + out.data(1)[i]);
                worst = std::max(worst, static_cast<double>(std::fabs(signals[i] - biquad.process(mono))));
            }
        }
        check(worst < 2e-5, "reference arithmetic matches BufferSource::read and Biquad");
    }

    // A one-shot that ends inside a block plans fewer frames and finishes, like read().
    {
        const auto sound = tone(1, 440.0, 0.01, 0.5f, 48000); // 480 frames
        BufferSource source(sound, 48000, false);
        check(source.plan_block(256).valid_frames == 256, "first block full");
        const auto second = source.plan_block(256);
        check(second.valid_frames == 224 && source.finished(), "tail block is short and the source finishes");
        check(source.plan_block(256).valid_frames == 0, "nothing after the end");
    }

    // The engine with a compute backend sounds the same as without.
    {
        const Result cpu = render(nullptr, 700, 30);
        const Result computed = render(make_cpu_compute_backend(), 700, 30);
        check(computed.stats.compute_voices > 400, "most voices went to the backend");
        check(cpu.stats.compute_voices == 0, "no backend, no compute voices");
        const double diff = max_difference(cpu, computed);
        std::cout << "max difference " << diff << ", compute voices " << computed.stats.compute_voices << '\n';
        check(diff < 1e-3, "compute mixing matches CPU mixing");
        check(*std::max_element(cpu.samples.begin(), cpu.samples.end()) > 0.05f, "the scene is audible");
    }

    if (failures == 0) {
        std::cout << "AudioComputeTest passed\n";
    }
    return failures == 0 ? 0 : 1;
}
