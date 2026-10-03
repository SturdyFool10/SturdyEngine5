#include <Audio/Sink.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

namespace SFT::Audio {

    std::expected<std::unique_ptr<SinkPump>, UString> SinkPump::start(AudioEngine &engine, OutputId output, std::shared_ptr<AudioSink> sink, const SinkPumpConfig &config) {
        if (!sink) {
            return std::unexpected("audio: a sink pump needs a sink");
        }
        if (output >= engine.output_count()) {
            return std::unexpected(UString{std::format("audio: the engine has no output {}", output)});
        }
        const OutputDesc &desc = engine.config().outputs[output];
        const SpeakerLayout layout = desc.kind == OutputDesc::Kind::Binaural ? SpeakerLayout::stereo() : desc.layout;
        std::unique_ptr<SinkPump> pump{new SinkPump};
        pump->engine_ = &engine;
        pump->output_ = output;
        pump->sink_ = std::move(sink);
        pump->config_ = config;
        pump->config_.block_frames = std::max(config.block_frames, 16u);
        pump->format_ = SinkFormat{layout.channel_count(), engine.config().sample_rate, ChannelLayoutInfo::from_speakers(layout)};
        if (auto opened = pump->sink_->open(pump->format_); !opened) {
            return std::unexpected(std::move(opened.error()));
        }
        pump->worker_ = std::thread([raw = pump.get()] { raw->run(); });
        return pump;
    }

    SinkPump::~SinkPump() {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
            sink_->close();
        }
    }

    void SinkPump::run() {
        using Clock = std::chrono::steady_clock;
        const u32 frames = config_.block_frames;
        const usize samples = static_cast<usize>(frames) * format_.channels;
        std::vector<f32> block(samples);
        const bool primary = output_ == primary_output;
        const auto block_time = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<f64>(static_cast<f64>(frames) / format_.sample_rate));
        auto next = Clock::now();
        while (!stop_.load(std::memory_order_acquire)) {
            u32 got = frames;
            if (primary) {
                engine_->pull(output_, block.data(), frames);
                if (config_.realtime) {
                    next += block_time;
                    std::this_thread::sleep_until(next);
                }
            } else {
                got = engine_->take_output(output_, block.data(), frames);
                if (got == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
            }
            sink_->write(std::span<const f32>{block.data(), static_cast<usize>(got) * format_.channels}, got);
            delivered_.fetch_add(got, std::memory_order_relaxed);
        }
    }

} // namespace SFT::Audio
