#pragma once

#include <Audio/DeviceSink.hpp>
#include <Audio/Encode.hpp>
#include <Audio/Source.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace SFT::Audio {

    /// Records a live source (a microphone tap, a network stream, anything feeding a `LiveSource`) to a file on a worker
    /// thread, in any format the encoders write, at any channel count. Recording a 32-channel interface to Wave64 or Opus is
    /// the same call as recording a headset to FLAC.
    ///
    ///     auto mic = CaptureDevice::start({.channels = 0});             // everything the interface offers
    ///     auto take = AudioRecorder::start(*mic, "take1.w64", {.format = AudioFormat::Wave64, .sample_format = SampleFormat::Int24});
    ///     ...
    ///     (*take)->stop();
    class AudioRecorder {
      public:
        /// Records `source` (use `CaptureDevice::add_tap()` so playing the microphone and recording it do not compete for frames).
        [[nodiscard]] static std::expected<std::unique_ptr<AudioRecorder>, UString> start(std::shared_ptr<LiveSource> source,
                                                                                              const std::filesystem::path &path, const EncodeOptions &options = {});
        /// Taps `device` and records the tap.
        [[nodiscard]] static std::expected<std::unique_ptr<AudioRecorder>, UString> start(CaptureDevice &device, const std::filesystem::path &path,
                                                                                              const EncodeOptions &options = {});
        ~AudioRecorder();
        AudioRecorder(const AudioRecorder &) = delete;
        AudioRecorder &operator=(const AudioRecorder &) = delete;

        /// Stops, writes what is buffered, and finishes the file (headers patched, moved into place).
        [[nodiscard]] std::expected<void, UString> stop();
        /// While paused the feed is read and discarded, so resuming does not replay old audio.
        void pause();
        void resume();

        [[nodiscard]] u64 frames_recorded() const noexcept;
        [[nodiscard]] f64 seconds_recorded() const noexcept;
        [[nodiscard]] bool failed() const noexcept;
        [[nodiscard]] UString error() const;

      private:
        struct Impl;
        explicit AudioRecorder(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

} // namespace SFT::Audio
