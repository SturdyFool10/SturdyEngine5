#include <AudioGpu/GpuComputeMixer.hpp>

#include <Core/Core.hpp>
#include <Audio/Text.hpp>
#include <Foundation/Log.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/ShaderTarget.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace SFT::AudioGpu {

    namespace rhi = SFT::RHI;
    namespace slang = SFT::Core::Slang;

    namespace {

        /// What a step that can fail returns: nothing on success, else the message.
        using Failure = std::optional<UString>;

        template <class... Args>
        [[nodiscard]] UString failure(std::format_string<Args...> message, Args &&...args) {
            return UString{std::format(message, std::forward<Args>(args)...)};
        }

        // Records mirrored by Shaders/audio_mix.slang: plain 4-byte words, so every layout rule agrees.
        struct GpuVoice {
            u32 sample, flags, first_frame, valid_frames;
            i32 base;
            f32 frac, step;
            u32 first_signal;
            f32 b0, b1, b2, a1, a2, z1, z2;
            u32 pad;
        };
        struct GpuSample {
            u32 offset, frames, channels, pad;
        };
        struct GpuTap {
            u32 signal;
            f32 gain_from, gain_to;
            u32 pad;
        };
        struct GpuItem {
            u32 first_tap, tap_count, destination, pad;
        };
        struct GpuRange {
            u32 first_item, item_count;
        };
        struct MixParams {
            u32 frames, voice_count, item_count, destination_count;
            u32 row_threads, pad0, pad1, pad2;
        };
        static_assert(sizeof(GpuVoice) == 64 && sizeof(GpuSample) == 16 && sizeof(GpuTap) == 16 && sizeof(GpuItem) == 16 && sizeof(GpuRange) == 8 &&
                      sizeof(MixParams) == 32);

        /// Taps one thread sums for one frame before the per-destination reduce adds the runs together.
        constexpr u32 kTapsPerItem = 64;
        /// Groups per dispatch row; larger dispatches wrap onto further rows (the kernels take `row_threads`).
        constexpr u32 kGroupsPerRow = 4096;
        constexpr u32 kGroupThreads = 64;

        u64 align_up(u64 value, u64 alignment) { return (value + alignment - 1) / alignment * alignment; }
        u64 next_pow2(u64 value) {
            u64 p = 1;
            while (p < value) {
                p <<= 1;
            }
            return p;
        }

        struct Program {
            rhi::ShaderModuleHandle module{};
            rhi::BindGroupLayoutHandle layout{};
            rhi::PipelineLayoutHandle pipeline_layout{};
            rhi::ComputePipelineHandle pipeline{};
            std::vector<rhi::BindGroupLayoutEntry> entries;
            std::vector<Renderer::ReflectedResource> resources;
            u64 uniform_size = 0;
            std::string entry;
            rhi::BindGroupHandle group{};
        };

        struct Buffer {
            rhi::BufferHandle handle{};
            u64 size = 0;
        };

    } // namespace

    struct GpuComputeMixer::Impl {
        rhi::RhiDevice &device;
        GpuMixerConfig config;
        UString device_name;
        bool broken = false;

        std::array<Program, 3> programs; // voiceMain, tapMain, reduceMain
        rhi::FenceHandle fence{};
        bool in_flight = false;

        // Fixed buffers.
        Buffer pool, table, uniform;
        // Per-block buffers, grown on demand (and the bind groups rebuilt when they are).
        Buffer voices, signals, filter_state, taps, items, ranges, partial, mix, readback;
        bool groups_dirty = true;

        // Samples.
        struct Slot {
            u32 offset = 0; // in floats
            u32 frames = 0;
            u32 channels = 1;
            std::shared_ptr<const std::vector<f32>> data;
            enum class State : u8 { Pending, Uploading, Resident } state = State::Pending;
        };
        mutable std::mutex mutex;
        std::unordered_map<u32, Slot> slots;
        std::vector<u32> free_ids;
        u32 next_id = 1;
        struct Range {
            u32 offset, size;
        };
        std::vector<Range> free_ranges; // sorted by offset, in floats
        std::vector<u32> uploading;

        // Block state.
        std::vector<GpuVoice> voice_records;
        std::vector<GpuTap> tap_records;
        std::vector<GpuItem> item_records;
        std::vector<GpuRange> range_records;
        std::vector<rhi::BufferHandle> staging;
        u64 readback_mix_bytes = 0;
        u32 block_frames = 0, block_destinations = 0;
        usize block_voices = 0;
        bool block_submitted = false;
        std::vector<f32> mix_result, state_result;

        explicit Impl(rhi::RhiDevice &d, GpuMixerConfig c) : device(d), config(std::move(c)) {}
        ~Impl() { teardown(); }

        void teardown() {
            device.wait_idle();
            for (rhi::BufferHandle b : staging) {
                device.destroy_buffer(b);
            }
            staging.clear();
            for (Program &p : programs) {
                if (p.group) device.destroy_bind_group(p.group);
                if (p.pipeline) device.destroy_compute_pipeline(p.pipeline);
                if (p.pipeline_layout) device.destroy_pipeline_layout(p.pipeline_layout);
                if (p.layout) device.destroy_bind_group_layout(p.layout);
                if (p.module) device.destroy_shader_module(p.module);
                p = Program{};
            }
            for (Buffer *b : {&pool, &table, &uniform, &voices, &signals, &filter_state, &taps, &items, &ranges, &partial, &mix, &readback}) {
                if (b->handle) device.destroy_buffer(b->handle);
                *b = Buffer{};
            }
            if (fence) {
                device.destroy_fence(fence);
                fence = {};
            }
        }

        [[nodiscard]] Failure make_buffer(Buffer &out, u64 size, rhi::BufferUsage usage, rhi::MemoryLocation memory, const char *label) {
            size = std::max<u64>(align_up(size, 16), 16);
            auto created = device.create_buffer(rhi::BufferDesc{.size = size, .usage = usage, .memory = memory, .label = label});
            if (!created) {
                return failure("create_buffer({}, {} bytes): {}", label, size, created.error().message);
            }
            out = Buffer{*created, size};
            return std::nullopt;
        }

        [[nodiscard]] Failure build_program(Program &program, const char *entry) {
            const auto target = Renderer::shader_target_for_device(device);
            if (!target) {
                return target.error().message;
            }
            slang::ShaderCompileOptions options{};
            options.targets = {target->slang_target};
            options.entry_points = {slang::ShaderEntryPointRequest{.name = entry, .stage = slang::ShaderStage::Compute}};
            slang::ShaderCompiler compiler;
            auto shader = compiler.compile(slang::ShaderSource::from_file(config.shader_path, "audio_mix"), options);
            if (!shader) {
                return failure("compile {} ({}): {}\n{}", config.shader_path, entry, shader.error().message, shader.error().diagnostics);
            }
            auto code = shader->entry_point_code(std::string_view{entry}, target->slang_target.format);
            if (!code) {
                return failure("entry_point_code({}): {}", entry, code.error().message);
            }
            program.entry = entry;
            auto module = device.create_shader_module(rhi::ShaderModuleDesc{
                .language = target->module_language, .code = std::span<const std::byte>{code->bytes.data(), code->bytes.size()}, .label = "audio mix"});
            if (!module) {
                return failure("create_shader_module({}): {}", entry, module.error().message);
            }
            program.module = *module;
            const slang::ShaderReflection &reflection = shader->reflection();
            const auto generated = Renderer::generate_bind_group_layouts(reflection, rhi::ShaderStage::Compute);
            if (generated.empty()) {
                return failure("reflection of {} produced no bind group layout", entry);
            }
            program.entries = generated.front().entries;
            program.resources = Renderer::collect_resource_bindings(reflection);
            program.uniform_size = reflection.global_constant_buffer_size;
            auto layout = device.create_bind_group_layout(rhi::BindGroupLayoutDesc{.entries = std::span<const rhi::BindGroupLayoutEntry>{program.entries}, .label = "audio mix layout"});
            if (!layout) {
                return failure("create_bind_group_layout({}): {}", entry, layout.error().message);
            }
            program.layout = *layout;
            const rhi::BindGroupLayoutHandle layouts[] = {*layout};
            auto pipeline_layout = device.create_pipeline_layout(rhi::PipelineLayoutDesc{.bind_group_layouts = layouts, .push_constant_ranges = {}, .label = "audio mix pipeline layout"});
            if (!pipeline_layout) {
                return failure("create_pipeline_layout({}): {}", entry, pipeline_layout.error().message);
            }
            program.pipeline_layout = *pipeline_layout;
            auto pipeline = device.create_compute_pipeline(rhi::ComputePipelineDesc{
                .layout = *pipeline_layout,
                .compute = rhi::ShaderEntry{.module = *module, .entry_point = program.entry.c_str(), .stage = rhi::ShaderStage::Compute},
                .label = "audio mix pipeline"});
            if (!pipeline) {
                return failure("create_compute_pipeline({}): {}", entry, pipeline.error().message);
            }
            program.pipeline = *pipeline;
            return std::nullopt;
        }

        [[nodiscard]] Failure initialise() {
            constexpr std::array<const char *, 3> entries{"voiceMain", "tapMain", "reduceMain"};
            for (usize i = 0; i < programs.size(); ++i) {
                if (auto error = build_program(programs[i], entries[i])) {
                    return error;
                }
            }
            using U = rhi::BufferUsage;
            if (auto e = make_buffer(pool, config.sample_pool_bytes, U::Storage | U::TransferDst, rhi::MemoryLocation::DeviceLocal, "audio sample pool")) return e;
            if (auto e = make_buffer(table, static_cast<u64>(config.max_samples + 1) * sizeof(GpuSample), U::Storage | U::TransferDst, rhi::MemoryLocation::HostUpload, "audio sample table")) return e;
            if (auto e = make_buffer(uniform, std::max<u64>(programs[0].uniform_size, sizeof(MixParams)), U::Uniform | U::TransferDst, rhi::MemoryLocation::HostUpload, "audio mix params")) return e;
            auto created = device.create_fence(rhi::FenceDesc{.signaled = false, .label = "audio mix"});
            if (!created) {
                return "create_fence: " + created.error().message;
            }
            fence = *created;
            free_ranges.push_back(Range{0, static_cast<u32>(std::min<u64>(config.sample_pool_bytes / sizeof(f32), 0xFFFFFFFFull))});
            device_name = Audio::text_from_bytes(device.adapter_info().name);
            return std::nullopt;
        }

        // ---- sample pool ------------------------------------------------------------------------------------------

        [[nodiscard]] std::optional<u32> allocate(u32 floats) {
            for (auto it = free_ranges.begin(); it != free_ranges.end(); ++it) {
                if (it->size >= floats) {
                    const u32 offset = it->offset;
                    it->offset += floats;
                    it->size -= floats;
                    if (it->size == 0) {
                        free_ranges.erase(it);
                    }
                    return offset;
                }
            }
            return std::nullopt;
        }
        void free_range(u32 offset, u32 size) {
            auto it = std::lower_bound(free_ranges.begin(), free_ranges.end(), offset, [](const Range &r, u32 o) { return r.offset < o; });
            it = free_ranges.insert(it, Range{offset, size});
            if (it + 1 != free_ranges.end() && it->offset + it->size == (it + 1)->offset) {
                it->size += (it + 1)->size;
                free_ranges.erase(it + 1);
            }
            if (it != free_ranges.begin() && (it - 1)->offset + (it - 1)->size == it->offset) {
                (it - 1)->size += it->size;
                free_ranges.erase(it);
            }
        }

        // ---- per block --------------------------------------------------------------------------------------------

        /// Grows `buffer` to hold `bytes` (rounded up to a power of two); true when it was reallocated.
        [[nodiscard]] Failure ensure(Buffer &buffer, u64 bytes, rhi::BufferUsage usage, rhi::MemoryLocation memory, const char *label) {
            if (buffer.size >= bytes && buffer.handle) {
                return std::nullopt;
            }
            if (buffer.handle) {
                device.destroy_buffer(buffer.handle);
                buffer = Buffer{};
            }
            groups_dirty = true;
            return make_buffer(buffer, next_pow2(std::max<u64>(bytes, 256)), usage, memory, label);
        }

        [[nodiscard]] std::optional<rhi::BufferHandle> lookup(std::string_view name) const {
            if (name == "samplePool") return pool.handle;
            if (name == "sampleTable") return table.handle;
            if (name == "params") return uniform.handle;
            if (name == "voices") return voices.handle;
            if (name == "signals") return signals.handle;
            if (name == "filterState") return filter_state.handle;
            if (name == "taps") return taps.handle;
            if (name == "items") return items.handle;
            if (name == "ranges") return ranges.handle;
            if (name == "partial") return partial.handle;
            if (name == "mix") return mix.handle;
            return std::nullopt;
        }
        [[nodiscard]] u64 size_of(std::string_view name) const {
            for (const Buffer *b : {&pool, &table, &uniform, &voices, &signals, &filter_state, &taps, &items, &ranges, &partial, &mix}) {
                const auto handle = lookup(name);
                if (handle && b->handle == *handle) {
                    return b->size;
                }
            }
            return 0;
        }

        [[nodiscard]] Failure rebuild_groups() {
            for (Program &p : programs) {
                if (p.group) {
                    device.destroy_bind_group(p.group);
                    p.group = {};
                }
                std::vector<rhi::BindGroupEntry> entries;
                for (const rhi::BindGroupLayoutEntry &entry : p.entries) {
                    std::string name;
                    for (const auto &res : p.resources) {
                        if (res.binding == entry.binding) {
                            name = res.name;
                        }
                    }
                    const auto handle = lookup(name);
                    if (!handle) {
                        return failure("kernel {} binds unknown resource '{}'", p.entry, name);
                    }
                    rhi::BindGroupEntry g{.binding = entry.binding};
                    g.buffer = *handle;
                    g.size = size_of(name);
                    entries.push_back(g);
                }
                auto group = device.create_bind_group(rhi::BindGroupDesc{.layout = p.layout, .entries = entries, .lifetime = rhi::BindGroupLifetime::Persistent, .label = "audio mix group"});
                if (!group) {
                    return failure("create_bind_group({}): {}", p.entry, group.error().message);
                }
                p.group = *group;
            }
            groups_dirty = false;
            return std::nullopt;
        }

        void record_dispatch(rhi::ComputePassEncoder &pass, const Program &program, u64 threads) {
            const u64 groups = (threads + kGroupThreads - 1) / kGroupThreads;
            const u32 x = static_cast<u32>(std::min<u64>(groups, kGroupsPerRow));
            const u32 y = static_cast<u32>((groups + kGroupsPerRow - 1) / kGroupsPerRow);
            pass.set_pipeline(program.pipeline);
            pass.set_bind_group(0, program.group);
            pass.dispatch(x, y);
            pass.end();
        }

        [[nodiscard]] Failure run(const Audio::ComputeBlock &block) {
            const u32 frames = block.frames;
            const usize voice_count = block.voices.size();
            const bool work = voice_count > 0 && !block.taps.empty() && block.destination_count > 0;
            if (!work) {
                return upload_only();
            }
            using U = rhi::BufferUsage;
            using M = rhi::MemoryLocation;

            // Records.
            voice_records.resize(voice_count);
            for (usize i = 0; i < voice_count; ++i) {
                const Audio::ComputeVoice &v = block.voices[i];
                voice_records[i] = GpuVoice{v.sample, (v.loop ? 1u : 0u) | (v.mix_down ? 2u : 0u) | (v.filter ? 4u : 0u), v.first_frame, v.valid_frames,
                                            static_cast<i32>(std::clamp<i64>(v.base, -0x7FFFFFFF, 0x7FFFFFFF)), v.frac, v.step, v.first_signal,
                                            v.b0, v.b1, v.b2, v.a1, v.a2, v.z1, v.z2, 0};
            }
            tap_records.resize(block.taps.size());
            item_records.clear();
            range_records.assign(block.destination_count, GpuRange{0, 0});
            for (usize t = 0; t < block.taps.size(); ++t) {
                const Audio::ComputeTap &tap = block.taps[t];
                tap_records[t] = GpuTap{tap.signal, tap.gain_from, tap.gain_to, 0};
                GpuRange &range = range_records[tap.destination];
                const bool open = range.item_count > 0 && item_records.back().destination == tap.destination && item_records.back().tap_count < kTapsPerItem;
                if (open) {
                    ++item_records.back().tap_count;
                } else {
                    if (range.item_count == 0) {
                        range.first_item = static_cast<u32>(item_records.size());
                    }
                    ++range.item_count;
                    item_records.push_back(GpuItem{static_cast<u32>(t), 1, tap.destination, 0});
                }
            }

            const u64 mix_floats = static_cast<u64>(block.destination_count) * frames;
            const u64 partial_floats = static_cast<u64>(item_records.size()) * frames;
            const u64 signal_floats = static_cast<u64>(block.signal_count) * frames;
            if (auto e = ensure(voices, voice_count * sizeof(GpuVoice), U::Storage | U::TransferDst, M::HostUpload, "audio voices")) return e;
            if (auto e = ensure(taps, tap_records.size() * sizeof(GpuTap), U::Storage | U::TransferDst, M::HostUpload, "audio taps")) return e;
            if (auto e = ensure(items, item_records.size() * sizeof(GpuItem), U::Storage | U::TransferDst, M::HostUpload, "audio items")) return e;
            if (auto e = ensure(ranges, range_records.size() * sizeof(GpuRange), U::Storage | U::TransferDst, M::HostUpload, "audio ranges")) return e;
            if (auto e = ensure(signals, signal_floats * sizeof(f32), U::Storage, M::DeviceLocal, "audio signals")) return e;
            if (auto e = ensure(filter_state, voice_count * 2 * sizeof(f32), U::Storage | U::TransferSrc, M::DeviceLocal, "audio filter state")) return e;
            if (auto e = ensure(partial, partial_floats * sizeof(f32), U::Storage, M::DeviceLocal, "audio partial sums")) return e;
            if (auto e = ensure(mix, mix_floats * sizeof(f32), U::Storage | U::TransferSrc, M::DeviceLocal, "audio mix")) return e;
            readback_mix_bytes = mix_floats * sizeof(f32);
            const u64 readback_bytes = readback_mix_bytes + voice_count * 2 * sizeof(f32);
            if (auto e = ensure(readback, readback_bytes, U::TransferDst, M::HostReadback, "audio readback")) return e;
            if (groups_dirty) {
                if (auto e = rebuild_groups()) return e;
            }

            auto write = [&](const Buffer &buffer, const void *data, u64 bytes) -> Failure {
                if (auto r = device.write_buffer(buffer.handle, 0, std::span<const std::byte>{static_cast<const std::byte *>(data), static_cast<usize>(bytes)}); !r) {
                    return "write_buffer: " + r.error().message;
                }
                return std::nullopt;
            };
            const MixParams params{frames, static_cast<u32>(voice_count), static_cast<u32>(item_records.size()), block.destination_count, kGroupsPerRow * kGroupThreads, 0, 0, 0};
            if (auto e = write(uniform, &params, sizeof(params))) return e;
            if (auto e = write(voices, voice_records.data(), voice_count * sizeof(GpuVoice))) return e;
            if (auto e = write(taps, tap_records.data(), tap_records.size() * sizeof(GpuTap))) return e;
            if (auto e = write(items, item_records.data(), item_records.size() * sizeof(GpuItem))) return e;
            if (auto e = write(ranges, range_records.data(), range_records.size() * sizeof(GpuRange))) return e;

            auto encoder = device.create_command_encoder(rhi::CommandEncoderDesc{.label = "audio mix"});
            if (!encoder) {
                return "create_command_encoder: " + encoder.error().message;
            }
            rhi::CommandEncoder &enc = **encoder;
            if (auto e = record_uploads(enc)) return e;

            auto buffer_barrier = [&](const Buffer &b, rhi::PipelineStage src_stage, rhi::AccessFlags src, rhi::PipelineStage dst_stage, rhi::AccessFlags dst) {
                const rhi::BufferBarrier barrier{.buffer = b.handle, .src_stage = src_stage, .src_access = src, .dst_stage = dst_stage, .dst_access = dst, .offset = 0, .size = b.size};
                enc.barrier({}, std::span<const rhi::BufferBarrier>{&barrier, 1}, {});
            };
            constexpr auto compute = rhi::PipelineStage::ComputeShader;
            {
                auto pass = enc.begin_compute_pass(rhi::ComputePassDesc{.label = "audio voices"});
                if (!pass) return "begin_compute_pass: " + pass.error().message;
                record_dispatch(**pass, programs[0], voice_count);
            }
            buffer_barrier(signals, compute, rhi::AccessFlags::ShaderWrite, compute, rhi::AccessFlags::ShaderRead);
            {
                auto pass = enc.begin_compute_pass(rhi::ComputePassDesc{.label = "audio taps"});
                if (!pass) return "begin_compute_pass: " + pass.error().message;
                record_dispatch(**pass, programs[1], partial_floats);
            }
            buffer_barrier(partial, compute, rhi::AccessFlags::ShaderWrite, compute, rhi::AccessFlags::ShaderRead);
            {
                auto pass = enc.begin_compute_pass(rhi::ComputePassDesc{.label = "audio reduce"});
                if (!pass) return "begin_compute_pass: " + pass.error().message;
                record_dispatch(**pass, programs[2], mix_floats);
            }
            buffer_barrier(mix, compute, rhi::AccessFlags::ShaderWrite, rhi::PipelineStage::Transfer, rhi::AccessFlags::TransferRead);
            buffer_barrier(filter_state, compute, rhi::AccessFlags::ShaderWrite, rhi::PipelineStage::Transfer, rhi::AccessFlags::TransferRead);
            enc.copy_buffer_to_buffer(mix.handle, readback.handle, rhi::BufferCopy{.src_offset = 0, .dst_offset = 0, .size = readback_mix_bytes});
            enc.copy_buffer_to_buffer(filter_state.handle, readback.handle, rhi::BufferCopy{.src_offset = 0, .dst_offset = readback_mix_bytes, .size = voice_count * 2 * sizeof(f32)});
            buffer_barrier(readback, rhi::PipelineStage::Transfer, rhi::AccessFlags::TransferWrite, rhi::PipelineStage::Host, rhi::AccessFlags::HostRead);

            if (auto e = submit_encoder(enc)) return e;
            block_frames = frames;
            block_destinations = block.destination_count;
            block_voices = voice_count;
            return std::nullopt;
        }

        /// A block with nothing to mix still carries any pending sample uploads, so samples become resident even while no
        /// voice is playing on the device yet (voices only go there once their sample is).
        [[nodiscard]] Failure upload_only() {
            auto encoder = device.create_command_encoder(rhi::CommandEncoderDesc{.label = "audio upload"});
            if (!encoder) {
                return "create_command_encoder: " + encoder.error().message;
            }
            if (auto e = record_uploads(**encoder)) return e;
            if (uploading.empty()) {
                return std::nullopt; // nothing was pending; the empty encoder is simply dropped
            }
            block_frames = 0;
            block_destinations = 0;
            block_voices = 0;
            readback_mix_bytes = 0;
            return submit_encoder(**encoder);
        }

        [[nodiscard]] Failure submit_encoder(rhi::CommandEncoder &enc) {
            auto finished = enc.finish();
            if (!finished) {
                return "finish: " + finished.error().message;
            }
            if (auto r = device.reset_fences(std::span<const rhi::FenceHandle>{&fence, 1}); !r) {
                return "reset_fences: " + r.error().message;
            }
            const rhi::CommandBufferHandle handles[] = {*finished};
            rhi::SubmitDesc submit{};
            submit.command_buffers = handles;
            submit.fence = fence;
            submit.label = "audio mix";
            if (auto r = device.submit(submit); !r) {
                return "submit: " + r.error().message;
            }
            in_flight = true;
            return std::nullopt;
        }

        /// Copies samples registered since the last block into the pool (bounded per block) and marks them uploading.
        [[nodiscard]] Failure record_uploads(rhi::CommandEncoder &enc) {
            struct Upload {
                u32 id, offset;
                std::shared_ptr<const std::vector<f32>> data;
            };
            std::vector<Upload> batch;
            u64 bytes = 0;
            {
                std::lock_guard lock(mutex);
                for (auto &[id, slot] : slots) {
                    if (slot.state != Slot::State::Pending) {
                        continue;
                    }
                    const u64 size = static_cast<u64>(slot.data->size()) * sizeof(f32);
                    if (!batch.empty() && bytes + size > config.upload_bytes_per_block) {
                        continue;
                    }
                    batch.push_back(Upload{id, slot.offset, slot.data});
                    bytes += size;
                    slot.state = Slot::State::Uploading;
                }
            }
            if (batch.empty()) {
                return std::nullopt;
            }
            Buffer stage;
            if (auto e = make_buffer(stage, bytes, rhi::BufferUsage::TransferSrc, rhi::MemoryLocation::HostUpload, "audio sample staging")) return e;
            staging.push_back(stage.handle);
            u64 cursor = 0;
            for (const Upload &upload : batch) {
                const u64 size = static_cast<u64>(upload.data->size()) * sizeof(f32);
                if (auto r = device.write_buffer(stage.handle, cursor, std::as_bytes(std::span<const f32>{*upload.data})); !r) {
                    return "write staging: " + r.error().message;
                }
                enc.copy_buffer_to_buffer(stage.handle, pool.handle, rhi::BufferCopy{.src_offset = cursor, .dst_offset = static_cast<u64>(upload.offset) * sizeof(f32), .size = size});
                cursor += size;
                uploading.push_back(upload.id);
            }
            const rhi::BufferBarrier barrier{.buffer = pool.handle,
                                             .src_stage = rhi::PipelineStage::Transfer,
                                             .src_access = rhi::AccessFlags::TransferWrite,
                                             .dst_stage = rhi::PipelineStage::ComputeShader,
                                             .dst_access = rhi::AccessFlags::ShaderRead,
                                             .offset = 0,
                                             .size = pool.size};
            enc.barrier({}, std::span<const rhi::BufferBarrier>{&barrier, 1}, {});
            return std::nullopt;
        }

        void finish_uploads() {
            for (rhi::BufferHandle b : staging) {
                device.destroy_buffer(b);
            }
            staging.clear();
            std::lock_guard lock(mutex);
            for (u32 id : uploading) {
                if (auto it = slots.find(id); it != slots.end() && it->second.state == Slot::State::Uploading) {
                    it->second.state = Slot::State::Resident;
                }
            }
            uploading.clear();
        }

        void fail(const UString &message) {
            if (!broken) {
                Foundation::log_error("AudioGpu: {} -- compute mixing is disabled; voices stay on the CPU mixer.", message);
            }
            broken = true;
        }
    };

    GpuComputeMixer::GpuComputeMixer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    GpuComputeMixer::~GpuComputeMixer() = default;

    std::expected<std::unique_ptr<GpuComputeMixer>, UString> GpuComputeMixer::create(rhi::RhiDevice &device, const GpuMixerConfig &config) {
        auto impl = std::make_unique<Impl>(device, config);
        if (auto error = impl->initialise()) {
            return std::unexpected(*error);
        }
        return std::unique_ptr<GpuComputeMixer>(new GpuComputeMixer(std::move(impl)));
    }

    Audio::ComputeBackendInfo GpuComputeMixer::info() const {
        return Audio::ComputeBackendInfo{"gpu", 1u << 17, 1u << 20};
    }

    const UString &GpuComputeMixer::device_name() const noexcept { return impl_->device_name; }

    Audio::ComputeSampleId GpuComputeMixer::register_sample(std::shared_ptr<const std::vector<f32>> samples, u32 channels) {
        Impl &m = *impl_;
        if (!samples || samples->empty() || channels == 0 || samples->size() > 0xFFFFFFFFull) {
            return 0;
        }
        std::lock_guard lock(m.mutex);
        const u32 floats = static_cast<u32>(samples->size());
        u32 id;
        if (!m.free_ids.empty()) {
            id = m.free_ids.back();
            m.free_ids.pop_back();
        } else if (m.next_id <= m.config.max_samples) {
            id = m.next_id++;
        } else {
            return 0;
        }
        const auto offset = m.allocate(floats);
        if (!offset) {
            m.free_ids.push_back(id);
            return 0; // the pool is full: this sample stays on the CPU mixer
        }
        Impl::Slot slot;
        slot.offset = *offset;
        slot.frames = floats / channels;
        slot.channels = channels;
        slot.data = std::move(samples);
        m.slots.emplace(id, std::move(slot));
        const GpuSample record{*offset, floats / channels, channels, 0};
        if (auto r = m.device.write_buffer(m.table.handle, static_cast<u64>(id) * sizeof(GpuSample), std::as_bytes(std::span{&record, 1})); !r) {
            m.fail(failure("write sample table: {}", r.error().message));
        }
        return id;
    }

    bool GpuComputeMixer::sample_ready(Audio::ComputeSampleId id) const {
        std::lock_guard lock(impl_->mutex);
        const auto it = impl_->slots.find(id);
        return it != impl_->slots.end() && it->second.state == Impl::Slot::State::Resident && !impl_->broken;
    }

    void GpuComputeMixer::release_sample(Audio::ComputeSampleId id) {
        Impl &m = *impl_;
        std::lock_guard lock(m.mutex);
        const auto it = m.slots.find(id);
        if (it == m.slots.end()) {
            return;
        }
        m.free_range(it->second.offset, static_cast<u32>(it->second.data->size()));
        m.slots.erase(it);
        m.free_ids.push_back(id);
    }

    void GpuComputeMixer::submit(const Audio::ComputeBlock &block) {
        Impl &m = *impl_;
        m.block_submitted = false;
        m.mix_result.clear();
        m.state_result.clear();
        if (m.broken) {
            return;
        }
        if (auto error = m.run(block)) {
            m.fail(*error);
            return;
        }
        m.block_submitted = m.in_flight;
    }

    Audio::ComputeResult GpuComputeMixer::collect() {
        Impl &m = *impl_;
        if (!m.in_flight) {
            return {};
        }
        m.in_flight = false;
        auto waited = m.device.wait_fences(std::span<const rhi::FenceHandle>{&m.fence, 1});
        if (!waited || !*waited) {
            m.fail("waiting for the mix fence failed"_ustr);
            return {};
        }
        m.finish_uploads();
        if (m.block_voices == 0) {
            return {}; // an upload-only block: nothing to read back
        }
        auto mapped = m.device.map_buffer(m.readback.handle);
        if (!mapped) {
            m.fail(failure("map_buffer: {}", mapped.error().message));
            return {};
        }
        const usize mix_floats = static_cast<usize>(m.block_destinations) * m.block_frames;
        m.mix_result.resize(mix_floats);
        m.state_result.resize(m.block_voices * 2);
        std::memcpy(m.mix_result.data(), mapped->data(), mix_floats * sizeof(f32));
        std::memcpy(m.state_result.data(), mapped->data() + m.readback_mix_bytes, m.block_voices * 2 * sizeof(f32));
        m.device.unmap_buffer(m.readback.handle);
        return Audio::ComputeResult{m.mix_result, m.state_result};
    }

} // namespace SFT::AudioGpu
