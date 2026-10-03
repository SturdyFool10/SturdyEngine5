#include <Audio/Music.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Audio {

    MusicTrack stream_track(const std::filesystem::path &path, u32 engine_rate, bool loop, f64 bpm) {
        MusicTrack track;
        track.name = UString{path.stem().string()};
        track.bpm = bpm;
        track.open = [path, engine_rate, loop]() -> std::expected<std::shared_ptr<DataSource>, UString> {
            StreamingOptions options;
            options.loop = loop;
            auto source = StreamingSource::open(path, engine_rate, options);
            if (!source) {
                return std::unexpected(source.error());
            }
            return std::shared_ptr<DataSource>(*source);
        };
        return track;
    }

    MusicTrack buffer_track(std::shared_ptr<const SampleBuffer> buffer, u32 engine_rate, bool loop, f64 bpm) {
        MusicTrack track;
        track.bpm = bpm;
        track.open = [buffer, engine_rate, loop]() -> std::expected<std::shared_ptr<DataSource>, UString> {
            if (!buffer) {
                return std::unexpected("music: no buffer");
            }
            return std::shared_ptr<DataSource>(std::make_shared<BufferSource>(buffer, engine_rate, loop));
        };
        return track;
    }

    bool MusicPlayer::start(const MusicTrack &track, f32 delay_seconds, f64 start_at_clock, f32 fade_in_seconds, SoundHandle &out) {
        if (!track.open) {
            return false;
        }
        auto source = track.open();
        if (!source) {
            return false;
        }
        PlayParams params;
        params.source = *source;
        params.bus = bus_;
        params.spatial = false;
        params.volume = track.volume * volume_;
        params.markers = track.markers;
        params.loop = track.loop;
        params.start_delay_seconds = delay_seconds;
        params.start_at_seconds = start_at_clock;
        params.fade_in_seconds = fade_in_seconds;
        out = sounds_.play(std::move(params));
        return out.valid();
    }

    f64 MusicPlayer::beat_position() const {
        if (!current_.alive()) {
            return 0.0;
        }
        const TempoMap grid = current_track_.grid();
        return grid.empty() ? 0.0 : grid.beat_at_seconds(current_.position_seconds());
    }

    f64 MusicPlayer::seconds_to_next(Quantize quantize) const {
        if (quantize == Quantize::Immediate || !current_.alive()) {
            return 0.0;
        }
        const TempoMap grid = current_track_.grid();
        if (grid.empty()) {
            return 0.0;
        }
        const f64 position = current_.position_seconds();
        const f64 beat = grid.beat_at_seconds(position);
        const f64 target = quantize == Quantize::NextBar ? grid.bar_at_or_after(beat) : grid.beat_at_or_after(beat);
        return std::max(0.0, grid.seconds_at_beat(target) - position);
    }

    bool MusicPlayer::play(const MusicTrack &track, const MusicTransition &transition) {
        queued_.reset();
        scheduled_ = SoundHandle{};
        scheduled_track_.reset();

        const bool had_music = current_.alive();
        const f32 delay = had_music ? static_cast<f32>(seconds_to_next(transition.quantize)) : 0.0f;
        const f32 fade = had_music ? std::max(transition.crossfade_seconds, 0.0f) : 0.0f;

        SoundHandle next;
        if (!start(track, delay, -1.0, fade, next)) {
            return false;
        }
        if (had_music) {
            if (delay > 0.0f) {
                // The old track starts fading exactly when the new one starts sounding.
                current_.add_marker("music.fade", current_.position_seconds() + static_cast<f64>(delay), MarkerAction::FadeStop, static_cast<f64>(fade));
            } else {
                current_.stop(fade);
            }
        }
        current_ = next;
        current_track_ = track;
        current_name_ = track.name;
        return true;
    }

    void MusicPlayer::queue(const MusicTrack &track, f32 crossfade_seconds) {
        queued_ = track;
        queued_crossfade_ = std::max(crossfade_seconds, 0.0f);
        if (!current_.alive()) {
            const MusicTrack copy = track;
            queued_.reset();
            play(copy, MusicTransition{0.0f, Quantize::Immediate});
        }
    }

    void MusicPlayer::stop(f32 fade_seconds) {
        queued_.reset();
        if (scheduled_.valid()) {
            scheduled_.stop(0.0f);
            scheduled_ = SoundHandle{};
            scheduled_track_.reset();
        }
        if (current_.valid()) {
            current_.stop(fade_seconds);
        }
    }

    void MusicPlayer::pause() {
        current_.pause();
    }
    void MusicPlayer::resume() {
        current_.resume();
    }

    void MusicPlayer::set_volume(f32 volume, f32 fade_seconds) {
        volume_ = std::max(volume, 0.0f);
        const f32 target = current_track_.volume * volume_;
        if (fade_seconds > 0.0f) {
            current_.fade_volume(target, fade_seconds);
        } else {
            current_.set_volume(target);
        }
    }

    SoundHandle MusicPlayer::play_stinger(std::shared_ptr<const SampleBuffer> stinger, Quantize quantize, f32 volume) {
        PlayParams params;
        params.bus = bus_;
        params.spatial = false;
        params.volume = volume;
        params.start_delay_seconds = static_cast<f32>(seconds_to_next(quantize));
        return sounds_.play_buffer(std::move(stinger), std::move(params), {}, false, false);
    }

    void MusicPlayer::update() {
        // The scheduled successor takes over once the old track is gone.
        if (scheduled_.valid() && scheduled_track_ && !current_.alive()) {
            current_ = scheduled_;
            current_track_ = *scheduled_track_;
            current_name_ = current_track_.name;
            scheduled_ = SoundHandle{};
            scheduled_track_.reset();
        }
        if (!queued_ || !current_.alive() || scheduled_.valid()) {
            return;
        }
        const f64 duration = current_.duration_seconds();
        if (duration <= 0.0) {
            return; // endless or unknown length: nothing to wait for
        }
        const f64 remaining = duration - current_.position_seconds();
        // Schedule slightly ahead of the join so the mixer sees the start time before it arrives.
        constexpr f64 lead = 0.5;
        if (remaining > lead + static_cast<f64>(queued_crossfade_)) {
            return;
        }
        const f64 join = sounds_.engine().clock_seconds() + std::max(remaining - static_cast<f64>(queued_crossfade_), 0.0);
        SoundHandle next;
        if (!start(*queued_, 0.0f, join, queued_crossfade_, next)) {
            queued_.reset();
            return;
        }
        if (queued_crossfade_ > 0.0f) {
            current_.add_marker("music.fade", std::max(duration - static_cast<f64>(queued_crossfade_), current_.position_seconds()), MarkerAction::FadeStop,
                                static_cast<f64>(queued_crossfade_));
        }
        scheduled_ = next;
        scheduled_track_ = std::move(queued_);
        queued_.reset();
    }

} // namespace SFT::Audio
