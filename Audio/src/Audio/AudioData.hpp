#pragma once

#include <Audio/Cue.hpp>
#include <Audio/Effects.hpp>
#include <Audio/Patch.hpp>
#include <Audio/Tempo.hpp>

#include <expected>
#include <functional>
#include <vector>

/// Audio described as data, so designers change sounds without a build: synthesis patches, sound cues, effect chains and tempo maps
/// from JSON text (`//` and `/* */` comments are accepted). Mistakes are reported with the offending name, never silently skipped.
namespace SFT::Audio {

    /// A synthesis patch.
    ///
    /// ```json
    /// { "nodes": [
    ///     {"id": "pitch", "type": "param", "name": "pitch", "default": 220},
    ///     {"id": "osc",   "type": "oscillator", "waveform": "saw", "frequency": "pitch"},
    ///     {"id": "gate",  "type": "param", "name": "gate"},
    ///     {"id": "env",   "type": "envelope", "gate": "gate", "attack": 0.01, "decay": 0.2, "sustain": 0.6, "release": 0.3},
    ///     {"id": "cut",   "type": "constant", "value": 1200},
    ///     {"id": "lp",    "type": "filter", "band": "low", "input": "osc", "cutoff": "cut", "q": 0.8},
    ///     {"id": "out",   "type": "multiply", "a": "lp", "b": "env"} ],
    ///   "output": "out" }
    /// ```
    /// Node types: constant (value), param (name, default), oscillator (waveform sine|saw|square|triangle, frequency, pulse_width),
    /// noise (pink, seed), envelope (gate, attack, decay, sustain, release), filter (band low|band|high, input, cutoff, q), add (a, b),
    /// multiply (a, b), mix (a, b, t), scale (input, scale, offset), saturate (input, drive), delay (input, max_seconds, time, feedback),
    /// midi_to_hz (note). Inputs name earlier nodes.
    [[nodiscard]] std::expected<std::shared_ptr<const PatchDef>, UString> load_patch_json(const ustr &text);

    /// Finds the decoded recording a cue names.
    using SoundResolver = std::function<std::shared_ptr<const SampleBuffer>(const UString &name)>;

    /// A sound cue.
    ///
    /// ```json
    /// { "name": "footstep", "selection": "random_no_repeat", "volume": 0.8, "volume_variation": 0.1,
    ///   "pitch_variation_semitones": 1.5, "max_instances": 6, "limit": "stop_oldest", "cooldown": 0.05,
    ///   "spatial": true, "min_distance": 1, "max_distance": 40, "rolloff": "inverse", "priority": 1,
    ///   "variations": [ {"sound": "step_a.ogg", "weight": 1}, {"sound": "step_b.ogg", "weight": 1, "volume": 0.9} ] }
    /// ```
    /// An array of such objects (or `{"cues": [...]}`) loads several.
    [[nodiscard]] std::expected<std::vector<SoundCue>, UString> load_cues_json(const ustr &text, const SoundResolver &resolve);

    /// An effect chain: `[{"kind": "compressor", "threshold": -18, "ratio": 3}, {"kind": "noise_reduction", "reduction": 15}]`. Kinds are the
    /// identifiers `effect_kind_token` returns; every other key is a parameter name (checked when the effect is built).
    [[nodiscard]] std::expected<std::vector<EffectSpec>, UString> load_effects_json(const ustr &text);

    /// A tempo map: `{"first_beat": 0.25, "changes": [{"beat": 0, "bpm": 100, "beats_per_bar": 4}, {"beat": 32, "bpm": 140, "beats_per_bar": 3}]}`.
    [[nodiscard]] std::expected<TempoMap, UString> load_tempo_json(const ustr &text);

} // namespace SFT::Audio
