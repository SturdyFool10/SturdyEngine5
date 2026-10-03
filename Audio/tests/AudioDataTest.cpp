#include <Audio/AudioData.hpp>

#include <cmath>
#include <iostream>

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
    bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
} // namespace

int main() {
    // ---- patches ----
    {
        const auto patch = load_patch_json(R"({
            // a plucked saw through a low-pass
            "nodes": [
              {"id": "pitch", "type": "param", "name": "pitch", "default": 220},
              {"id": "osc", "type": "oscillator", "waveform": "saw", "frequency": "pitch"},
              {"id": "gate", "type": "constant", "value": 1},
              {"id": "env", "type": "envelope", "gate": "gate", "attack": 0.001, "decay": 0.1, "sustain": 0.8, "release": 0.1},
              {"id": "lp", "type": "filter", "band": "low", "input": "osc", "cutoff": 2000, "q": 0.7},
              {"id": "out", "type": "multiply", "a": "lp", "b": "env"} ],
            "output": "out" })"_ustr);
        check(patch.has_value(), "a patch loads from JSON");
        if (patch) {
            PatchSource source(*patch, 48000);
            check(source.param_count() == 1 && source.set_param("pitch"_ustr, 440.0f), "its parameter is live");
            AudioBuffer out(1, 2048);
            source.read(out, 2048);
            check(out.peak() > 0.1f && out.peak() < 2.0f, "and it makes sound");
        }
        check(!load_patch_json(R"({"nodes":[{"id":"a","type":"add","a":"missing","b":1}],"output":"a"})"_ustr).has_value(), "a dangling input is an error");
        check(!load_patch_json(R"({"nodes":[{"id":"a","type":"wobble"}],"output":"a"})"_ustr).has_value(), "an unknown node type is an error");
        check(!load_patch_json(R"({"nodes":[{"id":"a","type":"constant"}],"output":"b"})"_ustr).has_value(), "an unknown output is an error");
        const auto bad = load_patch_json(R"({"nodes":[{"id":"o","type":"oscillator","waveform":"zigzag","frequency":100}],"output":"o"})"_ustr);
        check(!bad.has_value() && bad.error().contains("zigzag"_ustr), "errors name the offending value");
    }

    // ---- cues ----
    {
        auto sound = std::make_shared<SampleBuffer>();
        sound->channels = 1;
        sound->sample_rate = 48000;
        sound->samples = std::make_shared<std::vector<float>>(4800, 0.1f);
        const auto cues = load_cues_json(R"([{"name":"step","selection":"round_robin","volume":0.8,"max_instances":3,"limit":"reject_new","cooldown":0.05,
            "spatial":false,"rolloff":"linear","max_distance":20,"variations":[{"sound":"a.wav","weight":2},{"sound":"b.wav","volume":0.5}]}])"_ustr,
                                         [&](const UString &name) { return name == "missing.wav"_ustr ? nullptr : std::shared_ptr<const SampleBuffer>(sound); });
        check(cues.has_value() && cues->size() == 1, "a cue loads");
        if (cues && !cues->empty()) {
            const SoundCue &c = cues->front();
            check(c.name == "step"_ustr && c.selection == CueSelection::RoundRobin && c.limit == CueLimit::RejectNew && c.max_instances == 3 && near(c.cooldown_seconds, 0.05, 1e-6), "with its fields");
            check(!c.base.spatial && c.base.distance.rolloff == Rolloff::Linear && near(c.base.distance.max_distance, 20.0), "and its playback settings");
            check(c.variations.size() == 2 && near(c.variations[0].weight, 2.0) && near(c.variations[1].volume, 0.5), "and its variations");
        }
        const auto missing = load_cues_json(R"({"name":"x","variations":[{"sound":"missing.wav"}]})"_ustr, [](const UString &) { return std::shared_ptr<const SampleBuffer>{}; });
        check(!missing.has_value() && missing.error().contains("missing.wav"_ustr), "a sound that cannot be found is named in the error");
    }

    // ---- effects ----
    {
        const auto chain = load_effects_json(R"([{"kind":"compressor","threshold":-18,"ratio":3}, {"kind":"noise_reduction","reduction":15}, {"kind":"gain_pan","gain":-3}])"_ustr);
        check(chain.has_value() && chain->size() == 3 && chain->at(1).kind == EffectKind::NoiseReduction, "an effect chain loads by kind name");
        const auto typo = load_effects_json(R"([{"kind":"compressor","thresold":-18}])"_ustr);
        check(!typo.has_value() && typo.error().contains("valid:"_ustr), "a misspelt parameter is caught at load time");
        check(!load_effects_json(R"([{"kind":"flanger9000"}])"_ustr).has_value(), "an unknown kind is an error");
    }

    // ---- tempo ----
    {
        const auto loaded = load_tempo_json(R"({"first_beat": 0.5, "changes": [{"beat":0,"bpm":120,"beats_per_bar":4},{"beat":8,"bpm":60,"beats_per_bar":3}]})"_ustr);
        check(loaded.has_value(), "a tempo map loads");
        if (loaded) {
            const TempoMap &t = *loaded;
            check(near(t.seconds_at_beat(0), 0.5) && near(t.seconds_at_beat(8), 0.5 + 4.0) && near(t.seconds_at_beat(10), 0.5 + 4.0 + 2.0), "beats convert to seconds across a tempo change");
            check(near(t.beat_at_seconds(0.5 + 4.0 + 2.0), 10.0) && near(t.beat_at_seconds(1.0), 1.0), "and back");
            check(near(t.bpm_at_beat(3), 120) && near(t.bpm_at_beat(9), 60) && t.beats_per_bar_at(9) == 3, "the tempo and signature in force are known");
            check(near(t.next_beat(2.3), 3.0) && near(t.next_bar(2.3), 4.0) && near(t.next_bar(4.0), 8.0), "next beat and bar lines");
            check(near(t.next_bar(9.5), 11.0), "bars restart at a signature change");
            check(near(t.beat_at_or_after(5.0), 5.0) && near(t.beat_at_or_after(5.2), 6.0) && near(t.bar_at_or_after(8.0), 8.0) && near(t.bar_at_or_after(8.5), 11.0), "standing on a line means no wait");
            check(t.bar_at(7.9) == 1 && t.bar_at(8.0) == 2 && t.bar_at(11.0) == 3 && near(t.beat_in_bar(9.0), 1.0), "bar numbers and the beat within the bar");
            const auto markers = t.beat_markers(0, 12, 4, "bar");
            check(markers.size() == 4 && markers[1].name == "bar4"_ustr && near(markers[2].seconds, 4.5) && markers[2].id == 3, "markers are laid on the grid");
        }
        const TempoMap steady = TempoMap::constant(90.0, 4, 1.0);
        check(near(steady.seconds_at_beat(3), 1.0 + 3 * 60.0 / 90.0), "a constant map is one tempo");
        check(!load_tempo_json(R"({"changes":[]})"_ustr).has_value() && !load_tempo_json(R"({"changes":[{"bpm":-5}]})"_ustr).has_value(), "empty or nonsensical maps are errors");
    }
    return failures == 0 ? 0 : 1;
}
