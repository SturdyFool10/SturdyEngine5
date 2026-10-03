#include <Audio/Sound.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Audio {

    // ---- SoundHandle ----------------------------------------------------------------------------------------------

    bool SoundHandle::alive() const noexcept { return status_ && !status_->finished(); }
    bool SoundHandle::playing() const noexcept { return status_ && status_->playing(); }
    bool SoundHandle::paused() const noexcept { return status_ && status_->state() == VoiceStatus::State::Paused; }
    bool SoundHandle::finished() const noexcept { return !status_ || status_->finished(); }
    f64 SoundHandle::position_seconds() const noexcept { return status_ ? status_->position_seconds() : 0.0; }
    f64 SoundHandle::duration_seconds() const noexcept { return status_ ? status_->duration_seconds() : 0.0; }
    f32 SoundHandle::volume() const noexcept { return status_ ? status_->volume() : 0.0f; }
    f32 SoundHandle::pitch() const noexcept { return status_ ? status_->pitch() : 1.0f; }

    void SoundHandle::set_volume(f32 volume) const {
        if (valid()) manager_->engine().set_volume(id_, volume);
    }
    void SoundHandle::set_pitch(f32 pitch) const {
        if (valid()) manager_->engine().set_pitch(id_, pitch);
    }
    void SoundHandle::set_pitch_semitones(f32 semitones) const { set_pitch(std::exp2(semitones / 12.0f)); }
    void SoundHandle::fade_volume(f32 target, f32 seconds) const {
        if (valid()) manager_->engine().fade_volume(id_, target, seconds);
    }
    void SoundHandle::fade_pitch(f32 target, f32 seconds) const {
        if (valid()) manager_->engine().fade_pitch(id_, target, seconds);
    }
    void SoundHandle::stop(f32 fade_seconds) const {
        if (valid()) manager_->engine().stop(id_, fade_seconds);
    }
    void SoundHandle::pause() const {
        if (valid()) manager_->engine().set_paused(id_, true);
    }
    void SoundHandle::resume() const {
        if (valid()) manager_->engine().set_paused(id_, false);
    }
    void SoundHandle::set_position(const glm::vec3 &position, const glm::vec3 &velocity) const {
        if (valid()) manager_->engine().set_position(id_, position, velocity);
    }
    void SoundHandle::seek(f64 seconds) const {
        if (valid()) manager_->engine().seek(id_, seconds);
    }

    bool SoundHandle::seek_to_marker(const ustr &name) const {
        if (!valid()) return false;
        const auto *entry = manager_->find(id_);
        if (entry == nullptr) return false;
        const auto it = entry->seconds.find(name.to_owned());
        if (it == entry->seconds.end()) return false;
        manager_->engine().seek(id_, it->second);
        return true;
    }

    void SoundHandle::set_loop(f64 start_seconds, f64 end_seconds, i32 count) const {
        if (valid()) manager_->engine().set_loop(id_, LoopSpec{start_seconds, end_seconds, count});
    }
    void SoundHandle::clear_loop() const {
        if (valid()) manager_->engine().set_loop(id_, std::nullopt);
    }

    u32 SoundHandle::add_marker(const UString &name, f64 seconds, MarkerAction action, f64 jump_seconds, bool seamless, i32 max_triggers) const {
        if (!valid()) return 0;
        auto *entry = manager_->find(id_);
        if (entry == nullptr) return 0;
        // Re-using a name replaces the earlier marker.
        if (const auto old = entry->ids.find(name); old != entry->ids.end()) {
            manager_->engine().remove_marker(id_, old->second);
        }
        const u32 id = entry->next_id++;
        manager_->register_marker(*entry, name, id, seconds);
        MarkerSpec spec;
        spec.id = id;
        spec.name = name;
        spec.seconds = seconds;
        spec.action = action;
        spec.jump_seconds = jump_seconds;
        spec.seamless = seamless;
        spec.max_triggers = max_triggers;
        manager_->engine().add_marker(id_, spec);
        return id;
    }

    bool SoundHandle::jump_at_marker(const ustr &marker, f64 target_seconds, i32 max_triggers) const {
        if (!has_marker(marker)) return false;
        add_marker(marker.to_owned(), marker_seconds(marker), MarkerAction::Jump, target_seconds, false, max_triggers);
        return true;
    }

    void SoundHandle::stop_at_marker(const ustr &marker) const {
        if (has_marker(marker)) {
            add_marker(marker.to_owned(), marker_seconds(marker), MarkerAction::Stop);
        }
    }

    void SoundHandle::remove_marker(const ustr &name) const {
        if (!valid()) return;
        auto *entry = manager_->find(id_);
        if (entry == nullptr) return;
        const auto it = entry->ids.find(name.to_owned());
        if (it == entry->ids.end()) return;
        manager_->engine().remove_marker(id_, it->second);
        entry->names.erase(it->second);
        entry->seconds.erase(it->first);
        entry->ids.erase(it);
    }

    void SoundHandle::on_marker(const UString &marker, std::function<void(const MarkerHit &)> callback) const {
        if (!valid()) return;
        if (auto *entry = manager_->find(id_)) {
            entry->named_callbacks[marker].push_back(std::move(callback));
        }
    }

    bool SoundHandle::has_marker(const ustr &name) const {
        if (!valid()) return false;
        const auto *entry = manager_->find(id_);
        return entry != nullptr && entry->ids.contains(name.to_owned());
    }

    f64 SoundHandle::marker_seconds(const ustr &name) const {
        if (!valid()) return 0.0;
        const auto *entry = manager_->find(id_);
        if (entry == nullptr) return 0.0;
        const auto it = entry->seconds.find(name.to_owned());
        return it == entry->seconds.end() ? 0.0 : it->second;
    }

    // ---- SoundManager ----------------------------------------------------------------------------------------------

    SoundManager::SoundManager(AudioEngine &engine, bool pump_engine) : engine_(engine), pump_engine_(pump_engine) {}

    SoundManager::Entry *SoundManager::find(VoiceId id) {
        const auto it = entries_.find(id);
        return it == entries_.end() ? nullptr : &it->second;
    }
    const SoundManager::Entry *SoundManager::find(VoiceId id) const {
        const auto it = entries_.find(id);
        return it == entries_.end() ? nullptr : &it->second;
    }

    void SoundManager::register_marker(Entry &entry, const UString &name, u32 id, f64 seconds) {
        entry.ids[name] = id;
        entry.names[id] = name;
        entry.seconds[name] = seconds;
    }

    SoundHandle SoundManager::play(PlayParams params, SoundCallbacks callbacks) {
        // Give every marker a unique, non-zero id and remember the names.
        std::vector<u32> used;
        for (const MarkerSpec &m : params.markers) {
            if (m.id != 0) used.push_back(m.id);
        }
        u32 next = 1;
        const auto fresh = [&] {
            while (std::find(used.begin(), used.end(), next) != used.end()) ++next;
            used.push_back(next);
            return next;
        };
        for (MarkerSpec &m : params.markers) {
            if (m.id == 0) m.id = fresh();
        }
        const std::vector<MarkerSpec> specs = params.markers;

        const VoiceId id = engine_.play(std::move(params));
        if (id == 0) {
            return {};
        }
        Entry entry;
        entry.status = engine_.status(id);
        entry.callbacks = std::move(callbacks);
        u32 highest = 1000;
        for (const MarkerSpec &m : specs) {
            if (!m.name.empty()) {
                register_marker(entry, m.name, m.id, m.seconds);
            } else {
                entry.names[m.id] = {};
            }
            highest = std::max(highest, m.id);
        }
        entry.next_id = highest + 1;
        std::shared_ptr<const VoiceStatus> status = entry.status;
        entries_.emplace(id, std::move(entry));
        return SoundHandle(this, id, std::move(status));
    }

    std::expected<std::shared_ptr<const SampleBuffer>, UString> SoundManager::load(const std::filesystem::path &path) {
        const UString key = text_from_bytes(path.string());
        if (const auto it = cache_.find(key); it != cache_.end()) {
            return it->second;
        }
        auto buffer = load_sound_file(path);
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        std::shared_ptr<const SampleBuffer> shared = *buffer;
        cache_.emplace(key, shared);
        return shared;
    }

    SoundHandle SoundManager::play_buffer(std::shared_ptr<const SampleBuffer> buffer, PlayParams params, SoundCallbacks callbacks, bool loop, bool use_file_markers) {
        if (!buffer) {
            return {};
        }
        if (use_file_markers) {
            apply_file_metadata(params, *buffer, loop);
        }
        params.source = std::make_shared<BufferSource>(buffer, engine_.config().sample_rate, loop && !params.loop);
        return play(std::move(params), std::move(callbacks));
    }

    std::expected<SoundHandle, UString> SoundManager::play_file(const std::filesystem::path &path, PlayParams params, SoundCallbacks callbacks, bool use_file_markers) {
        auto buffer = load(path);
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        return play_buffer(*buffer, std::move(params), std::move(callbacks), false, use_file_markers);
    }

    std::expected<SoundHandle, UString> SoundManager::stream_file(const std::filesystem::path &path, PlayParams params, const StreamingOptions &options,
                                                                      SoundCallbacks callbacks, bool use_file_markers) {
        auto source = StreamingSource::open(path, engine_.config().sample_rate, options);
        if (!source) {
            return std::unexpected(source.error());
        }
        if (use_file_markers) {
            const AudioStreamInfo &info = (*source)->info();
            const f64 rate = static_cast<f64>(std::max(info.sample_rate, 1u));
            u32 id = 1;
            for (const AudioMarker &m : info.markers) {
                MarkerSpec spec;
                spec.id = id++;
                spec.name = m.name;
                spec.seconds = static_cast<f64>(m.frame) / rate;
                params.markers.push_back(std::move(spec));
            }
            // A streamed file's embedded loop is handled by the stream itself when `options.loop` is set.
        }
        params.source = *source;
        return play(std::move(params), std::move(callbacks));
    }

    void SoundManager::update() {
        if (pump_engine_) {
            (void)engine_.pump();
        }
        event_scratch_.clear();
        engine_.poll_events(event_scratch_);
        for (const VoiceEvent &event : event_scratch_) {
            auto it = entries_.find(event.voice);
            if (it == entries_.end()) {
                continue;
            }
            Entry &entry = it->second;
            const SoundHandle handle(this, event.voice, entry.status);
            // Callbacks may add or remove entries (start other sounds), so take copies of what they need first.
            switch (event.kind) {
                case VoiceEvent::Kind::Started:
                    if (auto cb = entry.callbacks.on_started) cb(handle);
                    break;
                case VoiceEvent::Kind::MarkerHit: {
                    MarkerHit hit;
                    hit.voice = event.voice;
                    hit.id = event.marker_id;
                    hit.seconds = event.position_seconds;
                    if (const auto n = entry.names.find(event.marker_id); n != entry.names.end()) {
                        hit.name = n->second;
                    }
                    std::vector<std::function<void(const MarkerHit &)>> named;
                    if (!hit.name.empty()) {
                        if (const auto c = entry.named_callbacks.find(hit.name); c != entry.named_callbacks.end()) {
                            named = c->second;
                        }
                    }
                    auto any = entry.callbacks.on_marker;
                    if (any) any(handle, hit);
                    for (auto &cb : named) cb(hit);
                    break;
                }
                case VoiceEvent::Kind::Jumped:
                    if (auto cb = entry.callbacks.on_jump) cb(handle, event.position_seconds);
                    break;
                case VoiceEvent::Kind::Looped:
                    if (auto cb = entry.callbacks.on_loop) cb(handle, event.position_seconds);
                    break;
                case VoiceEvent::Kind::Paused:
                    if (auto cb = entry.callbacks.on_paused) cb(handle);
                    break;
                case VoiceEvent::Kind::Finished: {
                    auto cb = entry.callbacks.on_finished;
                    if (cb) cb(handle);
                    entries_.erase(event.voice);
                    break;
                }
            }
        }
    }

} // namespace SFT::Audio
