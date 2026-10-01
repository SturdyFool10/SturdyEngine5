#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/RendererModule.hpp>

#include <tracy/Tracy.hpp>

using std::span;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {

        constexpr u32 kSkinWorkgroupSize = 64;

        struct SkinConstants {
            u32 dst_first_vertex = 0;
            u32 vertex_count = 0;
            u32 joint_base = 0;
            u32 morph_weight_base = 0;
            u32 morph_target_count = 0;
            u32 first_pose = 0;
            u32 padding[2]{};
        };
        static_assert(sizeof(SkinConstants) == 32);

        struct SkinJob {
            SkinConstants constants;
            RHI::BufferHandle bind_buffer;
            RHI::BufferHandle influence_buffer;
            RHI::BufferHandle morph_offset_buffer;
            RHI::BufferHandle morph_entry_buffer;
        };

        [[nodiscard]] Core::GraphicsBackendError skinning_error(const char *message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, message};
        }

    } // namespace

    Core::RendererResult Renderer::attach_skin(MeshHandle handle, const SkinAttachDesc &desc) {
        ZoneScopedN("Renderer::attach_skin");
        MeshResource *resource = mesh(handle);
        RHI::RhiDevice *device = rhi_device();
        if (resource == nullptr || !resource->gpu_resident || device == nullptr) {
            return unexpected(skinning_error("attach_skin needs a GPU-resident mesh and an RHI device."));
        }
        if (desc.influences.size() != resource->vertices.size()) {
            return unexpected(skinning_error("attach_skin needs one influence per mesh vertex."));
        }
        {
            auto skinning = skinning_.lock();
            SkinResource skin;
            skin.bind_vertices = resource->vertices;
            skin.influences.assign(desc.influences.begin(), desc.influences.end());
            if (desc.morph.target_count != 0 && desc.morph.vertex_offsets.size() == resource->vertices.size() + 1) {
                skin.morph_target_count = desc.morph.target_count;
                skin.morph_offsets.assign(desc.morph.vertex_offsets.begin(), desc.morph.vertex_offsets.end());
                skin.morph_entries.assign(desc.morph.entries.begin(), desc.morph.entries.end());
            }
            skinning->skins[handle.value] = std::move(skin);
        }
        resource->has_lens_variant = false;
        resource->skinned = true;
        resource->bounds_radius *= std::max(desc.bounds_scale, 1.0f);
        return {};
    }

    Core::RendererResult Renderer::set_skin_pose(MeshHandle handle, span<const glm::mat4> matrices,
                                                 span<const f32> morph_weights) {
        auto skinning = skinning_.lock();
        const auto it = skinning->skins.find(handle.value);
        if (it == skinning->skins.end()) {
            return unexpected(skinning_error("set_skin_pose: the mesh has no GPU skin."));
        }
        it->second.pending.assign(matrices.begin(), matrices.end());
        if (it->second.morph_target_count != 0) {
            it->second.pending_weights.assign(it->second.morph_target_count, 0.0f);
            std::copy_n(morph_weights.begin(), std::min<usize>(morph_weights.size(), it->second.morph_target_count),
                        it->second.pending_weights.begin());
        }
        it->second.dirty = true;
        return {};
    }

    RHI::BufferHandle Renderer::ensure_previous_positions_buffer(u64 min_vertices) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return {};
        }
        auto skinning = skinning_.lock();
        if (skinning->previous_positions && skinning->previous_capacity_vertices >= min_vertices) {
            return skinning->previous_positions;
        }
        const u64 capacity = std::max<u64>({min_vertices, skinning->previous_capacity_vertices * 2, 1024});
        auto created = device->create_buffer(RHI::BufferDesc{
            .size = capacity * sizeof(glm::vec4),
            .usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferDst,
            .memory = RHI::MemoryLocation::DeviceLocal,
            .label = "skinned previous positions",
        });
        if (!created) {
            return skinning->previous_positions;
        }
        if (skinning->previous_positions) {
            wait_idle(); // an earlier frame may still read the old buffer
            device->destroy_buffer(skinning->previous_positions);
        }
        skinning->previous_positions = *created;
        skinning->previous_capacity_vertices = capacity;
        // The new buffer holds nothing: every skin restarts its history from its next pose.
        for (auto &[_, skin] : skinning->skins) {
            skin.first_pose = true;
            skin.dirty = skin.dirty || !skin.pending.empty();
        }
        return skinning->previous_positions;
    }

    Core::RendererResult Renderer::build_frame_feature_mesh_skinning(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::mesh_skinning");
        RHI::RhiDevice *device = &context.device;

        const RHI::BufferHandle previous_positions =
            ensure_previous_positions_buffer(std::max<u64>(vertex_arena_.capacity_bytes / sizeof(GeometryVertex), 1));
        if (!previous_positions) {
            return {};
        }

        auto jobs = std::make_shared<vector<SkinJob>>();
        RHI::BufferHandle joint_buffer{};
        RHI::BufferHandle weight_buffer{};
        {
            auto skinning = skinning_.lock();
            vector<glm::mat4> packed;
            vector<f32> packed_weights;
            const auto make_storage = [&](u64 bytes, const char *label, RHI::MemoryLocation memory) {
                return device->create_buffer(RHI::BufferDesc{
                    .size = std::max<u64>(bytes, 64), .usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferDst,
                    .memory = memory, .label = label});
            };
            if (!skinning->dummy_buffer) {
                auto dummy = make_storage(64, "skin dummy buffer", RHI::MemoryLocation::DeviceLocal);
                if (dummy) {
                    skinning->dummy_buffer = *dummy;
                }
            }
            for (auto &[mesh_value, skin] : skinning->skins) {
                if ((!skin.dirty && !skin.settle_pending) || skin.pending.empty()) {
                    continue;
                }
                const MeshResource *resource = mesh(MeshHandle{mesh_value});
                if (resource == nullptr || !resource->gpu_resident) {
                    continue;
                }
                if (packed.size() + skin.pending.size() > SkinningState::kRingMatrices ||
                    packed_weights.size() + skin.pending_weights.size() > SkinningState::kRingWeights) {
                    continue; // over this frame's joint budget; stays dirty for the next one
                }
                if (!skin.bind_buffer) {
                    auto bind = device->create_buffer(RHI::BufferDesc{
                        .size = skin.bind_vertices.size() * sizeof(GeometryVertex),
                        .usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferDst,
                        .memory = RHI::MemoryLocation::DeviceLocal,
                        .label = "skin bind vertices",
                    });
                    if (!bind) {
                        continue;
                    }
                    auto influence = device->create_buffer(RHI::BufferDesc{
                        .size = skin.influences.size() * sizeof(SkinInfluence),
                        .usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferDst,
                        .memory = RHI::MemoryLocation::DeviceLocal,
                        .label = "skin influences",
                    });
                    if (!influence) {
                        device->destroy_buffer(*bind);
                        continue;
                    }
                    const auto w1 = device->write_buffer(*bind, 0, std::as_bytes(span<const GeometryVertex>{skin.bind_vertices}));
                    const auto w2 = device->write_buffer(*influence, 0, std::as_bytes(span<const SkinInfluence>{skin.influences}));
                    if (!w1 || !w2) {
                        device->destroy_buffer(*bind);
                        device->destroy_buffer(*influence);
                        continue;
                    }
                    skin.bind_buffer = *bind;
                    skin.influence_buffer = *influence;
                    if (skin.morph_target_count != 0) {
                        auto offsets = make_storage(skin.morph_offsets.size() * sizeof(u32), "skin morph offsets",
                                                    RHI::MemoryLocation::DeviceLocal);
                        auto entries = make_storage(skin.morph_entries.size() * sizeof(MorphDelta), "skin morph entries",
                                                    RHI::MemoryLocation::DeviceLocal);
                        const bool ok = offsets && entries &&
                                        device->write_buffer(*offsets, 0, std::as_bytes(span<const u32>{skin.morph_offsets})) &&
                                        (skin.morph_entries.empty() ||
                                         device->write_buffer(*entries, 0, std::as_bytes(span<const MorphDelta>{skin.morph_entries})));
                        if (!ok) {
                            if (offsets) device->destroy_buffer(*offsets);
                            if (entries) device->destroy_buffer(*entries);
                            device->destroy_buffer(skin.bind_buffer);
                            device->destroy_buffer(skin.influence_buffer);
                            skin.bind_buffer = {};
                            skin.influence_buffer = {};
                            continue;
                        }
                        skin.morph_offset_buffer = *offsets;
                        skin.morph_entry_buffer = *entries;
                    }
                }
                jobs->push_back(SkinJob{
                    .constants = SkinConstants{
                        .dst_first_vertex = resource->vertex_offset,
                        .vertex_count = static_cast<u32>(skin.bind_vertices.size()),
                        .joint_base = static_cast<u32>(packed.size()),
                        .morph_weight_base = static_cast<u32>(packed_weights.size()),
                        .morph_target_count = skin.morph_target_count,
                        .first_pose = skin.first_pose ? 1u : 0u,
                    },
                    .bind_buffer = skin.bind_buffer,
                    .influence_buffer = skin.influence_buffer,
                    .morph_offset_buffer = skin.morph_target_count != 0 ? skin.morph_offset_buffer : skinning->dummy_buffer,
                    .morph_entry_buffer = skin.morph_target_count != 0 ? skin.morph_entry_buffer : skinning->dummy_buffer,
                });
                packed.insert(packed.end(), skin.pending.begin(), skin.pending.end());
                packed_weights.insert(packed_weights.end(), skin.pending_weights.begin(), skin.pending_weights.end());
                // A moving skin is re-run once after its last change so its previous positions catch up with its
                // current ones; otherwise a character that stopped would keep reporting motion.
                skin.settle_pending = skin.dirty;
                skin.first_pose = false;
                skin.dirty = false;
            }
            if (jobs->empty()) {
                return {};
            }
            const u64 ring_index = skinning->ring_counter++ % SkinningState::kRingSlots;
            RHI::BufferHandle &slot = skinning->joint_ring[ring_index];
            RHI::BufferHandle &weight_slot = skinning->weight_ring[ring_index];
            if (!weight_slot) {
                auto created = device->create_buffer(RHI::BufferDesc{
                    .size = static_cast<u64>(SkinningState::kRingWeights) * sizeof(f32),
                    .usage = RHI::BufferUsage::Storage,
                    .memory = RHI::MemoryLocation::HostUpload,
                    .label = "skin morph weights",
                });
                if (!created) {
                    return unexpected(graphics_error_from_rhi(created.error(), "create skin morph weight ring buffer"));
                }
                weight_slot = *created;
            }
            if (!packed_weights.empty()) {
                if (auto written = device->write_buffer(weight_slot, 0, std::as_bytes(span<const f32>{packed_weights}));
                    !written) {
                    return unexpected(graphics_error_from_rhi(written.error(), "upload skin morph weights"));
                }
            }
            weight_buffer = weight_slot;
            if (!slot) {
                auto created = device->create_buffer(RHI::BufferDesc{
                    .size = static_cast<u64>(SkinningState::kRingMatrices) * sizeof(glm::mat4),
                    .usage = RHI::BufferUsage::Storage,
                    .memory = RHI::MemoryLocation::HostUpload,
                    .label = "skin joint matrices",
                });
                if (!created) {
                    return unexpected(graphics_error_from_rhi(created.error(), "create skin joint ring buffer"));
                }
                slot = *created;
            }
            if (auto written = device->write_buffer(slot, 0, std::as_bytes(span<const glm::mat4>{packed}));
                !written) {
                return unexpected(graphics_error_from_rhi(written.error(), "upload skin joint matrices"));
            }
            joint_buffer = slot;
        }

        auto kernel = prepare_compute_kernel(ComputeKernelDescription{
            .shader_path = "Shaders/mesh_skinning.slang",
            .module_name = "mesh_skinning",
            .entry_point = "skinMain",
            .label = "mesh skinning",
        });
        if (!kernel) {
            return unexpected(kernel.error());
        }
        const ComputeKernelId kernel_id = *kernel;

        std::vector<RHI::BindGroupHandle> *const transient_bind_groups = &context.transient_bind_groups;
        context.graph.add_compute_pass("mesh skinning"_ustr)
            .set_side_effect()
            .set_execute([this, jobs, joint_buffer, weight_buffer, previous_positions, kernel_id, transient_bind_groups](RenderGraphComputeContext &compute) -> Core::RendererResult {
                RHI::CommandEncoder &encoder = compute.command_encoder();
                // Earlier frames may still be reading the arena; then make the new pose visible to everything
                // that consumes vertices (draws, ray-tracing builds, other compute).
                const RHI::GlobalBarrier before{
                    .src_stage = RHI::PipelineStage::AllCommands,
                    .src_access = RHI::AccessFlags::MemoryRead | RHI::AccessFlags::MemoryWrite,
                    .dst_stage = RHI::PipelineStage::ComputeShader,
                    .dst_access = RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite,
                };
                encoder.barrier(span<const RHI::GlobalBarrier>{&before, 1}, {}, {});

                for (const SkinJob &job : *jobs) {
                    const RHI::BufferHandle arena = vertex_arena_.buffer;
                    const std::array<ComputeBufferBinding, 8> buffers{
                        ComputeBufferBinding{.name = "influences", .buffer = job.influence_buffer,
                                             .structure_stride = static_cast<u32>(sizeof(SkinInfluence))},
                        ComputeBufferBinding{.name = "morphOffsets", .buffer = job.morph_offset_buffer,
                                             .structure_stride = static_cast<u32>(sizeof(u32))},
                        ComputeBufferBinding{.name = "morphEntries", .buffer = job.morph_entry_buffer,
                                             .structure_stride = static_cast<u32>(sizeof(MorphDelta))},
                        ComputeBufferBinding{.name = "morphWeights", .buffer = weight_buffer,
                                             .structure_stride = static_cast<u32>(sizeof(f32))},
                        ComputeBufferBinding{.name = "jointMatrices", .buffer = joint_buffer,
                                             .structure_stride = static_cast<u32>(sizeof(glm::mat4))},
                        ComputeBufferBinding{.name = "bindVertices", .buffer = job.bind_buffer},
                        ComputeBufferBinding{.name = "deformedVertices", .buffer = arena},
                        ComputeBufferBinding{.name = "previousPositions", .buffer = previous_positions,
                                             .structure_stride = static_cast<u32>(sizeof(glm::vec4))},
                    };
                    const u32 groups = (job.constants.vertex_count + kSkinWorkgroupSize - 1) / kSkinWorkgroupSize;
                    if (Core::RendererResult recorded = record_compute_kernel(
                            compute.compute_pass(), kernel_id, {},
                            std::as_bytes(span<const SkinConstants>{&job.constants, 1}), glm::uvec3{groups, 1, 1},
                            *transient_bind_groups, buffers);
                        !recorded) {
                        return recorded;
                    }
                }

                const RHI::GlobalBarrier after{
                    .src_stage = RHI::PipelineStage::ComputeShader,
                    .src_access = RHI::AccessFlags::ShaderWrite,
                    .dst_stage = RHI::PipelineStage::AllCommands,
                    .dst_access = RHI::AccessFlags::MemoryRead | RHI::AccessFlags::ShaderRead |
                                  RHI::AccessFlags::VertexAttributeRead | RHI::AccessFlags::AccelerationStructureRead,
                };
                encoder.barrier(span<const RHI::GlobalBarrier>{&after, 1}, {}, {});
                return {};
            });
        return {};
    }

    void Renderer::destroy_skinning_gpu_resources() noexcept {
        ZoneScopedN("Renderer::destroy_skinning_gpu_resources");
        RHI::RhiDevice *device = rhi_device();
        auto skinning = skinning_.lock();
        for (auto &[_, skin] : skinning->skins) {
            if (device != nullptr) {
                if (skin.bind_buffer) device->destroy_buffer(skin.bind_buffer);
                if (skin.influence_buffer) device->destroy_buffer(skin.influence_buffer);
                if (skin.morph_offset_buffer) device->destroy_buffer(skin.morph_offset_buffer);
                if (skin.morph_entry_buffer) device->destroy_buffer(skin.morph_entry_buffer);
            }
            skin.bind_buffer = {};
            skin.influence_buffer = {};
            skin.morph_offset_buffer = {};
            skin.morph_entry_buffer = {};
            skin.dirty = !skin.pending.empty(); // re-pose after the backend comes back
        }
        for (RHI::BufferHandle &slot : skinning->joint_ring) {
            if (device != nullptr && slot) device->destroy_buffer(slot);
            slot = {};
        }
        for (RHI::BufferHandle &slot : skinning->weight_ring) {
            if (device != nullptr && slot) device->destroy_buffer(slot);
            slot = {};
        }
        if (device != nullptr && skinning->dummy_buffer) device->destroy_buffer(skinning->dummy_buffer);
        skinning->dummy_buffer = {};
        if (device != nullptr && skinning->previous_positions) device->destroy_buffer(skinning->previous_positions);
        skinning->previous_positions = {};
        skinning->previous_capacity_vertices = 0;
    }

} // namespace SFT::Renderer
