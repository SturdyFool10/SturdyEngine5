#include <Audio/AudioData.hpp>

#include <Audio/Text.hpp>

#include <nlohmann/json.hpp>

#include <format>
#include <stdexcept>
#include <unordered_map>

namespace SFT::Audio {

    namespace {
        using Json = nlohmann::json;

        std::expected<Json, UString> parse(const ustr &text) {
            try {
                return Json::parse(text.cpp_string_view(), nullptr, true, /*ignore_comments=*/true);
            } catch (const Json::exception &error) {
                return std::unexpected(UString{std::format("audio data: {}", error.what())});
            }
        }

        UString fail(std::string_view what) { return UString{std::format("audio data: {}", what)}; }

        f32 number(const Json &j, const char *key, f32 fallback) { return j.contains(key) && j[key].is_number() ? j[key].get<f32>() : fallback; }
        bool flag(const Json &j, const char *key, bool fallback) { return j.contains(key) && j[key].is_boolean() ? j[key].get<bool>() : fallback; }
        std::string text_of(const Json &j, const char *key, const std::string &fallback = {}) { return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback; }
    } // namespace

    std::expected<std::shared_ptr<const PatchDef>, UString> load_patch_json(const ustr &text) {
        auto doc = parse(text);
        if (!doc) return std::unexpected(std::move(doc.error()));
        if (!doc->contains("nodes") || !(*doc)["nodes"].is_array() || !doc->contains("output")) {
            return std::unexpected(fail("a patch needs a \"nodes\" array and an \"output\""));
        }
        PatchBuilder builder;
        std::unordered_map<std::string, PatchNode> ids;
        // An input that names nothing throws; the loop below turns it into the error return.
        const auto input = [&](const Json &node, const char *key, bool required = true) -> PatchNode {
            if (!node.contains(key)) {
                if (required) throw std::invalid_argument(std::format("node '{}' is missing \"{}\"", text_of(node, "id", "?"), key));
                return no_patch_node;
            }
            if (node[key].is_number()) return builder.constant(node[key].get<f32>()); // a bare number is a constant
            const auto it = node[key].is_string() ? ids.find(node[key].get<std::string>()) : ids.end();
            if (it == ids.end()) throw std::invalid_argument(std::format("node '{}': \"{}\" does not name an earlier node", text_of(node, "id", "?"), key));
            return it->second;
        };
        for (const Json &node : (*doc)["nodes"]) try {
            const std::string id = text_of(node, "id"), type = text_of(node, "type");
            if (id.empty() || type.empty()) return std::unexpected(fail("every patch node needs an \"id\" and a \"type\""));
            if (ids.contains(id)) return std::unexpected(fail(std::format("duplicate node id '{}'", id)));
            PatchNode made = no_patch_node;
            if (type == "constant") {
                made = builder.constant(number(node, "value", 0.0f));
            } else if (type == "param") {
                made = builder.param(UString{text_of(node, "name", id)}, number(node, "default", 0.0f));
            } else if (type == "oscillator") {
                const std::string w = text_of(node, "waveform", "sine");
                const Waveform waveform = w == "saw" ? Waveform::Saw : w == "square" ? Waveform::Square : w == "triangle" ? Waveform::Triangle : Waveform::Sine;
                if (w != "sine" && w != "saw" && w != "square" && w != "triangle") return std::unexpected(fail(std::format("node '{}': unknown waveform '{}'", id, w)));
                made = builder.oscillator(waveform, input(node, "frequency"), input(node, "pulse_width", false));
            } else if (type == "noise") {
                made = builder.noise(flag(node, "pink", false), static_cast<u32>(number(node, "seed", 1.0f)));
            } else if (type == "envelope") {
                made = builder.envelope(input(node, "gate"), number(node, "attack", 0.01f), number(node, "decay", 0.1f), number(node, "sustain", 0.7f), number(node, "release", 0.2f));
            } else if (type == "filter") {
                const std::string b = text_of(node, "band", "low");
                if (b != "low" && b != "band" && b != "high") return std::unexpected(fail(std::format("node '{}': unknown filter band '{}'", id, b)));
                made = builder.filter(b == "low" ? PatchBuilder::Band::Low : b == "band" ? PatchBuilder::Band::Band : PatchBuilder::Band::High, input(node, "input"), input(node, "cutoff"), number(node, "q", 0.7071f));
            } else if (type == "add") {
                made = builder.add(input(node, "a"), input(node, "b"));
            } else if (type == "multiply") {
                made = builder.multiply(input(node, "a"), input(node, "b"));
            } else if (type == "mix") {
                made = builder.mix(input(node, "a"), input(node, "b"), input(node, "t"));
            } else if (type == "scale") {
                made = builder.scale(input(node, "input"), number(node, "scale", 1.0f), number(node, "offset", 0.0f));
            } else if (type == "saturate") {
                made = builder.saturate(input(node, "input"), number(node, "drive", 1.0f));
            } else if (type == "delay") {
                made = builder.delay(input(node, "input"), number(node, "max_seconds", 1.0f), input(node, "time"), number(node, "feedback", 0.0f));
            } else if (type == "midi_to_hz") {
                made = builder.midi_to_hz(input(node, "note"));
            } else {
                return std::unexpected(fail(std::format("node '{}': unknown type '{}'", id, type)));
            }
            ids[id] = made;
        } catch (const std::invalid_argument &error) {
            return std::unexpected(fail(error.what()));
        }
        const std::string output = (*doc)["output"].is_string() ? (*doc)["output"].get<std::string>() : std::string{};
        const auto it = ids.find(output);
        if (it == ids.end()) return std::unexpected(fail(std::format("the output '{}' is not a node", output)));
        return builder.build(it->second);
    }

