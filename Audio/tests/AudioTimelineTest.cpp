// Waypoints, jumps, loops, seeking, scheduling, fades, ducking, pitch of any source, codecs, cues and analysis.

#include <Audio/Analysis.hpp>
#include <Audio/Cue.hpp>
#include <Audio/Decoder.hpp>
#include <Audio/Mixer.hpp>
#include <Audio/Music.hpp>
#include <Audio/Offline.hpp>
#include <Audio/Patch.hpp>
#include <Audio/Sound.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>

using namespace SFT::Audio;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }
    bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

    constexpr float kHalfPower = 0.70710678f; // mono 2D onto stereo

    // Sample i holds i * 1e-4: the value of a sample tells you which frame of the file it came from.
    std::shared_ptr<SampleBuffer> ramp_buffer(SFT::u32 rate, SFT::u32 frames) {
        auto b = std::make_shared<SampleBuffer>();
        b->channels = 1;
        b->sample_rate = rate;
        auto samples = std::make_shared<std::vector<float>>(frames);
        for (SFT::u32 i = 0; i < frames; ++i) (*samples)[i] = static_cast<float>(i) * 1e-4f;
        b->samples = std::move(samples);
        return b;
    }

    AudioEngineConfig stereo_config() {
        AudioEngineConfig c;
        c.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::stereo()}};
        return c;
    }

    // Renders `blocks` blocks and returns the left channel divided by the 2D mono pan factor, i.e. the source samples.
    std::vector<float> capture(AudioEngine &engine, int blocks) {
        std::vector<float> out;
        for (int b = 0; b < blocks; ++b) {
            engine.render_block();
            for (SFT::u32 i = 0; i < engine.config().block_frames; ++i) out.push_back(engine.bed(0).data(0)[i] / kHalfPower);
        }
        return out;
    }

    PlayParams plain(std::shared_ptr<DataSource> source) {
        PlayParams p;
        p.source = std::move(source);
        p.spatial = false;
        p.declick_seconds = 0.0f;
        return p;
    }
} // namespace

