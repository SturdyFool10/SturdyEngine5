#pragma once

#include <Audio/Dsp.hpp>
#include <Audio/Mixer.hpp>

#include <Physics/Ballistics.hpp>
#include <Physics/ContactEvents.hpp>

#include <Foundation/Foundation.hpp>

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace SFT::Audio {

    /// Resonances of one material: what it rings like when struck.
    struct ModalPreset {
        std::vector<Mode> modes;
        /// Overall level for this material.
        f32 gain = 1.0f;
        /// Noise content of a hit (0 = pure ringing, 1 = a crack/thud).
        f32 noisiness = 0.2f;
    };

    class ModalMaterialTable {
      public:
        void add(UString name, ModalPreset preset);
        [[nodiscard]] const ModalPreset *find(const ustr &name) const noexcept;
        /// Metal, wood, glass, ceramic, plastic, stone, concrete, dirt, flesh.
        static ModalMaterialTable with_defaults();

      private:
        std::unordered_map<UString, ModalPreset> presets_;
    };

    /// A one-shot data source that rings two modal banks (the two bodies in a contact) plus a short noise burst. It
    /// finishes when everything has decayed.
    class ModalImpactSource final : public DataSource {
      public:
        /// `strength` 0..1 scales how hard each body is struck; `detune` is a small relative pitch offset for variation.
        ModalImpactSource(u32 sample_rate, const ModalPreset &a, const ModalPreset &b, f32 strength, f32 detune, f32 noise_amount, u32 seed);

        u32 channel_count() const override { return 1; }
        u32 sample_rate() const override { return sample_rate_; }
        u32 read(AudioBuffer &out, u32 frames) override;
        bool finished() const override { return finished_; }

      private:
        u32 sample_rate_;
        ModalBank a_, b_;
        Noise noise_;
        Biquad noise_filter_;
        Envelope noise_envelope_;
        f32 noise_amount_;
        u32 elapsed_ = 0;
        bool finished_ = false;
    };

    struct ContactSoundConfig {
        /// Simultaneous contact voices; further events are dropped (the filter already rate-limits them).
        u32 max_active = 24;
        /// Bus the sounds play on.
        BusId bus = 0;
        f32 volume = 1.0f;
        /// Below this intensity nothing is played.
        f32 min_intensity = 0.02f;
        DistanceModel distance{Rolloff::Inverse, 1.0f, 40.0f, 1.0f};
    };

    /// Physics contacts to sound: each `ContactEvent` becomes a spatial modal impact, its materials resolved by name
    /// through the shared `SurfaceTable` so the same material drives bullet penetration, contact sound and effects.
    class ContactSoundSystem {
      public:
        ContactSoundSystem(AudioEngine &engine, const Physics::SurfaceTable &surfaces, ModalMaterialTable modal,
                           const ContactSoundConfig &config = {});

        /// Plays a sound per event; returns how many started.
        u32 process(std::span<const Physics::ContactEvent> events);
        /// Tell the system voices finished (from `AudioEngine::pump()`) so the active count stays right.
        void on_voices_finished(std::span<const VoiceId> finished);
        [[nodiscard]] u32 active() const noexcept { return static_cast<u32>(live_.size()); }
        void set_config(const ContactSoundConfig &config) noexcept { config_ = config; }

      private:
        [[nodiscard]] const ModalPreset &preset_for(Physics::MaterialId id) const;

        AudioEngine &engine_;
        const Physics::SurfaceTable &surfaces_;
        ModalMaterialTable modal_;
        ContactSoundConfig config_;
        ModalPreset fallback_;
        std::vector<VoiceId> live_;
    };

} // namespace SFT::Audio
