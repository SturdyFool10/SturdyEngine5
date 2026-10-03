#include <Audio/Hrtf.hpp>

#include <Audio/Decoder.hpp>
#include <Audio/Dsp.hpp>
#include <Audio/Hdf5.hpp>
#include <Audio/Kernels.hpp>
#include <Audio/Resample.hpp>
#include <Audio/Text.hpp>

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

namespace SFT::Audio {

    namespace {
        /// Index of the first sample whose magnitude reaches 10% of the response's peak: where the sound arrives at the ear.
        u32 detect_onset(std::span<const f32> ir) {
            f32 peak = 0.0f;
            for (const f32 v : ir) peak = std::max(peak, std::fabs(v));
            if (peak <= 0.0f) return 0;
            for (u32 i = 0; i < ir.size(); ++i) {
                if (std::fabs(ir[i]) >= 0.1f * peak) return i;
            }
            return 0;
        }
    } // namespace

    std::expected<std::shared_ptr<const HrtfSet>, UString> HrtfSet::from_measurements(u32 sample_rate, std::span<const glm::vec3> directions, std::span<const std::vector<f32>> left,
                                                                                         std::span<const std::vector<f32>> right, u32 max_taps) {
        if (directions.empty() || left.size() != directions.size() || right.size() != directions.size()) {
            return std::unexpected("audio: an HRTF set needs one left and one right response per direction");
        }
        usize length = left[0].size();
        for (usize m = 0; m < directions.size(); ++m) {
            if (left[m].size() != length || right[m].size() != length || length == 0) return std::unexpected("audio: HRTF responses must all have the same non-zero length");
        }
        const u32 pre_roll = 2; // keep a couple of samples before the detected onset so the leading edge is not clipped
        std::shared_ptr<HrtfSet> set(new HrtfSet());
        set->sample_rate_ = sample_rate;
        set->taps_ = std::clamp<u32>(max_taps, 16, static_cast<u32>(length));
        const u32 taps = set->taps_;
        set->directions_.assign(directions.begin(), directions.end());
        for (glm::vec3 &d : set->directions_) {
            const f32 n = glm::length(d);
            d = n > 1e-9f ? d / n : glm::vec3(0.0f, 0.0f, -1.0f);
        }
        set->irs_.assign(directions.size() * 2 * taps, 0.0f);
        set->onsets_.assign(directions.size() * 2, 0.0f);
        for (usize m = 0; m < directions.size(); ++m) {
            for (u32 ear = 0; ear < 2; ++ear) {
                const std::vector<f32> &ir = ear == 0 ? left[m] : right[m];
                const u32 onset = detect_onset(ir);
                const u32 shift = onset > pre_roll ? onset - pre_roll : 0;
                set->onsets_[m * 2 + ear] = static_cast<f32>(shift);
                f32 *out = set->irs_.data() + (m * 2 + ear) * taps;
                for (u32 k = 0; k < taps && shift + k < ir.size(); ++k) out[k] = ir[shift + k];
                // Fade the tail so cutting the response short does not click.
                const u32 fade = std::min<u32>(taps / 4, 32);
                for (u32 k = 0; k < fade; ++k) out[taps - 1 - k] *= static_cast<f32>(k) / static_cast<f32>(fade);
            }
        }
        return set;
    }

