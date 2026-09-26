// GPU-vs-CPU parity for the displacement heightfield blocks.
//
// Shaders/sturdy_heightfield.slang was written by hand to mirror Renderer/Displacement/HeightfieldTrace.cpp.
// This test executes the shader on a real device and checks it against that CPU reference:
//
//   * hf_trace_pom / hf_trace_grid (cell-exact and hierarchical) via Shaders/heightfield_trace_probe.slang,
//   * hf_sample_height / hf_sample_gradient / hf_parallax / hf_trace_shadow via
//     Shaders/heightfield_query_probe.slang,
//
// on rolling / noisy / non-square / non-POT / sparse fields with wrap and clamp addressing. Shaders are
// compiled through the engine's own Slang compiler for the device's native target (SPIR-V on Vulkan,
// WGSL on WebGPU/Dawn) and dispatched through the RHI.
//
// The test SKIPS (exit 0, message) when no device can be brought up: no display/window system, no Vulkan
// driver, or (WebGPU build) no Dawn adapter.
//
// Backends:  DisplacementGpuParityTest [vulkan|webgpu]     (default vulkan)
//   The WebGPU flavour is only built with STURDY_ENABLE_WEBGPU (see Renderer/CMakeLists.txt), as
//   RendererDisplacementGpuParityWebGpuTest.
//
// ---------------------------------------------------------------------------------------------------
// HOW TO ADD A VARIANT (re-verify a shader change, e.g. an opt-in define in sturdy_heightfield.slang)
// ---------------------------------------------------------------------------------------------------
// Append one entry to `variants()` below:
//
//     Variant{.name = "gather-corners",                      // shown in the report
//             .macros = {{"SFT_HF_GATHER_CORNERS", "1"}},    // extra preprocessor defines for BOTH probes
//             .hierarchy = HierarchyStorage::Unorm16,        // optional: see enum
//             .quantize_heights = false,                     // optional
//             .exact_steps = true}                           // expect GPU step count == CPU step count
//
// Every variant is run against every field, every algorithm in {POM, CellExact, Hierarchical} and every
// query mode, and must agree with the *baseline CPU reference* -- a default-OFF optimization is by
// definition not allowed to change the answer. SFT_HF_ALGORITHM is set by the harness per algorithm;
// do not put it in `macros`. If a variant needs new probe inputs (a start level, say), extend the
// settings structs in both probe .slang files and in `SettingsBlock` below (the two probe structs and
// SettingsBlock must stay layout-identical, std140), then teach `run_traces`/`run_queries` to fill them
// and the CPU call there to pass the matching reference parameter (e.g. start_level).
// ---------------------------------------------------------------------------------------------------

#include <Renderer/Displacement/HeightfieldHierarchy.hpp>
#include <Renderer/Displacement/HeightfieldTrace.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/ShaderTarget.hpp>

#include <Core/Core.hpp>
#include <Core/EngineBackend.hpp>
#include <Core/Renderer.hpp>
#include <Foundation/LogSink.hpp>
#include <RHI/RHI.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>
#include <WindowManager/WindowManager.hpp>

#if defined(DISPLACEMENT_PARITY_WEBGPU)
#include <Core/WebGPU/RHI/WebGpuAdapter.hpp>
#endif

