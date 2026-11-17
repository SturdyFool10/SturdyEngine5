// Starts the real audio engine on the machine's audio system and checks that the device is pulling audio: the engine clock must advance
// by about the time that passed. Works on whatever the machine has (PulseAudio, PipeWire through either of its compatibility layers, plain
// ALSA, JACK, nothing) because the audio libraries are opened at run time.
//
// Under CTest (no arguments) a machine without any audio system is not a failure: CI containers have none. A probe run on a machine that
// is supposed to have audio passes `--require-audio`: then "silent" (the Null backend) or "no device" fails with a distinct exit code.
//   exit 0 audio flows (or, without --require-audio, there is none to test)   2 no device could be started   3 only the silent Null backend
//   4 the device started but the engine clock did not advance
#include <Audio/DeviceSink.hpp>
#include <Audio/Mixer.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

int main(int argc, char **argv) {
    bool require_audio = false;
    for (int i = 1; i < argc; ++i) {
        require_audio = require_audio || std::strcmp(argv[i], "--require-audio") == 0;
    }

    SFT::Audio::AudioEngineConfig config;
    config.outputs.push_back(SFT::Audio::OutputDesc{});
    SFT::Audio::AudioEngine engine{std::move(config)};

    auto sink = SFT::Audio::DeviceSink::start(engine);
    if (!sink) {
        std::fprintf(stderr, "audio: no device could be started: %s\n", sink.error().c_str());
        return require_audio ? 2 : 0;
    }
    if ((*sink)->is_silent()) {
        std::fprintf(stderr, "audio: only the silent Null backend is available (no audio system found)\n");
        return require_audio ? 3 : 0;
    }

    // Some audio systems take a moment to start the stream (a PulseAudio sink resuming from suspend, PipeWire linking the stream to a
    // device): wait for the first block, then measure steady-state for a second.
    using Clock = std::chrono::steady_clock;
    const auto started_at = Clock::now();
    while (engine.clock_seconds() < 0.02 && Clock::now() - started_at < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const double first_block_ms = std::chrono::duration<double, std::milli>(Clock::now() - started_at).count();
    if (engine.clock_seconds() < 0.02) {
        std::fprintf(stderr, "audio: backend %s opened a device but delivered no audio within 5 s\n", (*sink)->backend_name());
        return 4;
    }
    const double before = engine.clock_seconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const double advanced = engine.clock_seconds() - before;
    std::fprintf(stderr, "audio: backend %s, %u device(s), output latency %.1f ms, first audio after %.0f ms, engine clock advanced %.3f s in 1 s\n",
                 (*sink)->backend_name(), (*sink)->device_count(), (*sink)->output_latency_seconds() * 1000.0, first_block_ms, advanced);
    if (advanced > 2.0) {
        // ALSA's "null" pcm (what the container matrix uses for hardware-less ALSA) consumes audio as fast as it is offered.
        std::fprintf(stderr, "audio: note: the clock ran faster than real time, so the device is an unpaced null sink, not a sound card\n");
    }
    if (advanced < 0.5) {
        std::fprintf(stderr, "audio: the device started but is not keeping up\n");
        return 4;
    }
    return 0;
}