    std::expected<std::shared_ptr<const HrtfSet>, UString> HrtfSet::from_sofa(std::span<const std::byte> file, u32 sample_rate, u32 max_taps) {
        auto hdf = Hdf5File::open(file);
        if (!hdf) return std::unexpected(std::move(hdf.error()));
        const Hdf5File &h = **hdf;
        if (const auto conv = h.string_attribute("/", "SOFAConventions"); conv && *conv != "SimpleFreeFieldHRIR"_ustr && *conv != "SimpleFreeFieldSOS"_ustr && *conv != "GeneralFIR"_ustr) {
            return std::unexpected(UString{std::format("audio: the SOFA convention '{}' is not an HRIR set", *conv)});
        }
        const auto ir = h.read("/Data.IR");
        const auto rate = h.read("/Data.SamplingRate");
        const auto positions = h.read("/SourcePosition");
        if (!ir || !rate || !positions) {
            return std::unexpected(UString{std::format("audio: the SOFA file lacks Data.IR, Data.SamplingRate or SourcePosition ({})", !ir ? ir.error() : !rate ? rate.error() : positions.error())});
        }
        if (ir->shape.size() != 3 || ir->shape[1] != 2 || rate->values.empty() || positions->shape.size() != 2 || positions->shape[1] != 3 || positions->shape[0] != ir->shape[0]) {
            return std::unexpected("audio: the SOFA file is not a two-ear free-field HRIR set");
        }
        const usize measurements = static_cast<usize>(ir->shape[0]), samples = static_cast<usize>(ir->shape[2]);
        const u32 file_rate = static_cast<u32>(std::lround(rate->values[0]));
        const bool cartesian = [&] {
            const auto type = h.string_attribute("/SourcePosition", "Type");
            return type && *type == "cartesian"_ustr;
        }();
        // Positions: spherical (azimuth, elevation in degrees, radius) or cartesian (x front, y left, z up; metres). SOFA's default listener
        // looks along +x with +y to the left, which is exactly `direction_from_angles`' convention (azimuth positive to the left).
        std::vector<glm::vec3> directions(measurements);
        for (usize m = 0; m < measurements; ++m) {
            const f64 a = positions->values[m * 3], b = positions->values[m * 3 + 1], c = positions->values[m * 3 + 2];
            if (cartesian) {
                f32 azimuth = 0.0f, elevation = 0.0f;
                angles_from_direction(glm::vec3(static_cast<f32>(-b), static_cast<f32>(c), static_cast<f32>(-a)), azimuth, elevation);
                directions[m] = direction_from_angles(azimuth, elevation);
            } else {
                directions[m] = direction_from_angles(static_cast<f32>(a), static_cast<f32>(b));
            }
        }
        // Optional measured delays (samples) per receiver; folded into the responses so the onset detection sees them.
        std::vector<f64> delays(measurements * 2, 0.0);
        if (const auto delay = h.read("/Data.Delay"); delay && !delay->values.empty()) {
            for (usize m = 0; m < measurements; ++m) {
                for (usize ear = 0; ear < 2; ++ear) {
                    const usize at = delay->values.size() >= measurements * 2 ? m * 2 + ear : std::min(ear, delay->values.size() - 1);
                    delays[m * 2 + ear] = delay->values[at];
                }
            }
        }
        std::vector<std::vector<f32>> left(measurements), right(measurements);
        const bool convert = file_rate != sample_rate && file_rate != 0;
        for (usize m = 0; m < measurements; ++m) {
            for (u32 ear = 0; ear < 2; ++ear) {
                std::vector<f32> response(samples + static_cast<usize>(std::max(0.0, std::ceil(delays[m * 2 + ear]))), 0.0f);
                const usize lead = response.size() - samples;
                for (usize k = 0; k < samples; ++k) response[lead + k] = static_cast<f32>(ir->values[(m * 2 + ear) * samples + k]);
                if (convert) {
                    Resampler resampler(1, file_rate, sample_rate, ResampleQuality::High);
                    std::vector<f32> converted;
                    resampler.process(response.data(), response.size(), converted);
                    resampler.flush(converted);
                    // The converter's own look-ahead delays everything equally; both ears share it, so only trim the common lead.
                    const usize latency = static_cast<usize>(resampler.latency_frames() * static_cast<f64>(sample_rate) / file_rate);
                    if (latency < converted.size()) converted.erase(converted.begin(), converted.begin() + static_cast<std::ptrdiff_t>(latency));
                    response = std::move(converted);
                }
                (ear == 0 ? left : right)[m] = std::move(response);
            }
        }
        // Resampling can leave unequal lengths by a sample or two; trim to the shortest.
        usize shortest = ~usize{0};
        for (usize m = 0; m < measurements; ++m) shortest = std::min({shortest, left[m].size(), right[m].size()});
        for (usize m = 0; m < measurements; ++m) {
            left[m].resize(shortest);
            right[m].resize(shortest);
        }
        return from_measurements(sample_rate, directions, left, right, max_taps);
    }

    std::expected<std::shared_ptr<const HrtfSet>, UString> HrtfSet::from_sofa_file(const std::filesystem::path &path, u32 sample_rate, u32 max_taps) {
        auto bytes = read_file_bytes(path, Foundation::Io::AccessHint::Sequential);
        if (!bytes) return std::unexpected(std::move(bytes.error()));
        return from_sofa(std::span<const std::byte>((*bytes)->data(), (*bytes)->size()), sample_rate, max_taps);
    }

    HrtfSet::Blend HrtfSet::nearest(const glm::vec3 &direction) const noexcept {
        const f32 length = glm::length(direction);
        const glm::vec3 d = length > 1e-9f ? direction / length : glm::vec3(0.0f, 0.0f, -1.0f);
        std::array<f32, 3> best_dot{-2.0f, -2.0f, -2.0f};
        Blend blend;
        for (u32 m = 0; m < directions_.size(); ++m) {
            const f32 dot = glm::dot(d, directions_[m]);
            if (dot <= best_dot[2]) continue;
            u32 slot = 2;
            while (slot > 0 && dot > best_dot[slot - 1]) --slot;
            for (u32 s = 2; s > slot; --s) {
                best_dot[s] = best_dot[s - 1];
                blend.index[s] = blend.index[s - 1];
            }
            best_dot[slot] = dot;
            blend.index[slot] = m;
        }
        blend.count = std::min<u32>(3, static_cast<u32>(directions_.size()));
        // Weights fall off with angular distance; a hit within half a degree is used alone.
        f32 total = 0.0f;
        for (u32 i = 0; i < blend.count; ++i) {
            const f32 angle = std::acos(std::clamp(best_dot[i], -1.0f, 1.0f));
            if (angle < 0.0087f) {
                blend.count = 1;
                blend.index[0] = blend.index[i];
                blend.weight[0] = 1.0f;
                return blend;
            }
            blend.weight[i] = 1.0f / (angle * angle);
            total += blend.weight[i];
        }
        for (u32 i = 0; i < blend.count; ++i) blend.weight[i] /= total;
        return blend;
    }

