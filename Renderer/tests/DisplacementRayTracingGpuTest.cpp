// GPU-vs-CPU parity for the ray-tracing displacement slot.
//
// Executes Shaders/heightfield_rt_prism.slang on a real device and compares it with the CPU reference
// (Renderer/Displacement/RayTracingReference.cpp):
//
//   * SOFTWARE  Shaders/heightfield_rt_software.slang — a compute shader that tests every ray against a
//               triangle list. Needs only compute. Run with the cell-exact and the hierarchical block.
//   * HARDWARE  Shaders/heightfield_rt_query_probe.slang — an inline ray query over a BLAS of AABB
//               (procedural) primitives, one prism box per triangle, whose candidate loop runs the
//               displaced-triangle test and commits the hit. Run only when the device reports
//               AccelerationStructures + RayQuery; otherwise that half is reported as skipped.
//
// The three results (CPU reference, software, hardware) are compared ray by ray: hit/miss, hit distance,
// position, normal and uv. Shaders go through the engine's own Slang compiler for the device's native
// target and are dispatched through the RHI. The software probe also runs on WebGPU/Dawn
// (argument `webgpu`, built only with STURDY_ENABLE_WEBGPU); the hardware half needs ray query and is
// skipped there.
//
// The test SKIPS (exit 0, message) when no device can be brought up.

#include <Renderer/Displacement/HeightfieldHierarchy.hpp>
#include <Renderer/Displacement/HeightfieldTrace.hpp>
#include <Renderer/Displacement/RayTracingReference.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/ShaderTarget.hpp>

#include <Core/Core.hpp>
#include <Core/EngineBackend.hpp>
#include <Core/Renderer.hpp>
#include <Foundation/LogSink.hpp>
#include <RHI/RHI.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>
#include <WindowManager/WindowManager.hpp>

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

#if defined(DISPLACEMENT_RT_WEBGPU)
#include <Core/WebGPU/RHI/WebGpuAdapter.hpp>
#endif

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

    int g_failures = 0;
    int g_comparisons = 0;

    void fail(const std::string &message) {
        ++g_failures;
        std::cerr << "FAILED: " << message << '\n';
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
            std::vector<std::string> out;
            out.swap(messages_);
            return out;
        }

      private:
        SFT::Foundation::LogSinkId sink_{};
        std::mutex mutex_;
        std::vector<std::string> messages_;
    };

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

    // ---------------------------------------------------------------------------------------------
    // Device bring-up
    // ---------------------------------------------------------------------------------------------

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
        config.title = "DisplacementRayTracingGpuTest";
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
        info.app_name = "DisplacementRayTracingGpuTest";
        info.enable_shader_disk_cache = false;
        info.features.raytracing = true; // optional: the hardware half is skipped when the device lacks it
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