#include <algorithm>
#include <array>
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
    using SFT::usize;
    using SFT::u8;
    using SFT::u16;

    // ---------------------------------------------------------------------------------------------
    // Variants
    // ---------------------------------------------------------------------------------------------

    enum class HierarchyStorage {
        /// Packed as Float32, uploaded as R32Float: bit-exact with the CPU hierarchy, so step counts can match.
        Float32,
        /// Packed as Unorm16 (min down / max up rounding) and uploaded dequantized into R32Float. The RHI has
        /// no R16Unorm format, so this is the closest a test can get to exercising the 16-bit conservative
        /// rounding on a real device: the GPU sees exactly the values an R16 texture would return.
        Unorm16,
    };

    struct Variant {
        std::string name;
        std::vector<slang::ShaderMacro> macros;
        HierarchyStorage hierarchy = HierarchyStorage::Float32;
        /// Heights are rounded to 16 bits before *both* sides see them (what an R16Unorm height texture holds).
        bool quantize_heights = false;
        /// GPU step counts must equal the CPU's (only meaningful when the hierarchy is bit-exact).
        bool exact_steps = true;
        /// Hierarchy level the hierarchical walk starts at (needs SFT_HF_START_LEVEL_FROM_FOOTPRINT in `macros`);
        /// also handed to the CPU reference. Default = top level (original behaviour).
        u32 start_level = disp::kStartAtTopLevel;
    };

    std::vector<Variant> variants() {
        return {
            Variant{.name = "baseline", .macros = {}},
            Variant{.name = "r16-emulated", .macros = {}, .hierarchy = HierarchyStorage::Unorm16,
                    .quantize_heights = true, .exact_steps = false},
            // D-owned optional optimizations: neither may change a hit versus the baseline CPU reference.
            Variant{.name = "gather", .macros = {{"SFT_HF_USE_GATHER", "1"}}},
            Variant{.name = "start-level-1", .macros = {{"SFT_HF_START_LEVEL_FROM_FOOTPRINT", "1"}}, .start_level = 1},
            Variant{.name = "start-level-0", .macros = {{"SFT_HF_START_LEVEL_FROM_FOOTPRINT", "1"}}, .start_level = 0},
            // <-- add new variants here (see the header comment)
        };
    }

    // ---------------------------------------------------------------------------------------------
    // Reporting
    // ---------------------------------------------------------------------------------------------

    int g_failures = 0;
    int g_checks = 0;

    void fail(const std::string &message) {
        ++g_failures;
        std::cerr << "FAILED: " << message << '\n';
    }

    /// Collects error-level log lines (Vulkan validation / Dawn validation) so a run that "worked" but
    /// tripped the API's own validation still fails.
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

        /// Returns and clears everything logged since the last call.
        std::vector<std::string> take() {
            const std::lock_guard guard{mutex_};
            return std::exchange(messages_, {});
        }

      private:
        SFT::Foundation::LogSinkId sink_{};
        std::mutex mutex_;
        std::vector<std::string> messages_;
    };

    // ---------------------------------------------------------------------------------------------
    // Device bring-up
    // ---------------------------------------------------------------------------------------------

    struct Gpu {
        // Destruction order matters: the device before the window system that hosts its surface.
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

    /// Vulkan: the engine's Vulkan backend still wants a window to pick a presentation-capable queue, so
    /// this makes a small hidden SDL3 window. No window system (CI without a display) means SKIP.
    std::unique_ptr<Gpu> make_vulkan_gpu(std::string &skip_reason) {
        auto gpu = std::make_unique<Gpu>();
        gpu->windows = std::make_unique<SFT::WindowManager::WindowManager>();
        using SDL3Window = SFT::WindowManager::SDL3::SDL3Window;
        SFT::WindowManager::WindowConfig config{};
        config.title = "DisplacementGpuParityTest";
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
        info.app_name = "DisplacementGpuParityTest";
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

#if defined(DISPLACEMENT_PARITY_WEBGPU)
    std::unique_ptr<Gpu> make_webgpu_gpu(std::string &skip_reason) {
        auto gpu = std::make_unique<Gpu>();
        const rhi::BackendRegistration registration = SFT::Core::WebGpu::webgpu_backend_registration();
        auto instance = registration.create_instance(rhi::InstanceDesc{
            .application_name = "DisplacementGpuParityTest",
            .enable_validation = true,
            .enable_debug_utils = true,
            .headless = true,
        });
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
        request.label = "displacement parity device";
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

    // ---------------------------------------------------------------------------------------------
    // Small RHI helpers
    // ---------------------------------------------------------------------------------------------

    struct Cleanup {
        std::vector<std::function<void()>> steps;
        ~Cleanup() {
            for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
                (*it)();
            }
        }
    };

    template <typename T>
    std::span<const std::byte> bytes_of(const std::vector<T> &v) {
        return std::as_bytes(std::span<const T>{v});
    }

    u64 align_up(u64 v, u64 a) { return (v + a - 1) / a * a; }

    /// The shader-visible uniform blocks. Layout-identical to the `ProbeSettings` / `QuerySettings` structs in the
    /// probe .slang files (std140: int2/float2 8-aligned; 48 bytes).
    struct SettingsBlock {
        std::int32_t tex_w = 0, tex_h = 0;
        u32 max_steps = 0;
        u32 hierarchy_levels = 0;
        f32 height_scale = 0;
        f32 reference_height = 0;
        f32 tile_x = 0, tile_y = 0;
        u32 wrap = 0;
        u32 field_a = 0; // ProbeSettings.pomSteps  / QuerySettings.mode
        u32 field_b = 0; // ProbeSettings.pomRefinements / QuerySettings.queryCount
        u32 field_c = 0; // ProbeSettings.rayCount / QuerySettings.pad0
        u32 start_level = 64; // ProbeSettings.startLevel (trace probe only; the query probe's block ends before it)
    };
    static_assert(sizeof(SettingsBlock) == 52);

    struct GpuRay {
        f32 origin_t[4];
        f32 direction[4];
    };

    /// std430 layout of HeightfieldHit (uint, float, float2, float, float2, float, uint => 40 bytes).
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

    struct Field {
        std::string name;
        std::vector<f32> heights; // final values both sides see (already quantized when the variant asks)
        u32 width = 0;
        u32 height = 0;
        bool wrap = true;
        f32 height_scale = 0.35f;
        glm::vec2 tile_size{1.0f, 1.0f};
        [[nodiscard]] disp::HeightfieldView view() const { return disp::HeightfieldView{heights, width, height, wrap}; }
    };

    /// A height texture (mip 0 only) + hierarchy texture (mip chain) resident on the device.
    struct GpuField {
        rhi::TextureHandle height_tex{};
        rhi::TextureViewHandle height_view{};
        rhi::TextureHandle hier_tex{};
        rhi::TextureViewHandle hier_view{};
        u32 hierarchy_levels = 0;
    };

    struct Session {
        rhi::RhiDevice &device;
        Cleanup cleanup;
        rhi::SamplerHandle sampler{};
        explicit Session(rhi::RhiDevice &d) : device(d) {}
    };

    /// Uploads every mip of an R32Float texture through a padded staging buffer (rows padded to 256 bytes,
    /// which WebGPU and D3D12 require and Vulkan tolerates).
    std::optional<rhi::TextureHandle> create_r32f_texture(Session &session, u32 width, u32 height,
                                                          const std::vector<std::vector<f32>> &mips,
                                                          const char *label) {
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

        // Stage all mips into one buffer.
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
        auto staging = device.create_buffer(rhi::BufferDesc{
            .size = staging_bytes.size(), .usage = rhi::BufferUsage::TransferSrc,
            .memory = rhi::MemoryLocation::HostUpload, .label = "parity staging"});
        if (!staging) {
            fail("create staging buffer failed: " + staging.error().message);
            return std::nullopt;
        }
        session.cleanup.steps.emplace_back([&device, b = *staging] { device.destroy_buffer(b); });
        if (auto r = device.write_buffer(*staging, 0, staging_bytes); !r) {
            fail("write staging buffer failed: " + r.error().message);
            return std::nullopt;
        }

        auto encoder = device.create_command_encoder(rhi::CommandEncoderDesc{.label = "parity upload"});
        if (!encoder) {
            fail("create_command_encoder failed: " + encoder.error().message);
            return std::nullopt;
        }
        const rhi::TextureBarrier to_dst{
            .texture = *texture, .src_stage = rhi::PipelineStage::None, .src_access = rhi::AccessFlags::None,
            .dst_stage = rhi::PipelineStage::Transfer, .dst_access = rhi::AccessFlags::TransferWrite,
            .old_layout = rhi::TextureLayout::Undefined, .new_layout = rhi::TextureLayout::TransferDst};
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
        const rhi::TextureBarrier to_read{
            .texture = *texture, .src_stage = rhi::PipelineStage::Transfer, .src_access = rhi::AccessFlags::TransferWrite,
            .dst_stage = rhi::PipelineStage::ComputeShader, .dst_access = rhi::AccessFlags::ShaderRead,
            .old_layout = rhi::TextureLayout::TransferDst, .new_layout = rhi::TextureLayout::ShaderReadOnly};
        (*encoder)->barrier({}, {}, std::span<const rhi::TextureBarrier>{&to_read, 1});
        auto cb = (*encoder)->finish();
        if (!cb) {
            fail("finish upload failed: " + cb.error().message);
            return std::nullopt;
        }
        const rhi::CommandBufferHandle handles[] = {*cb};
        rhi::SubmitDesc submit{};
        submit.command_buffers = handles;
        submit.label = "parity upload";
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
            .base_mip_level = 0, .mip_level_count = rhi::all_remaining, .base_array_layer = 0,
            .array_layer_count = 1, .label = label});
        if (!view) {
            fail(std::format("create_texture_view({}) failed: {}", label, view.error().message));
            return std::nullopt;
        }
        session.cleanup.steps.emplace_back([&session, v = *view] { session.device.destroy_texture_view(v); });
        return *view;
    }

    std::optional<GpuField> upload_field(Session &session, const Field &field, const disp::HeightfieldHierarchy &hierarchy,
                                         HierarchyStorage storage) {
        GpuField out;
        auto height = create_r32f_texture(session, field.width, field.height, {field.heights}, "parity height");
        if (!height) {
            return std::nullopt;
        }
        out.height_tex = *height;
        auto hv = create_view(session, *height, "parity height view");
        if (!hv) {
            return std::nullopt;
        }
        out.height_view = *hv;

        // Hierarchy: packed mip m holds hierarchy level m + 1 (level 0 is not stored).
        const bool unorm = storage == HierarchyStorage::Unorm16;
        const disp::PackedHierarchy packed = hierarchy.pack(
            unorm ? disp::HierarchyPrecision::Unorm16 : disp::HierarchyPrecision::Float32, disp::HierarchyChannels::MaxOnly);
        std::vector<std::vector<f32>> mips;
        for (u32 mip = 0; mip < packed.mip_count; ++mip) {
            const u32 mw = std::max(1u, packed.width >> mip);
            const u32 mh = std::max(1u, packed.height >> mip);
            std::vector<f32> level(static_cast<usize>(mw) * mh);
            const u8 *src = packed.data.data() + packed.mip_offsets[mip];
            for (usize i = 0; i < level.size(); ++i) {
                if (unorm) {
                    u16 q;
                    std::memcpy(&q, src + i * 2, 2);
                    level[i] = static_cast<f32>(q) / 65535.0f;
                } else {
                    std::memcpy(&level[i], src + i * 4, 4);
                }
            }
            mips.push_back(std::move(level));
        }
        auto hier = create_r32f_texture(session, packed.width, packed.height, mips, "parity hierarchy");
        if (!hier) {
            return std::nullopt;
        }
        out.hier_tex = *hier;
        auto hierv = create_view(session, *hier, "parity hierarchy view");
        if (!hierv) {
            return std::nullopt;
        }
        out.hier_view = *hierv;
        out.hierarchy_levels = hierarchy.level_count();
        return out;
    }

    // ---------------------------------------------------------------------------------------------
    // Shader program (engine Slang compiler -> RHI compute pipeline)
    // ---------------------------------------------------------------------------------------------

    struct Program {
        rhi::ShaderModuleHandle module{};
        rhi::BindGroupLayoutHandle bgl{};
        rhi::PipelineLayoutHandle layout{};
        rhi::ComputePipelineHandle pipeline{};
        std::vector<rhi::BindGroupLayoutEntry> entries;
        std::vector<SFT::Renderer::ReflectedResource> resources;
        u64 uniform_size = 0;
        std::string entry;
    };

    std::filesystem::path shaders_dir() {
        return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "Shaders";
    }

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
            .label = "parity probe"});
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
        program->uniform_size = reflection.global_constant_buffer_size;

        auto bgl = device.create_bind_group_layout(rhi::BindGroupLayoutDesc{
            .entries = std::span<const rhi::BindGroupLayoutEntry>{program->entries}, .label = "parity bgl"});
        if (!bgl) {
            error = "create_bind_group_layout failed: " + bgl.error().message;
            return nullptr;
        }
        program->bgl = *bgl;
        session.cleanup.steps.emplace_back([&device, h = *bgl] { device.destroy_bind_group_layout(h); });

        const rhi::BindGroupLayoutHandle layouts[] = {*bgl};
        auto layout = device.create_pipeline_layout(rhi::PipelineLayoutDesc{
            .bind_group_layouts = layouts, .push_constant_ranges = {}, .label = "parity layout"});
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
            .label = "parity pipeline"});
        if (!pipeline) {
            error = "create_compute_pipeline failed: " + pipeline.error().message;
            return nullptr;
        }
        program->pipeline = *pipeline;
        session.cleanup.steps.emplace_back([&device, h = *pipeline] { device.destroy_compute_pipeline(h); });
        return program;
    }

    /// Dispatches `program` over `count` inputs and returns the hits. `inputs` is the rays/queries buffer.
    std::optional<std::vector<GpuHit>> dispatch(Session &session, const Program &program, const GpuField &field,
                                                const SettingsBlock &settings, const std::vector<GpuRay> &inputs,
                                                std::string &error) {
        rhi::RhiDevice &device = session.device;
        Cleanup local;
        const u64 hits_size = static_cast<u64>(inputs.size()) * sizeof(GpuHit);

        auto make_buffer = [&](u64 size, rhi::BufferUsage usage, rhi::MemoryLocation memory,
                               const char *label) -> std::optional<rhi::BufferHandle> {
            auto b = device.create_buffer(rhi::BufferDesc{.size = size, .usage = usage, .memory = memory, .label = label});
            if (!b) {
                error = std::format("create_buffer({}) failed: {}", label, b.error().message);
                return std::nullopt;
            }
            local.steps.emplace_back([&device, h = *b] { device.destroy_buffer(h); });
            return *b;
        };
        auto uniform = make_buffer(align_up(std::max<u64>(program.uniform_size, sizeof(SettingsBlock)), 16),
                                   rhi::BufferUsage::Uniform | rhi::BufferUsage::TransferDst, rhi::MemoryLocation::HostUpload,
                                   "parity settings");
        auto input = make_buffer(inputs.size() * sizeof(GpuRay), rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst,
                                 rhi::MemoryLocation::HostUpload, "parity inputs");
        auto out = make_buffer(hits_size, rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc,
                               rhi::MemoryLocation::DeviceLocal, "parity hits");
        auto readback = make_buffer(hits_size, rhi::BufferUsage::TransferDst, rhi::MemoryLocation::HostReadback,
                                    "parity readback");
        if (!uniform || !input || !out || !readback) {
            return std::nullopt;
        }
        if (auto r = device.write_buffer(*uniform, 0, std::as_bytes(std::span{&settings, 1})); !r) {
            error = "write settings: " + r.error().message;
            return std::nullopt;
        }
        if (auto r = device.write_buffer(*input, 0, bytes_of(inputs)); !r) {
            error = "write inputs: " + r.error().message;
            return std::nullopt;
        }

        // Bind group by reflected name.
        std::vector<rhi::BindGroupEntry> group_entries;
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
                g.size = align_up(std::max<u64>(program.uniform_size, sizeof(SettingsBlock)), 16);
            } else if (entry.type == rhi::BindingType::CombinedImageSampler) {
                g.texture_view = name.find("hierarchy") != std::string::npos ? field.hier_view : field.height_view;
                g.sampler = session.sampler;
            } else if (entry.type == rhi::BindingType::SampledTexture) {
                g.texture_view = name.find("hierarchy") != std::string::npos ? field.hier_view : field.height_view;
            } else if (entry.type == rhi::BindingType::Sampler) {
                g.sampler = session.sampler;
            } else if (entry.type == rhi::BindingType::StorageBuffer || entry.type == rhi::BindingType::ReadOnlyStorageBuffer) {
                const bool is_hits = name.find("hits") != std::string::npos;
                g.buffer = is_hits ? *out : *input;
                g.size = is_hits ? hits_size : inputs.size() * sizeof(GpuRay);
            } else {
                error = "unexpected binding type in probe layout";
                return std::nullopt;
            }
            group_entries.push_back(g);
        }
        auto group = device.create_bind_group(rhi::BindGroupDesc{
            .layout = program.bgl, .entries = group_entries, .lifetime = rhi::BindGroupLifetime::Persistent,
            .label = "parity group"});
        if (!group) {
            error = "create_bind_group failed: " + group.error().message;
            return std::nullopt;
        }
        local.steps.emplace_back([&device, h = *group] { device.destroy_bind_group(h); });

        auto encoder = device.create_command_encoder(rhi::CommandEncoderDesc{.label = "parity dispatch"});
        if (!encoder) {
            error = "create_command_encoder: " + encoder.error().message;
            return std::nullopt;
        }
        {
            auto pass = (*encoder)->begin_compute_pass(rhi::ComputePassDesc{.label = "parity probe"});
            if (!pass) {
                error = "begin_compute_pass: " + pass.error().message;
                return std::nullopt;
            }
            (*pass)->set_pipeline(program.pipeline);
            (*pass)->set_bind_group(0, *group);
            (*pass)->dispatch(static_cast<u32>((inputs.size() + 63) / 64));
            (*pass)->end();
        }
        const rhi::BufferBarrier to_copy{
            .buffer = *out, .src_stage = rhi::PipelineStage::ComputeShader, .src_access = rhi::AccessFlags::ShaderWrite,
            .dst_stage = rhi::PipelineStage::Transfer, .dst_access = rhi::AccessFlags::TransferRead,
            .offset = 0, .size = hits_size};
        (*encoder)->barrier({}, std::span<const rhi::BufferBarrier>{&to_copy, 1}, {});
        (*encoder)->copy_buffer_to_buffer(*out, *readback, rhi::BufferCopy{.src_offset = 0, .dst_offset = 0, .size = hits_size});
        const rhi::BufferBarrier to_host{
            .buffer = *readback, .src_stage = rhi::PipelineStage::Transfer, .src_access = rhi::AccessFlags::TransferWrite,
            .dst_stage = rhi::PipelineStage::Host, .dst_access = rhi::AccessFlags::HostRead,
            .offset = 0, .size = hits_size};
        (*encoder)->barrier({}, std::span<const rhi::BufferBarrier>{&to_host, 1}, {});
        auto cb = (*encoder)->finish();
        if (!cb) {
            error = "finish: " + cb.error().message;
            return std::nullopt;
        }
        const rhi::CommandBufferHandle handles[] = {*cb};
        rhi::SubmitDesc submit{};
        submit.command_buffers = handles;
        submit.label = "parity dispatch";
        if (auto r = device.submit(submit); !r) {
            error = "submit: " + r.error().message;
            return std::nullopt;
        }
        device.wait_idle();
        auto mapped = device.map_buffer(*readback);
        if (!mapped) {
            error = "map_buffer: " + mapped.error().message;
            return std::nullopt;
        }
        std::vector<GpuHit> result(inputs.size());
        std::memcpy(result.data(), mapped->data(), hits_size);
        device.unmap_buffer(*readback);
        return result;
    }

    // ---------------------------------------------------------------------------------------------
    // Fixtures (same styles as Renderer/tests/DisplacementTest.cpp)
    // ---------------------------------------------------------------------------------------------

    Field make_rolling(u32 w, u32 h, f32 amplitude, bool wrap, const char *name) {
        Field f{name, {}, w, h, wrap};
        f.heights.resize(static_cast<usize>(w) * h);
        const f32 two_pi = 6.28318530718f;
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const f32 u = static_cast<f32>(x) / static_cast<f32>(w);
                const f32 v = static_cast<f32>(y) / static_cast<f32>(h);
                const f32 s = 0.5f + amplitude * (0.5f * std::sin(two_pi * 2.0f * u) * std::cos(two_pi * 3.0f * v) +
                                                  0.3f * std::sin(two_pi * 5.0f * (u + v)) + 0.2f * std::cos(two_pi * 7.0f * u));
                f.heights[static_cast<usize>(y) * w + x] = std::clamp(s, 0.0f, 1.0f);
            }
        }
        return f;
    }

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

    Field make_sparse(u32 w, u32 h, const char *name) {
        Field f{name, {}, w, h, true};
        f.heights.assign(static_cast<usize>(w) * h, 0.1f);
        for (u32 i = 0; i < 6; ++i) {
            f.heights[static_cast<usize>((i * 53 + 5) % h) * w + (i * 37 + 11) % w] = 0.95f;
        }
        return f;
    }

    void quantize(Field &f) {
        for (f32 &v : f.heights) {
            v = std::round(std::clamp(v, 0.0f, 1.0f) * 65535.0f) / 65535.0f;
        }
    }

    std::vector<Field> make_fields(bool quantize_heights) {
        std::vector<Field> fields;
        fields.push_back(make_rolling(64, 64, 0.9f, true, "rolling 64x64 wrap"));
        fields.push_back(make_noise(32, 32, 77, true, "noisy 32x32 wrap"));
        fields.push_back(make_noise(21, 10, 5, true, "non-square non-POT 21x10 wrap"));
        fields.push_back(make_sparse(256, 256, "sparse 256x256 wrap"));
        fields.push_back(make_rolling(32, 32, 0.8f, false, "rolling 32x32 clamp"));
        fields.push_back(make_noise(21, 10, 9, false, "non-square non-POT 21x10 clamp"));
        fields.push_back(make_noise(1, 4, 3, true, "degenerate 1x4 wrap"));
        fields[0].tile_size = {2.0f, 1.0f};
        fields[3].height_scale = 0.5f;
        if (quantize_heights) {
            for (Field &f : fields) {
                quantize(f);
            }
        }
        return fields;
    }

    // ---------------------------------------------------------------------------------------------
    // Comparison
    // ---------------------------------------------------------------------------------------------

    struct Mismatch {
        usize index;
        std::string what;
    };

    struct Stats {
        usize compared = 0;
        usize hits = 0;
        usize misses = 0;
        usize budget = 0;
        usize step_diffs = 0;
        u64 cpu_steps = 0;
        u64 gpu_steps = 0;
        f32 max_dt = 0;
        f32 max_duv = 0;
        std::vector<Mismatch> mismatches;
    };

    const char *status_name(disp::HitStatus s) {
        switch (s) {
            case disp::HitStatus::Hit: return "Hit";
            case disp::HitStatus::Miss: return "Miss";
            case disp::HitStatus::BudgetExhausted: return "Budget";
        }
        return "?";
    }

    disp::HitStatus gpu_status(u32 s) {
        return s == 1 ? disp::HitStatus::Hit : (s == 2 ? disp::HitStatus::BudgetExhausted : disp::HitStatus::Miss);
    }

    void compare_hit(Stats &stats, usize index, const disp::HeightfieldHit &cpu, const GpuHit &gpu, bool exact_steps,
                     bool compare_steps, bool step_slack = false, f32 t_tolerance_scale = 1.0f) {
        ++stats.compared;
        const disp::HitStatus gs = gpu_status(gpu.status);
        auto report = [&](const std::string &what) {
            if (stats.mismatches.size() < 8) {
                stats.mismatches.push_back({index, what});
            } else if (stats.mismatches.size() == 8) {
                stats.mismatches.push_back({index, "... (more suppressed)"});
            }
        };
        if (cpu.status == disp::HitStatus::Hit) ++stats.hits;
        if (cpu.status == disp::HitStatus::Miss) ++stats.misses;
        if (cpu.status == disp::HitStatus::BudgetExhausted) ++stats.budget;
        if (cpu.status != gs) {
            report(std::format("status cpu={} gpu={} (cpu t={:.6f} steps={}; gpu t={:.6f} steps={})", status_name(cpu.status),
                               status_name(gs), cpu.t, cpu.steps, gpu.t, gpu.steps));
            return;
        }
        if (cpu.status == disp::HitStatus::Miss) {
            return; // nothing else is defined on a miss
        }
        const f32 dt = std::abs(cpu.t - gpu.t);
        const f32 duv = std::max(std::abs(cpu.uv.x - gpu.uv[0]), std::abs(cpu.uv.y - gpu.uv[1]));
        stats.max_dt = std::max(stats.max_dt, dt / (1.0f + std::abs(cpu.t)));
        stats.max_duv = std::max(stats.max_duv, duv);
        const f32 t_tol = 2.0e-4f * t_tolerance_scale * (1.0f + std::abs(cpu.t));
        if (!(dt <= t_tol)) {
            report(std::format("t cpu={:.7f} gpu={:.7f} (|d|={:.3e})", cpu.t, gpu.t, dt));
        }
        if (!(duv <= 2.0e-4f * t_tolerance_scale * (1.0f + std::max(std::abs(cpu.uv.x), std::abs(cpu.uv.y))))) {
            report(std::format("uv cpu=({:.6f},{:.6f}) gpu=({:.6f},{:.6f})", cpu.uv.x, cpu.uv.y, gpu.uv[0], gpu.uv[1]));
        }
        if (!(std::abs(cpu.height - gpu.height) <= 2.0e-4f * t_tolerance_scale)) {
            report(std::format("height cpu={:.6f} gpu={:.6f}", cpu.height, gpu.height));
        }
        const f32 gtol = 2.0e-3f * t_tolerance_scale * (1.0f + std::max(std::abs(cpu.gradient.x), std::abs(cpu.gradient.y)));
        if (!(std::abs(cpu.gradient.x - gpu.gradient[0]) <= gtol && std::abs(cpu.gradient.y - gpu.gradient[1]) <= gtol)) {
            report(std::format("gradient cpu=({:.5f},{:.5f}) gpu=({:.5f},{:.5f})", cpu.gradient.x, cpu.gradient.y,
                               gpu.gradient[0], gpu.gradient[1]));
        }
        if (compare_steps) {
            stats.cpu_steps += cpu.steps;
            stats.gpu_steps += gpu.steps;
        }
        if (compare_steps && cpu.steps != gpu.steps) {
            ++stats.step_diffs;
            // Step counts of long traversals are not bit-stable across a CPU and a GPU. Root-caused with a
            // per-step replay (run both with maxSteps = 1, 2, 3...): the two agree step for step, at ~1e-7
            // relative t, until a ray has crossed several wrapped tiles (cell coordinates ~1e3); there the
            // 1e-4 cell-boundary nudge (kBoundaryNudge) is at the float resolution of `o + d*t`, so a
            // skip-vs-descend decision at a node boundary flips between an FMA and a non-FMA evaluation and
            // one side spends a few extra steps. The *result* is unaffected (t/uv/height/gradient agree).
            // Deterministic algorithms (POM, point queries) must match exactly; traversals get a small
            // proportional slack, and run_traces separately bounds the aggregate step ratio so a real logic
            // difference (a missed skip, a wrong descent) -- which shows up as a large factor -- still fails.
            const u32 slack = step_slack ? 4u + cpu.steps / 8u : 0u;
            const u32 diff = cpu.steps > gpu.steps ? cpu.steps - gpu.steps : gpu.steps - cpu.steps;
            if (exact_steps && diff > slack) {
                report(std::format("steps cpu={} gpu={} (slack {})", cpu.steps, gpu.steps, slack));
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Ray sets
    // ---------------------------------------------------------------------------------------------

    struct RaySet {
        std::vector<disp::HeightfieldRay> rays;
    };

    RaySet make_view_rays(const Field &f, u32 count, u32 seed) {
        RaySet set;
        std::mt19937 rng(seed);
        std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
        for (u32 i = 0; i < count; ++i) {
            // A few uv values land outside [0,1] on purpose (wrap tiles / clamp edges).
            glm::vec2 uv(unit(rng) * 1.2f - 0.1f, unit(rng) * 1.2f - 0.1f);
            const f32 phi = unit(rng) * 6.2831853f;
            const f32 cos_theta = 0.06f + 0.94f * unit(rng); // biased toward grazing
            const f32 sin_theta = std::sqrt(1.0f - cos_theta * cos_theta);
            glm::vec3 view_ts(sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta);
            if (i % 11 == 0) {
                view_ts.y = 0.0f; // axis-aligned: exercises d == 0 branches
                view_ts = glm::normalize(view_ts);
            }
            if (i % 13 == 0) {
                view_ts.x = 0.0f;
                view_ts = glm::normalize(view_ts);
            }
            const f32 reference = (i % 5 == 0) ? 0.5f : 1.0f;
            set.rays.push_back(disp::make_view_ray(uv, view_ts, f.height_scale, f.tile_size, reference));
        }
        // Rays with a finite max_t, and one that starts inside the slab.
        for (u32 i = 0; i < 24 && i < set.rays.size(); ++i) {
            set.rays[i].max_t = 0.3f + 0.4f * unit(rng);
        }
        return set;
    }

    std::vector<GpuRay> to_gpu(const RaySet &set) {
        std::vector<GpuRay> out;
        for (const auto &r : set.rays) {
            out.push_back(GpuRay{{r.origin.x, r.origin.y, r.origin.z, 0.0f}, {r.direction.x, r.direction.y, r.direction.z, r.max_t}});
        }
        return out;
    }

    SettingsBlock make_settings(const Field &f, u32 hierarchy_levels, u32 max_steps) {
        SettingsBlock s{};
        s.tex_w = static_cast<std::int32_t>(f.width);
        s.tex_h = static_cast<std::int32_t>(f.height);
        s.max_steps = max_steps;
        s.hierarchy_levels = hierarchy_levels;
        s.height_scale = f.height_scale;
        s.reference_height = 1.0f;
        s.tile_x = f.tile_size.x;
        s.tile_y = f.tile_size.y;
        s.wrap = f.wrap ? 1u : 0u;
        return s;
    }

    // ---------------------------------------------------------------------------------------------
    // The tests
    // ---------------------------------------------------------------------------------------------

    struct TraceCase {
        int algorithm;
        const char *name;
    };
    constexpr std::array<TraceCase, 3> kAlgorithms{{{2, "POM"}, {3, "CellExact"}, {4, "Hierarchical"}}};

    /// When POM's CPU and GPU statuses disagree, evaluates (in double precision) the ray-to-surface gap at
    /// the final sample, which is where such disagreements live: a descending ray's last sample sits on the
    /// slab floor, where the surface can be exactly 0 and the sign of the gap is rounding noise.
    void pom_diagnostic(const Field &f, const disp::HeightfieldRay &ray, u32 steps, const disp::HeightfieldHit &cpu,
                        const GpuHit &gpu) {
        if (cpu.status == gpu_status(gpu.status)) {
            return;
        }
        const double tExit = std::min<double>(ray.max_t, -static_cast<double>(ray.origin.z) / ray.direction.z);
        const double tEnter = ray.origin.z > 1.0f ? (1.0 - ray.origin.z) / ray.direction.z : 0.0;
        const double dt = (tExit - tEnter) / steps;
        const double t = tEnter + dt * steps;
        const double u = ray.origin.x + static_cast<double>(ray.direction.x) * t;
        const double v = ray.origin.y + static_cast<double>(ray.direction.y) * t;
        const double z = ray.origin.z + static_cast<double>(ray.direction.z) * t;
        const double h = disp::sample_height(f.view(), static_cast<f32>(u), static_cast<f32>(v));
        std::cout << std::format("        [diag] last-sample t={:.9f} ray z={:.3e} surface h={:.3e} gap={:.3e} (float gap decides)\n", t, z, h, z - h);
    }

    void run_traces(Session &session, const Variant &variant, const TraceCase &algorithm,
                    const std::vector<Field> &fields, const std::vector<GpuField> &gpu_fields,
                    const std::vector<disp::HeightfieldHierarchy> &hierarchies, const Program &program) {
        Stats total;
        for (usize fi = 0; fi < fields.size(); ++fi) {
            const Field &f = fields[fi];
            const RaySet set = make_view_rays(f, 640, 1000u + static_cast<u32>(fi));
            constexpr u32 max_steps = 1024;
            constexpr u32 pom_steps = 32;
            constexpr u32 pom_refinements = 5;
            SettingsBlock settings = make_settings(f, hierarchies[fi].level_count(), max_steps);
            settings.field_a = pom_steps;
            settings.field_b = pom_refinements;
            settings.field_c = static_cast<u32>(set.rays.size());
            settings.start_level = variant.start_level == disp::kStartAtTopLevel ? 64u : variant.start_level;
            std::string error;
            auto gpu = dispatch(session, program, gpu_fields[fi], settings, to_gpu(set), error);
            if (!gpu) {
                fail(std::format("[{}] {} on {}: {}", variant.name, algorithm.name, f.name, error));
                return;
            }
            Stats stats;
            for (usize i = 0; i < set.rays.size(); ++i) {
                disp::HeightfieldHit cpu;
                switch (algorithm.algorithm) {
                    case 2: cpu = disp::trace_parallax_occlusion(f.view(), set.rays[i], pom_steps, pom_refinements); break;
                    case 3: cpu = disp::trace_cell_exact(f.view(), set.rays[i], max_steps); break;
                    default: cpu = disp::trace_hierarchical(f.view(), hierarchies[fi], set.rays[i], max_steps, variant.start_level); break;
                }
                if (algorithm.algorithm == 2) {
                    pom_diagnostic(f, set.rays[i], pom_steps, cpu, (*gpu)[i]);
                }
                const bool exact = variant.exact_steps && (algorithm.algorithm != 4 || variant.hierarchy == HierarchyStorage::Float32);
                compare_hit(stats, i, cpu, (*gpu)[i], exact, /*compare_steps=*/true, /*step_slack=*/algorithm.algorithm != 2);
            }
            std::cout << std::format("    {:<32} rays={} hit/miss/budget={}/{}/{} max|dt|={:.2e} max|duv|={:.2e} step-diffs={} mismatches={}\n",
                                     f.name, stats.compared, stats.hits, stats.misses, stats.budget, stats.max_dt,
                                     stats.max_duv, stats.step_diffs, stats.mismatches.size());
            for (const Mismatch &m : stats.mismatches) {
                std::cout << std::format("        ray {}: {}\n", m.index, m.what);
            }
            if (algorithm.algorithm != 2 && stats.cpu_steps > 0) {
                const double ratio = static_cast<double>(stats.gpu_steps) / static_cast<double>(stats.cpu_steps);
                if (std::abs(ratio - 1.0) > 0.03) {
                    fail(std::format("[{}] {} on {}: aggregate GPU/CPU step ratio {:.4f} (cpu {} gpu {})", variant.name,
                                     algorithm.name, f.name, ratio, stats.cpu_steps, stats.gpu_steps));
                }
            }
            if (!stats.mismatches.empty()) {
                fail(std::format("[{}] {} on {}: {} GPU/CPU mismatches", variant.name, algorithm.name, f.name,
                                 stats.mismatches.size()));
            }
            ++g_checks;
        }
    }

    // ---- query probe: sample / parallax / shadow -------------------------------------------------

    /// Mirror of hf_parallax written from the documented formula, evaluated on the CPU height functions.
    disp::HeightfieldHit cpu_parallax(const Field &f, const glm::vec2 &uv, const glm::vec3 &view_ts, f32 reference) {
        const f32 vz = std::max(view_ts.z, 0.25f);
        const f32 h = disp::sample_height(f.view(), uv.x, uv.y);
        const f32 depth = reference - h;
        const glm::vec2 shift = -glm::vec2(view_ts.x, view_ts.y) / vz * (depth * f.height_scale) / f.tile_size;
        disp::HeightfieldHit hit;
        hit.status = disp::HitStatus::Hit;
        hit.t = 0.0f;
        hit.uv = uv + shift;
        hit.height = disp::sample_height(f.view(), hit.uv.x, hit.uv.y);
        hit.gradient = disp::sample_gradient(f.view(), hit.uv.x, hit.uv.y);
        hit.steps = 1;
        return hit;
    }

    void run_queries(Session &session, const Variant &variant, const std::vector<Field> &fields,
                     const std::vector<GpuField> &gpu_fields, const std::vector<disp::HeightfieldHierarchy> &hierarchies,
                     const Program &program) {
        for (usize fi = 0; fi < fields.size(); ++fi) {
            const Field &f = fields[fi];
            std::mt19937 rng(5000u + static_cast<u32>(fi));
            std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
            constexpr u32 count = 512;

            std::vector<GpuRay> uvs, parallax, shadows;
            std::vector<glm::vec3> view_dirs, light_dirs;
            for (u32 i = 0; i < count; ++i) {
                const f32 u = unit(rng) * 1.6f - 0.3f;
                const f32 v = unit(rng) * 1.6f - 0.3f;
                uvs.push_back(GpuRay{{u, v, 0, 0}, {0, 0, 0, 0}});
                const f32 phi = unit(rng) * 6.2831853f;
                const f32 cz = 0.05f + 0.95f * unit(rng);
                const f32 sz = std::sqrt(1.0f - cz * cz);
                const glm::vec3 dir(sz * std::cos(phi), sz * std::sin(phi), cz);
                view_dirs.push_back(dir);
                parallax.push_back(GpuRay{{u, v, 0, 0}, {dir.x, dir.y, dir.z, 0}});
                // Shadow queries start on the surface at (u,v) in [0,1]^2 with a real surface height.
                const f32 su = unit(rng);
                const f32 sv = unit(rng);
                const f32 sh = disp::sample_height(f.view(), su, sv);
                glm::vec3 light = dir;
                if (i % 17 == 0) {
                    light.z = -0.2f; // below the horizon: blocked by definition
                }
                light_dirs.push_back(light);
                shadows.push_back(GpuRay{{su, sv, sh, 2.0e-3f}, {light.x, light.y, light.z, 0}});
            }

            struct Mode { u32 id; const char *name; const std::vector<GpuRay> *inputs; };
            const Mode modes[] = {{0, "sample", &uvs}, {1, "parallax", &parallax}, {2, "shadow-cell", &shadows}, {3, "shadow-hier", &shadows}};
            for (const Mode &mode : modes) {
                SettingsBlock settings = make_settings(f, hierarchies[fi].level_count(), 1024);
                settings.field_a = mode.id;
                settings.field_b = count;
                std::string error;
                auto gpu = dispatch(session, program, gpu_fields[fi], settings, *mode.inputs, error);
                if (!gpu) {
                    fail(std::format("[{}] query {} on {}: {}", variant.name, mode.name, f.name, error));
                    return;
                }
                Stats stats;
                usize blocked = 0;
                for (u32 i = 0; i < count; ++i) {
                    if (mode.id == 0) {
                        disp::HeightfieldHit cpu;
                        cpu.status = disp::HitStatus::Hit;
                        const auto &in = (*mode.inputs)[i];
                        cpu.uv = {in.origin_t[0], in.origin_t[1]};
                        cpu.height = disp::sample_height(f.view(), in.origin_t[0], in.origin_t[1]);
                        cpu.gradient = disp::sample_gradient(f.view(), in.origin_t[0], in.origin_t[1]);
                        GpuHit g = (*gpu)[i];
                        g.status = 1;
                        g.t = 0;
                        compare_hit(stats, i, cpu, g, false, false);
                    } else if (mode.id == 1) {
                        const auto &in = (*mode.inputs)[i];
                        const disp::HeightfieldHit cpu = cpu_parallax(f, {in.origin_t[0], in.origin_t[1]}, view_dirs[i], 1.0f);
                        compare_hit(stats, i, cpu, (*gpu)[i], true, true);
                    } else {
                        const auto &in = (*mode.inputs)[i];
                        const bool cpu_blocked = disp::trace_shadow(
                            f.view(), mode.id == 3 ? hierarchies[fi] : disp::HeightfieldHierarchy{}, {in.origin_t[0], in.origin_t[1]},
                            in.origin_t[2], light_dirs[i], f.height_scale, f.tile_size, 1024, in.origin_t[3]);
                        const bool gpu_blocked = (*gpu)[i].status == 1;
                        blocked += cpu_blocked ? 1 : 0;
                        ++stats.compared;
                        if (cpu_blocked != gpu_blocked && stats.mismatches.size() < 8) {
                            stats.mismatches.push_back({i, std::format("shadow cpu={} gpu={}", cpu_blocked, gpu_blocked)});
                        } else if (cpu_blocked != gpu_blocked) {
                            stats.mismatches.push_back({i, "..."});
                        }
                    }
                }
                std::cout << std::format("    {:<32} {:<12} n={} max|duv|={:.2e}{} mismatches={}\n", f.name, mode.name, stats.compared,
                                         stats.max_duv, mode.id >= 2 ? std::format(" blocked={}", blocked) : std::string{},
                                         stats.mismatches.size());
                for (const Mismatch &m : stats.mismatches) {
                    std::cout << std::format("        query {}: {}\n", m.index, m.what);
                }
                if (!stats.mismatches.empty()) {
                    fail(std::format("[{}] query {} on {}: {} mismatches", variant.name, mode.name, f.name, stats.mismatches.size()));
                }
                ++g_checks;
            }
        }
    }


    // ---------------------------------------------------------------------------------------------
    // Material shader validation: compile the displaced G-buffer shaders for the device's own target
    // (WGSL on Dawn, SPIR-V on Vulkan), create the shader modules and full render pipelines, and let the
    // API's validation judge them. slangc alone gives false passes for WGSL: Dawn's Tint runs the
    // uniformity analysis, binding-compatibility and interface checks only at CreateShaderModule /
    // CreateRenderPipeline time.
    // ---------------------------------------------------------------------------------------------

    struct MaterialCase {
        std::string name;
        std::string file;
        std::string vertex_entry;
        std::string fragment_entry; // empty = vertex-only (depth-only pipeline)
        std::vector<slang::ShaderMacro> macros;
        bool color_targets = true;
        bool depth_target = false;
        bool depth_write_output = false;
    };

    std::vector<MaterialCase> material_cases() {
        std::vector<MaterialCase> cases;
        for (int algorithm = 0; algorithm <= 4; ++algorithm) {
            cases.push_back(MaterialCase{.name = std::format("gbuffer displaced, algorithm {}", algorithm),
                                         .file = "gbuffer_geometry_displaced", .vertex_entry = "vertexMain",
                                         .fragment_entry = "fragmentMain",
                                         .macros = {{"SFT_HF_ALGORITHM", std::to_string(algorithm)}}});
        }
        for (int algorithm : {2, 4}) {
            cases.push_back(MaterialCase{.name = std::format("gbuffer displaced + SV_Depth, algorithm {}", algorithm),
                                         .file = "gbuffer_geometry_displaced", .vertex_entry = "vertexMain",
                                         .fragment_entry = "fragmentMain",
                                         .macros = {{"SFT_HF_ALGORITHM", std::to_string(algorithm)}, {"SFT_DISPLACEMENT_WRITE_DEPTH", "1"}},
                                         .depth_target = true, .depth_write_output = true});
        }
        cases.push_back(MaterialCase{.name = "legacy per-vertex displaced geometry (vertexMainDisplaced)",
                                     .file = "gbuffer_geometry_displaced", .vertex_entry = "vertexMainDisplaced",
                                     .fragment_entry = "fragmentMain", .macros = {{"SFT_HF_ALGORITHM", "0"}}});
        for (const char *entry : {"vertexMainWithHistory", "vertexMainDisplacedWithHistory"}) {
            cases.push_back(MaterialCase{.name = std::format("history path ({})", entry),
                                         .file = "gbuffer_geometry_displaced_history", .vertex_entry = entry,
                                         .fragment_entry = "", .macros = {{"SFT_HF_ALGORITHM", "4"}},
                                         .color_targets = false, .depth_target = true});
        }
        cases.push_back(MaterialCase{.name = "depth prepass (depthOnlyMain)", .file = "gbuffer_geometry_displaced",
                                     .vertex_entry = "vertexMain", .fragment_entry = "depthOnlyMain",
                                     .macros = {{"SFT_HF_ALGORITHM", "4"}}, .color_targets = false, .depth_target = true});
        return cases;
    }

    void validate_material_cases(Gpu &gpu, ErrorWatch &watch) {
        rhi::RhiDevice &device = *gpu.device;
        const auto target = SFT::Renderer::shader_target_for_device(device);
        if (!target) {
            fail("no shader target: " + target.error().message);
            return;
        }
        std::cout << "== material shader validation on " << gpu.description << '\n';
        for (const MaterialCase &mc : material_cases()) {
            Cleanup cleanup;
            (void)watch.take();
            slang::ShaderCompileOptions options{};
            options.targets = {target->slang_target};
            options.entry_points = {slang::ShaderEntryPointRequest{.name = mc.vertex_entry, .stage = slang::ShaderStage::Vertex}};
            if (!mc.fragment_entry.empty()) {
                options.entry_points.push_back(slang::ShaderEntryPointRequest{.name = mc.fragment_entry, .stage = slang::ShaderStage::Fragment});
            }
            options.search_paths = {shaders_dir().string()};
            options.macros = mc.macros;
            slang::ShaderCompiler compiler;
            auto shader = compiler.compile(slang::ShaderSource::from_file((shaders_dir() / (mc.file + ".slang")).string(), mc.file), options);
            if (!shader) {
                fail(std::format("[{}] compile failed: {}\n{}", mc.name, shader.error().message, shader.error().diagnostics));
                continue;
            }
            auto make_module = [&](const std::string &entry) -> std::optional<rhi::ShaderModuleHandle> {
                auto code = shader->entry_point_code(std::string_view{entry}, target->slang_target.format);
                if (!code) {
                    fail(std::format("[{}] entry_point_code({}) failed: {}", mc.name, entry, code.error().message));
                    return std::nullopt;
                }
                auto module = device.create_shader_module(rhi::ShaderModuleDesc{
                    .language = target->module_language,
                    .code = std::span<const std::byte>{code->bytes.data(), code->bytes.size()}, .label = entry.c_str()});
                if (!module) {
                    fail(std::format("[{}] create_shader_module({}) failed: {}", mc.name, entry, module.error().message));
                    return std::nullopt;
                }
                cleanup.steps.emplace_back([&device, m = *module] { device.destroy_shader_module(m); });
                return *module;
            };
            const auto vertex = make_module(mc.vertex_entry);
            std::optional<rhi::ShaderModuleHandle> fragment;
            if (!mc.fragment_entry.empty()) {
                fragment = make_module(mc.fragment_entry);
            }
            bool ok = vertex && (fragment || mc.fragment_entry.empty());

            // Layouts exactly as Renderer::create_material_template does.
            const slang::ShaderReflection &reflection = shader->reflection();
            rhi::ShaderStage visibility = SFT::Renderer::reflected_stage_mask(reflection);
            std::vector<rhi::BindGroupLayoutHandle> layouts;
            if (ok) {
                for (const auto &generated : SFT::Renderer::generate_bind_group_layouts(reflection, visibility)) {
                    auto handle = device.create_bind_group_layout(rhi::BindGroupLayoutDesc{
                        .entries = std::span<const rhi::BindGroupLayoutEntry>{generated.entries}, .label = "material bgl"});
                    if (!handle) {
                        fail(std::format("[{}] create_bind_group_layout failed: {}", mc.name, handle.error().message));
                        ok = false;
                        break;
                    }
                    layouts.push_back(*handle);
                    cleanup.steps.emplace_back([&device, h = *handle] { device.destroy_bind_group_layout(h); });
                }
            }
            if (ok) {
                const auto push = SFT::Renderer::generate_push_constant_ranges(reflection, rhi::ShaderStage::Vertex);
                auto layout = device.create_pipeline_layout(rhi::PipelineLayoutDesc{
                    .bind_group_layouts = layouts, .push_constant_ranges = push, .label = "material layout"});
                if (!layout) {
                    fail(std::format("[{}] create_pipeline_layout failed: {}", mc.name, layout.error().message));
                    ok = false;
                } else {
                    cleanup.steps.emplace_back([&device, h = *layout] { device.destroy_pipeline_layout(h); });
                    const std::array<rhi::VertexAttribute, 5> attributes{{
                        {.format = rhi::VertexFormat::Float32x3, .offset = 0, .shader_location = 0, .semantic_name = "POSITION"},
                        {.format = rhi::VertexFormat::Float32x3, .offset = 12, .shader_location = 1, .semantic_name = "NORMAL"},
                        {.format = rhi::VertexFormat::Float32x2, .offset = 24, .shader_location = 2, .semantic_name = "TEXCOORD"},
                        {.format = rhi::VertexFormat::Float32x4, .offset = 32, .shader_location = 3, .semantic_name = "COLOR"},
                        {.format = rhi::VertexFormat::Float32x4, .offset = 48, .shader_location = 4, .semantic_name = "TANGENT"},
                    }};
                    const rhi::VertexBufferLayout vertex_layout{.stride = 64, .step_mode = rhi::VertexStepMode::Vertex, .attributes = attributes};
                    // The renderer's G-buffer: albedo, octahedral normal, material, emissive, motion.
                    std::vector<rhi::ColorTargetState> targets;
                    if (mc.color_targets) {
                        for (rhi::Format format : {rhi::Format::RGBA8Unorm, rhi::Format::RG16Float, rhi::Format::RGBA8Unorm,
                                                   rhi::Format::RGBA16Float, rhi::Format::RG16Float}) {
                            targets.push_back(rhi::ColorTargetState{.format = format});
                        }
                    }
                    rhi::DepthStencilState depth{};
                    if (mc.depth_target) {
                        depth = rhi::DepthStencilState{.format = rhi::Format::D32Float, .depth_test_enable = true,
                                                       .depth_write_enable = true, .depth_compare = rhi::CompareOp::Less};
                    }
                    auto pipeline = device.create_render_pipeline(rhi::RenderPipelineDesc{
                        .layout = *layout,
                        .vertex = rhi::ShaderEntry{.module = *vertex, .entry_point = mc.vertex_entry.c_str(), .stage = rhi::ShaderStage::Vertex},
                        .fragment = fragment ? rhi::ShaderEntry{.module = *fragment, .entry_point = mc.fragment_entry.c_str(), .stage = rhi::ShaderStage::Fragment}
                                            : rhi::ShaderEntry{},
                        .vertex_buffers = std::span<const rhi::VertexBufferLayout>{&vertex_layout, 1},
                        .topology = rhi::PrimitiveTopology::TriangleList,
                        .rasterization = rhi::RasterizationState{.cull_mode = rhi::CullMode::None},
                        .depth_stencil = depth,
                        .color_targets = targets,
                        .label = "material pipeline"});
                    if (!pipeline) {
                        fail(std::format("[{}] create_render_pipeline failed: {}", mc.name, pipeline.error().message));
                        ok = false;
                    } else {
                        cleanup.steps.emplace_back([&device, h = *pipeline] { device.destroy_render_pipeline(h); });
                    }
                }
            }
            device.wait_idle();
            const auto errors = watch.take();
            for (const std::string &message : errors) {
                fail(std::format("[{}] API validation error: {}", mc.name, message));
                ok = false;
            }
            std::cout << std::format("    {:<62} {}\n", mc.name, ok ? "ok" : "FAILED");
            ++g_checks;
        }
    }

    void run_variant(Gpu &gpu, const Variant &variant) {
        std::cout << std::format("== variant '{}'  (hierarchy={}, quantized heights={}, macros={})\n", variant.name,
                                 variant.hierarchy == HierarchyStorage::Float32 ? "Float32" : "Unorm16",
                                 variant.quantize_heights, variant.macros.size());
        Session session(*gpu.device);
        auto sampler = gpu.device->create_sampler(rhi::SamplerDesc{
            .min_filter = rhi::Filter::Nearest, .mag_filter = rhi::Filter::Nearest, .mipmap_mode = rhi::MipmapMode::Nearest,
            .address_u = rhi::AddressMode::ClampToEdge, .address_v = rhi::AddressMode::ClampToEdge,
            .address_w = rhi::AddressMode::ClampToEdge, .label = "parity sampler"});
        if (!sampler) {
            fail("create_sampler failed: " + sampler.error().message);
            return;
        }
        session.sampler = *sampler;
        session.cleanup.steps.emplace_back([&session] { session.device.destroy_sampler(session.sampler); });

        const std::vector<Field> fields = make_fields(variant.quantize_heights);
        std::vector<disp::HeightfieldHierarchy> hierarchies;
        std::vector<GpuField> gpu_fields;
        for (const Field &f : fields) {
            hierarchies.push_back(disp::HeightfieldHierarchy::build(f.view()));
            auto uploaded = upload_field(session, f, hierarchies.back(), variant.hierarchy);
            if (!uploaded) {
                return;
            }
            gpu_fields.push_back(*uploaded);
        }

        for (const TraceCase &algorithm : kAlgorithms) {
            std::vector<slang::ShaderMacro> macros = variant.macros;
            macros.push_back({"SFT_HF_ALGORITHM", std::to_string(algorithm.algorithm)});
            std::string error;
            auto program = build_program(session, "heightfield_trace_probe", "probeMain", macros, error);
            if (!program) {
                fail(std::format("[{}] {}: {}", variant.name, algorithm.name, error));
                continue;
            }
            std::cout << std::format("  -- trace: {} (SFT_HF_ALGORITHM={})\n", algorithm.name, algorithm.algorithm);
            run_traces(session, variant, algorithm, fields, gpu_fields, hierarchies, *program);
        }

        std::string error;
        auto queries = build_program(session, "heightfield_query_probe", "queryMain", variant.macros, error);
        if (!queries) {
            fail(std::format("[{}] query probe: {}", variant.name, error));
            return;
        }
        std::cout << "  -- point queries: sample / parallax / shadow\n";
        run_queries(session, variant, fields, gpu_fields, hierarchies, *queries);
    }

} // namespace

int main(int argc, char **argv) {
    const std::string backend = argc > 1 ? argv[1] : "vulkan";
    ErrorWatch watch;

    std::string skip_reason;
    std::unique_ptr<Gpu> gpu;
    if (backend == "vulkan") {
        gpu = make_vulkan_gpu(skip_reason);
    } else if (backend == "webgpu") {
#if defined(DISPLACEMENT_PARITY_WEBGPU)
        gpu = make_webgpu_gpu(skip_reason);
#else
        skip_reason = "built without WebGPU support (STURDY_ENABLE_WEBGPU)";
#endif
    } else {
        std::cerr << "usage: DisplacementGpuParityTest [vulkan|webgpu]\n";
        return 2;
    }
    if (!gpu) {
        std::cout << "SKIPPED: " << skip_reason << '\n';
        return 0;
    }
    std::cout << "Device: " << gpu->description << '\n';
    (void)watch.take(); // bring-up chatter is not under test

    for (const Variant &variant : variants()) {
        run_variant(*gpu, variant);
        for (const std::string &message : watch.take()) {
            fail("API error logged during variant '" + variant.name + "': " + message);
        }
    }

    validate_material_cases(*gpu, watch);
    for (const std::string &message : watch.take()) {
        fail("API error logged during material validation: " + message);
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s) across " << g_checks << " comparisons\n";
        return 1;
    }
    std::cout << "DisplacementGpuParityTest passed (" << g_checks << " comparison sets)\n";
    return 0;
}