    namespace {
        class HrtfFilter final : public BinauralFilter {
          public:
            explicit HrtfFilter(std::shared_ptr<const HrtfSet> set)
                : set_(std::move(set)), taps_(set_->taps()), history_(taps_ - 1, 0.0f), delay_{DelayLine(256), DelayLine(256)} {
                for (auto &ir : ir_) ir.assign(taps_, 0.0f);
                for (auto &ir : previous_ir_) ir.assign(taps_, 0.0f);
            }

            void process(const f32 *mono, f32 *left, f32 *right, u32 frames, const glm::vec3 &direction, f32) override {
                if (frames == 0) return;
                // Re-blend only when the direction has moved enough to matter.
                const glm::vec3 d = glm::length(direction) > 1e-9f ? glm::normalize(direction) : glm::vec3(0.0f, 0.0f, -1.0f);
                const bool moved = !initialised_ || glm::dot(d, last_direction_) < 0.99999f;
                bool crossfade = false;
                if (moved) {
                    const HrtfSet::Blend blend = set_->nearest(d);
                    for (u32 ear = 0; ear < 2; ++ear) {
                        std::swap(ir_[ear], previous_ir_[ear]);
                        std::fill(ir_[ear].begin(), ir_[ear].end(), 0.0f);
                        f32 onset = 0.0f;
                        for (u32 i = 0; i < blend.count; ++i) {
                            Kernels::add_gain(std::span<f32>(ir_[ear]), set_->response(blend.index[i], ear), blend.weight[i]);
                            onset += blend.weight[i] * set_->onset(blend.index[i], ear);
                        }
                        previous_onset_[ear] = onset_[ear];
                        onset_[ear] = onset;
                    }
                    // The nearer ear (smaller onset) plays undelayed; the other is late by the interaural difference.
                    const f32 common = std::min(onset_[0], onset_[1]);
                    for (f32 &o : onset_) o -= common;
                    crossfade = initialised_;
                    if (!initialised_) {
                        previous_onset_ = onset_;
                        initialised_ = true;
                    } else {
                        const f32 previous_common = std::min(previous_onset_[0], previous_onset_[1]);
                        for (f32 &o : previous_onset_) o -= previous_common;
                    }
                    last_direction_ = d;
                }
                // history | block
                signal_.assign(history_.begin(), history_.end());
                signal_.insert(signal_.end(), mono, mono + frames);
                for (u32 ear = 0; ear < 2; ++ear) {
                    convolve(ir_[ear], frames, wet_);
                    if (crossfade) {
                        convolve(previous_ir_[ear], frames, old_wet_);
                        for (u32 n = 0; n < frames; ++n) {
                            const f32 t = static_cast<f32>(n + 1) / static_cast<f32>(frames);
                            wet_[n] = old_wet_[n] * (1.0f - t) + wet_[n] * t;
                        }
                    }
                    f32 *out = ear == 0 ? left : right;
                    // Interaural delay as a fractional delay, ramped across the block when it changes.
                    const f32 from = crossfade ? previous_onset_[ear] : onset_[ear], to = onset_[ear];
                    for (u32 n = 0; n < frames; ++n) {
                        const f32 t = static_cast<f32>(n + 1) / static_cast<f32>(frames);
                        delay_[ear].write(wet_[n]);
                        out[n] += delay_[ear].read(1.0f + from + (to - from) * t);
                    }
                }
                std::copy(signal_.end() - static_cast<std::ptrdiff_t>(taps_ - 1), signal_.end(), history_.begin());
            }

          private:
            /// out[n] = sum_k ir[k] * x[n - k], with x read from `signal_` (whose first taps-1 samples are history).
            void convolve(const std::vector<f32> &ir, u32 frames, std::vector<f32> &out) {
                out.assign(frames, 0.0f);
                for (u32 k = 0; k < taps_; ++k) {
                    if (ir[k] == 0.0f) continue;
                    Kernels::add_gain(std::span<f32>(out), std::span<const f32>(signal_.data() + (taps_ - 1 - k), frames), ir[k]);
                }
            }

            std::shared_ptr<const HrtfSet> set_;
            u32 taps_;
            std::vector<f32> history_, signal_, wet_, old_wet_;
            std::array<std::vector<f32>, 2> ir_, previous_ir_;
            std::array<f32, 2> onset_{}, previous_onset_{};
            std::array<DelayLine, 2> delay_;
            glm::vec3 last_direction_{0.0f, 0.0f, -1.0f};
            bool initialised_ = false;
        };
    } // namespace

    std::unique_ptr<BinauralFilter> make_hrtf_filter(std::shared_ptr<const HrtfSet> set) { return std::make_unique<HrtfFilter>(std::move(set)); }

    BinauralFilterFactory make_hrtf_filter_factory(std::shared_ptr<const HrtfSet> set) {
        return [set = std::move(set)] { return make_hrtf_filter(set); };
    }

} // namespace SFT::Audio
