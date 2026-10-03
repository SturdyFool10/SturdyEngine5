#pragma once

#include <Audio/Decoder.hpp>
#include <Audio/Mixer.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace SFT::Audio {

    /// Renders an engine to memory faster than real time, with no device: for tests, tools that export audio, trailers,
    /// and deterministic replays. `on_block` is called before each block with the engine clock, which is the moment to
    /// start sounds, move the listener or change parameters at exact times. The engine must not be pulled by a device
    /// while this runs.
    [[nodiscard]] std::shared_ptr<SampleBuffer> render_offline(AudioEngine &engine, f64 seconds, OutputId output = primary_output,
                                                               const std::function<void(f64 clock_seconds)> &on_block = {});

    /// `render_offline`, written to a WAV file.
    [[nodiscard]] std::expected<void, UString> render_offline_to_wav(AudioEngine &engine, f64 seconds, const std::filesystem::path &path,
                                                                         OutputId output = primary_output, bool float32 = false,
                                                                         const std::function<void(f64 clock_seconds)> &on_block = {});

} // namespace SFT::Audio
