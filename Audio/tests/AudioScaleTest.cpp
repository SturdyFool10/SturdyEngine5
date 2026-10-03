#include <Audio/Effects.hpp>
#include <Audio/Encode.hpp>
#include <Audio/MediaAudio.hpp>
#include <Audio/Mixer.hpp>
#include <Audio/Stream.hpp>
#include <Audio/Source.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>

using namespace SFT::Audio;
using SFT::u32;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }

    AudioEngineConfig config(u32 mix_threads, u32 max_voices = 8192, u32 physical = 4096) {
        AudioEngineConfig c;
        c.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::surround_5_1()}};
        c.max_voices = max_voices;
        c.max_physical_voices = physical;
        c.mix_threads = mix_threads;
        c.parallel_voice_threshold = 32;
        return c;
    }

    std::shared_ptr<const SampleBuffer> tone(u32 channels, double frequency, double seconds, float amplitude, u32 rate = 48000) {
        auto b = std::make_shared<SampleBuffer>();
        b->channels = channels;
        b->sample_rate = rate;
        const size_t frames = static_cast<size_t>(rate * seconds);
        auto samples = std::make_shared<std::vector<float>>(frames * channels);
        for (size_t i = 0; i < frames; ++i) {
            for (u32 c = 0; c < channels; ++c) (*samples)[i * channels + c] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / rate));
        }
        b->samples = std::move(samples);
        return b;
    }

    // Renders `blocks` blocks of an engine playing `count` voices and returns the checksum of channel 0 plus the elapsed time.
    struct Run {
        double sum = 0.0;
        double milliseconds = 0.0;
        AudioStats stats{};
    };
    Run run_many(u32 mix_threads, u32 count, bool spatial) {
        AudioEngine engine(config(mix_threads));
        auto sound = tone(1, 440.0, 1.0, 0.01f);
        for (u32 i = 0; i < count; ++i) {
            PlayParams p;
            p.source = std::make_shared<BufferSource>(sound, 48000, true);
            p.spatial = spatial;
            p.position = {static_cast<float>(i % 17) - 8.0f, 0.0f, -2.0f - static_cast<float>(i % 5)};
            p.volume = 0.5f;
            engine.play(std::move(p));
        }
        Run run;
        const auto start = std::chrono::steady_clock::now();
        for (int b = 0; b < 40; ++b) {
            engine.render_block();
            for (u32 i = 0; i < engine.config().block_frames; ++i) run.sum += static_cast<double>(engine.bed(0).data(0)[i]) * (1.0 + 0.001 * i);
        }
        run.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        run.stats = engine.stats();
        return run;
    }
} // namespace

