// Opens the platform's audio system the way a game does and lists the playback devices. It must never crash or hang, whatever the machine
// has: PulseAudio/PipeWire, plain ALSA, JACK, or nothing at all (a headless server or container, where miniaudio falls back to its Null
// backend and DeviceSink logs a warning). The audio libraries are opened at run time, so a missing one is an empty list, not a failure.
#include <Audio/DeviceSink.hpp>

#include <cstdio>

int main() {
    const auto playback = SFT::Audio::enumerate_playback_devices();
    std::fprintf(stderr, "playback devices: %zu\n", playback.size());
    for (const auto &device : playback) {
        std::fprintf(stderr, "  %s%s (max %u channels)\n", device.name.c_str(),
                     device.is_default ? " [default]" : "", static_cast<unsigned>(device.max_channels));
    }
    const auto capture = SFT::Audio::enumerate_capture_devices();
    std::fprintf(stderr, "capture devices: %zu\n", capture.size());
    return 0;
}
