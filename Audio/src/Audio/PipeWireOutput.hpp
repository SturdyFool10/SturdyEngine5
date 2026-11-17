#pragma once

// Native PipeWire output (Linux).
//
// miniaudio reaches PipeWire only through its PulseAudio or ALSA compatibility layers, and a machine may have neither (a PipeWire-only
// install: `pipewire` + a session manager and nothing else). This talks to the PipeWire daemon directly. libpipewire is opened with
// dlopen at run time, so a machine without PipeWire runs the program unchanged, and only a small, long-stable part of its API is used so
// any PipeWire from 0.3 on works. Not part of the public engine API: DeviceSink chooses between this and miniaudio.
#include <Audio/Channels.hpp>
#include <Audio/Mixer.hpp>

#include <expected>
#include <memory>
#include <vector>

namespace SFT::Audio {

    struct AudioDeviceInfo;

    /// Playback sinks of the PipeWire daemon, or an empty list when libpipewire is missing or no daemon answers. The first entry is the
    /// default sink (named "Default"); the rest carry the sink's description, and `PipeWireOutput::open` accepts either.
    [[nodiscard]] std::vector<AudioDeviceInfo> pipewire_playback_devices();

    /// One engine output played through a PipeWire stream. The stream's process callback pulls the engine on PipeWire's real-time thread.
    class PipeWireOutput {
      public:
        /// Connects to the daemon and creates a stream for `output`. `device` is a description from `pipewire_playback_devices`; empty or
        /// "Default" follows the system default sink. Fails (rather than blocking) when no daemon answers or nothing links the stream.
        [[nodiscard]] static std::expected<std::unique_ptr<PipeWireOutput>, UString> open(AudioEngine &engine, OutputId output,
                                                                                            const SpeakerLayout &layout, const UString &device);
        ~PipeWireOutput();
        PipeWireOutput(const PipeWireOutput &) = delete;
        PipeWireOutput &operator=(const PipeWireOutput &) = delete;

        /// Starts pulling audio. Secondary outputs are started before the primary (see DeviceSink::start).
        [[nodiscard]] bool start() noexcept;
        /// The stream's buffering in seconds (its quantum, twice: PipeWire holds one buffer in flight).
        [[nodiscard]] f64 latency_seconds() const noexcept;

        struct Impl;

      private:
        explicit PipeWireOutput(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

} // namespace SFT::Audio