    std::expected<std::vector<SoundCue>, UString> load_cues_json(const ustr &text, const SoundResolver &resolve) {
        auto doc = parse(text);
        if (!doc) return std::unexpected(std::move(doc.error()));
        const Json &list = doc->is_array() ? *doc : (doc->contains("cues") ? (*doc)["cues"] : Json::array({*doc}));
        if (!list.is_array()) return std::unexpected(fail("\"cues\" must be an array"));
        std::vector<SoundCue> cues;
        for (const Json &j : list) {
            SoundCue cue;
            cue.name = UString{text_of(j, "name")};
            if (cue.name.empty()) return std::unexpected(fail("every cue needs a \"name\""));
            const std::string selection = text_of(j, "selection", "random_no_repeat");
            if (selection == "random") cue.selection = CueSelection::Random;
            else if (selection == "random_no_repeat") cue.selection = CueSelection::RandomNoRepeat;
            else if (selection == "round_robin") cue.selection = CueSelection::RoundRobin;
            else if (selection == "layered") cue.selection = CueSelection::Layered;
            else return std::unexpected(fail(std::format("cue '{}': unknown selection '{}'", cue.name, selection)));
            const std::string limit = text_of(j, "limit", "stop_oldest");
            if (limit == "reject_new") cue.limit = CueLimit::RejectNew;
            else if (limit == "stop_oldest") cue.limit = CueLimit::StopOldest;
            else return std::unexpected(fail(std::format("cue '{}': unknown limit '{}'", cue.name, limit)));
            cue.volume = number(j, "volume", 1.0f);
            cue.volume_variation = number(j, "volume_variation", 0.0f);
            cue.pitch = number(j, "pitch", 1.0f);
            cue.pitch_variation_semitones = number(j, "pitch_variation_semitones", 0.0f);
            cue.max_instances = static_cast<u32>(number(j, "max_instances", 0.0f));
            cue.cooldown_seconds = number(j, "cooldown", 0.0f);
            cue.base.spatial = flag(j, "spatial", true);
            cue.base.priority = number(j, "priority", 1.0f);
            cue.base.spread = number(j, "spread", 0.0f);
            cue.base.distance.min_distance = number(j, "min_distance", cue.base.distance.min_distance);
            cue.base.distance.max_distance = number(j, "max_distance", cue.base.distance.max_distance);
            cue.base.distance.rolloff_factor = number(j, "rolloff_factor", cue.base.distance.rolloff_factor);
            const std::string rolloff = text_of(j, "rolloff", "inverse");
            if (rolloff == "inverse") cue.base.distance.rolloff = Rolloff::Inverse;
            else if (rolloff == "linear") cue.base.distance.rolloff = Rolloff::Linear;
            else if (rolloff == "exponential") cue.base.distance.rolloff = Rolloff::Exponential;
            else if (rolloff == "logarithmic") cue.base.distance.rolloff = Rolloff::Logarithmic;
            else return std::unexpected(fail(std::format("cue '{}': unknown rolloff '{}'", cue.name, rolloff)));
            if (!j.contains("variations") || !j["variations"].is_array() || j["variations"].empty()) {
                return std::unexpected(fail(std::format("cue '{}' has no variations", cue.name)));
            }
            for (const Json &v : j["variations"]) {
                const UString sound = UString{text_of(v, "sound")};
                CueVariation variation;
                variation.sound = resolve ? resolve(sound) : nullptr;
                if (!variation.sound) return std::unexpected(fail(std::format("cue '{}': the sound '{}' could not be found", cue.name, sound)));
                variation.weight = number(v, "weight", 1.0f);
                variation.volume = number(v, "volume", 1.0f);
                cue.variations.push_back(std::move(variation));
            }
            cues.push_back(std::move(cue));
        }
        return cues;
    }