#if defined(DISPLACEMENT_RT_WEBGPU)
    std::unique_ptr<Gpu> make_webgpu_gpu(std::string &skip_reason) {
        auto gpu = std::make_unique<Gpu>();
        const rhi::BackendRegistration registration = SFT::Core::WebGpu::webgpu_backend_registration();
        auto instance = registration.create_instance(rhi::InstanceDesc{
            .application_name = "DisplacementRayTracingGpuTest", .enable_validation = true, .enable_debug_utils = true,
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
        request.label = "displacement rt device";
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
    // Fixtures
    // ---------------------------------------------------------------------------------------------

    struct Field {
        std::vector<f32> heights;
        u32 width = 0;
        u32 height = 0;
        [[nodiscard]] disp::HeightfieldView view() const { return disp::HeightfieldView{heights, width, height, true}; }
    };

    Field make_rolling_field(u32 w, u32 h, f32 amplitude) {
        Field f{{}, w, h};
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

    struct Rng {
        std::mt19937 gen;
        explicit Rng(u32 seed) : gen(seed) {}
        f32 uniform(f32 lo, f32 hi) { return std::uniform_real_distribution<f32>(lo, hi)(gen); }
        glm::vec3 unit_vector() {
            for (;;) {
                const glm::vec3 v(uniform(-1, 1), uniform(-1, 1), uniform(-1, 1));
                const f32 l = glm::length(v);
                if (l > 0.1f && l <= 1.0f) {
                    return v / l;
                }
            }
        }
    };

    /// A patch of a gently curved surface tessellated into triangles: a grid of vertices on a bump with
    /// smooth normals, so neighbouring prisms genuinely overlap and share edges (which is what makes the
    /// "closest of several prisms" part of the test meaningful).
    struct Mesh {
        std::vector<disp::DisplacedTriangle> triangles;
    };

    Mesh make_curved_patch(u32 cells) {
        const auto vertex = [&](u32 i, u32 j, glm::vec3 &position, glm::vec3 &normal, glm::vec2 &uv) {
            const f32 x = static_cast<f32>(i) / static_cast<f32>(cells) * 2.0f - 1.0f;
            const f32 y = static_cast<f32>(j) / static_cast<f32>(cells) * 2.0f - 1.0f;
            const f32 z = 0.35f * std::cos(1.7f * x) * std::cos(1.3f * y);
            position = glm::vec3(x, y, z);
            const f32 dzdx = -0.35f * 1.7f * std::sin(1.7f * x) * std::cos(1.3f * y);
            const f32 dzdy = -0.35f * 1.3f * std::cos(1.7f * x) * std::sin(1.3f * y);
            normal = glm::normalize(glm::vec3(-dzdx, -dzdy, 1.0f));
            uv = glm::vec2(x * 1.5f + 0.37f, y * 1.5f + 0.61f);
        };
        Mesh mesh;
        for (u32 j = 0; j < cells; ++j) {
            for (u32 i = 0; i < cells; ++i) {
                glm::vec3 p[4], n[4];
                glm::vec2 uv[4];
                vertex(i, j, p[0], n[0], uv[0]);
                vertex(i + 1, j, p[1], n[1], uv[1]);
                vertex(i, j + 1, p[2], n[2], uv[2]);
                vertex(i + 1, j + 1, p[3], n[3], uv[3]);
                const int order[2][3] = {{0, 1, 2}, {1, 3, 2}};
                for (const auto &tri : order) {
                    disp::DisplacedTriangle t;
                    for (int k = 0; k < 3; ++k) {
                        t.position[k] = p[tri[k]];
                        t.normal[k] = n[tri[k]];
                        t.uv[k] = uv[tri[k]];
                    }
                    mesh.triangles.push_back(t);
                }
            }
        }
        return mesh;
    }

    // ---------------------------------------------------------------------------------------------
    // GPU-side layouts (twins of the .slang structs)
    // ---------------------------------------------------------------------------------------------

    struct alignas(16) GpuRecord { // HfRtDisplacementRecord
        std::uint32_t textures[4];
        std::int32_t sizes[4];
        f32 scale[4];
        f32 height_range[4];
        f32 uv_transform[4];
    };
    static_assert(sizeof(GpuRecord) == 80);

    struct GpuRay { // HfRtProbeRay
        f32 origin_tmin[4];
        f32 direction_tmax[4];
    };
    struct GpuTriangle { // HfRtPackedTriangle
        f32 v[3][8];    // per vertex: position.xyz, uv.x, normal.xyz, uv.y
    };
    static_assert(sizeof(GpuTriangle) == 96);

    struct GpuHit { // HfRtProbeHit
        std::uint32_t status;
        std::uint32_t triangle;
        f32 t;
        std::uint32_t steps;
        f32 position_u[4];
        f32 normal_v[4];
        f32 bary_height[4];
    };
    static_assert(sizeof(GpuHit) == 64);

    GpuTriangle pack(const disp::DisplacedTriangle &t) {
        GpuTriangle g{};
        for (int i = 0; i < 3; ++i) {
            g.v[i][0] = t.position[i].x;
            g.v[i][1] = t.position[i].y;
            g.v[i][2] = t.position[i].z;
            g.v[i][3] = t.uv[i].x;
            g.v[i][4] = t.normal[i].x;
            g.v[i][5] = t.normal[i].y;
            g.v[i][6] = t.normal[i].z;
            g.v[i][7] = t.uv[i].y;
        }
        return g;
    }

    /// Shader `HfRtPackedTriangle` is (v0a = position + uv.x, v0b = normal + uv.y): the packing above is
    /// exactly that per-vertex order (float4 a, float4 b).
    static_assert(sizeof(GpuTriangle::v[0]) == 32);

    // ---------------------------------------------------------------------------------------------
    // RHI plumbing
    // ---------------------------------------------------------------------------------------------

    struct Session {
        rhi::RhiDevice &device;
        Cleanup cleanup;
        rhi::SamplerHandle sampler{};
        explicit Session(rhi::RhiDevice &d) : device(d) {}
    };

    std::optional<rhi::BufferHandle> make_buffer(Session &s, u64 size, rhi::BufferUsage usage, rhi::MemoryLocation memory,
                                                 const char *label) {
        auto b = s.device.create_buffer(rhi::BufferDesc{.size = size, .usage = usage, .memory = memory, .label = label});
        if (!b) {
            fail(std::format("create_buffer({}) failed: {}", label, b.error().message));
            return std::nullopt;
        }
        s.cleanup.steps.emplace_back([&s, h = *b] { s.device.destroy_buffer(h); });
        return *b;
    }

    struct GpuField {
        rhi::TextureViewHandle height_view{};
        rhi::TextureViewHandle hier_view{};
        u32 hierarchy_levels = 0;
    };

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
        auto staging = make_buffer(session, staging_bytes.size(), rhi::BufferUsage::TransferSrc,
                                   rhi::MemoryLocation::HostUpload, "rt staging");
        if (!staging) {
            return std::nullopt;
        }
        if (auto r = device.write_buffer(*staging, 0, staging_bytes); !r) {
            fail("write staging buffer failed: " + r.error().message);
            return std::nullopt;
        }
        auto encoder = device.create_command_encoder(rhi::CommandEncoderDesc{.label = "rt upload"});
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
        submit.label = "rt upload";
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

    std::optional<GpuField> upload_field(Session &session, const Field &field, const disp::HeightfieldHierarchy &hierarchy) {
        GpuField out;
        auto height = create_r32f_texture(session, field.width, field.height, {field.heights}, "rt height");
        if (!height) {
            return std::nullopt;
        }
        auto hv = create_view(session, *height, "rt height view");
        if (!hv) {
            return std::nullopt;
        }
        out.height_view = *hv;
        const disp::PackedHierarchy packed =
            hierarchy.pack(disp::HierarchyPrecision::Float32, disp::HierarchyChannels::MaxOnly);
        std::vector<std::vector<f32>> mips;
        for (u32 mip = 0; mip < packed.mip_count; ++mip) {
            const u32 mw = std::max(1u, packed.width >> mip);
            const u32 mh = std::max(1u, packed.height >> mip);
            std::vector<f32> level(static_cast<usize>(mw) * mh);
            std::memcpy(level.data(), packed.data.data() + packed.mip_offsets[mip], level.size() * 4);
            mips.push_back(std::move(level));
        }
        auto hier = create_r32f_texture(session, packed.width, packed.height, mips, "rt hierarchy");
        if (!hier) {
            return std::nullopt;
        }
        auto hierv = create_view(session, *hier, "rt hierarchy view");
        if (!hierv) {
            return std::nullopt;
        }
        out.hier_view = *hierv;
        out.hierarchy_levels = hierarchy.level_count();
        return out;
    }

    std::filesystem::path shaders_dir() {
        return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "Shaders";
    }

    struct Program {
        rhi::BindGroupLayoutHandle bgl{};
        rhi::ComputePipelineHandle pipeline{};
        std::vector<rhi::BindGroupLayoutEntry> entries;
        std::vector<SFT::Renderer::ReflectedResource> resources;
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
        auto module = device.create_shader_module(rhi::ShaderModuleDesc{
            .language = target->module_language,
            .code = std::span<const std::byte>{code->bytes.data(), code->bytes.size()},
            .label = "rt probe"});
        if (!module) {
            error = "create_shader_module failed: " + module.error().message;
            return nullptr;
        }
        session.cleanup.steps.emplace_back([&device, m = *module] { device.destroy_shader_module(m); });

        const slang::ShaderReflection &reflection = shader->reflection();
        const auto generated = SFT::Renderer::generate_bind_group_layouts(reflection, rhi::ShaderStage::Compute);
        if (generated.empty()) {
            error = "reflection produced no bind group layout";
            return nullptr;
        }
        program->entries = generated.front().entries;
        program->resources = SFT::Renderer::collect_resource_bindings(reflection);

        auto bgl = device.create_bind_group_layout(rhi::BindGroupLayoutDesc{
            .entries = std::span<const rhi::BindGroupLayoutEntry>{program->entries}, .label = "rt bgl"});
        if (!bgl) {
            error = "create_bind_group_layout failed: " + bgl.error().message;
            return nullptr;
        }
        program->bgl = *bgl;
        session.cleanup.steps.emplace_back([&device, h = *bgl] { device.destroy_bind_group_layout(h); });
        const rhi::BindGroupLayoutHandle layouts[] = {*bgl};
        auto layout = device.create_pipeline_layout(rhi::PipelineLayoutDesc{
            .bind_group_layouts = layouts, .push_constant_ranges = {}, .label = "rt layout"});
        if (!layout) {
            error = "create_pipeline_layout failed: " + layout.error().message;
            return nullptr;
        }
        session.cleanup.steps.emplace_back([&device, h = *layout] { device.destroy_pipeline_layout(h); });
        static thread_local std::string entry_storage;
        entry_storage = entry;
        auto pipeline = device.create_compute_pipeline(rhi::ComputePipelineDesc{
            .layout = *layout,
            .compute = rhi::ShaderEntry{.module = *module, .entry_point = entry_storage.c_str(),
                                        .stage = rhi::ShaderStage::Compute},
            .label = "rt pipeline"});
        if (!pipeline) {
            error = "create_compute_pipeline failed: " + pipeline.error().message;
            return nullptr;
        }
        program->pipeline = *pipeline;
        session.cleanup.steps.emplace_back([&device, h = *pipeline] { device.destroy_compute_pipeline(h); });
        return program;
    }

    struct Inputs {
        const GpuField *field = nullptr;
        GpuRecord record{};
        std::vector<GpuRay> rays;
        std::vector<GpuTriangle> triangles;
        rhi::AccelerationStructureHandle tlas{}; // hardware probe only
    };

    std::optional<std::vector<GpuHit>> dispatch(Session &session, const Program &program, const Inputs &in,
                                                std::string &error) {
        rhi::RhiDevice &device = session.device;
        Cleanup local;
        const u64 hits_size = static_cast<u64>(in.rays.size()) * sizeof(GpuHit);
        auto buf = [&](u64 size, rhi::BufferUsage usage, rhi::MemoryLocation memory,
                       const char *label) -> std::optional<rhi::BufferHandle> {
            auto b = device.create_buffer(rhi::BufferDesc{.size = size, .usage = usage, .memory = memory, .label = label});
            if (!b) {
                error = std::format("create_buffer({}) failed: {}", label, b.error().message);
                return std::nullopt;
            }
            local.steps.emplace_back([&device, h = *b] { device.destroy_buffer(h); });
            return *b;
        };
        const u32 counts[4] = {static_cast<u32>(in.rays.size()), static_cast<u32>(in.triangles.size()), 0, 0};
        auto record_buf = buf(align_up(sizeof(GpuRecord), 16), rhi::BufferUsage::Uniform | rhi::BufferUsage::TransferDst,
                              rhi::MemoryLocation::HostUpload, "rt record");
        auto counts_buf = buf(16, rhi::BufferUsage::Uniform | rhi::BufferUsage::TransferDst,
                              rhi::MemoryLocation::HostUpload, "rt counts");
        auto rays_buf = buf(in.rays.size() * sizeof(GpuRay), rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst,
                            rhi::MemoryLocation::HostUpload, "rt rays");
        auto tris_buf = buf(in.triangles.size() * sizeof(GpuTriangle), rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst,
                            rhi::MemoryLocation::HostUpload, "rt triangles");
        auto hits_buf = buf(hits_size, rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc,
                            rhi::MemoryLocation::DeviceLocal, "rt hits");
        auto readback = buf(hits_size, rhi::BufferUsage::TransferDst, rhi::MemoryLocation::HostReadback, "rt readback");
        if (!record_buf || !counts_buf || !rays_buf || !tris_buf || !hits_buf || !readback) {
            return std::nullopt;
        }
        if (!device.write_buffer(*record_buf, 0, std::as_bytes(std::span{&in.record, 1})) ||
            !device.write_buffer(*counts_buf, 0, std::as_bytes(std::span{counts, 4})) ||
            !device.write_buffer(*rays_buf, 0, bytes_of(in.rays)) ||
            !device.write_buffer(*tris_buf, 0, bytes_of(in.triangles))) {
            error = "buffer write failed";
            return std::nullopt;
        }

        std::vector<rhi::BindGroupEntry> group_entries;
        for (const auto &entry : program.entries) {
            std::string name;
            for (const auto &res : program.resources) {
                if (res.binding == entry.binding) {
                    name = res.name;
                }
            }
            rhi::BindGroupEntry g{.binding = entry.binding};
            switch (entry.type) {
                case rhi::BindingType::UniformBuffer:
                    if (name.find("displacement") != std::string::npos) {
                        g.buffer = *record_buf;
                        g.size = align_up(sizeof(GpuRecord), 16);
                    } else {
                        g.buffer = *counts_buf;
                        g.size = 16;
                    }
                    break;
                case rhi::BindingType::CombinedImageSampler:
                    g.texture_view = name.find("hierarchy") != std::string::npos ? in.field->hier_view : in.field->height_view;
                    g.sampler = session.sampler;
                    break;
                case rhi::BindingType::SampledTexture:
                    g.texture_view = name.find("hierarchy") != std::string::npos ? in.field->hier_view : in.field->height_view;
                    break;
                case rhi::BindingType::Sampler:
                    g.sampler = session.sampler;
                    break;
                case rhi::BindingType::StorageBuffer:
                case rhi::BindingType::ReadOnlyStorageBuffer:
                    if (name.find("hits") != std::string::npos) {
                        g.buffer = *hits_buf;
                        g.size = hits_size;
                    } else if (name.find("rays") != std::string::npos) {
                        g.buffer = *rays_buf;
                        g.size = in.rays.size() * sizeof(GpuRay);
                    } else {
                        g.buffer = *tris_buf;
                        g.size = in.triangles.size() * sizeof(GpuTriangle);
                    }
                    break;
                case rhi::BindingType::AccelerationStructure:
                    g.acceleration_structure = in.tlas;
                    break;
                default:
                    error = "unexpected binding type in probe layout (" + name + ")";
                    return std::nullopt;
            }
            group_entries.push_back(g);
        }
        auto group = device.create_bind_group(rhi::BindGroupDesc{
            .layout = program.bgl, .entries = group_entries, .lifetime = rhi::BindGroupLifetime::Persistent,
            .label = "rt group"});
        if (!group) {
            error = "create_bind_group failed: " + group.error().message;
            return std::nullopt;
        }
        local.steps.emplace_back([&device, h = *group] { device.destroy_bind_group(h); });

        auto encoder = device.create_command_encoder(rhi::CommandEncoderDesc{.label = "rt dispatch"});
        if (!encoder) {
            error = "create_command_encoder: " + encoder.error().message;
            return std::nullopt;
        }
        {
            auto pass = (*encoder)->begin_compute_pass(rhi::ComputePassDesc{.label = "rt probe"});
            if (!pass) {
                error = "begin_compute_pass: " + pass.error().message;
                return std::nullopt;
            }
            (*pass)->set_pipeline(program.pipeline);
            (*pass)->set_bind_group(0, *group);
            (*pass)->dispatch(static_cast<u32>((in.rays.size() + 63) / 64));
            (*pass)->end();
        }
        const rhi::BufferBarrier to_copy{
            .buffer = *hits_buf, .src_stage = rhi::PipelineStage::ComputeShader, .src_access = rhi::AccessFlags::ShaderWrite,
            .dst_stage = rhi::PipelineStage::Transfer, .dst_access = rhi::AccessFlags::TransferRead,
            .offset = 0, .size = hits_size};
        (*encoder)->barrier({}, std::span<const rhi::BufferBarrier>{&to_copy, 1}, {});
        (*encoder)->copy_buffer_to_buffer(*hits_buf, *readback, rhi::BufferCopy{.src_offset = 0, .dst_offset = 0, .size = hits_size});
        const rhi::BufferBarrier to_host{
            .buffer = *readback, .src_stage = rhi::PipelineStage::Transfer, .src_access = rhi::AccessFlags::TransferWrite,
            .dst_stage = rhi::PipelineStage::Host, .dst_access = rhi::AccessFlags::HostRead, .offset = 0, .size = hits_size};
        (*encoder)->barrier({}, std::span<const rhi::BufferBarrier>{&to_host, 1}, {});
        auto cb = (*encoder)->finish();
        if (!cb) {
            error = "finish: " + cb.error().message;
            return std::nullopt;
        }
        const rhi::CommandBufferHandle handles[] = {*cb};
        rhi::SubmitDesc submit{};
        submit.command_buffers = handles;
        submit.label = "rt dispatch";
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
        std::vector<GpuHit> result(in.rays.size());
        std::memcpy(result.data(), mapped->data(), hits_size);
        device.unmap_buffer(*readback);
        return result;
    }

    // ---------------------------------------------------------------------------------------------
    // Acceleration structure over prism boxes (hardware probe)
    // ---------------------------------------------------------------------------------------------

    bool build_and_wait(Session &session, std::function<void(rhi::CommandEncoder &)> record, const char *label) {
        rhi::RhiDevice &device = session.device;
        auto encoder = device.create_command_encoder(rhi::CommandEncoderDesc{.label = label});
        if (!encoder) {
            fail(std::string(label) + ": create_command_encoder: " + encoder.error().message);
            return false;
        }
        record(**encoder);
        auto cb = (*encoder)->finish();
        if (!cb) {
            fail(std::string(label) + ": finish: " + cb.error().message);
            return false;
        }
        const rhi::CommandBufferHandle handles[] = {*cb};
        rhi::SubmitDesc submit{};
        submit.command_buffers = handles;
        submit.label = label;
        if (auto r = device.submit(submit); !r) {
            fail(std::string(label) + ": submit: " + r.error().message);
            return false;
        }
        device.wait_idle();
        return true;
    }

    std::optional<rhi::AccelerationStructureHandle> build_prism_tlas(Session &session, const Mesh &mesh,
                                                                     const disp::DisplacedMaterialParams &params) {
        rhi::RhiDevice &device = session.device;
        const u64 scratch_alignment =
            std::max<u64>(device.feature_properties().ray_tracing.min_acceleration_structure_scratch_offset_alignment, 1u);

        // One AABB per triangle: VkAabbPositionsKHR = (min xyz, max xyz), 24-byte stride.
        std::vector<f32> boxes;
        for (const disp::DisplacedTriangle &t : mesh.triangles) {
            const disp::PrismBounds b = disp::prism_bounds(disp::make_prism(t, params));
            const glm::vec3 pad(1.0e-4f);
            const glm::vec3 lo = b.min - pad, hi = b.max + pad;
            boxes.insert(boxes.end(), {lo.x, lo.y, lo.z, hi.x, hi.y, hi.z});
        }
        auto aabb_buffer = make_buffer(session, boxes.size() * sizeof(f32), rhi::BufferUsage::AccelerationStructureInput,
                                       rhi::MemoryLocation::HostUpload, "rt prism aabbs");
        if (!aabb_buffer) {
            return std::nullopt;
        }
        if (auto w = device.write_buffer(*aabb_buffer, 0, std::as_bytes(std::span<const f32>{boxes})); !w) {
            fail("write aabbs: " + w.error().message);
            return std::nullopt;
        }

        const rhi::AccelerationStructureGeometryDesc blas_geometry{
            .type = rhi::AccelerationStructureGeometryType::Aabbs,
            // Procedural primitives are reported to the candidate loop whatever the opaque flag says.
            .flags = rhi::AccelerationStructureGeometryFlags::None,
            .aabbs = rhi::AccelerationStructureAabbsDesc{.buffer = *aabb_buffer, .offset = 0, .stride = 6 * sizeof(f32)},
        };
        const rhi::AccelerationStructureBuildRangeInfo blas_range{.primitive_count = static_cast<u32>(mesh.triangles.size())};
        rhi::AccelerationStructureBuildDesc blas_build{
            .type = rhi::AccelerationStructureType::BottomLevel,
            .flags = rhi::AccelerationStructureBuildFlags::PreferFastTrace,
            .geometries = std::span<const rhi::AccelerationStructureGeometryDesc>{&blas_geometry, 1},
            .ranges = std::span<const rhi::AccelerationStructureBuildRangeInfo>{&blas_range, 1},
        };
        auto blas_sizes = device.acceleration_structure_build_sizes(blas_build);
        if (!blas_sizes) {
            fail("BLAS build sizes: " + blas_sizes.error().message);
            return std::nullopt;
        }
        auto blas = device.create_acceleration_structure(rhi::AccelerationStructureDesc{
            .type = rhi::AccelerationStructureType::BottomLevel, .size = blas_sizes->acceleration_structure_size,
            .label = "rt prism BLAS"});
        if (!blas) {
            fail("create BLAS: " + blas.error().message);
            return std::nullopt;
        }
        session.cleanup.steps.emplace_back([&device, h = *blas] { device.destroy_acceleration_structure(h); });
        auto blas_scratch = make_buffer(session, blas_sizes->build_scratch_size + scratch_alignment - 1u,
                                        rhi::BufferUsage::AccelerationStructureScratch, rhi::MemoryLocation::DeviceLocal,
                                        "rt BLAS scratch");
        if (!blas_scratch) {
            return std::nullopt;
        }
        auto blas_scratch_address = device.buffer_device_address(*blas_scratch);
        if (!blas_scratch_address) {
            fail("BLAS scratch address: " + blas_scratch_address.error().message);
            return std::nullopt;
        }
        blas_build.dst = *blas;
        blas_build.scratch_buffer = *blas_scratch;
        blas_build.scratch_offset =
            ((*blas_scratch_address + scratch_alignment - 1u) / scratch_alignment) * scratch_alignment - *blas_scratch_address;
        if (!build_and_wait(session, [&](rhi::CommandEncoder &e) {
                e.build_acceleration_structures(std::span<const rhi::AccelerationStructureBuildDesc>{&blas_build, 1});
            }, "rt BLAS build")) {
            return std::nullopt;
        }

        auto blas_address = device.acceleration_structure_device_address(*blas);
        if (!blas_address) {
            fail("BLAS address: " + blas_address.error().message);
            return std::nullopt;
        }
        rhi::AccelerationStructureInstance instance{};
        instance.set_custom_index_and_mask(0, 0xffu);
        instance.set_shader_binding_table_offset_and_flags(0, 0);
        instance.acceleration_structure_device_address = *blas_address;
        auto instance_buffer = make_buffer(session, sizeof(instance), rhi::BufferUsage::AccelerationStructureInput,
                                           rhi::MemoryLocation::HostUpload, "rt TLAS instances");
        if (!instance_buffer) {
            return std::nullopt;
        }
        if (auto w = device.write_buffer(*instance_buffer, 0, std::as_bytes(std::span{&instance, 1})); !w) {
            fail("write instance: " + w.error().message);
            return std::nullopt;
        }
        const rhi::AccelerationStructureGeometryDesc tlas_geometry{
            .type = rhi::AccelerationStructureGeometryType::Instances,
            .instances = rhi::AccelerationStructureInstancesDesc{.buffer = *instance_buffer},
        };
        const rhi::AccelerationStructureBuildRangeInfo tlas_range{.primitive_count = 1};
        rhi::AccelerationStructureBuildDesc tlas_build{
            .type = rhi::AccelerationStructureType::TopLevel,
            .flags = rhi::AccelerationStructureBuildFlags::PreferFastTrace,
            .geometries = std::span<const rhi::AccelerationStructureGeometryDesc>{&tlas_geometry, 1},
            .ranges = std::span<const rhi::AccelerationStructureBuildRangeInfo>{&tlas_range, 1},
        };
        auto tlas_sizes = device.acceleration_structure_build_sizes(tlas_build);
        if (!tlas_sizes) {
            fail("TLAS build sizes: " + tlas_sizes.error().message);
            return std::nullopt;
        }
        auto tlas = device.create_acceleration_structure(rhi::AccelerationStructureDesc{
            .type = rhi::AccelerationStructureType::TopLevel, .size = tlas_sizes->acceleration_structure_size,
            .label = "rt prism TLAS"});
        if (!tlas) {
            fail("create TLAS: " + tlas.error().message);
            return std::nullopt;
        }
        session.cleanup.steps.emplace_back([&device, h = *tlas] { device.destroy_acceleration_structure(h); });
        auto tlas_scratch = make_buffer(session, tlas_sizes->build_scratch_size + scratch_alignment - 1u,
                                        rhi::BufferUsage::AccelerationStructureScratch, rhi::MemoryLocation::DeviceLocal,
                                        "rt TLAS scratch");
        if (!tlas_scratch) {
            return std::nullopt;
        }
        auto tlas_scratch_address = device.buffer_device_address(*tlas_scratch);
        if (!tlas_scratch_address) {
            fail("TLAS scratch address: " + tlas_scratch_address.error().message);
            return std::nullopt;
        }
        tlas_build.dst = *tlas;
        tlas_build.scratch_buffer = *tlas_scratch;
        tlas_build.scratch_offset =
            ((*tlas_scratch_address + scratch_alignment - 1u) / scratch_alignment) * scratch_alignment - *tlas_scratch_address;
        if (!build_and_wait(session, [&](rhi::CommandEncoder &e) {
                e.build_acceleration_structures(std::span<const rhi::AccelerationStructureBuildDesc>{&tlas_build, 1});
                const rhi::GlobalBarrier ready{.src_stage = rhi::PipelineStage::AccelerationStructureBuild,
                                               .src_access = rhi::AccessFlags::AccelerationStructureWrite,
                                               .dst_stage = rhi::PipelineStage::ComputeShader,
                                               .dst_access = rhi::AccessFlags::AccelerationStructureRead};
                e.barrier(std::span<const rhi::GlobalBarrier>{&ready, 1}, {}, {});
            }, "rt TLAS build")) {
            return std::nullopt;
        }
        return *tlas;
    }

    // ---------------------------------------------------------------------------------------------
    // Comparison
    // ---------------------------------------------------------------------------------------------

    struct CpuHit {
        bool hit = false;
        u32 triangle = 0;
        disp::DisplacedHit h;
    };

    CpuHit cpu_closest(const Mesh &mesh, const disp::DisplacedMaterialParams &params, const disp::HeightfieldView &view,
                       const disp::HeightfieldHierarchy *hier, const glm::vec3 &o, const glm::vec3 &d, f32 t_min, f32 t_max) {
        CpuHit best;
        for (u32 i = 0; i < mesh.triangles.size(); ++i) {
            const disp::DisplacedHit h =
                disp::intersect_displaced_triangle(mesh.triangles[i], params, view, hier, o, d, t_min, t_max);
            if (h.status != disp::HitStatus::Miss && (!best.hit || h.t < best.h.t)) {
                best.hit = true;
                best.triangle = i;
                best.h = h;
            }
        }
        return best;
    }

    struct Comparison {
        u32 rays = 0;
        u32 both_hit = 0;
        u32 both_miss = 0;
        u32 disagree = 0;
        u32 field_off = 0;
        f32 max_t = 0.0f, max_pos = 0.0f, max_normal_angle = 0.0f, max_uv = 0.0f;
        u32 other_triangle = 0;
    };

    /// `a` is the reference (CPU), `b` the GPU result. A hit within a hair of a prism wall (barycentric
    /// ~ 0) is a numerical graze: prisms of adjacent triangles share the wall, so which of the two claims
    /// it may differ between CPU and GPU float rounding; such rays are not counted as disagreements.
    Comparison compare(const std::vector<CpuHit> &a, const std::vector<GpuHit> &b, f32 t_tol, f32 pos_tol) {
        Comparison c;
        for (usize i = 0; i < a.size(); ++i) {
            ++c.rays;
            const bool ha = a[i].hit;
            const bool hb_hit = b[i].status != 0u; // shader: HF_STATUS_MISS == 0, HIT == 1, BUDGET == 2
            if (!ha && !hb_hit) {
                ++c.both_miss;
                continue;
            }
            const f32 gpu_min_bary = hb_hit ? std::min({b[i].bary_height[0], b[i].bary_height[1], b[i].bary_height[2]}) : 1.0f;
            const f32 cpu_min_bary = ha ? std::min({a[i].h.barycentrics.x, a[i].h.barycentrics.y, a[i].h.barycentrics.z}) : 1.0f;
            if (ha != hb_hit) {
                if (std::min(gpu_min_bary, cpu_min_bary) < 2.0e-3f) {
                    continue; // wall graze
                }
                ++c.disagree;
                continue;
            }
            ++c.both_hit;
            const f32 dt = std::abs(a[i].h.t - b[i].t);
            const glm::vec3 gpos(b[i].position_u[0], b[i].position_u[1], b[i].position_u[2]);
            const glm::vec3 gnormal(b[i].normal_v[0], b[i].normal_v[1], b[i].normal_v[2]);
            const f32 dpos = glm::length(gpos - a[i].h.position);
            const f32 ang = std::acos(std::clamp(glm::dot(gnormal, a[i].h.normal), -1.0f, 1.0f));
            const f32 duv = glm::length(glm::vec2(b[i].position_u[3], b[i].normal_v[3]) - a[i].h.uv);
            if (b[i].triangle != a[i].triangle) {
                // Two prisms both hit at (nearly) the same t: a tie on a shared wall.
                ++c.other_triangle;
                if (dt > t_tol) {
                    ++c.field_off;
                    if (c.field_off <= 6) {
                        std::cout << std::format("    other-triangle ray {}: cpu tri {} t {:.6f} bary ({:.4f},{:.4f},{:.4f}) | gpu tri {} t {:.6f} bary ({:.4f},{:.4f},{:.4f})\n",
                                                 i, a[i].triangle, a[i].h.t, a[i].h.barycentrics.x, a[i].h.barycentrics.y, a[i].h.barycentrics.z,
                                                 b[i].triangle, b[i].t, b[i].bary_height[0], b[i].bary_height[1], b[i].bary_height[2]);
                    }
                }
                continue;
            }
            c.max_t = std::max(c.max_t, dt);
            c.max_pos = std::max(c.max_pos, dpos);
            c.max_normal_angle = std::max(c.max_normal_angle, ang);
            c.max_uv = std::max(c.max_uv, duv);
            if (dt > t_tol || dpos > pos_tol || ang > 0.02f || duv > 5.0e-3f) {
                ++c.field_off;
            }
        }
        return c;
    }

    void report(const char *label, const Comparison &c) {
        ++g_comparisons;
        std::cout << std::format(
            "  {}: rays {}, both hit {}, both miss {}, disagree {}, fields off {}, tie-other-triangle {}; max |dt| {:.2e}, |dpos| "
            "{:.2e}, normal angle {:.2e} rad, |duv| {:.2e}\n",
            label, c.rays, c.both_hit, c.both_miss, c.disagree, c.field_off, c.other_triangle, c.max_t, c.max_pos,
            c.max_normal_angle, c.max_uv);
        if (c.both_hit < c.rays / 10) {
            fail(std::string(label) + ": fixture produced too few hits to mean anything");
        }
        if (c.disagree > c.rays / 200 + 1) {
            fail(std::string(label) + ": hit/miss disagreement between GPU and CPU reference");
        }
        if (c.field_off > c.both_hit / 200 + 1) {
            fail(std::string(label) + ": hit fields (t/position/normal/uv) differ from the CPU reference");
        }
    }

    void run(Gpu &gpu) {
        Session session(*gpu.device);
        auto sampler = gpu.device->create_sampler(rhi::SamplerDesc{
            .min_filter = rhi::Filter::Nearest, .mag_filter = rhi::Filter::Nearest, .mipmap_mode = rhi::MipmapMode::Nearest,
            .address_u = rhi::AddressMode::ClampToEdge, .address_v = rhi::AddressMode::ClampToEdge,
            .address_w = rhi::AddressMode::ClampToEdge, .label = "rt sampler"});
        if (!sampler) {
            fail("create_sampler failed: " + sampler.error().message);
            return;
        }
        session.sampler = *sampler;
        session.cleanup.steps.emplace_back([&session] { session.device.destroy_sampler(session.sampler); });

        const Field field = make_rolling_field(64, 64, 0.9f);
        const disp::HeightfieldView view = field.view();
        const disp::HeightfieldHierarchy hierarchy = disp::HeightfieldHierarchy::build(view);
        auto gpu_field = upload_field(session, field, hierarchy);
        if (!gpu_field) {
            return;
        }

        disp::DisplacedMaterialParams params;
        params.height_scale = 0.25f;
        params.reference_height = 1.0f;
        params.max_steps = 256;
        const Mesh mesh = make_curved_patch(6);

        Inputs in;
        in.field = &*gpu_field;
        in.record = GpuRecord{
            .textures = {0u, 0u, 1u /* wrap */, params.max_steps},
            .sizes = {static_cast<std::int32_t>(field.width), static_cast<std::int32_t>(field.height),
                      static_cast<std::int32_t>(gpu_field->hierarchy_levels), 0},
            .scale = {params.height_scale, params.reference_height, params.tile_size.x, params.tile_size.y},
            .height_range = {params.height_min, params.height_max, 1.0f, 1.0f},
            .uv_transform = {0.0f, 0.0f, 0.0f, 0.0f},
        };
        for (const auto &t : mesh.triangles) {
            in.triangles.push_back(pack(t));
        }

        // Rays: from a hemisphere above the patch aimed at random points on it (mostly hits, with grazing
        // and side-entering ones), plus surface-leaving rays that start just above a displaced point.
        Rng rng(2024);
        std::vector<CpuHit> expected_cell, expected_hier;
        constexpr u32 kRays = 1500;
        for (u32 i = 0; i < kRays; ++i) {
            const disp::DisplacedTriangle &tri = mesh.triangles[static_cast<usize>(rng.uniform(0.0f, static_cast<f32>(mesh.triangles.size()) - 0.001f))];
            glm::vec3 origin, dir;
            if (i % 5 == 4) {
                // Leave the surface: start above the displaced point, go sideways/upward.
                glm::vec3 b(rng.uniform(0.05f, 0.9f), 0, 0);
                b.y = rng.uniform(0.05f, 0.95f - b.x);
                b.z = 1.0f - b.x - b.y;
                const glm::vec3 bary(b.z, b.x, b.y);
                const glm::vec2 uv = tri.uv[0] * bary.x + tri.uv[1] * bary.y + tri.uv[2] * bary.z;
                const f32 h = disp::sample_height(view, uv.x, uv.y);
                const glm::vec3 up = glm::normalize(bary.x * tri.normal[0] + bary.y * tri.normal[1] + bary.z * tri.normal[2]);
                origin = disp::prism_position(tri, params, bary, h) + up * 2.0e-3f;
                dir = glm::normalize(up * rng.uniform(0.05f, 1.0f) + glm::normalize(glm::cross(up, rng.unit_vector())) * 0.9f);
            } else {
                const glm::vec3 target = (tri.position[0] + tri.position[1] + tri.position[2]) / 3.0f +
                                         (tri.position[0] - tri.position[1]) * rng.uniform(-0.3f, 0.3f);
                dir = glm::normalize(glm::vec3(rng.uniform(-0.8f, 0.8f), rng.uniform(-0.8f, 0.8f), -1.0f));
                origin = target - dir * 2.0f;
            }
            const f32 t_min = 0.0f, t_max = 8.0f;
            in.rays.push_back(GpuRay{{origin.x, origin.y, origin.z, t_min}, {dir.x, dir.y, dir.z, t_max}});
            expected_cell.push_back(cpu_closest(mesh, params, view, nullptr, origin, dir, t_min, t_max));
            expected_hier.push_back(cpu_closest(mesh, params, view, &hierarchy, origin, dir, t_min, t_max));
        }

        // ---- Software fallback: compute only -----------------------------------------------------------
        std::cout << "== software fallback (compute only), " << mesh.triangles.size() << " triangles, " << in.rays.size()
                  << " rays\n";
        for (const bool hierarchical : {false, true}) {
            std::vector<slang::ShaderMacro> macros;
            if (hierarchical) {
                macros.push_back({"SFT_HF_RT_USE_HIERARCHY", "1"});
            }
            std::string error;
            auto program = build_program(session, "heightfield_rt_software", "softwareTraceMain", macros, error);
            if (!program) {
                fail("software probe: " + error);
                continue;
            }
            auto hits = dispatch(session, *program, in, error);
            if (!hits) {
                fail("software probe dispatch: " + error);
                continue;
            }
            report(hierarchical ? "software / hierarchical vs CPU" : "software / cell-exact vs CPU", compare(hierarchical ? expected_hier : expected_cell, *hits, 2.0e-3f, 2.0e-3f));
        }

        // ---- Hardware: inline ray query over prism AABBs ----------------------------------------------
        const rhi::FeatureSet &features = gpu.device->enabled_features();
        if (!features.has(rhi::Feature::AccelerationStructures) || !features.has(rhi::Feature::RayQuery)) {
            std::cout << "== hardware prism path: SKIPPED (device lacks AccelerationStructures/RayQuery)\n";
            return;
        }
        std::cout << "== hardware prism path (AABB BLAS + inline ray query)\n";
        auto tlas = build_prism_tlas(session, mesh, params);
        if (!tlas) {
            return;
        }
        in.tlas = *tlas;
        std::string error;
        auto program = build_program(session, "heightfield_rt_query_probe", "queryTraceMain", {}, error);
        if (!program) {
            fail("query probe: " + error);
            return;
        }
        auto hits = dispatch(session, *program, in, error);
        if (!hits) {
            fail("query probe dispatch: " + error);
            return;
        }
        report("ray query / prism BVH vs CPU (hierarchical)", compare(expected_hier, *hits, 2.0e-3f, 2.0e-3f));
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
#if defined(DISPLACEMENT_RT_WEBGPU)
        gpu = make_webgpu_gpu(skip_reason);
#else
        skip_reason = "built without WebGPU support (STURDY_ENABLE_WEBGPU)";
#endif
    } else {
        std::cerr << "usage: DisplacementRayTracingGpuTest [vulkan|webgpu]\n";
        return 2;
    }
    if (!gpu) {
        std::cout << "SKIPPED: " << skip_reason << '\n';
        return 0;
    }
    std::cout << "Device: " << gpu->description << '\n';
    (void)watch.take();

    run(*gpu);
    for (const std::string &message : watch.take()) {
        fail("API error logged: " + message);
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "DisplacementRayTracingGpuTest passed (" << g_comparisons << " comparison sets)\n";
    return 0;
}
