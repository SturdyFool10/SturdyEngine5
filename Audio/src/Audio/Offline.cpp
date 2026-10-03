#include <Audio/Offline.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Audio {

    std::shared_ptr<SampleBuffer> render_offline(AudioEngine &engine, f64 seconds, OutputId output, const std::function<void(f64)> &on_block) {
        const u32 block = engine.config().block_frames;
        const u32 rate = engine.config().sample_rate;
        const u64 total_frames = static_cast<u64>(std::max(seconds, 0.0) * static_cast<f64>(rate));
        const u32 channels = std::max(engine.bed(output).channels(), 1u);

        auto samples = std::make_shared<std::vector<f32>>();
        samples->reserve(static_cast<usize>(total_frames + block) * channels);
        u64 done = 0;
        while (done < total_frames) {
            if (on_block) {
                on_block(engine.clock_seconds());
            }
            engine.render_block();
            const AudioBuffer &bed = engine.bed(output);
            const u32 take = static_cast<u32>(std::min<u64>(block, total_frames - done));
            const usize base = samples->size();
            samples->resize(base + static_cast<usize>(take) * channels);
            bed.store_interleaved(std::span<f32>{*samples}.subspan(base), channels, take);
            done += take;
            (void)engine.pump(); // free retired voices as we go
        }
        auto buffer = std::make_shared<SampleBuffer>();
        buffer->channels = channels;
        buffer->sample_rate = rate;
        buffer->samples = std::move(samples);
        return buffer;
    }

    std::expected<void, UString> render_offline_to_wav(AudioEngine &engine, f64 seconds, const std::filesystem::path &path, OutputId output,
                                                           bool float32, const std::function<void(f64)> &on_block) {
        const auto buffer = render_offline(engine, seconds, output, on_block);
        return write_wav(path, *buffer, float32);
    }

} // namespace SFT::Audio