    std::expected<std::vector<EffectSpec>, UString> load_effects_json(const ustr &text) {
        auto doc = parse(text);
        if (!doc) return std::unexpected(std::move(doc.error()));
        const Json &list = doc->is_array() ? *doc : (doc->contains("effects") ? (*doc)["effects"] : Json::array({*doc}));
        if (!list.is_array()) return std::unexpected(fail("\"effects\" must be an array"));
        std::vector<EffectSpec> specs;
        for (const Json &j : list) {
            const std::string kind = text_of(j, "kind");
            std::optional<EffectKind> found;
            for (const EffectKind candidate : all_effect_kinds()) {
                if (effect_kind_token(candidate) == kind) found = candidate;
            }
            if (!found) return std::unexpected(fail(std::format("unknown effect kind '{}'", kind)));
            EffectSpec spec(*found);
            for (const auto &[key, value] : j.items()) {
                if (key == "kind") continue;
                if (!value.is_number() && !value.is_boolean()) return std::unexpected(fail(std::format("effect '{}': parameter '{}' must be a number or boolean", kind, key)));
                spec.set(ustr{key}, value.is_boolean() ? (value.get<bool>() ? 1.0f : 0.0f) : value.get<f32>());
            }
            // Build once so a misspelt parameter is reported here, where the file is being loaded, not later on the audio thread.
            if (auto built = make_effect(spec); !built) return std::unexpected(std::move(built.error()));
            specs.push_back(std::move(spec));
        }
        return specs;
    }

    std::expected<TempoMap, UString> load_tempo_json(const ustr &text) {
        auto doc = parse(text);
        if (!doc) return std::unexpected(std::move(doc.error()));
        if (!doc->contains("changes") || !(*doc)["changes"].is_array() || (*doc)["changes"].empty()) return std::unexpected(fail("a tempo map needs a non-empty \"changes\" array"));
        TempoMap map;
        map.set_first_beat_seconds(static_cast<f64>(number(*doc, "first_beat", 0.0f)));
        for (const Json &c : (*doc)["changes"]) {
            if (!c.contains("bpm") || !c["bpm"].is_number() || c["bpm"].get<f64>() <= 0.0) return std::unexpected(fail("every tempo change needs a positive \"bpm\""));
            map.add(TempoChange{c.value("beat", 0.0), c["bpm"].get<f64>(), static_cast<u32>(c.value("beats_per_bar", 4.0))});
        }
        return map;
    }

} // namespace SFT::Audio
