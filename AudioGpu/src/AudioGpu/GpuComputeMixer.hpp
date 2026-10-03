#pragma once

#include <Audio/Compute.hpp>

#include <Foundation/Foundation.hpp>

#include <RHI/RHI.hpp>

#include <expected>
#include <memory>
#include <string>

/// Compute-mode audio mixing on a GPU (see Audio/Compute.hpp for what is offloaded and why).
///
/// Loaded samples are copied once into a pool in video memory; each block uploads one small record per voice and per tap,
/// runs three dispatches (resample + filter per voice, gain-ramped partial sums per run of taps, per-destination reduce) and
/// reads the mixed channels back. A block costs a few hundred microseconds whether it holds a thousand voices or fifty
/// thousand, so this is the mode for very large voice counts; below a few hundred voices the CPU mixer is faster because a
/// device round trip has a floor.
///
/// Everything here runs on the thread that calls `submit`/`collect` (the audio thread) except `register_sample` and
/// `release_sample`, which are thread safe. Uploads of new samples are bounded per block and happen inside `submit`, so a
/// sample becomes usable a block or two after it is registered.
namespace SFT::AudioGpu {

    struct GpuMixerConfig {
        /// Video memory reserved for resident samples (interleaved f32). Samples that do not fit stay on the CPU mixer.
        u64 sample_pool_bytes = 128ull << 20;
        /// Samples that may be resident at once.
        u32 max_samples = 8192;
        /// Most bytes of new samples copied to the device per block.
        u64 upload_bytes_per_block = 16ull << 20;
        /// Slang source of the mixing kernels; the embedded copy is used when this path does not exist on disk.
        std::string shader_path = "Shaders/audio_mix.slang";
    };

    class GpuComputeMixer final : public Audio::ComputeMixBackend {
      public:
        /// Builds the pipelines and buffers on `device`, which must outlive the mixer. Fails (with a reason) when the device
        /// cannot run the kernels; the caller then keeps mixing on the CPU.
        [[nodiscard]] static std::expected<std::unique_ptr<GpuComputeMixer>, UString> create(RHI::RhiDevice &device, const GpuMixerConfig &config = {});
        ~GpuComputeMixer() override;

        [[nodiscard]] Audio::ComputeBackendInfo info() const override;
        [[nodiscard]] Audio::ComputeSampleId register_sample(std::shared_ptr<const std::vector<f32>> samples, u32 channels) override;
        [[nodiscard]] bool sample_ready(Audio::ComputeSampleId id) const override;
        void release_sample(Audio::ComputeSampleId id) override;
        void submit(const Audio::ComputeBlock &block) override;
        [[nodiscard]] Audio::ComputeResult collect() override;

        /// Name of the adapter the kernels run on.
        [[nodiscard]] const UString &device_name() const noexcept;

      private:
        struct Impl;
        explicit GpuComputeMixer(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

} // namespace SFT::AudioGpu
