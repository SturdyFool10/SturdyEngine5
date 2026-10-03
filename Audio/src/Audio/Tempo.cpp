#include <Audio/Tempo.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace SFT::Audio {

    namespace {
        constexpr f64 kEpsilon = 1e-6;
    }

    TempoMap TempoMap::constant(f64 bpm, u32 beats_per_bar, f64 first_beat_seconds) {
        TempoMap map;
        map.add(TempoChange{0.0, bpm, beats_per_bar});
        map.set_first_beat_seconds(first_beat_seconds);
        return map;
    }

    TempoMap::TempoMap(std::vector<TempoChange> changes, f64 first_beat_seconds) : first_beat_seconds_(first_beat_seconds) {
        for (const TempoChange &c : changes) add(c);
    }

    void TempoMap::add(const TempoChange &change) {
        TempoChange c = change;
        c.bpm = std::max(c.bpm, 1.0);
        c.beats_per_bar = std::max(c.beats_per_bar, 1u);
        const auto at = std::ranges::lower_bound(changes_, c.beat, {}, &TempoChange::beat);
        if (at != changes_.end() && std::fabs(at->beat - c.beat) < kEpsilon) {
            *at = c;
        } else {
            changes_.insert(at, c);
        }
        rebuild();
    }

    void TempoMap::set_first_beat_seconds(f64 seconds) { first_beat_seconds_ = seconds; }

    void TempoMap::rebuild() {
        start_seconds_.assign(changes_.size(), 0.0);
        for (usize i = 1; i < changes_.size(); ++i) {
            start_seconds_[i] = start_seconds_[i - 1] + (changes_[i].beat - changes_[i - 1].beat) * 60.0 / changes_[i - 1].bpm;
        }
    }

    usize TempoMap::segment_for_beat(f64 beat) const {
        const auto after = std::ranges::upper_bound(changes_, beat + kEpsilon, {}, &TempoChange::beat);
        return after == changes_.begin() ? 0 : static_cast<usize>(after - changes_.begin()) - 1;
    }

    f64 TempoMap::seconds_at_beat(f64 beat) const {
        if (changes_.empty()) return first_beat_seconds_;
        const usize s = segment_for_beat(beat);
        return first_beat_seconds_ + start_seconds_[s] + (beat - changes_[s].beat) * 60.0 / changes_[s].bpm;
    }

    f64 TempoMap::beat_at_seconds(f64 seconds) const {
        if (changes_.empty()) return 0.0;
        const f64 relative = seconds - first_beat_seconds_;
        const auto after = std::ranges::upper_bound(start_seconds_, relative);
        const usize s = after == start_seconds_.begin() ? 0 : static_cast<usize>(after - start_seconds_.begin()) - 1;
        return changes_[s].beat + (relative - start_seconds_[s]) * changes_[s].bpm / 60.0;
    }

    f64 TempoMap::bpm_at_beat(f64 beat) const { return changes_.empty() ? 0.0 : changes_[segment_for_beat(beat)].bpm; }
    u32 TempoMap::beats_per_bar_at(f64 beat) const { return changes_.empty() ? 4u : changes_[segment_for_beat(beat)].beats_per_bar; }

    f64 TempoMap::next_beat(f64 beat) const {
        const f64 next = std::floor(beat + kEpsilon) + 1.0;
        // Beats are whole numbers from the first beat unless a change lands between them: that change is a grid point too.
        const usize s = segment_for_beat(beat);
        if (s + 1 < changes_.size() && changes_[s + 1].beat > beat + kEpsilon && changes_[s + 1].beat < next) return changes_[s + 1].beat;
        return next;
    }

    f64 TempoMap::next_bar(f64 beat) const {
        if (changes_.empty()) return beat;
        const usize s = segment_for_beat(beat);
        const f64 bar_length = changes_[s].beats_per_bar;
        const f64 bars = std::floor((beat - changes_[s].beat + kEpsilon) / bar_length) + 1.0;
        f64 next = changes_[s].beat + bars * bar_length;
        if (s + 1 < changes_.size() && changes_[s + 1].beat < next - kEpsilon) next = changes_[s + 1].beat; // a new signature opens a bar
        return next;
    }

    f64 TempoMap::beat_at_or_after(f64 beat) const {
        const f64 nearest = std::round(beat);
        return std::fabs(beat - nearest) < 1e-5 ? nearest : next_beat(beat);
    }

    f64 TempoMap::bar_at_or_after(f64 beat) const {
        if (changes_.empty()) return beat;
        const f64 within = beat_in_bar(beat);
        return std::fabs(within) < 1e-5 || std::fabs(within - beats_per_bar_at(beat)) < 1e-5 ? beat : next_bar(beat);
    }

    f64 TempoMap::beat_in_bar(f64 beat) const {
        if (changes_.empty()) return 0.0;
        const usize s = segment_for_beat(beat);
        const f64 bar_length = changes_[s].beats_per_bar;
        const f64 within = beat - changes_[s].beat;
        return within - std::floor((within + kEpsilon) / bar_length) * bar_length;
    }

    u32 TempoMap::bar_at(f64 beat) const {
        if (changes_.empty()) return 0;
        u32 bars = 0;
        const usize target = segment_for_beat(beat);
        for (usize s = 0; s < target; ++s) {
            bars += static_cast<u32>(std::ceil((changes_[s + 1].beat - changes_[s].beat) / changes_[s].beats_per_bar - kEpsilon));
        }
        return bars + static_cast<u32>(std::floor((beat - changes_[target].beat + kEpsilon) / changes_[target].beats_per_bar));
    }

    std::vector<MarkerSpec> TempoMap::beat_markers(f64 first_beat, f64 last_beat, u32 every_n, const UString &prefix, u32 first_id) const {
        std::vector<MarkerSpec> out;
        every_n = std::max(every_n, 1u);
        u32 id = first_id;
        for (f64 beat = first_beat; beat <= last_beat + kEpsilon; beat += every_n) {
            MarkerSpec marker;
            marker.id = id++;
            marker.name = UString{std::format("{}{}", prefix, static_cast<long long>(std::llround(beat)))};
            marker.seconds = seconds_at_beat(beat);
            out.push_back(std::move(marker));
        }
        return out;
    }

} // namespace SFT::Audio
