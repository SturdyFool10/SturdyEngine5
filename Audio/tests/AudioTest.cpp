#include <Audio/Acoustics.hpp>
#include <Audio/ContactSounds.hpp>
#include <Audio/Dsp.hpp>
#include <Audio/Mixer.hpp>
#include <Audio/Patch.hpp>
#include <Audio/Source.hpp>
#include <Audio/Spatial.hpp>

#include <cmath>
#include <iostream>
#include <memory>

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

    std::shared_ptr<SampleBuffer> sine_buffer(SFT::u32 rate, float frequency, float seconds) {
        auto b = std::make_shared<SampleBuffer>();
        b->channels = 1;
        b->sample_rate = rate;
        const SFT::u32 n = static_cast<SFT::u32>(rate * seconds);
        auto samples = std::make_shared<std::vector<float>>(n);
        for (SFT::u32 i = 0; i < n; ++i) (*samples)[i] = 0.5f * std::sin(6.2831853f * frequency * static_cast<float>(i) / static_cast<float>(rate));
        b->samples = std::move(samples);
        return b;
    }

    // Constant-level mono source for deterministic mixer checks.
    std::shared_ptr<DataSource> constant_source(SFT::u32 rate, float level, SFT::u32 frames_total = 0xFFFFFFFFu) {
        auto remaining = std::make_shared<SFT::u32>(frames_total);
        return std::make_shared<CallbackSource>(1, rate, [level, remaining](AudioBuffer &out, SFT::u32 frames) {
            const SFT::u32 n = std::min(frames, *remaining);
            for (SFT::u32 i = 0; i < n; ++i) out.data(0)[i] = level;
            *remaining -= n;
            return n;
        });
    }

    AudioEngineConfig config_for(const SpeakerLayout &layout) {
        AudioEngineConfig c;
        c.block_frames = 256;
        c.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", layout}};
        return c;
    }
} // namespace

