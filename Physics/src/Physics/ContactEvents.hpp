#pragma once

#include <Physics/Ballistics.hpp>

#include <Foundation/Foundation.hpp>

#include <glm/vec3.hpp>

#include <span>
#include <unordered_map>
#include <vector>

namespace SFT::Physics {

    /// A raw contact reported by the solver after a step.
    struct ContactPoint {
        u64 body_a = 0;
        u64 body_b = 0;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        /// Impulse along the normal (N*s).
        f32 normal_impulse = 0.0f;
        /// Closing speed along the normal (m/s) and relative speed along the surface (m/s).
        f32 approach_speed = 0.0f;
        f32 slide_speed = 0.0f;
        MaterialId material_a = no_material;
        MaterialId material_b = no_material;
    };

    enum class ContactKind : u8 { Impact, Scrape };

    /// A contact worth reacting to: what audio, decals and particle systems consume. Material ids index the shared
    /// `SurfaceTable`, so one table drives bullet penetration, impact sounds and impact effects alike.
    struct ContactEvent {
        ContactPoint contact;
        ContactKind kind = ContactKind::Impact;
        /// 0..1, a perceptual mapping of the impulse (log scale) for volume and effect intensity.
        f32 intensity = 0.0f;
    };

    struct ContactEventConfig {
        /// Contacts weaker than both of these are dropped.
        f32 min_impulse = 0.5f;
        f32 min_speed = 0.6f;
        /// A body pair cannot raise another event for this long after one.
        f32 pair_cooldown_seconds = 0.1f;
        /// Events of the same pair closer than this within a frame merge into the stronger one.
        f32 merge_radius = 0.25f;
        /// Hard cap per frame; once reached only events stronger than the weakest kept one get in.
        u32 max_events_per_frame = 64;
        /// Impulse that maps to intensity 1.
        f32 full_intensity_impulse = 200.0f;
    };

    /// Turns the solver's contact stream into a sparse, budgeted event list: thresholds, per-pair cooldowns, merging
    /// of near-duplicate contacts and a per-frame cap, so a rolling crate does not spam the audio mixer.
    class ContactEventFilter {
      public:
        explicit ContactEventFilter(const ContactEventConfig &config = {}) : config_(config) {}

        /// Clears last frame's events and sets the clock used for cooldowns.
        void begin_frame(f64 time_seconds);

        /// Offers a contact; returns true if it became (or replaced) an event.
        bool submit(const ContactPoint &contact);

        [[nodiscard]] std::span<const ContactEvent> events() const noexcept { return events_; }
        [[nodiscard]] const ContactEventConfig &config() const noexcept { return config_; }
        void set_config(const ContactEventConfig &config) noexcept { config_ = config; }

      private:
        [[nodiscard]] static u64 pair_key(u64 a, u64 b) noexcept;
        [[nodiscard]] f32 intensity_of(f32 impulse) const noexcept;

        ContactEventConfig config_;
        f64 time_ = 0.0;
        std::vector<ContactEvent> events_;
        std::unordered_map<u64, f64> next_allowed_;
    };

} // namespace SFT::Physics