int main() {
    // ---- thousands of voices, with and without helper threads ---------------------------------------------------------------
    for (bool spatial : {false, true}) {
        const Run serial = run_many(0, 3000, spatial);
        const Run parallel = run_many(3, 3000, spatial);
        std::cout << (spatial ? "spatial" : "2D") << " 3000 voices: " << serial.milliseconds / 40.0 << " ms/block serial, " << parallel.milliseconds / 40.0
                  << " ms/block with " << parallel.stats.mix_threads << " mixers (" << serial.stats.simd << ")\n";
        check(serial.stats.voices == 3000 && serial.stats.physical == 3000, "all 3000 voices are mixed, none grouped or dropped");
        check(serial.stats.dropped_commands == 0, "no command was dropped");
        check(std::fabs(serial.sum - parallel.sum) <= 1e-3 * std::max(1.0, std::fabs(serial.sum)), "parallel mixing gives the serial result");
        check(parallel.stats.mix_threads >= 1, "helper threads were created");
    }

    // ---- voice slots are reused safely ---------------------------------------------------------------------------------------
    {
        AudioEngine engine(config(0, 16, 16));
        auto blip = tone(1, 1000.0, 0.01, 0.2f);
        std::vector<VoiceId> first;
        for (int i = 0; i < 16; ++i) {
            PlayParams p;
            p.source = std::make_shared<BufferSource>(blip, 48000, false);
            p.spatial = false;
            first.push_back(engine.play(std::move(p)));
        }
        check(first.back() != 0, "all 16 slots hand out voices");
        PlayParams extra;
        extra.source = std::make_shared<BufferSource>(blip, 48000, false);
        check(engine.play(std::move(extra)) == 0, "a full table refuses a 17th voice instead of corrupting");
        for (int b = 0; b < 8; ++b) engine.render_block();
        const auto done = engine.pump();
        check(done.size() == 16, "finished voices are reported");
        PlayParams again;
        again.source = std::make_shared<BufferSource>(blip, 48000, false);
        const VoiceId reused = engine.play(std::move(again));
        check(reused != 0 && std::find(first.begin(), first.end(), reused) == first.end(), "a recycled slot gets a new generation, so stale ids stay stale");
        engine.set_volume(first.front(), 0.0f); // a stale command: must not touch the new voice
        engine.render_block();
        check(engine.status(reused) && engine.status(reused)->state() == VoiceStatus::State::Playing, "stale commands do not hit the successor");
        check(engine.status(first.front()) == nullptr, "and the old id has no status");
    }

    // ---- many streams share a small decode pool ------------------------------------------------------------------------------------
    {
        const auto wav = std::filesystem::temp_directory_path() / "sturdy_scale_stream.wav";
        auto source = tone(2, 330.0, 3.0, 0.5f);
        const auto written = encode_buffer(wav, *source, EncodeOptions{});
        if (!written) std::cerr << "  " << written.error() << "\n";
        check(written.has_value(), "stream test file written");
        std::vector<std::shared_ptr<StreamingSource>> streams;
        for (int i = 0; i < 32; ++i) {
            auto opened = StreamingSource::open(wav, 48000);
            check(opened.has_value(), "a stream opens");
            if (opened) streams.push_back(*opened);
        }
        AudioBuffer block(2, 480);
        usize healthy = 0;
        for (auto &stream : streams) {
            u32 got = 0;
            for (int tries = 0; tries < 400; ++tries) {
                got = stream->read(block, 480);
                if (got == 480 && !stream->starved()) break; // a starved read is silence while the decoder catches up
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (got == 480 && std::fabs(block.data(0)[100] - (*source->samples)[100 * 2]) < 2e-4f) ++healthy;
        }
        check(healthy == streams.size(), "32 streams all deliver exact audio from two shared decode threads");
        check(streams.front()->seek_frames(48000), "a stream seeks");
        u32 after = 0;
        for (int tries = 0; tries < 400; ++tries) {
            after = streams.front()->read(block, 480);
            if (after == 480 && !streams.front()->starved()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(after == 480 && std::fabs(block.data(0)[10] - (*source->samples)[(48000 + 10) * 2]) < 2e-4f, "and lands on the right frame");
        streams.clear();
        std::filesystem::remove(wav);
    }

    // ---- audio for video: timestamps, gaps, the clock ---------------------------------------------------------------------------------
    {
        MediaAudioOptions options;
        options.cushion_seconds = 0.0f;
        options.buffer_seconds = 4.0f;
        MediaAudioStream media(2, 48000, 48000, options);
        std::vector<float> half_second(48000 / 2 * 2, 0.25f);
        check(media.push(10.0, half_second.data(), 24000) == 24000, "the first packet is accepted");
        // The next packet starts 0.1 s late: the gap must be played as silence so audio stays aligned with video.
        check(media.push(10.6, half_second.data(), 24000) == 24000, "a late packet is accepted");
        check(std::fabs(media.buffered_seconds() - 1.1) < 0.001, "and the gap is filled with 0.1 s of silence");
        AudioBuffer block(2, 4800);
        media.source()->read(block, 4800);
        check(std::fabs(media.playback_pts() - 10.1) < 0.001, "the clock reports the media time of what has been played");
        media.set_output_latency(0.05);
        check(std::fabs(media.playback_pts() - 10.05) < 0.001, "and subtracts the device latency");
        MediaClock clock;
        auto shared = std::make_shared<MediaAudioStream>(2, 48000, 48000, options);
        shared->push(5.0, half_second.data(), 24000);
        clock.follow(shared);
        clock.play(5.0);
        shared->source()->read(block, 4800);
        check(std::fabs(clock.now() - 5.1) < 0.001 && clock.should_drop(5.0, 0.04) && !clock.should_drop(5.09, 0.04), "the video clock follows the audio");
        shared->flush(60.0);
        shared->push(60.0, half_second.data(), 24000);
        check(std::fabs(shared->playback_pts() - 60.0) < 0.001 && shared->buffered_seconds() > 0.49, "a seek restarts the timeline");
    }

    // ---- acoustics results reach the mix: delay, Doppler, reverb retuning ------------------------------------------------------------
    {
        const auto first_sound = [](bool delayed) {
            AudioEngineConfig c;
            c.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::stereo()}};
            AudioEngine engine(c);
            engine.set_acoustics_delay_capacity(0.5f);
            auto click = std::make_shared<SampleBuffer>();
            click->channels = 1;
            click->sample_rate = 48000;
            click->samples = std::make_shared<std::vector<float>>(4800, 0.0f);
            const_cast<std::vector<float> &>(*click->samples)[0] = 1.0f;
            PlayParams p;
            p.source = std::make_shared<BufferSource>(click, 48000, false);
            p.position = {0, 0, -1};
            const VoiceId id = engine.play(std::move(p));
            if (delayed) {
                AcousticsResult r;
                r.extra_delay_seconds = 0.02f; // 960 frames
                r.has_doppler = true;
                engine.set_acoustics(id, r);
            }
            for (int b = 0; b < 20; ++b) {
                engine.render_block();
                for (u32 i = 0; i < 256; ++i) {
                    if (std::fabs(engine.bed(0).data(0)[i]) > 0.05f || std::fabs(engine.bed(0).data(1)[i]) > 0.05f) return static_cast<int>(b * 256 + i);
                }
            }
            return -1;
        };
        const int plain = first_sound(false), delayed = first_sound(true);
        check(plain >= 0 && plain < 20 && delayed > plain + 300, "a provider's propagation delay holds the sound back");

        AudioEngineConfig c;
        c.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::stereo()}};
        AudioEngine engine(c);
        const BusId reverb = engine.add_reverb_bus(0, "room", 1.0f);
        engine.set_effect_parameter(reverb, 0, "rt60", 4.0f);
        engine.set_effect_parameter(reverb, 0, "damping", 0.6f);
        for (int b = 0; b < 4; ++b) engine.render_block();
        check(engine.stats().dropped_commands == 0, "a reverb bus accepts decay and damping changes at run time");
    }

    // ---- idle buses are skipped, but never before their tail has rung out -------------------------------------------------------------
    {
        AudioEngineConfig c;
        c.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::stereo()}};
        AudioEngine engine(c);
        const BusId room = engine.add_reverb_bus(0, "room", 2.0f);
        auto click = std::make_shared<SampleBuffer>();
        click->channels = 1;
        click->sample_rate = 48000;
        click->samples = std::make_shared<std::vector<float>>(256, 0.0f);
        const_cast<std::vector<float> &>(*click->samples)[0] = 1.0f;
        PlayParams p;
        p.source = std::make_shared<BufferSource>(click, 48000, false);
        p.spatial = false;
        p.aux_bus = room;
        p.aux_send = 1.0f;
        engine.play(std::move(p));
        float early = 0.0f, late = 0.0f;
        for (int b = 0; b < 400; ++b) {
            engine.render_block();
            if (b > 20 && b < 60) early = std::max(early, std::fabs(engine.bed(0).data(0)[100]) + std::fabs(engine.bed(0).data(1)[100]));
            if (b > 300) late = std::max(late, engine.bed(0).peak());
        }
        check(early > 1e-5f, "the reverb tail is still sounding long after its voice ended (the idle bus was not cut short)");
        check(late < early, "and it does decay away");
    }

    // ---- multichannel sources ---------------------------------------------------------------------------------------------------
    {
        // A 6-channel 5.1 file played on stereo is folded down by role: centre at -3 dB to both sides, LFE dropped.
        AudioEngineConfig c;
        c.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::stereo()}};
        AudioEngine engine(c);
        auto six = std::make_shared<SampleBuffer>();
        six->channels = 6;
        six->sample_rate = 48000;
        auto samples = std::make_shared<std::vector<float>>(6 * 4800, 0.0f);
        for (size_t i = 0; i < 4800; ++i) (*samples)[i * 6 + 2] = 0.5f; // centre only
        six->samples = samples;
        six->layout = ChannelLayoutInfo::from_speakers(SpeakerLayout::surround_5_1());
        PlayParams p;
        p.source = std::make_shared<BufferSource>(six, 48000, false);
        p.spatial = false;
        apply_file_metadata(p, *six);
        engine.play(std::move(p));
        engine.render_block();
        engine.render_block();
        check(std::fabs(engine.bed(0).data(0)[100] - 0.3535f) < 0.01f && std::fabs(engine.bed(0).data(1)[100] - 0.3535f) < 0.01f, "5.1 centre folds into both stereo channels at -3 dB");

        // A 32-channel discrete source reaches the first two outputs and nothing explodes.
        AudioEngine wide(c);
        auto many = std::make_shared<SampleBuffer>();
        many->channels = 32;
        many->sample_rate = 48000;
        auto many_samples = std::make_shared<std::vector<float>>(32 * 4800, 0.0f);
        for (size_t i = 0; i < 4800; ++i) { (*many_samples)[i * 32 + 0] = 0.25f; (*many_samples)[i * 32 + 1] = -0.25f; (*many_samples)[i * 32 + 20] = 0.9f; }
        many->samples = many_samples;
        PlayParams q;
        q.source = std::make_shared<BufferSource>(many, 48000, false);
        q.spatial = false;
        wide.play(std::move(q));
        wide.render_block();
        wide.render_block();
        check(std::fabs(wide.bed(0).data(0)[50] - 0.25f) < 0.01f && std::fabs(wide.bed(0).data(1)[50] + 0.25f) < 0.01f, "a 32-channel discrete source maps channels 0 and 1 onto a stereo output");
    }

    // ---- ambisonic sources follow the listener ----------------------------------------------------------------------------------
    {
        AudioEngineConfig c;
        c.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::quad()}};
        AudioEngine engine(c);
        // First-order field of a sound straight ahead (AmbiX: W=1, Y=0, Z=0, X=1).
        auto field = std::make_shared<SampleBuffer>();
        field->channels = 4;
        field->sample_rate = 48000;
        auto s = std::make_shared<std::vector<float>>(4 * 9600, 0.0f);
        for (size_t i = 0; i < 9600; ++i) { (*s)[i * 4 + 0] = 0.3f; (*s)[i * 4 + 3] = 0.3f; }
        field->samples = s;
        field->layout = ChannelLayoutInfo::ambisonic(1);
        PlayParams p;
        p.source = std::make_shared<BufferSource>(field, 48000, false);
        p.spatial = true;
        p.space = SourceSpace::World;
        p.distance.min_distance = 1000.0f; // no distance fall-off for a field
        apply_file_metadata(p, *field);
        engine.play(std::move(p));
        for (int b = 0; b < 4; ++b) engine.render_block();
        const float front_left = engine.bed(0).data(0)[100], front_right = engine.bed(0).data(1)[100];
        check(front_left > 0.05f && std::fabs(front_left - front_right) < 0.02f, "an ambisonic source straight ahead is centred (quad: front pair equal)");
        // Turn the listener 90 degrees to the left: the field's front is now on their right.
        ListenerState turned;
        turned.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
        engine.set_listener(turned);
        for (int b = 0; b < 3; ++b) engine.render_block();
        const float left_after = engine.bed(0).data(0)[100], right_after = engine.bed(0).data(1)[100];
        check(right_after > left_after + 0.05f, "turning the head moves the sound field to the other side");
    }

    // ---- per-voice effects --------------------------------------------------------------------------------------------------------
    {
        AudioEngineConfig c;
        c.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::stereo()}};
        const auto level_with = [&](bool filtered) {
            AudioEngine engine(c);
            PlayParams p;
            p.source = std::make_shared<BufferSource>(tone(1, 9000.0, 1.0, 0.5f), 48000, true);
            p.spatial = false;
            if (filtered) {
                p.effects = {EffectSpec(EffectKind::Filter, {{"type", 0}, {"frequency", 500.0f}, {"order", 4}})};
            }
            engine.play(std::move(p));
            for (int b = 0; b < 10; ++b) engine.render_block();
            return engine.bed(0).peak();
        };
        check(level_with(false) > 0.3f && level_with(true) < 0.02f, "a per-voice low-pass removes the 9 kHz tone before mixing");
    }

    // ---- bus effects by name ----------------------------------------------------------------------------------------------------
    {
        AudioEngineConfig c;
        c.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::stereo()}};
        AudioEngine engine(c);
        const BusId bus = engine.add_bus(0, "sfx");
        check(engine.add_effect(bus, EffectSpec(EffectKind::GainPan, {{"gain", -6.0f}})), "an effect spec is added to a bus");
        check(!engine.add_effect(bus, EffectSpec(EffectKind::GainPan, {{"gian", -6.0f}})), "and a typo in it is rejected");
        PlayParams p;
        p.source = std::make_shared<BufferSource>(tone(1, 440.0, 1.0, 0.8f), 48000, true);
        p.spatial = false;
        p.bus = bus;
        engine.play(std::move(p));
        for (int b = 0; b < 6; ++b) engine.render_block();
        const float before = engine.bed(0).peak();
        engine.set_effect_parameter(bus, 0, "gain", -26.0f);
        for (int b = 0; b < 6; ++b) engine.render_block();
        check(before > 0.25f && engine.bed(0).peak() < before * 0.4f, "a bus effect parameter changes at run time, by name");
    }

    return failures == 0 ? 0 : 1;
}
