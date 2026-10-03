// GPU compute mixing against the CPU: the same scene is rendered by an engine mixing everything on the CPU and by one whose
// voices go through GpuComputeMixer on a real device; the beds must agree to float rounding. SKIPs (exit 0) when no device
// can be brought up (no window system, no Vulkan driver).

#include <AudioGpu/GpuComputeMixer.hpp>

#include <Audio/Dsp.hpp>
#include <Audio/Mixer.hpp>
#include <Audio/Source.hpp>

#include <Core/Core.hpp>
#include <Core/EngineBackend.hpp>
#include <Core/Renderer.hpp>
#include <RHI/RHI.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>
#include <WindowManager/WindowManager.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>

using namespace SFT::Audio;
using SFT::u32;
namespace rhi = SFT::RHI;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }

    struct Gpu {
        std::unique_ptr<SFT::WindowManager::WindowManager> windows;
        std::unique_ptr<SFT::Core::EngineBackend> backend;
        rhi::RhiDevice *device = nullptr;
        ~Gpu() {
            if (device != nullptr) {
                device->wait_idle();
            }
            backend.reset();
            windows.reset();
        }
    };

    std::unique_ptr<Gpu> make_gpu(std::string &skip_reason) {
        auto gpu = std::make_unique<Gpu>();
        gpu->windows = std::make_unique<SFT::WindowManager::WindowManager>();
        using SDL3Window = SFT::WindowManager::SDL3::SDL3Window;
        SFT::WindowManager::WindowConfig config{};
        config.title = "AudioGpuTest";
        config.extent = {64, 64};
        config.visible = false;
        config.graphics_api = SFT::WindowManager::WindowGraphicsApi::Vulkan;
        auto id = gpu->windows->spawn_window<SDL3Window>(config);
        if (!id) {
            skip_reason = "no window system: " + std::string{id.error().message};
            return nullptr;
        }
        gpu->backend = SFT::Core::create_vulkan_backend();
        SFT::Core::RendererCreateInfo info{};
        info.backend = rhi::BackendType::Vulkan;
        info.app_name = "AudioGpuTest";
        info.enable_shader_disk_cache = false;
        std::optional<std::string> error;
        auto result = gpu->windows->with_window(*id, [&](SFT::WindowManager::Window &window) {
            info.window = &window;
            auto surface = gpu->backend->initialize(info);
            if (!surface) {
                error = surface.error().message;
            }
            return true;
        });
        if (!result || error) {
            skip_reason = "Vulkan backend initialization failed: " + error.value_or("window vanished");
            return nullptr;
        }
        gpu->device = gpu->backend->rhi_device();
        if (gpu->device == nullptr) {
            skip_reason = "Vulkan backend exposed no RHI device";
            return nullptr;
        }
        return gpu;
    }

    std::shared_ptr<const SampleBuffer> tone(u32 channels, double frequency, double seconds, float amplitude, u32 rate) {
        auto b = std::make_shared<SampleBuffer>();
        b->channels = channels;
        b->sample_rate = rate;
        const size_t frames = static_cast<size_t>(rate * seconds);
        auto samples = std::make_shared<std::vector<float>>(frames * channels);
        for (size_t i = 0; i < frames; ++i) {
            for (u32 c = 0; c < channels; ++c) {
                (*samples)[i * channels + c] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * (1.0 + 0.37 * c) * static_cast<double>(i) / rate));
            }
        }
        b->samples = std::move(samples);
        return b;
    }

    struct Result {
        std::vector<float> samples;
        u32 peak_compute = 0;
        double milliseconds = 0.0;
    };
    Result render(std::shared_ptr<ComputeMixBackend> backend, u32 count, u32 blocks) {
        AudioEngineConfig config;
        config.outputs = {OutputDesc{OutputDesc::Kind::Speakers, "main", SpeakerLayout::surround_5_1()}};
        config.max_voices = 8192;
        config.max_physical_voices = 8192;
        config.mix_threads = 0;
        config.compute_backend = std::move(backend);
        config.compute_min_voices = 1;
        AudioEngine engine(config);
        const auto mono = tone(1, 330.0, 0.40, 0.05f, 48000);
        const auto stereo = tone(2, 220.0, 0.25, 0.05f, 44100);
        const auto shortclip = tone(1, 700.0, 0.03, 0.05f, 22050);
        for (u32 i = 0; i < count; ++i) {
            PlayParams p;
            const auto &sound = i % 3 == 0 ? stereo : (i % 3 == 1 ? mono : shortclip);
            p.source = std::make_shared<BufferSource>(sound, 48000, i % 4 == 0);
            p.spatial = i % 5 != 0;
            p.pitch = 0.6f + 0.13f * static_cast<float>(i % 11);
            p.volume = 0.3f + 0.05f * static_cast<float>(i % 7);
            p.position = {static_cast<float>(i % 17) - 8.0f, static_cast<float>(i % 3) - 1.0f, -2.0f - static_cast<float>(i % 9)};
            engine.play(std::move(p));
        }
        Result result;
        const auto start = std::chrono::steady_clock::now();
        for (u32 b = 0; b < blocks; ++b) {
            engine.render_block();
            result.peak_compute = std::max(result.peak_compute, engine.stats().compute_voices);
            for (u32 c = 0; c < engine.bed(0).channels(); ++c) {
                result.samples.insert(result.samples.end(), engine.bed(0).data(c), engine.bed(0).data(c) + engine.config().block_frames);
            }
        }
        result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return result;
    }
} // namespace

