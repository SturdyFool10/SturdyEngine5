#include <Audio/Cue.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Audio {

    f32 CuePlayer::random01() {
        // xorshift64*: deterministic across platforms, good enough for variation.
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return static_cast<f32>((state_ * 0x2545F4914F6CDD1Dull) >> 40) * (1.0f / 16777216.0f);
    }

    usize CuePlayer::pick(const SoundCue &cue, State &state) {
        const usize n = cue.variations.size();
        if (n == 1) {
            return 0;
        }
        if (cue.selection == CueSelection::RoundRobin) {
            const usize chosen = state.next % n;
            state.next = chosen + 1;
            return chosen;
        }
        f32 total = 0.0f;
        for (usize i = 0; i < n; ++i) {
            if (cue.selection == CueSelection::RandomNoRepeat && i == state.last) {
                continue;
            }
            total += std::max(cue.variations[i].weight, 0.0f);
        }
        if (total <= 0.0f) {
            return 0;
        }
        f32 roll = random01() * total;
        for (usize i = 0; i < n; ++i) {
            if (cue.selection == CueSelection::RandomNoRepeat && i == state.last) {
                continue;
            }
            roll -= std::max(cue.variations[i].weight, 0.0f);
            if (roll <= 0.0f) {
                return i;
            }
        }
        return n - 1;
    }

    u32 CuePlayer::active_instances(const SoundCue &cue) {
        State &state = states_[&cue];
        state.instances.erase(std::remove_if(state.instances.begin(), state.instances.end(), [](const SoundHandle &h) { return h.finished(); }),
                              state.instances.end());
        return static_cast<u32>(state.instances.size());
    }

    void CuePlayer::stop_all(const SoundCue &cue, f32 fade_seconds) {
        State &state = states_[&cue];
        for (const SoundHandle &h : state.instances) {
            h.stop(fade_seconds);
        }
        state.instances.clear();
    }

    SoundHandle CuePlayer::play(const SoundCue &cue, f64 now_seconds, const PlayParams &overrides) {
        if (cue.variations.empty()) {
            return {};
        }
        State &state = states_[&cue];
        if (cue.cooldown_seconds > 0.0f && now_seconds - state.last_start < static_cast<f64>(cue.cooldown_seconds)) {
            return {};
        }
        const u32 layers = cue.selection == CueSelection::Layered ? static_cast<u32>(cue.variations.size()) : 1u;
        if (cue.max_instances > 0) {
            // Layered cues count as one instance per layer group: limit by starts of the cue, not by layers.
            while (active_instances(cue) + layers > cue.max_instances) {
                if (cue.limit == CueLimit::RejectNew || state.instances.empty()) {
                    return {};
                }
                state.instances.front().stop(0.02f); // oldest fades out quickly
                state.instances.erase(state.instances.begin());
            }
        }

        const f32 volume_scale = 1.0f + (random01() * 2.0f - 1.0f) * cue.volume_variation;
        const f32 semitones = (random01() * 2.0f - 1.0f) * cue.pitch_variation_semitones;
        const f32 pitch = cue.pitch * std::exp2(semitones / 12.0f);

        SoundHandle first;
        const auto start = [&](usize index) {
            const CueVariation &variation = cue.variations[index];
            PlayParams params = cue.base;
            // Per-call overrides take the placement and routing fields.
            if (overrides.bus != 0) params.bus = overrides.bus;
            params.position = overrides.position;
            params.velocity = overrides.velocity;
            params.space = overrides.space;
            if (overrides.aux_bus != no_bus) {
                params.aux_bus = overrides.aux_bus;
                params.aux_send = overrides.aux_send;
            }
            params.volume = cue.volume * variation.volume * volume_scale * overrides.volume;
            params.pitch = pitch * overrides.pitch;
            if (overrides.start_delay_seconds > 0.0f) params.start_delay_seconds = overrides.start_delay_seconds;
            SoundHandle handle = sounds_.play_buffer(variation.sound, std::move(params), {}, false, false);
            if (handle.valid()) {
                state.instances.push_back(handle);
                if (!first.valid()) first = handle;
            }
        };
        if (cue.selection == CueSelection::Layered) {
            for (usize i = 0; i < cue.variations.size(); ++i) start(i);
        } else {
            const usize chosen = pick(cue, state);
            state.last = chosen;
            start(chosen);
        }
        if (first.valid()) {
            state.last_start = now_seconds;
        }
        return first;
    }

} // namespace SFT::Audio
