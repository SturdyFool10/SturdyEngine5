#pragma once

#include <Audio/Channels.hpp>
#include <Audio/Mixer.hpp>
#include <Audio/Source.hpp>

#include <Foundation/Foundation.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace SFT::Audio {

    struct AudioDeviceInfo {
        UString name;
        bool is_default = false;
        /// Highest channel count the device reports (0 when unknown).
        u32 max_channels = 0;
        /// Native sample rates the device lists (empty when it only says "anything").
        std::vector<u32> sample_rates;
    };

    /// Playback devices on this machine (WASAPI, CoreAudio, ALSA/PulseAudio/PipeWire, Web Audio, ... through miniaudio).
    [[nodiscard]] std::vector<AudioDeviceInfo> enumerate_playback_devices();

    /// Microphones and line inputs.
    [[nodiscard]] std::vector<AudioDeviceInfo> enumerate_capture_devices();

    struct DeviceSinkConfig {
        /// Device for each engine output, by name as reported by `enumerate_playback_devices`; an empty entry (or a
        /// missing one) uses the default device for output 0 and plays nothing for the others. Several outputs on several
        /// devices is how a game feeds a TV and a headset (or a party-mode second speaker) at once.
        std::vector<UString> device_names;
    };

    /// Pulls the engine's outputs into real audio devices. Output 0 drives the engine clock; the others read the FIFOs
    /// its callback fills. Speaker layouts are handed to the OS with explicit channel positions, so 5.1/7.1/7.1.4
    /// land on the right speakers regardless of each backend's native channel order.
    ///
    /// Object-based (Atmos-class) rendering is a platform spatial-audio client's job: consume
    /// `AudioEngine::objects()` yourself and set `OutputDesc::objects_consumed`; this sink plays the bed only.
    class DeviceSink {
      public:
        [[nodiscard]] static std::expected<std::unique_ptr<DeviceSink>, UString> start(AudioEngine &engine,
                                                                                          const DeviceSinkConfig &config = {});
        ~DeviceSink();
        DeviceSink(const DeviceSink &) = delete;
        DeviceSink &operator=(const DeviceSink &) = delete;

        /// Devices actually running (outputs without one are skipped).
        [[nodiscard]] u32 device_count() const noexcept;
        /// Seconds between the engine producing a sample on the primary output and it leaving the speakers (the device's buffer);
        /// give it to `MediaAudioStream::set_output_latency` so video lines up with what is heard.
        [[nodiscard]] f64 output_latency_seconds() const noexcept;

      private:
        struct Impl;
        explicit DeviceSink(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

    struct CaptureConfig {
        /// Input device by name as reported by `enumerate_capture_devices`; empty selects the default input.
        UString device_name;
        /// Rate to capture at; 0 uses the device's native rate. The audio the source hands the engine is converted to
        /// `engine_rate`.
        u32 sample_rate = 48000;
        /// The rate the source presents to the mixer (0 = same as `sample_rate`). Set it to the engine's rate when they differ.
        u32 engine_rate = 0;
        /// Channels to capture; 0 takes everything the device offers (stereo mics, 8/16/32/64-channel interfaces, ambisonic arrays).
        u32 channels = 0;
        /// Ring capacity per source.
        f32 buffer_seconds = 1.0f;
        /// The jitter cushion: how much audio is held back so the device's bursts never underrun the mixer. Latency is
        /// roughly this plus the device's own period.
        f32 cushion_seconds = 0.02f;
        /// What the channels are. Unlabelled captures are mono, stereo, or discrete; say `ChannelLayoutInfo::ambisonic(1)` for
        /// a B-format mic or a speaker layout for a surround rig.
        std::optional<ChannelLayoutInfo> layout;
    };

    /// A microphone or audio interface as a live source: any channel count the hardware offers, at low latency. The
    /// captured audio appears in `source()`, a lock-free multi-channel `LiveSource` a voice can play (`engine.play` with that
    /// source, on a bus with effects) or code can read; `add_tap()` makes further independent copies for recorders, meters or
    /// network senders. Samples the device could not hand over are counted rather than blocking it.
    class CaptureDevice {
      public:
        [[nodiscard]] static std::expected<std::unique_ptr<CaptureDevice>, UString> start(const CaptureConfig &config);
        /// Mono-or-native capture at `sample_rate` (the original signature).
        [[nodiscard]] static std::expected<std::unique_ptr<CaptureDevice>, UString> start(const UString &device_name = {}, u32 sample_rate = 48000,
                                                                                             f32 buffer_seconds = 1.0f);
        ~CaptureDevice();
        CaptureDevice(const CaptureDevice &) = delete;
        CaptureDevice &operator=(const CaptureDevice &) = delete;

        [[nodiscard]] std::shared_ptr<LiveSource> source() const;
        /// Another copy of the capture stream with its own ring (up to seven). Taps never adjust their rate (they are for
        /// recording and analysis, not playback): they deliver every captured frame once.
        [[nodiscard]] std::shared_ptr<LiveSource> add_tap(f32 buffer_seconds = 2.0f);
        /// The device's actual channel count and capture rate.
        [[nodiscard]] u32 channels() const noexcept;
        [[nodiscard]] u32 sample_rate() const noexcept;
        [[nodiscard]] const UString &device_name() const noexcept;
        [[nodiscard]] ChannelLayoutInfo layout() const;
        [[nodiscard]] u64 dropped_samples() const noexcept;
        /// Highest absolute sample of a channel since the last call (for level meters); resets the meter.
        [[nodiscard]] f32 take_channel_peak(u32 channel) noexcept;

      private:
        struct Impl;
        explicit CaptureDevice(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

} // namespace SFT::Audio
