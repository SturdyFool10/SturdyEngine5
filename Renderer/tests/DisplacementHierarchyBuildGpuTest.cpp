// On-device checks for the displacement optimizations that have a shader half:
//
//   1. Shaders/heightfield_hierarchy_build.slang (HierarchyBuild::Compute): the GPU-built hierarchy must equal
//      HeightfieldHierarchy::pack() BYTE FOR BYTE — full builds and dirty-tile updates, Unorm16/Float32,
//      max-only/min-max, power-of-two and not, wrap and clamp — driven by the same pass list the CPU model
//      (Renderer/Displacement/HierarchyGpuBuild.hpp) executes.
//   2. The opt-in traversal defines SFT_HF_USE_GATHER and SFT_HF_START_LEVEL_FROM_FOOTPRINT through
//      Shaders/heightfield_optim_probe.slang: neither may change a hit relative to the baseline CPU trace, and
//      the gather must be independent of the sampler's address mode (checked with both Repeat and ClampToEdge).
//
// SKIPS (exit 0) when no device can be brought up. Vulkan by default; `webgpu` argument (RendererDisplacementHierarchyBuildGpuWebGpuTest,
// built with STURDY_ENABLE_WEBGPU) runs the same checks against Dawn/WGSL.
//
// Bring-up and buffer/texture plumbing follow DisplacementGpuParityTest.cpp.

#include <Renderer/Displacement/HeightfieldHierarchy.hpp>
#include <Renderer/Displacement/HeightfieldTrace.hpp>
#include <Renderer/Displacement/HierarchyGpuBuild.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/ShaderTarget.hpp>

#include <Core/Core.hpp>
#include <Core/EngineBackend.hpp>
#include <Core/Renderer.hpp>
#include <Foundation/LogSink.hpp>
#include <RHI/RHI.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>
#include <WindowManager/WindowManager.hpp>

#if defined(DISPLACEMENT_HB_WEBGPU)
#include <Core/WebGPU/RHI/WebGpuAdapter.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <glm/geometric.hpp>

namespace {

    namespace rhi = SFT::RHI;
    namespace slang = SFT::Core::Slang;
    namespace disp = SFT::Renderer::Displacement;
    using SFT::f32;
    using SFT::u32;
    using SFT::u64;
    using SFT::u8;
    using SFT::usize;

    /// WebGPU has no non-zero fill_buffer, so the 0xDEADBEEF poison pass is skipped there (fresh WebGPU buffers are
    /// zero-initialised, so an unwritten word still reads as 0 rather than as poison -- a weaker unwritten-word check).
    bool g_no_poison_fill = false;

    int g_failures = 0;
    int g_checks = 0;

    void fail(const std::string &message) {
        ++g_failures;
        std::cerr << "FAILED: " << message << '\n';
    }

    void expect(bool condition, const std::string &message) {
        ++g_checks;
        if (!condition) {
            fail(message);
        }
    }

    class ErrorWatch {
      public:
        ErrorWatch() {
            sink_ = SFT::Foundation::add_log_sink([this](SFT::Foundation::LogLevel level, std::string_view message) {
                if (level < SFT::Foundation::LogLevel::Error) {
                    return;
                }
                const std::lock_guard guard{mutex_};
                messages_.emplace_back(message);
            });
        }
        ~ErrorWatch() { SFT::Foundation::remove_log_sink(sink_); }
        ErrorWatch(const ErrorWatch &) = delete;
        ErrorWatch &operator=(const ErrorWatch &) = delete;
        std::vector<std::string> take() {
            const std::lock_guard guard{mutex_};
            return std::exchange(messages_, {});
        }

      private:
        SFT::Foundation::LogSinkId sink_{};
        std::mutex mutex_;
        std::vector<std::string> messages_;
    };

    struct Gpu {
        std::unique_ptr<SFT::WindowManager::WindowManager> windows;
        std::unique_ptr<SFT::Core::EngineBackend> vulkan_backend;
        std::unique_ptr<rhi::RhiInstance> instance;
        std::unique_ptr<rhi::RhiDevice> owned_device;
        rhi::RhiDevice *device = nullptr;
        std::string description;
        ~Gpu() {
            if (device != nullptr) {
                device->wait_idle();
            }
            owned_device.reset();
            vulkan_backend.reset();
            instance.reset();
            windows.reset();
        }
    };

    std::unique_ptr<Gpu> make_vulkan_gpu(std::string &skip_reason) {
        auto gpu = std::make_unique<Gpu>();
        gpu->windows = std::make_unique<SFT::WindowManager::WindowManager>();
        using SDL3Window = SFT::WindowManager::SDL3::SDL3Window;
        SFT::WindowManager::WindowConfig config{};
        config.title = "DisplacementHierarchyBuildGpuTest";
        config.extent = {64, 64};
        config.visible = false;
        config.graphics_api = SFT::WindowManager::WindowGraphicsApi::Vulkan;
        auto id = gpu->windows->spawn_window<SDL3Window>(config);
        if (!id) {
            skip_reason = "no window system: " + std::string{id.error().message};
            return nullptr;
        }
        gpu->vulkan_backend = SFT::Core::create_vulkan_backend();
        SFT::Core::RendererCreateInfo info{};
        info.backend = rhi::BackendType::Vulkan;
        info.app_name = "DisplacementHierarchyBuildGpuTest";
        info.enable_shader_disk_cache = false;
        std::optional<std::string> error;
        auto result = gpu->windows->with_window(*id, [&](SFT::WindowManager::Window &window) {
            info.window = &window;
            auto surface = gpu->vulkan_backend->initialize(info);
            if (!surface) {
                error = surface.error().message;
            }
            return true;
        });
        if (!result || error) {
            skip_reason = "Vulkan backend initialization failed: " + error.value_or("window vanished");
            return nullptr;
        }
        gpu->device = gpu->vulkan_backend->rhi_device();
        if (gpu->device == nullptr) {
            skip_reason = "Vulkan backend exposed no RHI device";
            return nullptr;
        }
        gpu->description = std::string{"Vulkan / "} + std::string{gpu->device->adapter_info().name};
        return gpu;
    }

#if defined(DISPLACEMENT_HB_WEBGPU)
    std::unique_ptr<Gpu> make_webgpu_gpu(std::string &skip_reason) {
        auto gpu = std::make_unique<Gpu>();
        const rhi::BackendRegistration registration = SFT::Core::WebGpu::webgpu_backend_registration();
        auto instance = registration.create_instance(rhi::InstanceDesc{
            .application_name = "DisplacementHierarchyBuildGpuTest", .enable_validation = true, .enable_debug_utils = true,
            .headless = true});
        if (!instance) {
            skip_reason = "no WebGPU instance (" + instance.error().message + ")";
            return nullptr;
        }
        gpu->instance = std::move(*instance);
        auto adapters = gpu->instance->enumerate_adapters();
        if (!adapters || adapters->empty()) {
            skip_reason = "the WebGPU instance enumerated no adapters";
            return nullptr;
        }
        rhi::DeviceRequest request{};
        request.label = "displacement hb device";
        auto device = (*adapters->front()).create_device(request);
        if (!device) {
            skip_reason = "no WebGPU device (" + device.error().message + ")";
            return nullptr;
        }
        gpu->owned_device = std::move(*device);
        gpu->device = gpu->owned_device.get();
        gpu->description = std::string{"WebGPU (Dawn) / "} + std::string{gpu->device->adapter_info().name};
        return gpu;
    }
#endif

