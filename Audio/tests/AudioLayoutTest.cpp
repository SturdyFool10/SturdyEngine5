#include <Audio/Mixer.hpp>
#include <Audio/Source.hpp>
#include <Audio/Spatial.hpp>

#include <cmath>
#include <iostream>
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

    std::shared_ptr<const SampleBuffer> tone(u32 channels) {
        auto b = std::make_shared<SampleBuffer>();
        b->channels = channels;
        b->sample_rate = 48000;
        auto samples = std::make_shared<std::vector<float>>(48000 * channels);
        for (std::size_t i = 0; i < 48000; ++i)
            for (u32 c = 0; c < channels; ++c) (*samples)[i * channels + c] = 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 250.0 * static_cast<double>(i) / 48000.0));
        b->samples = std::move(samples);
        return b;
    }

    // Plays one sound on an engine whose output has `layout`; returns per-channel energy.
    std::vector<double> listen(const SpeakerLayout &layout, bool spatial, glm::vec3 position, u32 source_channels = 1) {
        AudioEngineConfig config;
        config.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", layout}};
        AudioEngine engine(config);
        PlayParams p;
        p.source = std::make_shared<BufferSource>(tone(source_channels), 48000, true);
        p.spatial = spatial;
        p.position = position;
        engine.play(std::move(p));
        std::vector<double> energy(layout.channel_count(), 0.0);
        for (int b = 0; b < 12; ++b) {
            engine.render_block();
            const AudioBuffer &bed = engine.bed(0);
            for (u32 c = 0; c < bed.channels(); ++c)
                for (u32 i = 0; i < bed.frames(); ++i) energy[c] += static_cast<double>(bed.data(c)[i]) * bed.data(c)[i];
        }
        return energy;
    }

    u32 loudest(const std::vector<double> &energy) {
        return static_cast<u32>(std::max_element(energy.begin(), energy.end()) - energy.begin());
    }
} // namespace

int main() {
    // Every channel count has a layout with exactly that many speakers, none of them degenerate.
    for (u32 n = 1; n <= max_channels; ++n) {
        const SpeakerLayout layout = SpeakerLayout::from_channel_count(n);
        check(layout.channel_count() == n, "from_channel_count gives the count asked for");
    }
    check(SpeakerLayout::from_channel_count(24).name == "22.2"_ustr, "24 channels is NHK 22.2");
    check(SpeakerLayout::surround_22_2().channel_of(ChannelRole::Lfe2) != ~0u && SpeakerLayout::surround_22_2().has_height(), "22.2 has two LFEs and height");

    // A source placed at a speaker comes out of that speaker, on rare layouts too.
    for (u32 n : {3u, 5u, 7u, 9u, 11u, 13u, 14u, 24u}) {
        const SpeakerLayout layout = SpeakerLayout::from_channel_count(n);
        u32 hits = 0, tried = 0;
        for (u32 s = 0; s < n; ++s) {
            if (is_lfe(layout.speakers[s].role)) continue;
            ++tried;
            const glm::vec3 direction = direction_from_angles(layout.speakers[s].azimuth_degrees, layout.speakers[s].elevation_degrees);
            const auto energy = listen(layout, true, direction * 3.0f);
            double total = 0.0;
            for (double e : energy) total += e;
            if (total > 0.0 && energy[s] > total * 0.5) ++hits;
        }
        check(hits == tried, "a source in a speaker's direction plays mostly from that speaker");
    }

    // Non-spatial stereo on a 22.2 output reaches the front pair; nothing is lost, nothing goes to the LFEs.
    {
        const SpeakerLayout layout = SpeakerLayout::surround_22_2();
        const auto energy = listen(layout, false, {}, 2);
        double total = 0.0;
        for (double e : energy) total += e;
        check(total > 0.0, "a 2D stereo sound is audible on 22.2");
        check(energy[layout.lfe_channel()] == 0.0 && energy[layout.channel_of(ChannelRole::Lfe2)] == 0.0, "and keeps out of the LFE channels");
    }

    // Custom layouts get sensible roles.
    {
        const std::pair<f32, f32> angles[] = {{20, 0}, {-20, 0}, {100, 0}, {-100, 0}, {180, 0}, {0, 60}};
        const SpeakerLayout layout = SpeakerLayout::custom("odd", angles);
        check(layout.speakers[0].role == ChannelRole::FrontLeft && layout.speakers[1].role == ChannelRole::FrontRight && layout.speakers[2].role == ChannelRole::SideLeft &&
                  layout.speakers[4].role == ChannelRole::BackCenter && layout.speakers[5].role == ChannelRole::TopFrontCenter,
              "custom layouts are given roles from their angles");
    }
    return failures == 0 ? 0 : 1;
}