int main() {
    const auto buffer = ramp_buffer(48000, 12000);

    // ---- sample-accurate seamless jump ------------------------------------------------------------------------------------
    {
        AudioEngine engine(stereo_config());
        PlayParams p = plain(std::make_shared<BufferSource>(buffer, 48000, false));
        MarkerSpec jump;
        jump.id = 7;
        jump.seconds = 1000.0 / 48000.0;      // mid-block
        jump.action = MarkerAction::Jump;
        jump.jump_seconds = 5000.0 / 48000.0;
        jump.seamless = true;
        p.markers = {jump};
        engine.play(std::move(p));
        const auto out = capture(engine, 8); // 2048 frames
        check(near(out[999], 0.0999f, 2e-4f), "audio before the marker plays straight through");
        check(near(out[1000], 0.5000f, 2e-4f), "the first sample after the marker comes from the jump target");
        check(near(out[1500], 0.5500f, 2e-4f), "and playback continues from there");
        std::vector<VoiceEvent> events;
        engine.poll_events(events);
        bool hit = false, jumped = false;
        for (const auto &e : events) {
            hit = hit || (e.kind == VoiceEvent::Kind::MarkerHit && e.marker_id == 7);
            jumped = jumped || (e.kind == VoiceEvent::Kind::Jumped && e.marker_id == 7);
        }
        check(hit && jumped, "the game thread is told about the marker and the jump");
    }

    // ---- loop region -----------------------------------------------------------------------------------------------------------
    {
        AudioEngine engine(stereo_config());
        PlayParams p = plain(std::make_shared<BufferSource>(buffer, 48000, false));
        p.loop = LoopSpec{960.0 / 48000.0, 1920.0 / 48000.0, -1};
        engine.play(std::move(p));
        const auto out = capture(engine, 16); // 4096 frames: more than two passes of the region
        check(near(out[1919], 0.1919f, 2e-4f), "the loop plays up to its end");
        check(near(out[1920], 0.0960f, 2e-4f), "then wraps to its start with no gap");
        check(near(out[2880], 0.0960f, 2e-4f), "and wraps again");
        check(engine.stats().voices == 1, "a looping voice keeps playing");

        std::vector<VoiceEvent> events;
        engine.poll_events(events);
        int loops = 0;
        for (const auto &e : events) loops += e.kind == VoiceEvent::Kind::Looped ? 1 : 0;
        check(loops >= 2, "each wrap is reported");
    }

    // ---- a loop that runs out lets the sound continue ---------------------------------------------------------------------------------
    {
        AudioEngine engine(stereo_config());
        PlayParams p = plain(std::make_shared<BufferSource>(buffer, 48000, false));
        p.loop = LoopSpec{0.0, 480.0 / 48000.0, 2};
        engine.play(std::move(p));
        const auto out = capture(engine, 8);
        check(near(out[480], 0.0f, 2e-4f) && near(out[960], 0.0f, 2e-4f), "two loops wrap back to the start");
        check(out[1440] > 0.04f, "after the count is used the audio carries on past the end");
    }

    // ---- seek and declick --------------------------------------------------------------------------------------------------------------
    {
        AudioEngine engine(stereo_config());
        PlayParams p = plain(std::make_shared<BufferSource>(buffer, 48000, false));
        p.declick_seconds = 0.003f;
        const VoiceId id = engine.play(std::move(p));
        capture(engine, 2);
        engine.seek(id, 6000.0 / 48000.0);
        const auto out = capture(engine, 2);
        check(near(out[0], 0.0f, 0.05f), "the first sample after a seek fades in from silence");
        check(near(out[300], (6000.0f + 300.0f) * 1e-4f, 2e-3f), "and then plays from the target");
        const auto status = engine.status(id);
        check(status && status->position_seconds() > 0.12, "the status reports the new position");
    }

    // ---- delayed and scheduled starts ----------------------------------------------------------------------------------------------------
    {
        AudioEngine engine(stereo_config());
        PlayParams p = plain(std::make_shared<BufferSource>(buffer, 48000, false));
        p.start_delay_seconds = 480.0f / 48000.0f;
        engine.play(std::move(p));
        const auto out = capture(engine, 4);
        check(near(out[479], 0.0f, 1e-6f) && near(out[480], 0.0f, 1e-4f) && near(out[481], 1e-4f, 1e-4f), "a delayed voice is silent until its start frame");

        AudioEngine scheduled(stereo_config());
        scheduled.render_block();
        scheduled.render_block(); // clock is now 512 frames
        PlayParams q = plain(std::make_shared<BufferSource>(buffer, 48000, false));
        q.start_at_seconds = 1024.0 / 48000.0;
        scheduled.play(std::move(q));
        const auto later = capture(scheduled, 4);
        // The command applies at the next block (clock 512); the start is 512 frames after that.
        check(near(later[511], 0.0f, 1e-6f) && later[600] > 0.0f, "start_at_seconds lines up with the engine clock");
    }

    // ---- fades ------------------------------------------------------------------------------------------------------------------------------
    {
        AudioEngine engine(stereo_config());
        PlayParams p = plain(std::make_shared<CallbackSource>(1, 48000, [](AudioBuffer &out, SFT::u32 frames) {
            for (SFT::u32 i = 0; i < frames; ++i) out.data(0)[i] = 0.5f;
            return frames;
        }));
        const VoiceId id = engine.play(std::move(p));
        capture(engine, 2);
        engine.fade_volume(id, 0.0f, 0.1f);
        const auto out = capture(engine, 40); // ~213 ms
        check(near(out[0], 0.5f, 0.1f) && std::fabs(out.back()) < 0.01f, "fade_volume ramps to the target and stays there");
        engine.fade_volume(id, 1.0f, 0.0f);
        const auto back = capture(engine, 2);
        check(near(back[300], 0.5f, 0.02f), "a zero-length fade is immediate");
    }

    // ---- any source can be pitched ---------------------------------------------------------------------------------------------------------------
    {
        AudioEngine engine(stereo_config());
        auto counter = std::make_shared<SFT::u64>(0);
        PlayParams p = plain(std::make_shared<CallbackSource>(1, 48000, [counter](AudioBuffer &out, SFT::u32 frames) {
            for (SFT::u32 i = 0; i < frames; ++i) out.data(0)[i] = static_cast<float>((*counter)++) * 1e-5f;
            return frames;
        }));
        p.pitch = 2.0f;
        engine.play(std::move(p));
        const auto out = capture(engine, 6);
        const float slope = out[600] - out[500];
        check(near(slope, 100.0f * 2e-5f, 2e-4f), "a callback source (no native rate support) plays an octave up through the rate converter");
    }

    // ---- ducking -------------------------------------------------------------------------------------------------------------------------------------
    {
        AudioEngine engine(stereo_config());
        const BusId music = engine.add_bus(0, "music");
        const BusId dialogue = engine.add_bus(0, "dialogue");
        engine.add_ducking(music, dialogue, 12.0f, -50.0f, 0.01f, 0.2f);
        const auto constant = [](float level) {
            return std::make_shared<CallbackSource>(1, 48000, [level](AudioBuffer &out, SFT::u32 frames) {
                for (SFT::u32 i = 0; i < frames; ++i) out.data(0)[i] = level;
                return frames;
            });
        };
        PlayParams m = plain(constant(0.4f));
        m.bus = music;
        engine.play(std::move(m));
        const auto before = capture(engine, 8);
        PlayParams d = plain(constant(0.3f));
        d.bus = dialogue;
        const VoiceId voice = engine.play(std::move(d));
        const auto during = capture(engine, 60);
        check(std::fabs(during.back()) > 0.0f, "audio keeps flowing");
        // music alone is 0.4 on each side; with dialogue the music part must be well below that.
        const float music_level = engine.bus_levels(music).rms;
        check(music_level < 0.4f * kHalfPower * 0.5f, "dialogue ducks the music bus");
        engine.stop(voice);
        capture(engine, 120);
        check(engine.bus_levels(music).rms > 0.4f * kHalfPower * 0.85f, "the music recovers when dialogue stops");
        (void)before;
    }

    // ---- SoundManager: named waypoints and callbacks -----------------------------------------------------------------------------------------------------
    {
        AudioEngine engine(stereo_config());
        SoundManager sounds(engine, true);
        PlayParams p;
        p.source = std::make_shared<BufferSource>(buffer, 48000, false);
        p.spatial = false;
        MarkerSpec cue;
        cue.name = "drop";
        cue.seconds = 0.02;
        p.markers = {cue};
        int markers = 0, finished = 0, named = 0;
        SoundCallbacks cb;
        cb.on_marker = [&](const SoundHandle &, const MarkerHit &hit) { markers += hit.name == UString{"drop"} ? 1 : 0; };
        cb.on_finished = [&](const SoundHandle &) { ++finished; };
        SoundHandle handle = sounds.play(std::move(p), cb);
        check(handle.valid() && handle.has_marker("drop") && near(static_cast<float>(handle.marker_seconds("drop")), 0.02f, 1e-6f), "markers keep their names");
        handle.on_marker("drop", [&](const MarkerHit &) { ++named; });
        const u32 late = handle.add_marker("late", 0.1, MarkerAction::None);
        check(late != 0 && handle.has_marker("late"), "waypoints can be added while playing");
        for (int i = 0; i < 12; ++i) {
            engine.render_block();
            sounds.update();
        }
        check(markers == 1 && named == 1, "callbacks run when the marker is reached (general and by name)");
        for (int i = 0; i < 60; ++i) {
            engine.render_block();
            sounds.update();
        }
        check(finished == 1 && handle.finished(), "on_finished runs once the sound ends");

        // jump_at_marker: reaching "drop" skips ahead.
        PlayParams q;
        q.source = std::make_shared<BufferSource>(buffer, 48000, false);
        q.spatial = false;
        q.declick_seconds = 0.0f;
        MarkerSpec drop;
        drop.name = "drop";
        drop.seconds = 0.01;
        q.markers = {drop};
        SoundHandle jumper = sounds.play(std::move(q));
        check(jumper.jump_at_marker("drop", 0.15), "an existing marker can be turned into a jump");
        const auto out = capture(engine, 6);
        const SFT::usize at = 480;
        check(near(out[at + 10], (7200.0f + 10.0f) * 1e-4f, 3e-3f), "playback jumped at the marker");
        check(jumper.seek_to_marker("drop"), "seek_to_marker finds named markers");
        check(!jumper.seek_to_marker("nonexistent"), "and rejects unknown ones");
    }

    // ---- codecs ------------------------------------------------------------------------------------------------------------------------------------------------
    {
        // WAV round trip keeps cue points.
        SampleBuffer source;
        source.channels = 2;
        source.sample_rate = 22050;
        auto samples = std::make_shared<std::vector<float>>(2 * 1000);
        for (SFT::usize i = 0; i < samples->size(); ++i) (*samples)[i] = std::sin(static_cast<float>(i) * 0.01f) * 0.5f;
        source.samples = samples;
        source.markers = {{"intro", 100}, {"verse", 500}};
        const auto path = std::filesystem::temp_directory_path() / "sturdy_audio_roundtrip.wav";
        check(write_wav(path, source, false).has_value(), "WAV written");
        auto loaded = load_sound_file(path);
        check(loaded.has_value(), "WAV decoded through the registry");
        if (loaded) {
            check((*loaded)->channels == 2 && (*loaded)->sample_rate == 22050 && (*loaded)->frames() == 1000, "format preserved");
            check((*loaded)->markers.size() == 2 && (*loaded)->markers[0].name == UString{"intro"} && (*loaded)->markers[1].frame == 500, "cue points survive a WAV round trip");
            check(near((*(*loaded)->samples)[400], (*samples)[400], 1e-3f), "samples survive 16-bit quantisation");
        }
        std::filesystem::remove(path);

        // A tiny 16-bit AIFF built by hand (big-endian, 80-bit extended sample rate 44100).
        std::vector<std::byte> aiff;
        const auto put = [&](const void *data, SFT::usize n) {
            const auto *p = static_cast<const unsigned char *>(data);
            for (SFT::usize i = 0; i < n; ++i) aiff.push_back(static_cast<std::byte>(p[i]));
        };
        const auto be32 = [&](SFT::u32 v) { const unsigned char b[4] = {static_cast<unsigned char>(v >> 24), static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v)}; put(b, 4); };
        const auto be16 = [&](SFT::u16 v) { const unsigned char b[2] = {static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v)}; put(b, 2); };
        put("FORM", 4); be32(4 + 8 + 18 + 8 + 8 + 8); put("AIFF", 4);
        put("COMM", 4); be32(18); be16(1); be32(4); be16(16);
        const unsigned char rate[10] = {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0}; // 44100
        put(rate, 10);
        put("SSND", 4); be32(8 + 8); be32(0); be32(0);
        const std::int16_t pcm[4] = {0, 16384, -16384, 32767};
        for (std::int16_t s : pcm) be16(static_cast<SFT::u16>(s));
        auto decoded = load_sound_memory(SFT::Foundation::Io::ByteBlob::from_vector(aiff), ".aiff");
        check(decoded.has_value(), "AIFF decodes");
        if (decoded) {
            check((*decoded)->sample_rate == 44100 && (*decoded)->channels == 1 && (*decoded)->frames() == 4, "AIFF format");
            check(near((*(*decoded)->samples)[1], 0.5f, 1e-3f) && near((*(*decoded)->samples)[2], -0.5f, 1e-3f), "AIFF big-endian PCM");
        }
        check(!load_sound_memory(SFT::Foundation::Io::ByteBlob::from_vector(std::vector<std::byte>(64, std::byte{0x42}))).has_value(), "garbage is rejected");
        check(DecoderRegistry::global().supported_extensions().size() >= 5, "several extensions are registered");
    }

    // ---- cues ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        AudioEngine engine(stereo_config());
        SoundManager sounds(engine, true);
        CuePlayer player(sounds, 42);
        SoundCue steps;
        for (int i = 0; i < 3; ++i) steps.variations.push_back(CueVariation{ramp_buffer(48000, 4800), 1.0f, 1.0f});
        steps.selection = CueSelection::RoundRobin;
        steps.base.spatial = false;
        steps.max_instances = 2;
        steps.limit = CueLimit::RejectNew;
        const SoundHandle a = player.play(steps, 0.0);
        const SoundHandle b = player.play(steps, 0.1);
        const SoundHandle c = player.play(steps, 0.2);
        check(a.valid() && b.valid() && !c.valid(), "a cue at its instance limit rejects new plays");
        check(player.active_instances(steps) == 2, "two instances are tracked");

        SoundCue cooled = steps;
        cooled.max_instances = 0;
        cooled.cooldown_seconds = 0.5f;
        check(player.play(cooled, 10.0).valid() && !player.play(cooled, 10.2).valid() && player.play(cooled, 10.7).valid(), "the cooldown spaces out starts");

        SoundCue layered = steps;
        layered.max_instances = 0;
        layered.selection = CueSelection::Layered;
        player.play(layered, 20.0);
        check(player.active_instances(layered) == 3, "a layered cue starts every variation");
    }

    // ---- analysis -----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        AudioEngine engine(stereo_config());
        PatchBuilder b;
        auto patch = b.build(b.scale(b.oscillator(Waveform::Sine, b.constant(1000.0f)), 0.5f));
        PlayParams p;
        p.source = std::make_shared<PatchSource>(patch, 48000);
        p.spatial = false;
        engine.play(std::move(p));
        for (int i = 0; i < 24; ++i) engine.render_block();
        SpectrumAnalyzer analyzer(2048);
        analyzer.analyze(engine);
        SFT::u32 peak_bin = 0;
        for (SFT::u32 k = 1; k < analyzer.magnitudes().size(); ++k) {
            if (analyzer.magnitudes()[k] > analyzer.magnitudes()[peak_bin]) peak_bin = k;
        }
        const float peak_hz = SpectrumAnalyzer::bin_frequency(peak_bin, analyzer.fft_size(), 48000);
        check(std::fabs(peak_hz - 1000.0f) < 50.0f, "the spectrum peaks at the tone's frequency");
        check(analyzer.centroid(48000) > 800.0f && analyzer.centroid(48000) < 1300.0f, "the centroid sits near a pure tone");
        const auto bands = analyzer.bands(8, 48000);
        check(bands.size() == 8, "log-spaced bands");
        check(engine.bus_levels(engine.master_bus(0)).peak > 0.1f, "bus meters report level");
    }

    // ---- offline rendering ----------------------------------------------------------------------------------------------------------------------------------------------------
    {
        AudioEngine engine(stereo_config());
        bool started = false;
        const auto rendered = render_offline(engine, 0.25, 0, [&](double clock) {
            if (!started && clock >= 0.1) {
                started = true;
                PlayParams p = plain(std::make_shared<BufferSource>(ramp_buffer(48000, 9600), 48000, false));
                engine.play(std::move(p));
            }
        });
        check(rendered && rendered->frames() == 12000 && rendered->channels == 2, "offline render has the requested length");
        const auto &s = *rendered->samples;
        check(std::fabs(s[2 * 4000]) < 1e-6f && std::fabs(s[2 * 6000]) > 0.01f, "events scripted by clock time land where asked");
    }

    return failures == 0 ? 0 : 1;
}