int main() {
    // ---- filters ---------------------------------------------------------------------------------------------
    {
        Biquad lp;
        lp.set(FilterType::LowPass, 48000.0f, 1000.0f);
        check(near(lp.magnitude_at(100.0f, 48000.0f), 1.0f, 0.02f), "low-pass passes lows");
        check(lp.magnitude_at(8000.0f, 48000.0f) < 0.05f, "low-pass rejects highs");
        Biquad notch;
        notch.set(FilterType::Notch, 48000.0f, 2000.0f, 4.0f);
        check(notch.magnitude_at(2000.0f, 48000.0f) < 0.05f, "notch rejects its centre");
        Biquad peak;
        peak.set(FilterType::Peak, 48000.0f, 1000.0f, 1.0f, 12.0f);
        check(near(20.0f * std::log10(peak.magnitude_at(1000.0f, 48000.0f)), 12.0f, 0.3f), "peak filter reaches its gain");
    }

    // ---- oscillators, noise, envelope --------------------------------------------------------------------------
    {
        Oscillator osc;
        osc.set(Waveform::Sine, 48000.0f, 480.0f);
        float peak = 0.0f;
        for (int i = 0; i < 480; ++i) peak = std::max(peak, std::fabs(osc.next()));
        check(near(peak, 1.0f, 0.01f), "sine reaches full scale");
        Noise a(7), b(7), c(8);
        check(a.white() == b.white() && a.white() != c.white(), "noise is seeded and deterministic");
        Envelope env;
        env.set(48000.0f, 0.01f, 0.05f, 0.5f, 0.05f);
        env.gate_on();
        float level = 0.0f;
        for (int i = 0; i < 48000 && env.stage() != Envelope::Stage::Sustain; ++i) level = env.next();
        check(near(level, 0.5f, 0.01f), "envelope settles at the sustain level");
        env.gate_off();
        for (int i = 0; i < 48000 && env.active(); ++i) env.next();
        check(!env.active(), "envelope releases to idle");
    }

    // ---- delay, reverb, modal, limiter ----------------------------------------------------------------------------
    {
        DelayLine d(100);
        d.write(1.0f);
        for (int i = 0; i < 9; ++i) d.write(0.0f);
        check(near(d.read(10.0f), 1.0f), "delay line returns a sample delay calls later");
        FdnReverb reverb;
        reverb.set(48000.0f, 1.5f);
        std::vector<float> in(48000, 0.0f), l(48000), r(48000);
        in[0] = 1.0f;
        reverb.process(in.data(), l.data(), r.data(), 48000);
        float early = 0.0f, late = 0.0f;
        for (int i = 4000; i < 8000; ++i) early += l[i] * l[i];
        for (int i = 40000; i < 44000; ++i) late += l[i] * l[i];
        check(early > 0.0f && late < early, "reverb tail decays");
        ModalBank bank;
        const Mode modes[] = {{1000.0f, 0.2f, 1.0f}};
        bank.set(48000.0f, modes);
        bank.excite(1.0f);
        float peak = 0.0f;
        for (int i = 0; i < 480; ++i) peak = std::max(peak, std::fabs(bank.next()));
        check(peak > 0.5f, "a struck mode rings");
        for (int i = 0; i < 48000; ++i) bank.next();
        check(bank.silent(), "and decays to silence");
        Limiter limiter;
        limiter.set(48000.0f, 0.5f);
        AudioBuffer loud(1, 64);
        for (SFT::u32 i = 0; i < 64; ++i) loud.data(0)[i] = 2.0f;
        limiter.process(loud);
        check(loud.peak() <= 0.5f + 1e-4f, "the limiter holds the ceiling");
    }

    // ---- layouts and VBAP ---------------------------------------------------------------------------------------
    {
        const SpeakerLayout five = SpeakerLayout::surround_5_1();
        check(five.channel_count() == 6 && five.lfe_channel() == 3 && !five.has_height(), "5.1 layout");
        const SpeakerLayout atmos = SpeakerLayout::surround_7_1_4();
        check(atmos.channel_count() == 12 && atmos.has_height(), "7.1.4 layout has height");
        check(SpeakerLayout::surround_9_1_6().channel_count() == 16, "9.1.6 has sixteen channels");

        float az = 0, el = 0;
        angles_from_direction(direction_from_angles(40.0f, 20.0f), az, el);
        check(near(az, 40.0f, 0.01f) && near(el, 20.0f, 0.01f), "angle/direction round trip");
        check(direction_from_angles(90.0f, 0.0f).x < -0.99f, "positive azimuth is to the left (-X)");

        VbapPanner stereo(SpeakerLayout::stereo());
        std::array<float, max_channels> g{};
        stereo.gains(direction_from_angles(30.0f, 0.0f), 0.0f, g);
        check(g[0] > 0.99f && g[1] < 0.05f, "a source at the left speaker uses only it");
        stereo.gains(direction_from_angles(0.0f, 0.0f), 0.0f, g);
        check(near(g[0], g[1], 0.01f) && near(g[0] * g[0] + g[1] * g[1], 1.0f, 0.01f), "centre pans equal power");

        VbapPanner surround(five);
        surround.gains(direction_from_angles(0.0f, 0.0f), 0.0f, g);
        check(g[2] > 0.95f && g[3] == 0.0f, "front centre goes to the centre speaker, never the LFE");
        surround.gains(direction_from_angles(180.0f, 0.0f), 0.0f, g);
        check(g[4] > 0.5f && g[5] > 0.5f && near(g[4], g[5], 0.05f), "directly behind uses both surrounds");

        VbapPanner height(atmos);
        height.gains(direction_from_angles(0.0f, 90.0f), 0.0f, g);
        float top = 0.0f, ring = 0.0f;
        for (SFT::u32 c = 8; c < 12; ++c) top += g[c] * g[c];
        for (SFT::u32 c = 0; c < 8; ++c) ring += g[c] * g[c];
        check(top > 0.9f && ring < 0.1f, "straight up lands on the height speakers");
        float power = 0.0f;
        for (SFT::u32 c = 0; c < 12; ++c) power += g[c] * g[c];
        check(near(power, 1.0f, 0.02f), "VBAP gains are power normalised");
    }

    // ---- ambisonics -------------------------------------------------------------------------------------------------
    {
        std::array<float, 16> front{}, left{}, up{};
        encode_ambisonic(direction_from_angles(0.0f, 0.0f), 3, front.data());
        encode_ambisonic(direction_from_angles(90.0f, 0.0f), 3, left.data());
        encode_ambisonic(direction_from_angles(0.0f, 90.0f), 3, up.data());
        check(near(front[0], 1.0f) && near(front[3], 1.0f) && near(front[1], 0.0f), "front encodes to +X in AmbiX");
        check(near(left[1], 1.0f) && near(left[3], 0.0f, 1e-3f), "left encodes to +Y");
        check(near(up[2], 1.0f), "up encodes to +Z");

        AmbisonicDecoder decoder;
        decoder.build(SpeakerLayout::surround_7_1_4(), 3);
        AudioBuffer bed(16, 4), out(12, 4);
        std::array<float, 16> c{};
        encode_ambisonic(direction_from_angles(30.0f, 0.0f), 3, c.data()); // exactly at the front-left speaker
        for (SFT::u32 ch = 0; ch < 16; ++ch) for (SFT::u32 i = 0; i < 4; ++i) bed.data(ch)[i] = c[ch];
        decoder.decode(bed, out);
        SFT::u32 loudest = 0;
        for (SFT::u32 s = 1; s < 12; ++s) if (std::fabs(out.data(s)[0]) > std::fabs(out.data(loudest)[0])) loudest = s;
        check(loudest == 0, "an ambisonic source at a speaker decodes loudest there");
    }

    // ---- distance, doppler, air --------------------------------------------------------------------------------------
    {
        DistanceModel inverse;
        check(near(distance_gain(inverse, 0.5f), 1.0f) && near(distance_gain(inverse, 2.0f), 0.5f) && distance_gain(inverse, 20.0f) < 0.1f,
              "inverse distance attenuation");
        DistanceModel linear{Rolloff::Linear, 1.0f, 11.0f, 1.0f};
        check(near(distance_gain(linear, 6.0f), 0.5f) && near(distance_gain(linear, 100.0f), 0.0f), "linear attenuation reaches zero at max distance");
        check(doppler_ratio({100, 0, 0}, {-34.3f, 0, 0}, {0, 0, 0}, {0, 0, 0}) > 1.05f, "an approaching source raises pitch");
        check(doppler_ratio({100, 0, 0}, {34.3f, 0, 0}, {0, 0, 0}, {0, 0, 0}) < 0.95f, "a receding source lowers pitch");
        check(air_absorption_cutoff(2.0f) > air_absorption_cutoff(100.0f), "distance darkens the sound");
    }

    // ---- binaural --------------------------------------------------------------------------------------------------------
    {
        auto filter = make_spherical_head_filter(48000.0f);
        std::vector<float> in(960, 0.0f), left(960, 0.0f), right(960, 0.0f);
        in[0] = 1.0f;
        filter->process(in.data(), left.data(), right.data(), 960, direction_from_angles(90.0f, 0.0f), 2.0f);
        int first_left = -1, first_right = -1;
        for (int i = 0; i < 960; ++i) {
            if (first_left < 0 && std::fabs(left[i]) > 0.05f) first_left = i;
            if (first_right < 0 && std::fabs(right[i]) > 0.02f) first_right = i;
        }
        check(first_left >= 0 && first_right > first_left, "a source on the left reaches the left ear first");
    }

    // ---- sources ------------------------------------------------------------------------------------------------------
    {
        auto buffer = sine_buffer(24000, 440.0f, 0.1f);
        BufferSource source(buffer, 48000, false);
        AudioBuffer out(1, 256);
        SFT::u32 total = 0;
        while (!source.finished()) {
            total += source.read(out, 256);
            if (total > 100000) break;
        }
        check(total >= 4700 && total <= 4900, "a 0.1 s clip at 24 kHz resamples to ~4800 frames at 48 kHz");
        BufferSource looping(buffer, 48000, true);
        looping.read(out, 256);
        check(!looping.finished(), "looping sources never finish");

        RingSource ring(48000, 1024);
        const float samples[4] = {0.1f, 0.2f, 0.3f, 0.4f};
        check(ring.push(samples, 4) == 4, "ring accepts samples");
        AudioBuffer ring_out(1, 8);
        check(ring.read(ring_out, 8) == 4 && near(ring_out.data(0)[2], 0.3f), "ring source drains in order");
        ring.close();
        check(ring.finished(), "a closed, drained ring source is finished");
    }

    // ---- acoustics: BVH, occlusion, room -------------------------------------------------------------------------------
    {
        auto materials = std::make_shared<AcousticMaterialTable>(AcousticMaterialTable::with_defaults());
        auto bvh = std::make_shared<TriangleBvh>();
        // A concrete wall at x = 5 (a big quad facing the origin).
        const std::vector<glm::vec3> wall{{5, -10, -10}, {5, 10, -10}, {5, 10, 10}, {5, -10, 10}};
        const std::vector<SFT::u32> indices{0, 1, 2, 0, 2, 3};
        bvh->add_mesh(wall, indices, materials->find("concrete"));
        bvh->build();
        AudioRayHit hit;
        check(bvh->closest_hit({0, 0, 0}, {1, 0, 0}, 20.0f, hit) && near(hit.distance, 5.0f, 1e-3f) && hit.normal.x < 0.0f, "BVH ray hits the wall");
        check(!bvh->closest_hit({0, 0, 0}, {-1, 0, 0}, 20.0f, hit), "and misses away from it");
        check(!bvh->closest_hit({0, 0, 0}, {1, 0, 0}, 3.0f, hit), "and respects the maximum distance");

        RaycastAcoustics acoustics(bvh, materials);
        const AcousticsQuery blocked{{8, 0, 0}, {0, 0, 0}, 0.25f};
        const AcousticsQuery clear{{0, 0, 8}, {0, 0, 0}, 0.25f};
        AcousticsResult results[2];
        const AcousticsQuery queries[2] = {blocked, clear};
        acoustics.evaluate(queries, results);
        check(results[0].broadband_gain < 0.1f && results[0].lowpass_cutoff < 5000.0f, "a source behind a wall is quiet and dull");
        check(results[1].broadband_gain > 0.99f && results[1].lowpass_cutoff > 19000.0f, "an unobstructed source is untouched");
        NullAcoustics null_provider;
        AcousticsResult neutral[2];
        null_provider.evaluate(queries, neutral);
        check(neutral[0].broadband_gain == 1.0f, "the null provider changes nothing");
        const RoomEstimate open_air = acoustics.estimate_room({0, 0, 0});
        check(open_air.openness > 0.5f, "a position beside one wall is mostly open");
    }

    // ---- mixer ---------------------------------------------------------------------------------------------------------------
    {
        AudioEngine engine(config_for(SpeakerLayout::stereo()));
        engine.set_listener({});
        PlayParams p;
        p.source = constant_source(48000, 0.25f);
        p.position = {-5.0f, 0.0f, 0.0f}; // to the listener's left
        p.distance.rolloff_factor = 0.0f;  // no attenuation for this test
        const VoiceId id = engine.play(std::move(p));
        check(id != 0, "play returns a voice id");
        for (int i = 0; i < 4; ++i) engine.render_block();
        const AudioBuffer &bed = engine.bed(0);
        const float left = std::fabs(bed.data(0)[200]), right = std::fabs(bed.data(1)[200]);
        check(left > 0.2f && right < 0.05f, "a world source on the left plays from the left speaker");

        // The same voice moved to the right swaps sides.
        engine.set_position(id, {5.0f, 0.0f, 0.0f});
        for (int i = 0; i < 6; ++i) engine.render_block();
        check(std::fabs(engine.bed(0).data(1)[200]) > 0.2f && std::fabs(engine.bed(0).data(0)[200]) < 0.05f, "moving the source moves the sound");

        // Turning the listener moves where a world source is heard.
        ListenerState turned;
        turned.rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(0, 1, 0)); // now facing +X
        engine.set_listener(turned);
        for (int i = 0; i < 6; ++i) engine.render_block();
        float l = std::fabs(engine.bed(0).data(0)[200]), r = std::fabs(engine.bed(0).data(1)[200]);
        check(near(l, r, 0.06f) && l > 0.1f, "after turning to face it, the source is in front");
        engine.stop(id);
        for (int i = 0; i < 2; ++i) engine.render_block();
        check(!engine.pump().empty(), "a stopped voice is reported finished");
    }
    {
        // Listener-relative sources stay put in listener space however the listener moves and turns.
        AudioEngine engine(config_for(SpeakerLayout::stereo()));
        PlayParams p;
        p.source = constant_source(48000, 0.25f);
        p.space = SourceSpace::ListenerRelative;
        p.position = {-1.0f, 0.0f, 0.0f};
        p.distance.rolloff_factor = 0.0f;
        engine.play(std::move(p));
        ListenerState moving;
        moving.position = {100.0f, 5.0f, -30.0f};
        moving.rotation = glm::angleAxis(glm::radians(137.0f), glm::vec3(0, 1, 0));
        engine.set_listener(moving);
        for (int i = 0; i < 6; ++i) engine.render_block();
        check(std::fabs(engine.bed(0).data(0)[200]) > 0.2f && std::fabs(engine.bed(0).data(1)[200]) < 0.05f,
              "a listener-relative sound stays on the player's left wherever they go");
    }
    {
        // Non-spatial voices play straight through, and bus volumes scale them.
        AudioEngine engine(config_for(SpeakerLayout::stereo()));
        const BusId music = engine.add_bus(0, "music", engine.master_bus(0), 0.5f);
        check(music != no_bus, "bus created");
        PlayParams p;
        p.source = constant_source(48000, 0.4f);
        p.spatial = false;
        p.bus = music;
        engine.play(std::move(p));
        for (int i = 0; i < 6; ++i) engine.render_block();
        check(near(std::fabs(engine.bed(0).data(0)[200]), 0.4f * 0.7071f * 0.5f, 0.02f), "2D mono centred at -3 dB, halved by the bus");
    }
    {
        // Voice limit: with a single physical voice, the louder one is heard.
        AudioEngineConfig config = config_for(SpeakerLayout::stereo());
        config.max_physical_voices = 1;
        AudioEngine engine(config);
        PlayParams quiet, loud;
        quiet.source = constant_source(48000, 0.5f);
        quiet.spatial = false;
        quiet.volume = 0.1f;
        loud.source = constant_source(48000, 0.5f);
        loud.spatial = false;
        loud.volume = 1.0f;
        engine.play(std::move(quiet));
        engine.play(std::move(loud));
        for (int i = 0; i < 4; ++i) engine.render_block();
        check(engine.stats().physical == 1 && engine.stats().virtualised == 1, "one voice is real, the other virtual");
        check(std::fabs(engine.bed(0).data(0)[200]) > 0.3f, "the audible voice wins the slot");
    }
    {
        // Multi-output: the same sound on two buses reaches two outputs (a TV and a headset).
        AudioEngineConfig config;
        config.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "tv", SpeakerLayout::surround_5_1()},
                          OutputDesc{OutputDesc::Kind::Binaural, "headset", SpeakerLayout::stereo()}};
        AudioEngine engine(config);
        check(engine.output_count() == 2, "two outputs");
        PlayParams p;
        p.source = constant_source(48000, 0.3f);
        p.spatial = false;
        p.bus = engine.master_bus(0);
        p.mirror_buses = {engine.master_bus(1)};
        engine.play(std::move(p));
        for (int i = 0; i < 4; ++i) engine.render_block();
        check(engine.bed(0).channels() == 6 && engine.bed(0).peak() > 0.1f, "the TV output has the 5.1 mix");
        check(engine.bed(1).channels() == 2 && engine.bed(1).peak() > 0.1f, "the headset output has the stereo mix");
    }
    {
        // Objects output: positioned sources become objects with metadata, the bed stays free of them.
        AudioEngineConfig config;
        OutputDesc atmos{OutputDesc::Kind::Objects, "atmos", SpeakerLayout::surround_7_1_4(), 16};
        atmos.objects_consumed = true;
        config.outputs = {atmos};
        AudioEngine engine(config);
        PlayParams p;
        p.source = constant_source(48000, 0.3f);
        p.position = {2.0f, 1.0f, -3.0f};
        p.distance.rolloff_factor = 0.0f;
        engine.play(std::move(p));
        for (int i = 0; i < 4; ++i) engine.render_block();
        const ObjectBlock &objects = engine.objects(0);
        check(objects.active == 1, "one active object");
        bool found = false;
        for (const ObjectMetadata &m : objects.metadata) {
            if (m.voice != 0) {
                found = near(m.position.x, 2.0f, 0.01f) && near(m.position.y, 1.0f, 0.01f) && near(m.position.z, -3.0f, 0.01f);
            }
        }
        check(found, "object metadata carries the listener-space position");
        check(engine.objects(0).samples.peak() > 0.2f && engine.bed(0).peak() < 0.01f, "the object carries the audio, not the bed");

        // Without a consumer the same output falls back to panning into the bed.
        AudioEngineConfig fallback_config;
        fallback_config.outputs = {OutputDesc{OutputDesc::Kind::Objects, "atmos", SpeakerLayout::surround_7_1_4(), 16}};
        AudioEngine fallback(fallback_config);
        PlayParams q;
        q.source = constant_source(48000, 0.3f);
        q.position = {2.0f, 1.0f, -3.0f};
        q.distance.rolloff_factor = 0.0f;
        fallback.play(std::move(q));
        for (int i = 0; i < 4; ++i) fallback.render_block();
        check(fallback.objects(0).active == 0 && fallback.bed(0).peak() > 0.1f, "an unconsumed Objects output pans into its bed");
    }
    {
        // Pulling interleaved audio from the primary feeds a secondary output's FIFO.
        AudioEngineConfig config;
        config.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "a", SpeakerLayout::stereo()},
                          OutputDesc{OutputDesc::Kind::Speakers, "b", SpeakerLayout::stereo()}};
        AudioEngine engine(config);
        PlayParams p;
        p.source = constant_source(48000, 0.3f);
        p.spatial = false;
        p.mirror_buses = {engine.master_bus(1)};
        engine.play(std::move(p));
        std::vector<float> a(512 * 2), b(512 * 2);
        engine.pull(0, a.data(), 512);
        engine.pull(1, b.data(), 512);
        check(std::fabs(a[600]) > 0.1f && std::fabs(b[600]) > 0.1f, "secondary output receives what the primary rendered");
    }

    // ---- contact sounds -----------------------------------------------------------------------------------------------------
    {
        AudioEngine engine(config_for(SpeakerLayout::stereo()));
        SFT::Physics::SurfaceTable surfaces;
        const auto metal = surfaces.add({.name = "metal"});
        const auto wood = surfaces.add({.name = "wood"});
        ContactSoundSystem sounds(engine, surfaces, ModalMaterialTable::with_defaults());
        SFT::Physics::ContactEvent event;
        event.contact.body_a = 1;
        event.contact.body_b = 2;
        event.contact.material_a = metal;
        event.contact.material_b = wood;
        event.contact.point = {0, 0, -2};
        event.intensity = 0.8f;
        const SFT::Physics::ContactEvent events[1] = {event};
        check(sounds.process(events) == 1 && sounds.active() == 1, "a contact starts a voice");
        for (int i = 0; i < 8; ++i) engine.render_block();
        check(engine.bed(0).peak() > 0.001f, "and it is audible");
    }

    // ---- procedural patches ---------------------------------------------------------------------------------------------
    {
        PatchBuilder b;
        const PatchNode rpm = b.param("rpm", 800.0f);
        const PatchNode hz = b.scale(rpm, 1.0f / 60.0f);
        const PatchNode tone = b.oscillator(Waveform::Saw, hz);
        const PatchNode output = b.scale(b.filter(PatchBuilder::Band::Low, tone, b.constant(600.0f)), 0.5f);
        auto patch = b.build(output);
        PatchSource source(patch, 48000);
        check(source.param_count() == 1 && source.set_param("rpm", 1200.0f) && !source.set_param("nope", 1.0f), "patch parameters are addressable by name");
        AudioBuffer out(1, 4800);
        check(source.read(out, 4800) == 4800 && out.peak() > 0.05f && out.peak() <= 1.0f, "a saw engine patch produces audio");
        // The filter keeps the output smooth compared to the raw saw.
        float max_step = 0.0f;
        for (SFT::u32 i = 1; i < 4800; ++i) max_step = std::max(max_step, std::fabs(out.data(0)[i] - out.data(0)[i - 1]));
        check(max_step < 0.4f, "low-passing tames the saw's edges");

        // An enveloped blip: silent before the gate, audible after, gone after release.
        PatchBuilder blip;
        const PatchNode gate = blip.param("gate", 0.0f);
        const PatchNode envelope = blip.envelope(gate, 0.001f, 0.01f, 0.5f, 0.02f);
        const PatchNode sine = blip.oscillator(Waveform::Sine, blip.midi_to_hz(blip.constant(69.0f)));
        PatchSource voice(blip.build(blip.multiply(sine, envelope)), 48000);
        AudioBuffer block(1, 480);
        voice.read(block, 480);
        check(block.peak() < 1e-3f, "silent before the gate opens");
        voice.set_param("gate", 1.0f);
        voice.read(block, 480);
        voice.read(block, 480);
        check(block.peak() > 0.3f, "audible while gated");
        voice.set_param("gate", 0.0f);
        for (int i = 0; i < 20; ++i) voice.read(block, 480);
        check(block.peak() < 1e-3f, "silent after release");

        PatchSource timed(patch, 48000);
        timed.set_duration(0.01f);
        AudioBuffer timed_out(1, 1024);
        check(timed.read(timed_out, 1024) == 480 && timed.finished(), "a timed patch ends after its duration");
    }

    return failures == 0 ? 0 : 1;
}
