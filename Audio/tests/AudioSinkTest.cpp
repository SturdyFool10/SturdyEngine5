#include <Audio/Mixer.hpp>
#include <Audio/Sink.hpp>
#include <Audio/Source.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <numbers>
#include <thread>

using namespace SFT::Audio;
using SFT::u32;
using SFT::u64;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }

    std::shared_ptr<const SampleBuffer> tone(u32 rate = 48000) {
        auto b = std::make_shared<SampleBuffer>();
        b->channels = 1;
        b->sample_rate = rate;
        auto samples = std::make_shared<std::vector<float>>(rate);
        for (std::size_t i = 0; i < samples->size(); ++i) (*samples)[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / rate));
        b->samples = std::move(samples);
        return b;
    }

    struct Capture : AudioSink {
        std::mutex mutex;
        SinkFormat format;
        std::vector<float> data;
        bool opened = false, closed = false;
        std::expected<void, SFT::Foundation::UString> open(const SinkFormat &f) override {
            format = f;
            opened = true;
            return {};
        }
        void write(std::span<const float> interleaved, u32) override {
            std::lock_guard lock(mutex);
            data.insert(data.end(), interleaved.begin(), interleaved.end());
        }
        void close() override { closed = true; }
    };

    bool wait_for(const std::function<bool()> &done) {
        for (int i = 0; i < 400 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return done();
    }
} // namespace

int main() {
    // The primary output driven by a pump (no device): audio reaches a custom sink.
    {
        AudioEngineConfig config;
        config.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::surround_5_1()}};
        AudioEngine engine(config);
        PlayParams p;
        p.source = std::make_shared<BufferSource>(tone(), 48000, true);
        p.spatial = false;
        engine.play(std::move(p));
        auto capture = std::make_shared<Capture>();
        {
            auto pump = SinkPump::start(engine, primary_output, capture, SinkPumpConfig{.block_frames = 512, .realtime = false});
            check(pump.has_value(), "the pump starts on the primary output");
            if (pump) {
                check(wait_for([&] { return (*pump)->frames_delivered() > 4096; }), "frames flow to the sink");
                check((*pump)->format().channels == 6, "the sink is told the output has six channels");
            }
        }
        check(capture->opened && capture->closed, "the sink is opened and closed");
        float peak = 0.0f;
        {
            std::lock_guard lock(capture->mutex);
            check(capture->data.size() % 6 == 0 && !capture->data.empty(), "whole frames arrive");
            for (float v : capture->data) peak = std::max(peak, std::fabs(v));
        }
        check(peak > 0.05f, "the sink hears the playing sound");
    }

    // A secondary output, followed as the primary is pulled.
    {
        AudioEngineConfig config;
        config.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::stereo()}, OutputDesc{OutputDesc::Kind::Speakers, "stream", SpeakerLayout::surround_7_1()}};
        AudioEngine engine(config);
        auto capture = std::make_shared<Capture>();
        auto pump = SinkPump::start(engine, 1, capture);
        check(pump.has_value() && capture->format.channels == 8, "a 7.1 secondary output is delivered as eight channels");
        std::vector<float> scratch(256 * 2);
        for (int i = 0; i < 20; ++i) {
            engine.pull(primary_output, scratch.data(), 256);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(wait_for([&] { return pump && (*pump)->frames_delivered() > 0; }), "secondary output frames reach the sink");
    }

    check(!SinkPump::start(*std::make_unique<AudioEngine>(AudioEngineConfig{}), 9, std::make_shared<Capture>()).has_value(), "an unknown output is refused");
    return failures == 0 ? 0 : 1;
}