int main() {
    std::string skip;
    auto gpu = make_gpu(skip);
    if (!gpu) {
        std::cout << "SKIP: " << skip << '\n';
        return 0;
    }
    auto mixer = SFT::AudioGpu::GpuComputeMixer::create(*gpu->device);
    if (!mixer) {
        std::cerr << "FAILED: could not create the GPU mixer: " << mixer.error() << '\n';
        return 1;
    }
    std::cout << "GPU mixer on " << (*mixer)->device_name() << '\n';
    std::shared_ptr<ComputeMixBackend> backend = std::move(*mixer);

    constexpr u32 voices = 900, blocks = 40;
    const Result cpu = render(nullptr, voices, blocks);
    const Result gpu_result = render(backend, voices, blocks);
    check(gpu_result.peak_compute > voices * 8 / 10, "most voices were mixed on the device");
    double worst = 0.0;
    check(cpu.samples.size() == gpu_result.samples.size(), "same amount of output");
    for (size_t i = 0; i < std::min(cpu.samples.size(), gpu_result.samples.size()); ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(cpu.samples[i] - gpu_result.samples[i])));
    }
    std::cout << "max difference " << worst << ", device voices " << gpu_result.peak_compute << ", cpu " << cpu.milliseconds << " ms, gpu " << gpu_result.milliseconds << " ms\n";
    check(worst < 1e-3, "GPU mixing matches CPU mixing");
    check(*std::max_element(cpu.samples.begin(), cpu.samples.end()) > 0.05f, "the scene is audible");

    // Throughput at a voice count that is routine for this mode (informational: timings depend on the machine).
    {
        constexpr u32 many = 20000;
        const Result cpu_many = render(nullptr, many, 30);
        const Result gpu_many = render(backend, many, 30);
        double diff = 0.0;
        for (size_t i = 0; i < std::min(cpu_many.samples.size(), gpu_many.samples.size()); ++i) {
            diff = std::max(diff, static_cast<double>(std::fabs(cpu_many.samples[i] - gpu_many.samples[i])));
        }
        std::cout << many << " voices: cpu " << cpu_many.milliseconds / 30.0 << " ms/block, gpu " << gpu_many.milliseconds / 30.0 << " ms/block, device voices "
                  << gpu_many.peak_compute << ", max difference " << diff << '\n';
        check(diff < 5e-3, "GPU mixing matches CPU mixing at 20000 voices");
    }

    backend.reset();
    if (failures == 0) {
        std::cout << "AudioGpuTest passed\n";
    }
    return failures == 0 ? 0 : 1;
}
