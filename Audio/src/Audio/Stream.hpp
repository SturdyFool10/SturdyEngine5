#pragma once

#include <Audio/Decoder.hpp>
#include <Audio/Source.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace SFT::Audio {

    struct StreamingOptions {
        /// How much decoded audio the worker keeps ahead of playback.
        f32 buffer_seconds = 2.0f;
        /// Loop at the end of the file (gapless), or at the file's embedded loop region when it has one.
        bool loop = false;
        bool use_embedded_loop = true;
    };

    /// A decoded-on-the-fly source for music and long ambience. A worker thread decodes ahead into a lock-free chunk ring,
    /// so the audio thread never touches the codec; seeks are requests the worker serves in a few milliseconds without ever
    /// blocking mixing. Pitch is applied here (cubic resampling), and the file's own rate is converted to the mixer's.
    class StreamingSource final : public DataSource {
      public:
        [[nodiscard]] static std::expected<std::shared_ptr<StreamingSource>, UString> open(
            const std::filesystem::path &path, u32 output_rate, const StreamingOptions &options = {});
        [[nodiscard]] static std::expected<std::shared_ptr<StreamingSource>, UString> open_memory(
            EncodedBytes bytes, const UString &extension_hint, u32 output_rate, const StreamingOptions &options = {});
        [[nodiscard]] static std::expected<std::shared_ptr<StreamingSource>, UString> from_decoder(
            std::unique_ptr<StreamDecoder> decoder, u32 output_rate, const StreamingOptions &options = {});

        ~StreamingSource() override;

        [[nodiscard]] const AudioStreamInfo &info() const noexcept;

        u32 channel_count() const override;
        u32 sample_rate() const override;
        u32 read(AudioBuffer &out, u32 frames) override;
        bool finished() const override;
        void set_rate(f32 rate) override;
        bool supports_rate() const override { return true; }
        bool skip(u64 frames) override;
        u32 native_rate() const override;
        bool seekable() const override { return true; }
        bool seek_frames(u64 frame) override;
        u64 position_frames() const override;
        u64 length_frames() const override;

        /// True when the last `read` could not get enough decoded audio (a slow disk or an overloaded worker).
        [[nodiscard]] bool starved() const noexcept;

      public:
        struct Impl; // shared with the decode pool (internal)

      private:
        explicit StreamingSource(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

} // namespace SFT::Audio