    struct Cleanup {
        std::vector<std::function<void()>> steps;
        ~Cleanup() {
            for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
                (*it)();
            }
        }
    };

    u64 align_up(u64 v, u64 a) { return (v + a - 1) / a * a; }

    struct Session {
        rhi::RhiDevice &device;
        Cleanup cleanup;
        rhi::SamplerHandle repeat_sampler{};
        rhi::SamplerHandle clamp_sampler{};
        explicit Session(rhi::RhiDevice &d) : device(d) {}
    };

    struct Field {
        std::string name;
        std::vector<f32> heights;
        u32 width = 0;
        u32 height = 0;
        bool wrap = true;
        [[nodiscard]] disp::HeightfieldView view() const { return disp::HeightfieldView{heights, width, height, wrap}; }
    };

    Field make_noise(u32 w, u32 h, u32 seed, bool wrap, const char *name) {
        Field f{name, {}, w, h, wrap};
        f.heights.resize(static_cast<usize>(w) * h);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<f32> dist(0.0f, 1.0f);
        for (f32 &v : f.heights) {
            v = dist(rng);
        }
        return f;
    }

    Field make_sparse(u32 w, u32 h, bool wrap, const char *name) {
        Field f{name, {}, w, h, wrap};
        f.heights.assign(static_cast<usize>(w) * h, 0.1f);
        for (u32 i = 0; i < 6; ++i) {
            f.heights[static_cast<usize>((i * 53 + 5) % h) * w + (i * 37 + 11) % w] = 0.95f;
        }
        return f;
    }

    std::optional<rhi::TextureHandle> create_r32f_texture(Session &session, u32 width, u32 height,
                                                          const std::vector<std::vector<f32>> &mips, const char *label) {
        rhi::RhiDevice &device = session.device;
        auto texture = device.create_texture(rhi::TextureDesc{
            .dimension = rhi::TextureDimension::Dim2D,
            .format = rhi::Format::R32Float,
            .extent = rhi::Extent3D{.width = width, .height = height, .depth_or_layers = 1},
            .mip_levels = static_cast<u32>(mips.size()),
            .samples = rhi::SampleCount::X1,
            .usage = rhi::TextureUsage::TransferDst | rhi::TextureUsage::Sampled,
            .label = label,
        });
        if (!texture) {
            fail(std::format("create_texture({}) failed: {}", label, texture.error().message));
            return std::nullopt;
        }
        session.cleanup.steps.emplace_back([&device, t = *texture] { device.destroy_texture(t); });

        std::vector<std::byte> staging_bytes;
        std::vector<u64> offsets;
        std::vector<u32> pitches;
        for (u32 mip = 0; mip < mips.size(); ++mip) {
            const u32 mw = std::max(1u, width >> mip);
            const u32 mh = std::max(1u, height >> mip);
            const u32 pitch = static_cast<u32>(align_up(static_cast<u64>(mw) * 4, 256));
            const u64 base = align_up(staging_bytes.size(), 256);
            staging_bytes.resize(base + static_cast<u64>(pitch) * mh);
            offsets.push_back(base);
            pitches.push_back(pitch);
            for (u32 y = 0; y < mh; ++y) {
                std::memcpy(staging_bytes.data() + base + static_cast<u64>(y) * pitch,
                            mips[mip].data() + static_cast<usize>(y) * mw, static_cast<usize>(mw) * 4);
            }
        }
        auto staging = device.create_buffer(rhi::BufferDesc{.size = staging_bytes.size(),
                                                            .usage = rhi::BufferUsage::TransferSrc,
                                                            .memory = rhi::MemoryLocation::HostUpload,
                                                            .label = "hb staging"});
        if (!staging) {
            fail("create staging buffer failed: " + staging.error().message);
            return std::nullopt;
        }
        session.cleanup.steps.emplace_back([&device, b = *staging] { device.destroy_buffer(b); });
        if (auto r = device.write_buffer(*staging, 0, staging_bytes); !r) {
            fail("write staging buffer failed: " + r.error().message);
            return std::nullopt;
        }
        auto encoder = device.create_command_encoder(rhi::CommandEncoderDesc{.label = "hb upload"});
        if (!encoder) {
            fail("create_command_encoder failed: " + encoder.error().message);
            return std::nullopt;
        }
        const rhi::TextureBarrier to_dst{.texture = *texture, .src_stage = rhi::PipelineStage::None,
                                         .src_access = rhi::AccessFlags::None, .dst_stage = rhi::PipelineStage::Transfer,
                                         .dst_access = rhi::AccessFlags::TransferWrite,
                                         .old_layout = rhi::TextureLayout::Undefined,
                                         .new_layout = rhi::TextureLayout::TransferDst};
        (*encoder)->barrier({}, {}, std::span<const rhi::TextureBarrier>{&to_dst, 1});
        for (u32 mip = 0; mip < mips.size(); ++mip) {
            const u32 mw = std::max(1u, width >> mip);
            const u32 mh = std::max(1u, height >> mip);
            (*encoder)->copy_buffer_to_texture(
                *staging, *texture,
                rhi::BufferTextureCopy{.buffer_offset = offsets[mip], .buffer_row_length = pitches[mip] / 4,
                                       .buffer_image_height = mh, .mip_level = mip, .base_array_layer = 0,
                                       .array_layer_count = 1, .texture_offset = {0, 0, 0},
                                       .texture_extent = rhi::Extent3D{.width = mw, .height = mh, .depth_or_layers = 1}});
        }
        const rhi::TextureBarrier to_read{.texture = *texture, .src_stage = rhi::PipelineStage::Transfer,
                                          .src_access = rhi::AccessFlags::TransferWrite,
                                          .dst_stage = rhi::PipelineStage::ComputeShader,
                                          .dst_access = rhi::AccessFlags::ShaderRead,
                                          .old_layout = rhi::TextureLayout::TransferDst,
                                          .new_layout = rhi::TextureLayout::ShaderReadOnly};
        (*encoder)->barrier({}, {}, std::span<const rhi::TextureBarrier>{&to_read, 1});
        auto cb = (*encoder)->finish();
        if (!cb) {
            fail("finish upload failed: " + cb.error().message);
            return std::nullopt;
        }
        const rhi::CommandBufferHandle handles[] = {*cb};
        rhi::SubmitDesc submit{};
        submit.command_buffers = handles;
        submit.label = "hb upload";
        if (auto r = device.submit(submit); !r) {
            fail("submit upload failed: " + r.error().message);
            return std::nullopt;
        }
        device.wait_idle();
        return *texture;
    }

    std::optional<rhi::TextureViewHandle> create_view(Session &session, rhi::TextureHandle texture, const char *label) {
        auto view = session.device.create_texture_view(rhi::TextureViewDesc{
            .texture = texture, .view_type = rhi::TextureViewType::View2D, .format = rhi::Format::R32Float,
            .base_mip_level = 0, .mip_level_count = rhi::all_remaining, .base_array_layer = 0, .array_layer_count = 1,
            .label = label});
        if (!view) {
            fail(std::format("create_texture_view({}) failed: {}", label, view.error().message));
            return std::nullopt;
        }
        session.cleanup.steps.emplace_back([&session, v = *view] { session.device.destroy_texture_view(v); });
        return *view;
    }

    std::filesystem::path shaders_dir() {
        return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "Shaders";
    }

    struct Program {
        rhi::ShaderModuleHandle module{};
        rhi::BindGroupLayoutHandle bgl{};
        rhi::PipelineLayoutHandle layout{};
        rhi::ComputePipelineHandle pipeline{};
        std::vector<rhi::BindGroupLayoutEntry> entries;
        std::vector<SFT::Renderer::ReflectedResource> resources;
        std::vector<rhi::PushConstantRange> push_ranges;
        u64 uniform_size = 0;
        std::string entry;
    };

    std::unique_ptr<Program> build_program(Session &session, const std::string &file, const std::string &entry,
                                           const std::vector<slang::ShaderMacro> &macros, std::string &error) {
        rhi::RhiDevice &device = session.device;
        const auto target = SFT::Renderer::shader_target_for_device(device);
        if (!target) {
            error = target.error().message;
            return nullptr;
        }
        const std::filesystem::path path = shaders_dir() / (file + ".slang");
        slang::ShaderCompileOptions options{};
        options.targets = {target->slang_target};
        options.entry_points = {slang::ShaderEntryPointRequest{.name = entry, .stage = slang::ShaderStage::Compute}};
        options.search_paths = {shaders_dir().string()};
        options.macros = macros;
        slang::ShaderCompiler compiler;
        auto shader = compiler.compile(slang::ShaderSource::from_file(path.string(), file), options);
        if (!shader) {
            error = "compile " + file + " failed: " + shader.error().message + "\n" + shader.error().diagnostics;
            return nullptr;
        }
        auto code = shader->entry_point_code(std::string_view{entry}, target->slang_target.format);
        if (!code) {
            error = "entry_point_code failed: " + code.error().message;
            return nullptr;
        }
        auto program = std::make_unique<Program>();
        program->entry = entry;
        auto module = device.create_shader_module(rhi::ShaderModuleDesc{
            .language = target->module_language,
            .code = std::span<const std::byte>{code->bytes.data(), code->bytes.size()},
            .label = file.c_str()});
        if (!module) {
            error = "create_shader_module failed: " + module.error().message;
            return nullptr;
        }
        program->module = *module;
        session.cleanup.steps.emplace_back([&device, m = *module] { device.destroy_shader_module(m); });

        const slang::ShaderReflection &reflection = shader->reflection();
        const auto generated = SFT::Renderer::generate_bind_group_layouts(reflection, rhi::ShaderStage::Compute);
        if (generated.empty()) {
            error = "reflection produced no bind group layout";
            return nullptr;
        }
        program->entries = generated.front().entries;
        program->resources = SFT::Renderer::collect_resource_bindings(reflection);
        program->push_ranges = SFT::Renderer::generate_push_constant_ranges(reflection, rhi::ShaderStage::Compute);
        program->uniform_size = reflection.global_constant_buffer_size;

        auto bgl = device.create_bind_group_layout(rhi::BindGroupLayoutDesc{
            .entries = std::span<const rhi::BindGroupLayoutEntry>{program->entries}, .label = "hb bgl"});
        if (!bgl) {
            error = "create_bind_group_layout failed: " + bgl.error().message;
            return nullptr;
        }
        program->bgl = *bgl;
        session.cleanup.steps.emplace_back([&device, h = *bgl] { device.destroy_bind_group_layout(h); });
        const rhi::BindGroupLayoutHandle layouts[] = {*bgl};
        auto layout = device.create_pipeline_layout(rhi::PipelineLayoutDesc{
            .bind_group_layouts = layouts, .push_constant_ranges = program->push_ranges, .label = "hb layout"});
        if (!layout) {
            error = "create_pipeline_layout failed: " + layout.error().message;
            return nullptr;
        }
        program->layout = *layout;
        session.cleanup.steps.emplace_back([&device, h = *layout] { device.destroy_pipeline_layout(h); });
        auto pipeline = device.create_compute_pipeline(rhi::ComputePipelineDesc{
            .layout = *layout,
            .compute = rhi::ShaderEntry{.module = *module, .entry_point = program->entry.c_str(),
                                        .stage = rhi::ShaderStage::Compute},
            .label = "hb pipeline"});
        if (!pipeline) {
            error = "create_compute_pipeline failed: " + pipeline.error().message;
            return nullptr;
        }
        program->pipeline = *pipeline;
        session.cleanup.steps.emplace_back([&device, h = *pipeline] { device.destroy_compute_pipeline(h); });
        return program;
    }

    // ---------------------------------------------------------------------------------------------
    // 1. Hierarchy build
    // ---------------------------------------------------------------------------------------------

    /// A persistent build: height texture + word buffer, so dirty updates can be replayed on top of a full build.
    struct BuildRun {
        Session &session;
        const Program &program;
        disp::HierarchyBufferLayout layout;
        rhi::TextureHandle height_tex{};
        rhi::TextureViewHandle height_view{};
        rhi::BufferHandle words{};
        rhi::BufferHandle readback{};
        Cleanup local;
    };

    bool submit_and_wait(rhi::RhiDevice &device, rhi::CommandBufferHandle handle, const char *label) {
        const rhi::CommandBufferHandle handles[] = {handle};
        rhi::SubmitDesc submit{};
        submit.command_buffers = handles;
        submit.label = label;
        if (auto r = device.submit(submit); !r) {
            fail(std::string{"submit "} + label + ": " + r.error().message);
            return false;
        }
        device.wait_idle();
        return true;
    }

    /// Replays a pass list: one compute pass per level-run, a buffer barrier between runs, readback at the end.
    std::optional<std::vector<u8>> run_passes(BuildRun &run, const std::vector<disp::HierarchyBuildPass> &passes,
                                              bool poison_first) {
        rhi::RhiDevice &device = run.session.device;
        auto encoder = device.create_command_encoder(rhi::CommandEncoderDesc{.label = "hb build"});
        if (!encoder) {
            fail("create_command_encoder: " + encoder.error().message);
            return std::nullopt;
        }
        const u64 total = run.layout.total_bytes;
        if (poison_first && !g_no_poison_fill) {
            (*encoder)->fill_buffer(run.words, 0, total, 0xDEADBEEFu);
            const rhi::BufferBarrier b{.buffer = run.words, .src_stage = rhi::PipelineStage::Transfer,
                                       .src_access = rhi::AccessFlags::TransferWrite,
                                       .dst_stage = rhi::PipelineStage::ComputeShader,
                                       .dst_access = rhi::AccessFlags::ShaderWrite | rhi::AccessFlags::ShaderRead,
                                       .offset = 0, .size = total};
            (*encoder)->barrier({}, std::span<const rhi::BufferBarrier>{&b, 1}, {});
        }

        auto group = device.create_bind_group(rhi::BindGroupDesc{
            .layout = run.program.bgl,
            .entries = [&] {
                std::vector<rhi::BindGroupEntry> entries;
                for (const auto &entry : run.program.entries) {
                    rhi::BindGroupEntry g{.binding = entry.binding};
                    if (entry.type == rhi::BindingType::CombinedImageSampler) {
                        g.texture_view = run.height_view;
                        g.sampler = run.session.clamp_sampler;
                    } else if (entry.type == rhi::BindingType::SampledTexture) {
                        g.texture_view = run.height_view;
                    } else if (entry.type == rhi::BindingType::Sampler) {
                        g.sampler = run.session.clamp_sampler;
                    } else {
                        g.buffer = run.words;
                        g.size = total;
                    }
                    entries.push_back(g);
                }
                return entries;
            }(),
            .lifetime = rhi::BindGroupLifetime::Persistent, .label = "hb group"});
        if (!group) {
            fail("create_bind_group: " + group.error().message);
            return std::nullopt;
        }
        run.local.steps.emplace_back([&device, h = *group] { device.destroy_bind_group(h); });

        u32 current_level = 0;
        std::unique_ptr<rhi::ComputePassEncoder> pass_owner;
        rhi::ComputePassEncoder *pass = nullptr;
        auto end_pass = [&] {
            if (pass != nullptr) {
                pass->end();
                pass = nullptr;
                pass_owner.reset();
            }
        };
        for (const disp::HierarchyBuildPass &p : passes) {
            if (pass == nullptr || p.constants.level != current_level) {
                if (pass != nullptr) {
                    end_pass();
                    const rhi::BufferBarrier b{.buffer = run.words, .src_stage = rhi::PipelineStage::ComputeShader,
                                               .src_access = rhi::AccessFlags::ShaderWrite,
                                               .dst_stage = rhi::PipelineStage::ComputeShader,
                                               .dst_access = rhi::AccessFlags::ShaderRead | rhi::AccessFlags::ShaderWrite,
                                               .offset = 0, .size = total};
                    (*encoder)->barrier({}, std::span<const rhi::BufferBarrier>{&b, 1}, {});
                }
                auto begun = (*encoder)->begin_compute_pass(rhi::ComputePassDesc{.label = "hb level"});
                if (!begun) {
                    fail("begin_compute_pass: " + begun.error().message);
                    return std::nullopt;
                }
                pass_owner = std::move(*begun);
                pass = pass_owner.get();
                pass->set_pipeline(run.program.pipeline);
                pass->set_bind_group(0, *group);
                current_level = p.constants.level;
            }
            pass->set_push_constants(rhi::ShaderStage::Compute, 0,
                                     std::as_bytes(std::span{&p.constants, 1}));
            pass->dispatch(p.group_count_x, p.group_count_y, 1);
        }
        end_pass();

        const rhi::BufferBarrier to_copy{.buffer = run.words, .src_stage = rhi::PipelineStage::ComputeShader,
                                         .src_access = rhi::AccessFlags::ShaderWrite,
                                         .dst_stage = rhi::PipelineStage::Transfer,
                                         .dst_access = rhi::AccessFlags::TransferRead, .offset = 0, .size = total};
        (*encoder)->barrier({}, std::span<const rhi::BufferBarrier>{&to_copy, 1}, {});
        (*encoder)->copy_buffer_to_buffer(run.words, run.readback, rhi::BufferCopy{.src_offset = 0, .dst_offset = 0, .size = total});
        const rhi::BufferBarrier to_host{.buffer = run.readback, .src_stage = rhi::PipelineStage::Transfer,
                                         .src_access = rhi::AccessFlags::TransferWrite,
                                         .dst_stage = rhi::PipelineStage::Host, .dst_access = rhi::AccessFlags::HostRead,
                                         .offset = 0, .size = total};
        (*encoder)->barrier({}, std::span<const rhi::BufferBarrier>{&to_host, 1}, {});
        auto cb = (*encoder)->finish();
        if (!cb) {
            fail("finish: " + cb.error().message);
            return std::nullopt;
        }
        if (!submit_and_wait(device, *cb, "hb build")) {
            return std::nullopt;
        }
        auto mapped = device.map_buffer(run.readback);
        if (!mapped) {
            fail("map_buffer: " + mapped.error().message);
            return std::nullopt;
        }
        std::vector<u8> bytes(static_cast<usize>(total));
        std::memcpy(bytes.data(), mapped->data(), bytes.size());
        device.unmap_buffer(run.readback);
        return bytes;
    }

    void test_hierarchy_build(Session &session, const Program &program) {
        struct Format {
            disp::HierarchyPrecision p;
            disp::HierarchyChannels c;
            const char *name;
        };
        const Format formats[] = {{disp::HierarchyPrecision::Unorm16, disp::HierarchyChannels::MaxOnly, "u16/max"},
                                  {disp::HierarchyPrecision::Unorm16, disp::HierarchyChannels::MinMax, "u16/minmax"},
                                  {disp::HierarchyPrecision::Float32, disp::HierarchyChannels::MaxOnly, "f32/max"},
                                  {disp::HierarchyPrecision::Float32, disp::HierarchyChannels::MinMax, "f32/minmax"}};
        std::vector<Field> fields;
        fields.push_back(make_noise(64, 64, 1, true, "64x64 wrap"));
        fields.push_back(make_noise(37, 23, 2, true, "37x23 wrap (non-POT)"));
        fields.push_back(make_noise(37, 23, 3, false, "37x23 clamp (non-POT)"));
        fields.push_back(make_sparse(100, 3, true, "100x3 sparse"));
        fields.push_back(make_noise(1, 1, 4, true, "1x1"));
        fields.push_back(make_noise(2, 9, 5, false, "2x9 clamp"));
        fields.push_back(make_noise(256, 130, 6, true, "256x130 wrap"));
        // Values on and around the 16-bit grid, where rounding direction shows.
        for (Field &f : fields) {
            if (f.heights.size() > 4) {
                f.heights[0] = 0.0f;
                f.heights[1] = 1.0f;
                f.heights[2] = 1.0f / 65535.0f;
                f.heights[3] = 32768.0f / 65535.0f;
            }
        }

        for (Field &field : fields) {
            for (const Format &fmt : formats) {
                for (const u32 row_align : {4u, 256u}) {
                    Cleanup local;
                    disp::HierarchyBufferLayout layout =
                        disp::hierarchy_buffer_layout(field.width, field.height, fmt.p, fmt.c, row_align,
                                                      row_align == 256 ? 512u : 4u);
                    rhi::RhiDevice &device = session.device;
                    auto height = create_r32f_texture(session, field.width, field.height, {field.heights}, "hb height");
                    if (!height) {
                        return;
                    }
                    auto height_view = create_view(session, *height, "hb height view");
                    const u64 total = align_up(std::max<u64>(layout.total_bytes, 16), 16);
                    auto words = device.create_buffer(rhi::BufferDesc{
                        .size = total, .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc | rhi::BufferUsage::TransferDst,
                        .memory = rhi::MemoryLocation::DeviceLocal, .label = "hb words"});
                    auto readback = device.create_buffer(rhi::BufferDesc{.size = total, .usage = rhi::BufferUsage::TransferDst,
                                                                         .memory = rhi::MemoryLocation::HostReadback,
                                                                         .label = "hb readback"});
                    if (!words || !readback || !height_view) {
                        fail("buffer/view creation failed");
                        return;
                    }
                    local.steps.emplace_back([&device, w = *words, r = *readback] {
                        device.destroy_buffer(w);
                        device.destroy_buffer(r);
                    });
                    layout.total_bytes = total;
                    BuildRun run{session, program, layout, *height, *height_view, *words, *readback, {}};

                    const auto expected_of = [&](const Field &f) {
                        return disp::HeightfieldHierarchy::build(f.view()).pack(fmt.p, fmt.c);
                    };
                    const auto passes = disp::plan_hierarchy_build(layout, field.width, field.height, field.wrap, fmt.p, fmt.c);
                    auto bytes = run_passes(run, passes, true);
                    const std::string tag = std::format("{} {} align={}", field.name, fmt.name, row_align);
                    if (!bytes) {
                        return;
                    }
                    const std::vector<u8> tight = disp::repack_tight(layout, *bytes);
                    const disp::PackedHierarchy golden = expected_of(field);
                    const bool same = tight == golden.data;
                    if (!same) {
                        usize first = 0;
                        while (first < tight.size() && first < golden.data.size() && tight[first] == golden.data[first]) {
                            ++first;
                        }
                        std::cerr << "  first differing byte " << first << " of " << golden.data.size() << '\n';
                    }
                    expect(same, "GPU full build == pack(): " + tag);

                    // Dirty updates on top of the resident build: edit texels, re-upload the height texture,
                    // run only the dirty passes (no poison), expect a full rebuild's bytes.
                    if (row_align == 4 && field.width * field.height > 1) {
                        std::mt19937 rng(77);
                        for (u32 round = 0; round < 4; ++round) {
                            u32 x0 = static_cast<u32>(rng() % field.width);
                            u32 y0 = static_cast<u32>(rng() % field.height);
                            u32 x1 = std::min(field.width, x0 + 1 + static_cast<u32>(rng() % 5));
                            u32 y1 = std::min(field.height, y0 + 1 + static_cast<u32>(rng() % 5));
                            if (round == 1) {
                                x0 = 0;
                                y0 = 0;
                            }
                            if (round == 2) {
                                x0 = field.width - 1;
                                x1 = field.width;
                                y0 = field.height - 1;
                                y1 = field.height;
                            }
                            for (u32 y = y0; y < y1; ++y) {
                                for (u32 x = x0; x < x1; ++x) {
                                    field.heights[static_cast<usize>(y) * field.width + x] =
                                        static_cast<f32>(rng() % 10001) / 10000.0f;
                                }
                            }
                            auto height2 = create_r32f_texture(session, field.width, field.height, {field.heights}, "hb height2");
                            auto view2 = height2 ? create_view(session, *height2, "hb height2 view") : std::nullopt;
                            if (!view2) {
                                return;
                            }
                            run.height_tex = *height2;
                            run.height_view = *view2;
                            const auto dirty_passes = disp::plan_hierarchy_build(
                                layout, field.width, field.height, field.wrap, fmt.p, fmt.c,
                                disp::HierarchyDirtyTexels{x0, y0, x1, y1});
                            auto dirty_bytes = run_passes(run, dirty_passes, false);
                            if (!dirty_bytes) {
                                return;
                            }
                            expect(disp::repack_tight(layout, *dirty_bytes) == expected_of(field).data,
                                   std::format("GPU dirty update == full rebuild: {} round {} rect [{},{})x[{},{})", tag,
                                               round, x0, x1, y0, y1));
                        }
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // 2. Optional traversal defines
    // ---------------------------------------------------------------------------------------------

    struct OptimSettings {
        std::int32_t tex_w, tex_h;
        u32 max_steps, hierarchy_levels;
        f32 height_scale, reference_height;
        f32 tile_x, tile_y;
        u32 wrap, use_hierarchy, ray_count, start_level;
    };
    static_assert(sizeof(OptimSettings) == 48);

    struct GpuRay {
        f32 origin_t[4];
        f32 direction[4];
    };
    struct alignas(8) GpuHit {
        u32 status;
        f32 t;
        alignas(8) f32 uv[2];
        f32 height;
        alignas(8) f32 gradient[2];
        f32 residual;
        u32 steps;
    };
    static_assert(sizeof(GpuHit) == 40);

    std::optional<std::vector<GpuHit>> run_optim_probe(Session &session, const Program &program, const Field &field,
                                                      const disp::HeightfieldHierarchy &hierarchy,
                                                      rhi::SamplerHandle sampler, u32 start_level, bool use_hierarchy,
                                                      const std::vector<GpuRay> &rays) {
        rhi::RhiDevice &device = session.device;
        Cleanup local;
        auto height = create_r32f_texture(session, field.width, field.height, {field.heights}, "op height");
        if (!height) {
            return std::nullopt;
        }
        auto hv = create_view(session, *height, "op height view");
        const disp::PackedHierarchy packed = hierarchy.pack(disp::HierarchyPrecision::Float32, disp::HierarchyChannels::MaxOnly);
        std::vector<std::vector<f32>> mips;
        for (u32 mip = 0; mip < packed.mip_count; ++mip) {
            std::vector<f32> level(static_cast<usize>(std::max(1u, packed.width >> mip)) * std::max(1u, packed.height >> mip));
            std::memcpy(level.data(), packed.data.data() + packed.mip_offsets[mip], level.size() * 4);
            mips.push_back(std::move(level));
        }
        auto hier = create_r32f_texture(session, packed.width, packed.height, mips, "op hierarchy");
        if (!hier || !hv) {
            return std::nullopt;
        }
        auto hierv = create_view(session, *hier, "op hierarchy view");
        if (!hierv) {
            return std::nullopt;
        }

        const u64 hits_size = static_cast<u64>(rays.size()) * sizeof(GpuHit);
        auto make = [&](u64 size, rhi::BufferUsage usage, rhi::MemoryLocation mem, const char *label) {
            auto b = device.create_buffer(rhi::BufferDesc{.size = size, .usage = usage, .memory = mem, .label = label});
            if (b) {
                local.steps.emplace_back([&device, h = *b] { device.destroy_buffer(h); });
            }
            return b;
        };
        auto uniform = make(64, rhi::BufferUsage::Uniform | rhi::BufferUsage::TransferDst, rhi::MemoryLocation::HostUpload, "op settings");
        auto input = make(rays.size() * sizeof(GpuRay), rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst,
                          rhi::MemoryLocation::HostUpload, "op rays");
        auto out = make(hits_size, rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc, rhi::MemoryLocation::DeviceLocal, "op hits");
        auto readback = make(hits_size, rhi::BufferUsage::TransferDst, rhi::MemoryLocation::HostReadback, "op readback");
        if (!uniform || !input || !out || !readback) {
            fail("op buffers");
            return std::nullopt;
        }
        OptimSettings s{};
        s.tex_w = static_cast<std::int32_t>(field.width);
        s.tex_h = static_cast<std::int32_t>(field.height);
        s.max_steps = 4096;
        s.hierarchy_levels = hierarchy.level_count();
        s.height_scale = 0.3f;
        s.reference_height = 1.0f;
        s.tile_x = s.tile_y = 1.0f;
        s.wrap = field.wrap ? 1u : 0u;
        s.use_hierarchy = use_hierarchy ? 1u : 0u;
        s.ray_count = static_cast<u32>(rays.size());
        s.start_level = start_level;
        std::vector<std::byte> settings_bytes(64);
        std::memcpy(settings_bytes.data(), &s, sizeof(s));
        if (!device.write_buffer(*uniform, 0, settings_bytes) ||
            !device.write_buffer(*input, 0, std::as_bytes(std::span<const GpuRay>{rays}))) {
            fail("op write");
            return std::nullopt;
        }

        std::vector<rhi::BindGroupEntry> entries;
        for (const auto &entry : program.entries) {
            std::string name;
            for (const auto &res : program.resources) {
                if (res.binding == entry.binding) {
                    name = res.name;
                }
            }
            rhi::BindGroupEntry g{.binding = entry.binding};
            if (entry.type == rhi::BindingType::UniformBuffer) {
                g.buffer = *uniform;
                g.size = 64;
            } else if (entry.type == rhi::BindingType::CombinedImageSampler) {
                g.texture_view = name.find("hierarchy") != std::string::npos ? *hierv : *hv;
                g.sampler = sampler;
            } else if (entry.type == rhi::BindingType::SampledTexture) {
                g.texture_view = name.find("hierarchy") != std::string::npos ? *hierv : *hv;
            } else if (entry.type == rhi::BindingType::Sampler) {
                g.sampler = sampler;
            } else {
                const bool is_hits = name.find("hits") != std::string::npos;
                g.buffer = is_hits ? *out : *input;
                g.size = is_hits ? hits_size : rays.size() * sizeof(GpuRay);
            }
            entries.push_back(g);
        }
        auto group = device.create_bind_group(rhi::BindGroupDesc{.layout = program.bgl, .entries = entries,
                                                                 .lifetime = rhi::BindGroupLifetime::Persistent,
                                                                 .label = "op group"});
        if (!group) {
            fail("op bind group: " + group.error().message);
            return std::nullopt;
        }
        local.steps.emplace_back([&device, h = *group] { device.destroy_bind_group(h); });
        auto encoder = device.create_command_encoder(rhi::CommandEncoderDesc{.label = "op dispatch"});
        if (!encoder) {
            fail("op encoder");
            return std::nullopt;
        }
        {
            auto pass = (*encoder)->begin_compute_pass(rhi::ComputePassDesc{.label = "op probe"});
            if (!pass) {
                fail("op pass");
                return std::nullopt;
            }
            (*pass)->set_pipeline(program.pipeline);
            (*pass)->set_bind_group(0, *group);
            (*pass)->dispatch(static_cast<u32>((rays.size() + 63) / 64));
            (*pass)->end();
        }
        const rhi::BufferBarrier to_copy{.buffer = *out, .src_stage = rhi::PipelineStage::ComputeShader,
                                         .src_access = rhi::AccessFlags::ShaderWrite, .dst_stage = rhi::PipelineStage::Transfer,
                                         .dst_access = rhi::AccessFlags::TransferRead, .offset = 0, .size = hits_size};
        (*encoder)->barrier({}, std::span<const rhi::BufferBarrier>{&to_copy, 1}, {});
        (*encoder)->copy_buffer_to_buffer(*out, *readback, rhi::BufferCopy{.src_offset = 0, .dst_offset = 0, .size = hits_size});
        const rhi::BufferBarrier to_host{.buffer = *readback, .src_stage = rhi::PipelineStage::Transfer,
                                         .src_access = rhi::AccessFlags::TransferWrite, .dst_stage = rhi::PipelineStage::Host,
                                         .dst_access = rhi::AccessFlags::HostRead, .offset = 0, .size = hits_size};
        (*encoder)->barrier({}, std::span<const rhi::BufferBarrier>{&to_host, 1}, {});
        auto cb = (*encoder)->finish();
        if (!cb || !submit_and_wait(device, *cb, "op dispatch")) {
            return std::nullopt;
        }
        auto mapped = device.map_buffer(*readback);
        if (!mapped) {
            fail("op map");
            return std::nullopt;
        }
        std::vector<GpuHit> result(rays.size());
        std::memcpy(result.data(), mapped->data(), hits_size);
        device.unmap_buffer(*readback);
        return result;
    }

    void test_optimizations(Session &session, const std::string &variant_name,
                            const std::vector<slang::ShaderMacro> &macros, bool start_levels) {
        std::string error;
        auto program = build_program(session, "heightfield_optim_probe", "optimProbeMain", macros, error);
        if (!program) {
            fail("optim probe (" + variant_name + "): " + error);
            return;
        }
        std::vector<Field> fields;
        fields.push_back(make_noise(32, 32, 1, true, "32x32 wrap"));
        fields.push_back(make_sparse(64, 48, true, "sparse 64x48 wrap"));
        fields.push_back(make_noise(37, 23, 2, true, "37x23 wrap"));
        fields.push_back(make_sparse(64, 64, false, "sparse 64x64 clamp"));
        fields.push_back(make_noise(29, 41, 3, false, "29x41 clamp"));

        std::mt19937 rng(9);
        std::uniform_real_distribution<f32> uni(0.0f, 1.0f);
        for (const Field &field : fields) {
            const disp::HeightfieldHierarchy hier = disp::HeightfieldHierarchy::build(field.view());
            std::vector<GpuRay> rays;
            std::vector<disp::HeightfieldRay> cpu_rays;
            for (u32 i = 0; i < 256; ++i) {
                const glm::vec2 uv(0.1f + 0.8f * uni(rng), 0.1f + 0.8f * uni(rng));
                glm::vec3 v(uni(rng) * 2.0f - 1.0f, uni(rng) * 2.0f - 1.0f, 0.15f + 0.85f * uni(rng));
                v = glm::normalize(v);
                const disp::HeightfieldRay ray = disp::make_view_ray(uv, v, 0.3f, glm::vec2(1.0f), 1.0f);
                cpu_rays.push_back(ray);
                rays.push_back(GpuRay{{ray.origin.x, ray.origin.y, ray.origin.z, 0.0f},
                                      {ray.direction.x, ray.direction.y, ray.direction.z, ray.max_t}});
            }
            const u32 top = hier.level_count() - 1;
            struct Sampler {
                rhi::SamplerHandle handle;
                const char *name;
            };
            for (const Sampler &sampler : {Sampler{session.repeat_sampler, "repeat"}, Sampler{session.clamp_sampler, "clamp"}}) {
                for (u32 start = 0; start <= (start_levels ? top : 0u); ++start) {
                    const u32 pass_start = start_levels ? start : 64u;
                    auto hits = run_optim_probe(session, *program, field, hier, sampler.handle, pass_start, true, rays);
                    if (!hits) {
                        return;
                    }
                    u32 status_bad = 0;
                    u32 pos_bad = 0;
                    u32 step_equal = 0;
                    u32 compared = 0;
                    for (usize i = 0; i < rays.size(); ++i) {
                        // The reference is ALWAYS the baseline CPU trace (start at the top level).
                        const disp::HeightfieldHit ref = disp::trace_hierarchical(field.view(), hier, cpu_rays[i], 4096);
                        if (ref.status == disp::HitStatus::BudgetExhausted) {
                            continue;
                        }
                        ++compared;
                        const GpuHit &g = (*hits)[i];
                        // GPU HF_STATUS_*: 0 miss, 1 hit, 2 budget. CPU HitStatus: Hit, Miss, BudgetExhausted.
                        const u32 ref_status = ref.status == disp::HitStatus::Hit ? 1u : ref.status == disp::HitStatus::Miss ? 0u : 2u;
                        if (g.status != ref_status) {
                            ++status_bad;
                            continue;
                        }
                        if (ref.status == disp::HitStatus::Hit &&
                            (std::abs(g.t - ref.t) > 1.0e-3f * std::max(1.0f, std::abs(ref.t)) ||
                             std::abs(g.uv[0] - ref.uv.x) > 1.0e-3f || std::abs(g.uv[1] - ref.uv.y) > 1.0e-3f)) {
                            ++pos_bad;
                        }
                        const disp::HeightfieldHit same_start = disp::trace_hierarchical(
                            field.view(), hier, cpu_rays[i], 4096, pass_start);
                        step_equal += g.steps == same_start.steps ? 1u : 0u;
                    }
                    expect(status_bad == 0 && pos_bad == 0,
                           std::format("[{}] {} sampler={} start={}: GPU vs baseline CPU hit: {} status / {} position mismatches of {}",
                                       variant_name, field.name, sampler.name, start_levels ? std::to_string(start) : "top",
                                       status_bad, pos_bad, compared));
                    if (start_levels && start == 0 && sampler.name[0] == 'r') {
                        std::cout << std::format("     [{}] {} start=0: GPU steps == CPU steps (same start) on {}/{}\n",
                                                 variant_name, field.name, step_equal, compared);
                    }
                }
            }
        }
    }

} // namespace

int main(int argc, char **argv) {
    const std::string backend = argc > 1 ? argv[1] : "vulkan";
    ErrorWatch watch;
    std::string skip_reason;
    std::unique_ptr<Gpu> gpu;
    if (backend == "webgpu") {
#if defined(DISPLACEMENT_HB_WEBGPU)
        gpu = make_webgpu_gpu(skip_reason);
#else
        skip_reason = "built without WebGPU support (STURDY_ENABLE_WEBGPU)";
#endif
    } else {
        gpu = make_vulkan_gpu(skip_reason);
    }
    if (!gpu) {
        std::cout << "SKIPPED: " << skip_reason << '\n';
        return 0;
    }
    g_no_poison_fill = backend == "webgpu";
    std::cout << "Device: " << gpu->description << '\n';
    (void)watch.take();

    Session session{*gpu->device};
    for (const bool repeat : {true, false}) {
        auto sampler = gpu->device->create_sampler(rhi::SamplerDesc{
            .min_filter = rhi::Filter::Nearest, .mag_filter = rhi::Filter::Nearest, .mipmap_mode = rhi::MipmapMode::Nearest,
            .address_u = repeat ? rhi::AddressMode::Repeat : rhi::AddressMode::ClampToEdge,
            .address_v = repeat ? rhi::AddressMode::Repeat : rhi::AddressMode::ClampToEdge,
            .address_w = repeat ? rhi::AddressMode::Repeat : rhi::AddressMode::ClampToEdge,
            .label = "hb sampler"});
        if (!sampler) {
            fail("create_sampler failed: " + sampler.error().message);
            return 1;
        }
        (repeat ? session.repeat_sampler : session.clamp_sampler) = *sampler;
        session.cleanup.steps.emplace_back([&session, s = *sampler] { session.device.destroy_sampler(s); });
    }

    {
        std::string error;
        auto program = build_program(session, "heightfield_hierarchy_build", "hierarchyBuildMain", {}, error);
        if (!program) {
            fail("hierarchy build shader: " + error);
        } else {
            test_hierarchy_build(session, *program);
            for (const std::string &m : watch.take()) {
                fail("API error during hierarchy build: " + m);
            }
        }
    }
    test_optimizations(session, "baseline", {}, false);
    test_optimizations(session, "gather", {{"SFT_HF_USE_GATHER", "1"}}, false);
    test_optimizations(session, "start-level", {{"SFT_HF_START_LEVEL_FROM_FOOTPRINT", "1"}}, true);
    test_optimizations(session, "gather+start-level",
                       {{"SFT_HF_USE_GATHER", "1"}, {"SFT_HF_START_LEVEL_FROM_FOOTPRINT", "1"}}, true);
    for (const std::string &m : watch.take()) {
        fail("API error logged: " + m);
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s) across " << g_checks << " checks\n";
        return 1;
    }
    std::cout << "DisplacementHierarchyBuildGpuTest passed (" << g_checks << " checks)\n";
    return 0;
}
