#include <Audio/Hrtf.hpp>
#include <Audio/Mixer.hpp>
#include <Audio/Source.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <random>

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

    // A tiny head model as measured responses: the ear facing the source gets an undelayed impulse, the far ear a quieter, later one.
    std::shared_ptr<const HrtfSet> model_set() {
        std::vector<glm::vec3> directions;
        std::vector<std::vector<float>> left, right;
        for (int az = -180; az < 180; az += 15) {
            for (int el : {-30, 0, 30}) {
                directions.push_back(direction_from_angles(static_cast<float>(az), static_cast<float>(el)));
                const float side = std::sin(static_cast<float>(az) * std::numbers::pi_v<float> / 180.0f); // + = source on the left
                std::vector<float> l(64, 0.0f), r(64, 0.0f);
                const int far_delay = 20;
                (side >= 0 ? l : r)[10] = 1.0f;
                (side >= 0 ? r : l)[10 + static_cast<int>(static_cast<float>(far_delay) * std::fabs(side))] = 1.0f - 0.6f * std::fabs(side);
                left.push_back(l);
                right.push_back(r);
            }
        }
        auto set = HrtfSet::from_measurements(48000, directions, left, right, 64);
        return set ? *set : nullptr;
    }

    struct Ears {
        double left_energy = 0.0, right_energy = 0.0;
        int left_onset = -1, right_onset = -1;
    };

    // Plays an impulse through a fresh filter from `direction`.
    Ears impulse(const std::shared_ptr<const HrtfSet> &set, const glm::vec3 &direction) {
        auto filter = make_hrtf_filter(set);
        std::vector<float> in(256, 0.0f), left(256, 0.0f), right(256, 0.0f);
        in[0] = 1.0f;
        filter->process(in.data(), left.data(), right.data(), 256, direction, 1.0f);
        Ears e;
        for (int i = 0; i < 256; ++i) {
            e.left_energy += static_cast<double>(left[i]) * left[i];
            e.right_energy += static_cast<double>(right[i]) * right[i];
            if (e.left_onset < 0 && std::fabs(left[i]) > 0.05f) e.left_onset = i;
            if (e.right_onset < 0 && std::fabs(right[i]) > 0.05f) e.right_onset = i;
        }
        return e;
    }
} // namespace

int main() {
    const auto set = model_set();
    check(set != nullptr && set->measurement_count() == 24 * 3, "a set builds from raw measurements");
    if (!set) return 1;

    const auto front = impulse(set, direction_from_angles(0, 0));
    check(std::fabs(front.left_energy - front.right_energy) < 0.05 * front.left_energy && front.left_onset == front.right_onset, "straight ahead is symmetric");
    const auto left_side = impulse(set, direction_from_angles(90, 0));
    check(left_side.left_energy > 4.0 * left_side.right_energy, "a source on the left is louder in the left ear");
    check(left_side.right_onset - left_side.left_onset >= 17 && left_side.right_onset - left_side.left_onset <= 23, "and arrives there first, by the interaural time difference");
    const auto right_side = impulse(set, direction_from_angles(-90, 0));
    check(right_side.right_energy > 4.0 * right_side.left_energy && right_side.left_onset > right_side.right_onset, "mirrored on the right");
    // Between two measurements the delay is interpolated, not smeared into two impulses.
    const auto between = impulse(set, direction_from_angles(52.5f, 0));
    std::cout << "  between: " << between.left_onset << " " << between.right_onset << " " << between.left_energy << " " << between.right_energy << "\n";
    check(between.left_onset >= 0 && between.right_onset > between.left_onset, "an unmeasured direction still has one clean onset per ear");

    // Moving smoothly: the output stays continuous when the direction changes between blocks.
    {
        auto filter = make_hrtf_filter(set);
        std::vector<float> tone(256), left(256), right(256);
        double worst_jump = 0.0;
        float last = 0.0f;
        for (int block = 0; block < 40; ++block) {
            for (int i = 0; i < 256; ++i) tone[i] = 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * 500.0f * static_cast<float>(block * 256 + i) / 48000.0f);
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
            filter->process(tone.data(), left.data(), right.data(), 256, direction_from_angles(static_cast<float>(block * 4), 0), 1.0f);
            if (block > 4) for (int i = 0; i < 256; ++i) { worst_jump = std::max(worst_jump, static_cast<double>(std::fabs(left[i] - last))); last = left[i]; }
            else last = left[255];
        }
        check(worst_jump < 0.3, "sweeping the source round the head does not click");
    }

    // Through an engine's binaural output.
    {
        AudioEngineConfig config;
        OutputDesc headphones;
        headphones.kind = OutputDesc::Kind::Binaural;
        headphones.name = "headphones";
        headphones.binaural = make_hrtf_filter_factory(set);
        config.outputs = {headphones};
        AudioEngine engine(config);
        auto buffer = std::make_shared<SampleBuffer>();
        buffer->channels = 1;
        buffer->sample_rate = 48000;
        auto samples = std::make_shared<std::vector<float>>(48000);
        for (size_t i = 0; i < samples->size(); ++i) (*samples)[i] = 0.4f * std::sin(0.08f * static_cast<float>(i));
        buffer->samples = samples;
        PlayParams p;
        p.source = std::make_shared<BufferSource>(buffer, 48000, true);
        p.spatial = true;
        p.position = {-4.0f, 0.0f, -0.1f}; // to the left
        engine.play(std::move(p));
        double l = 0, r = 0;
        for (int b = 0; b < 20; ++b) {
            engine.render_block();
            for (u32 i = 0; i < engine.bed(0).frames(); ++i) {
                l += engine.bed(0).data(0)[i] * engine.bed(0).data(0)[i];
                r += engine.bed(0).data(1)[i] * engine.bed(0).data(1)[i];
            }
        }
        check(l > 2.0 * r && r > 0.0, "an engine voice on the left is heard mostly in the left ear through the HRTF output");
    }

    // A real SOFA file, when one is supplied: the MIT KEMAR set.
    if (const char *path = std::getenv("STURDY_TEST_SOFA")) {
        for (u32 rate : {44100u, 48000u}) {
            auto real = HrtfSet::from_sofa_file(path, rate, 256);
            check(real.has_value(), "the SOFA file loads");
            if (!real) { std::cerr << real.error() << "\n"; continue; }
            check((*real)->measurement_count() == 710 && (*real)->sample_rate() == rate && (*real)->taps() == 256, "710 measurements at the requested rate");
            const auto lft = impulse(*real, direction_from_angles(90, 0));
            const auto rgt = impulse(*real, direction_from_angles(-90, 0));
            std::cout << "  KEMAR @" << rate << ": left source energy L/R = " << lft.left_energy << "/" << lft.right_energy << ", onsets " << lft.left_onset << "/" << lft.right_onset << "\n";
            check(lft.left_energy > 2.0 * lft.right_energy && lft.right_onset > lft.left_onset, "a real head shadows the far ear and delays it");
            check(rgt.right_energy > 2.0 * rgt.left_energy && rgt.left_onset > rgt.right_onset, "symmetrically on the right");
            const double itd_ms = 1000.0 * (lft.right_onset - lft.left_onset) / rate;
            check(itd_ms > 0.3 && itd_ms < 0.9, "with an interaural delay in the human range");
        }
    }
    return failures == 0 ? 0 : 1;
}
