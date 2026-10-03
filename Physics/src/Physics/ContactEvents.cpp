#include <Physics/ContactEvents.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Physics {

    u64 ContactEventFilter::pair_key(u64 a, u64 b) noexcept {
        if (a > b) {
            std::swap(a, b);
        }
        // Order-independent mix of the two ids (boost::hash_combine over the sorted pair).
        u64 h = a + 0x9E3779B97F4A7C15ull;
        h ^= b + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h;
    }

    f32 ContactEventFilter::intensity_of(f32 impulse) const noexcept {
        const f32 reference = std::max(config_.full_intensity_impulse, 1e-3f);
        // Log mapping: perceived loudness grows with the logarithm of the impulse.
        const f32 scaled = std::log1p(std::max(impulse, 0.0f)) / std::log1p(reference);
        return std::clamp(scaled, 0.0f, 1.0f);
    }

    void ContactEventFilter::begin_frame(f64 time_seconds) {
        time_ = time_seconds;
        events_.clear();
        if (next_allowed_.size() > 4096) {
            for (auto it = next_allowed_.begin(); it != next_allowed_.end();) {
                it = it->second <= time_ ? next_allowed_.erase(it) : std::next(it);
            }
        }
    }

    bool ContactEventFilter::submit(const ContactPoint &contact) {
        if (contact.normal_impulse < config_.min_impulse && std::max(contact.approach_speed, contact.slide_speed) < config_.min_speed) {
            return false;
        }
        const u64 key = pair_key(contact.body_a, contact.body_b);

        ContactEvent event;
        event.contact = contact;
        event.kind = contact.slide_speed > 2.0f * std::max(contact.approach_speed, 0.0f) ? ContactKind::Scrape : ContactKind::Impact;
        event.intensity = intensity_of(contact.normal_impulse);

        // Merge with a nearby event of the same pair already raised this frame (keep the stronger one).
        for (ContactEvent &existing : events_) {
            if (pair_key(existing.contact.body_a, existing.contact.body_b) != key) {
                continue;
            }
            const glm::vec3 d = existing.contact.point - contact.point;
            if (glm::dot(d, d) <= config_.merge_radius * config_.merge_radius) {
                if (event.contact.normal_impulse > existing.contact.normal_impulse) {
                    existing = event;
                    return true;
                }
                return false;
            }
        }

        if (const auto it = next_allowed_.find(key); it != next_allowed_.end() && it->second > time_) {
            return false;
        }

        if (events_.size() >= config_.max_events_per_frame) {
            // Over budget: displace the weakest event only for a stronger one.
            const auto weakest = std::min_element(events_.begin(), events_.end(), [](const ContactEvent &a, const ContactEvent &b) {
                return a.contact.normal_impulse < b.contact.normal_impulse;
            });
            if (weakest == events_.end() || weakest->contact.normal_impulse >= contact.normal_impulse) {
                return false;
            }
            *weakest = event;
        } else {
            events_.push_back(event);
        }
        next_allowed_[key] = time_ + static_cast<f64>(config_.pair_cooldown_seconds);
        return true;
    }

} // namespace SFT::Physics
