#include <Audio/ContactSounds.hpp>
#include <Foundation/Iter.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Audio {

    void ModalMaterialTable::add(UString name, ModalPreset preset) { presets_[std::move(name)] = std::move(preset); }

    const ModalPreset *ModalMaterialTable::find(const ustr &name) const noexcept {
        const auto it = presets_.find(UString{name});
        return it == presets_.end() ? nullptr : &it->second;
    }

    ModalMaterialTable ModalMaterialTable::with_defaults() {
        ModalMaterialTable t;
        t.add("metal", {{{800, 0.9f, 1.0f}, {2100, 0.6f, 0.7f}, {3700, 0.4f, 0.5f}, {5600, 0.25f, 0.3f}}, 1.0f, 0.15f});
        t.add("wood", {{{220, 0.12f, 1.0f}, {540, 0.09f, 0.6f}, {1100, 0.06f, 0.4f}}, 1.0f, 0.3f});
        t.add("glass", {{{1800, 0.7f, 1.0f}, {3300, 0.5f, 0.7f}, {5400, 0.35f, 0.5f}, {8100, 0.2f, 0.3f}}, 0.9f, 0.1f});
        t.add("ceramic", {{{1200, 0.35f, 1.0f}, {2900, 0.25f, 0.6f}, {4700, 0.18f, 0.4f}}, 0.9f, 0.15f});
        t.add("plastic", {{{450, 0.08f, 1.0f}, {1300, 0.05f, 0.5f}}, 0.8f, 0.3f});
        t.add("stone", {{{350, 0.15f, 1.0f}, {900, 0.1f, 0.6f}, {1900, 0.06f, 0.3f}}, 1.0f, 0.4f});
        t.add("concrete", {{{300, 0.12f, 1.0f}, {850, 0.08f, 0.5f}, {1700, 0.05f, 0.3f}}, 1.0f, 0.5f});
        t.add("dirt", {{{120, 0.05f, 1.0f}, {320, 0.04f, 0.5f}}, 0.7f, 0.7f});
        t.add("flesh", {{{90, 0.06f, 1.0f}, {240, 0.04f, 0.4f}}, 0.8f, 0.6f});
        return t;
    }

    ModalImpactSource::ModalImpactSource(u32 sample_rate, const ModalPreset &a, const ModalPreset &b, f32 strength, f32 detune,
                                         f32 noise_amount, u32 seed)
        : sample_rate_(sample_rate), noise_(seed), noise_amount_(noise_amount) {
        const auto detuned = [&](const ModalPreset &preset) {
            std::vector<Mode> modes = preset.modes;
            for (Mode &m : modes) {
                m.frequency *= 1.0f + detune;
                m.gain *= preset.gain;
            }
            return modes;
        };
        const std::vector<Mode> modes_a = detuned(a), modes_b = detuned(b);
        a_.set(static_cast<f32>(sample_rate), modes_a);
        b_.set(static_cast<f32>(sample_rate), modes_b);
        a_.excite(strength);
        b_.excite(strength * 0.7f);
        // The noise burst is the hit itself (the transient the modes ring out of): short and band-limited.
        noise_filter_.set(FilterType::BandPass, static_cast<f32>(sample_rate), a.modes.empty() ? 1500.0f : std::min(a.modes.front().frequency * 2.0f, 8000.0f), 0.7f);
        noise_envelope_.set(static_cast<f32>(sample_rate), 0.0005f, 0.01f, 0.0f, 0.005f);
        noise_envelope_.gate_on(); // attack, decay to 0, then it rests in Sustain at level 0
        (void)strength;
    }

    u32 ModalImpactSource::read(AudioBuffer &out, u32 frames) {
        if (finished_ || out.channels() == 0) {
            return 0;
        }
        f32 *dst = out.data(0);
        for (u32 i = 0; i < frames; ++i) {
            const f32 hit = noise_filter_.process(noise_.white()) * noise_envelope_.next() * noise_amount_;
            dst[i] = a_.next() + b_.next() + hit;
        }
        elapsed_ += frames;
        if (elapsed_ > sample_rate_ / 50 && a_.silent() && b_.silent() && noise_envelope_.stage() == Envelope::Stage::Sustain) {
            finished_ = true;
        }
        // Hard stop after two seconds regardless (a very long ring on a tiny threshold).
        if (elapsed_ > sample_rate_ * 2) {
            finished_ = true;
        }
        return frames;
    }

    ContactSoundSystem::ContactSoundSystem(AudioEngine &engine, const Physics::SurfaceTable &surfaces, ModalMaterialTable modal,
                                           const ContactSoundConfig &config)
        : engine_(engine), surfaces_(surfaces), modal_(std::move(modal)), config_(config) {
        fallback_ = ModalPreset{{{400, 0.1f, 1.0f}, {1100, 0.07f, 0.5f}}, 0.8f, 0.4f};
    }

    const ModalPreset &ContactSoundSystem::preset_for(Physics::MaterialId id) const {
        if (const Physics::SurfaceMaterial *surface = surfaces_.get(id)) {
            if (const ModalPreset *preset = modal_.find(surface->name)) {
                return *preset;
            }
        }
        return fallback_;
    }

    void ContactSoundSystem::on_voices_finished(std::span<const VoiceId> finished) {
        for (VoiceId id : finished) {
            const auto index = Foundation::iter(live_).position([id](VoiceId live) { return live == id; });
            if (index) {
                live_[*index] = live_.back();
                live_.pop_back();
            }
        }
    }

    u32 ContactSoundSystem::process(std::span<const Physics::ContactEvent> events) {
        u32 started = 0;
        for (const Physics::ContactEvent &event : events) {
            if (event.intensity < config_.min_intensity || live_.size() >= config_.max_active) {
                continue;
            }
            const ModalPreset &a = preset_for(event.contact.material_a);
            const ModalPreset &b = preset_for(event.contact.material_b);
            // Deterministic per-contact variation: the same collision always sounds the same, different ones differ.
            const u32 seed = static_cast<u32>((event.contact.body_a * 2654435761u) ^ (event.contact.body_b * 40503u) ^ static_cast<u32>(started));
            const f32 detune = (static_cast<f32>(seed % 1000u) / 1000.0f - 0.5f) * 0.06f;
            const f32 noise = event.kind == Physics::ContactKind::Scrape ? 0.6f : 0.5f * (a.noisiness + b.noisiness);

            PlayParams params;
            params.source = std::make_shared<ModalImpactSource>(engine_.config().sample_rate, a, b, event.intensity, detune, noise, seed);
            params.bus = config_.bus;
            params.volume = config_.volume;
            params.position = event.contact.point;
            params.distance = config_.distance;
            params.priority = 0.5f + event.intensity;
            const VoiceId voice = engine_.play(std::move(params));
            if (voice != 0) {
                live_.push_back(voice);
                ++started;
            }
        }
        return started;
    }

} // namespace SFT::Audio
