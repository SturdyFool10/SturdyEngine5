#include <Audio/Record.hpp>

#include <Audio/Kernels.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace SFT::Audio {

    struct AudioRecorder::Impl {
        std::shared_ptr<LiveSource> source;
        std::unique_ptr<AudioEncoder> encoder;
        std::thread worker;
        std::atomic<bool> stop_requested{false};
        std::atomic<bool> paused{false};
        std::atomic<u64> frames{0};
        mutable std::mutex error_mutex;
        UString error;
        u32 sample_rate = 48000;

        void fail(UString message) {
            std::scoped_lock lock(error_mutex);
            if (error.empty()) {
                error = std::move(message);
            }
        }

        void run() {
            constexpr u32 kBlock = 2048;
            const u32 channels = source->channel_count();
            AudioBuffer block(channels, kBlock);
            std::vector<f32> interleaved(static_cast<usize>(kBlock) * channels);
            bool draining = false;
            for (;;) {
                const u32 got = source->read(block, kBlock);
                if (got > 0) {
                    if (!paused.load(std::memory_order_relaxed)) {
                        block.store_interleaved(interleaved, channels, got);
                        if (!encoder->write(interleaved.data(), got)) {
                            fail(encoder->error());
                            return;
                        }
                        frames.fetch_add(got, std::memory_order_relaxed);
                    }
                    continue;
                }
                if (draining || source->finished()) {
                    return;
                }
                if (stop_requested.load(std::memory_order_acquire)) {
                    draining = true; // one more pass picks up anything pushed since the last read
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        }
    };

    AudioRecorder::AudioRecorder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    AudioRecorder::~AudioRecorder() {
        if (impl_ && impl_->worker.joinable()) {
            (void)stop();
        }
    }

    std::expected<std::unique_ptr<AudioRecorder>, UString> AudioRecorder::start(std::shared_ptr<LiveSource> source, const std::filesystem::path &path,
                                                                                    const EncodeOptions &options) {
        if (!source) {
            return std::unexpected("audio: nothing to record");
        }
        auto impl = std::make_unique<Impl>();
        impl->sample_rate = source->native_rate();
        auto encoder = open_encoder(path, source->channel_count(), impl->sample_rate, options, source->channel_layout());
        if (!encoder) {
            return std::unexpected(encoder.error());
        }
        impl->encoder = std::move(*encoder);
        impl->source = std::move(source);
        Impl *raw = impl.get();
        impl->worker = std::thread([raw] { raw->run(); });
        return std::unique_ptr<AudioRecorder>(new AudioRecorder(std::move(impl)));
    }

    std::expected<std::unique_ptr<AudioRecorder>, UString> AudioRecorder::start(CaptureDevice &device, const std::filesystem::path &path, const EncodeOptions &options) {
        auto tap = device.add_tap();
        if (!tap) {
            return std::unexpected("audio: the capture device has no free taps left");
        }
        return start(std::move(tap), path, options);
    }

    std::expected<void, UString> AudioRecorder::stop() {
        if (!impl_ || !impl_->encoder) {
            return std::unexpected("audio: the recording was already stopped");
        }
        impl_->stop_requested.store(true, std::memory_order_release);
        if (impl_->worker.joinable()) {
            impl_->worker.join();
        }
        {
            std::scoped_lock lock(impl_->error_mutex);
            if (!impl_->error.empty()) {
                return std::unexpected(impl_->error);
            }
        }
        auto result = impl_->encoder->finish();
        impl_->encoder.reset();
        return result;
    }

    void AudioRecorder::pause() { impl_->paused.store(true, std::memory_order_relaxed); }
    void AudioRecorder::resume() { impl_->paused.store(false, std::memory_order_relaxed); }
    u64 AudioRecorder::frames_recorded() const noexcept { return impl_->frames.load(std::memory_order_relaxed); }
    f64 AudioRecorder::seconds_recorded() const noexcept { return static_cast<f64>(frames_recorded()) / static_cast<f64>(std::max(impl_->sample_rate, 1u)); }
    bool AudioRecorder::failed() const noexcept {
        std::scoped_lock lock(impl_->error_mutex);
        return !impl_->error.empty();
    }
    UString AudioRecorder::error() const {
        std::scoped_lock lock(impl_->error_mutex);
        return impl_->error;
    }

} // namespace SFT::Audio
