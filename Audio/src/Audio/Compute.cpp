#include <Audio/Compute.hpp>
#include <Audio/Kernels.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace SFT::Audio {

    void reference_voice_signals(const ComputeVoice &voice, const f32 *samples, u32 channels, u64 sample_frames, u32 frames, f32 *planes, f32 *state) noexcept {
        const u32 signal_count = voice.mix_down != 0 ? 1u : channels;
        std::fill_n(planes, static_cast<usize>(signal_count) * frames, 0.0f);
        state[0] = voice.z1;
        state[1] = voice.z2;
        if (sample_frames == 0 || channels == 0) {
            return;
        }
        const i64 length = static_cast<i64>(sample_frames);
        const u32 first = std::min(frames, voice.first_frame);
        const u32 last = std::min(frames, voice.first_frame + voice.valid_frames);
        auto fetch = [&](i64 frame, u32 channel) -> f32 {
            if (voice.loop != 0) {
                frame %= length;
                if (frame < 0) {
                    frame += length;
                }
            } else if (frame < 0 || frame >= length) {
                return 0.0f;
            }
            return samples[static_cast<usize>(frame) * channels + channel];
        };
        const f32 inverse = 1.0f / static_cast<f32>(channels);
        for (u32 i = first; i < last; ++i) {
            const f32 position = voice.frac + static_cast<f32>(i - voice.first_frame) * voice.step;
            const f32 whole = std::floor(position);
            const f32 t = position - whole;
            const i64 i1 = voice.base + static_cast<i64>(whole);
            f32 sum = 0.0f;
            for (u32 c = 0; c < channels; ++c) {
                const f32 p0 = fetch(i1 - 1, c), p1 = fetch(i1, c), p2 = fetch(i1 + 1, c), p3 = fetch(i1 + 2, c);
                const f32 value = Kernels::catmull_rom(p0, p1, p2, p3, t);
                if (voice.mix_down != 0) {
                    sum += value;
                } else {
                    planes[static_cast<usize>(c) * frames + i] = value;
                }
            }
            if (voice.mix_down != 0) {
                planes[i] = sum * inverse;
            }
        }
        if (voice.mix_down != 0 && voice.filter != 0) {
            f32 z1 = voice.z1, z2 = voice.z2;
            for (u32 i = 0; i < frames; ++i) {
                const f32 x = planes[i];
                const f32 y = voice.b0 * x + z1;
                z1 = voice.b1 * x - voice.a1 * y + z2;
                z2 = voice.b2 * x - voice.a2 * y;
                planes[i] = y;
            }
            state[0] = z1;
            state[1] = z2;
        }
    }

    void reference_tap(const ComputeTap &tap, const f32 *signal, u32 frames, f32 *destination) noexcept {
        const f32 ramp = frames > 0 ? (tap.gain_to - tap.gain_from) / static_cast<f32>(frames) : 0.0f;
        for (u32 i = 0; i < frames; ++i) {
            destination[i] += signal[i] * (tap.gain_from + ramp * static_cast<f32>(i + 1));
        }
    }

    namespace {

        class CpuComputeBackend final : public ComputeMixBackend {
          public:
            [[nodiscard]] ComputeBackendInfo info() const override { return ComputeBackendInfo{"cpu-reference"}; }

            [[nodiscard]] ComputeSampleId register_sample(std::shared_ptr<const std::vector<f32>> samples, u32 channels) override {
                std::lock_guard lock(mutex_);
                const ComputeSampleId id = next_++;
                samples_.emplace(id, Entry{std::move(samples), std::max(channels, 1u)});
                return id;
            }
            [[nodiscard]] bool sample_ready(ComputeSampleId id) const override {
                std::lock_guard lock(mutex_);
                return samples_.contains(id);
            }
            void release_sample(ComputeSampleId id) override {
                std::lock_guard lock(mutex_);
                samples_.erase(id);
            }

            void submit(const ComputeBlock &block) override {
                const u32 frames = block.frames;
                mix_.assign(static_cast<usize>(block.destination_count) * frames, 0.0f);
                signals_.assign(static_cast<usize>(block.signal_count) * frames, 0.0f);
                state_.assign(block.voices.size() * 2, 0.0f);
                std::lock_guard lock(mutex_);
                for (usize v = 0; v < block.voices.size(); ++v) {
                    const ComputeVoice &voice = block.voices[v];
                    state_[v * 2] = voice.z1;
                    state_[v * 2 + 1] = voice.z2;
                    const auto it = samples_.find(voice.sample);
                    if (it == samples_.end()) {
                        continue;
                    }
                    const Entry &entry = it->second;
                    reference_voice_signals(voice, entry.samples->data(), entry.channels, entry.samples->size() / entry.channels, frames,
                                            signals_.data() + static_cast<usize>(voice.first_signal) * frames, &state_[v * 2]);
                }
                for (const ComputeTap &tap : block.taps) {
                    if (tap.destination < block.destination_count && tap.signal < block.signal_count) {
                        reference_tap(tap, signals_.data() + static_cast<usize>(tap.signal) * frames, frames, mix_.data() + static_cast<usize>(tap.destination) * frames);
                    }
                }
            }
            [[nodiscard]] ComputeResult collect() override { return ComputeResult{mix_, state_}; }

          private:
            struct Entry {
                std::shared_ptr<const std::vector<f32>> samples;
                u32 channels = 1;
            };
            mutable std::mutex mutex_;
            std::unordered_map<ComputeSampleId, Entry> samples_;
            ComputeSampleId next_ = 1;
            std::vector<f32> mix_, signals_, state_;
        };

    } // namespace

    std::unique_ptr<ComputeMixBackend> make_cpu_compute_backend() { return std::make_unique<CpuComputeBackend>(); }

} // namespace SFT::Audio
