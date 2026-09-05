#include <Renderer/ReflectionBinding.hpp>

#include <algorithm>
#include <limits>

namespace SFT::Renderer {

/// Converts the backend-specific value to the corresponding RHI representation.
///
/// @param stage `stage` value used by the operation.
///
/// @return Returns the value converted to RHI shader stage representation.
/// @note This function does not throw exceptions.
RHI::ShaderStage to_rhi_shader_stage(slang::ShaderStage stage) noexcept {
        switch (stage) {
            case slang::ShaderStage::Vertex: return RHI::ShaderStage::Vertex;
            case slang::ShaderStage::Fragment: return RHI::ShaderStage::Fragment;
            case slang::ShaderStage::Compute: return RHI::ShaderStage::Compute;
            case slang::ShaderStage::Geometry: return RHI::ShaderStage::Geometry;
            case slang::ShaderStage::Hull: return RHI::ShaderStage::TessControl;
            case slang::ShaderStage::Domain: return RHI::ShaderStage::TessEval;
            case slang::ShaderStage::Amplification: return RHI::ShaderStage::Task;
            case slang::ShaderStage::Mesh: return RHI::ShaderStage::Mesh;
            case slang::ShaderStage::RayGeneration: return RHI::ShaderStage::RayGeneration;
            case slang::ShaderStage::Intersection: return RHI::ShaderStage::Intersection;
            case slang::ShaderStage::AnyHit: return RHI::ShaderStage::AnyHit;
            case slang::ShaderStage::ClosestHit: return RHI::ShaderStage::ClosestHit;
            case slang::ShaderStage::Miss: return RHI::ShaderStage::Miss;
            case slang::ShaderStage::Callable: return RHI::ShaderStage::Callable;
            case slang::ShaderStage::Unknown:
            case slang::ShaderStage::Dispatch:
                break;
        }
        return RHI::ShaderStage::None;
    }

/// Performs the reflected stage mask operation for `Renderer` using the supplied arguments.
///
/// @param reflection `reflection` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
RHI::ShaderStage reflected_stage_mask(const slang::ShaderReflection &reflection) noexcept {
        RHI::ShaderStage mask = RHI::ShaderStage::None;
        for (const slang::ShaderEntryPointReflection &entry : reflection.entry_points) {
            mask = mask | to_rhi_shader_stage(entry.stage);
        }
        return mask == RHI::ShaderStage::None ? RHI::ShaderStage::AllGraphics : mask;
    }

/// Converts the backend-specific value to the corresponding RHI representation.
///
/// @param type Type value to inspect, select, or convert.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
/// @note This function does not throw exceptions.
optional<RHI::BindingType> to_rhi_binding_type(slang::ShaderBindingType type) noexcept {
        switch (type) {
            case slang::ShaderBindingType::Sampler: return RHI::BindingType::Sampler;
            case slang::ShaderBindingType::Texture: return RHI::BindingType::SampledTexture;
            case slang::ShaderBindingType::MutableTexture: return RHI::BindingType::StorageTexture;
            case slang::ShaderBindingType::ConstantBuffer: return RHI::BindingType::UniformBuffer;
            case slang::ShaderBindingType::TypedBuffer:
            case slang::ShaderBindingType::RawBuffer: return RHI::BindingType::ReadOnlyStorageBuffer;
            case slang::ShaderBindingType::MutableTypedBuffer:
            case slang::ShaderBindingType::MutableRawBuffer: return RHI::BindingType::StorageBuffer;
            case slang::ShaderBindingType::CombinedTextureSampler: return RHI::BindingType::CombinedImageSampler;
            case slang::ShaderBindingType::RayTracingAccelerationStructure: return RHI::BindingType::AccelerationStructure;
            case slang::ShaderBindingType::InputRenderTarget: return RHI::BindingType::InputAttachment;
            case slang::ShaderBindingType::Unknown:
            case slang::ShaderBindingType::ParameterBlock:
            case slang::ShaderBindingType::InlineUniformData:
            case slang::ShaderBindingType::VaryingInput:
            case slang::ShaderBindingType::VaryingOutput:
            case slang::ShaderBindingType::ExistentialValue:
            case slang::ShaderBindingType::PushConstant:
                break;
        }
        return std::nullopt;
    }

namespace {

    /// Converts a raw SlangImageFormat value (see slang-image-format-defs.h) to the closest
    /// matching RHI::Format, for a StorageTexture/MutableTexture binding range's reflected
    /// image_format. Only formats this engine's storage-texture passes actually use need to be
    /// covered precisely; anything else safely falls back to Undefined (the WebGPU backend already
    /// rejects a StorageTexture binding with no usable format rather than misconfiguring one).
    ///
    /// @param slang_image_format Raw SlangImageFormat enumerator value.
    ///
    /// @return Returns the closest matching RHI::Format, or RHI::Format::Undefined if none map cleanly.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RHI::Format to_rhi_storage_format(u32 slang_image_format) noexcept {
        // Mirrors slang-image-format-defs.h's SLANG_FORMAT(...) declaration order exactly (that
        // header has no explicit numeric values -- the enum is declared in this same order, so the
        // index here must match the include order 1:1).
        switch (slang_image_format) {
            case 0: return RHI::Format::Undefined; // unknown
            case 1: return RHI::Format::RGBA32Float; // rgba32f
            case 2: return RHI::Format::RGBA16Float; // rgba16f
            case 3: return RHI::Format::RG32Float; // rg32f
            case 4: return RHI::Format::RG16Float; // rg16f
            case 5: return RHI::Format::RG11B10Float; // r11f_g11f_b10f
            case 6: return RHI::Format::R32Float; // r32f
            case 7: return RHI::Format::R16Float; // r16f
            case 8: return RHI::Format::RGBA16Uint; // rgba16 (unorm16x4, closest available RHI format)
            case 9: return RHI::Format::RGB10A2Unorm; // rgb10_a2
            case 10: return RHI::Format::RGBA8Unorm; // rgba8
            case 21: return RHI::Format::RGBA32Sint; // rgba32i
            case 27: return RHI::Format::R32Sint; // r32i
            case 30: return RHI::Format::RGBA32Uint; // rgba32ui
            case 37: return RHI::Format::R32Uint; // r32ui
            case 42: return RHI::Format::BGRA8Unorm; // bgra8
            default: return RHI::Format::Undefined;
        }
    }

    /// Converts the reflected resource access of a StorageTexture binding range to the RHI's
    /// coarser write/read/read-write classification WebGPU's WGSL storage texture types require.
    ///
    /// @param access Reflected Slang resource access.
    ///
    /// @return Returns the corresponding RHI::StorageTextureAccess.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RHI::StorageTextureAccess to_rhi_storage_access(slang::ShaderResourceAccess access) noexcept {
        switch (access) {
            case slang::ShaderResourceAccess::Read: return RHI::StorageTextureAccess::ReadOnly;
            case slang::ShaderResourceAccess::ReadWrite:
            case slang::ShaderResourceAccess::RasterOrdered:
                return RHI::StorageTextureAccess::ReadWrite;
            case slang::ShaderResourceAccess::Write:
            case slang::ShaderResourceAccess::None:
            case slang::ShaderResourceAccess::Append:
            case slang::ShaderResourceAccess::Consume:
            case slang::ShaderResourceAccess::Feedback:
            case slang::ShaderResourceAccess::Unknown:
                return RHI::StorageTextureAccess::WriteOnly;
        }
        return RHI::StorageTextureAccess::WriteOnly;
    }

    struct ReflectedDescriptorBinding {
        string name;
        u32 set = 0;
        u32 binding = 0;
        u32 shader_register = 0;
        RHI::BindingType type = RHI::BindingType::SampledTexture;
        u32 count = 1;
        // Only set (!= ~0u) for a CombinedImageSampler on a target (WGSL) that splits it into two
        // descriptor ranges; see RHI::BindGroupLayoutEntry::paired_binding's own doc comment.
        u32 paired_binding = ~0u;
        // Only meaningful for a StorageTexture binding; see RHI::BindGroupLayoutEntry's own doc
        // comment on the fields these get copied to.
        RHI::Format storage_format = RHI::Format::Undefined;
        RHI::StorageTextureAccess storage_access = RHI::StorageTextureAccess::WriteOnly;
        // Only meaningful for a Sampler binding; see RHI::BindGroupLayoutEntry::
        // sampler_is_comparison's own doc comment.
        bool sampler_is_comparison = false;
        // Only meaningful for a SampledTexture binding; see RHI::BindGroupLayoutEntry::
        // sampled_texture_is_depth's own doc comment.
        bool sampled_texture_is_depth = false;
        // Only meaningful for a SampledTexture binding; see RHI::BindGroupLayoutEntry::
        // sampled_texture_is_multisampled's own doc comment.
        bool sampled_texture_is_multisampled = false;
    };

    /// Groups a binding type by the register space D3D12 would place it in.
    ///
    /// Mirrors the backend's own classification: constant buffers, shader resources, unordered
    /// access and samplers each get an independent register namespace, so registers must be
    /// numbered within a class rather than across a descriptor set.
    ///
    /// @param type Binding type to classify.
    ///
    /// @return Index of the register class, in [0, 4).
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize register_class_index(RHI::BindingType type) noexcept {
        switch (type) {
            case RHI::BindingType::UniformBuffer:
                return 0;
            case RHI::BindingType::StorageBuffer:
            case RHI::BindingType::StorageTexture:
                return 1;
            case RHI::BindingType::Sampler:
                return 2;
            default:
                return 3;
        }
    }

    /// Reports whether `reflection` has an implicit global-uniform constant buffer (Slang's
    /// auto-collected "$Globals"-style block for a shader's loose top-level uniform parameters),
    /// and if so, which descriptor set and binding the compiler actually placed it at.
    ///
    /// @param reflection `reflection` value used by the operation.
    ///
    /// @return The set/binding pair when present; `std::nullopt` when the shader has no loose
    ///         global uniform parameters at all.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] optional<std::pair<u32, u32>> find_global_constant_buffer_location(
        const slang::ShaderReflection &reflection) {
        const bool has_global_constant_buffer =
            reflection.global_constant_buffer_size != 0 &&
            reflection.global_constant_buffer_size != slang::shader_unbounded_size &&
            reflection.global_constant_buffer_size != slang::shader_unknown_size;
        if (!has_global_constant_buffer) {
            return std::nullopt;
        }
        const u32 uniform_binding = reflection.global_constant_buffer_binding;
        u32 uniform_set = 0;
        for (const slang::ShaderDescriptorSetReflection &descriptor_set : reflection.descriptor_sets) {
            const bool contains_global_constant_buffer = std::ranges::any_of(
                descriptor_set.ranges,
                [uniform_binding](const slang::ShaderDescriptorRangeReflection &range) {
                    return range.type == slang::ShaderBindingType::ConstantBuffer &&
                           range.binding == uniform_binding;
                });
            if (contains_global_constant_buffer) {
                uniform_set = descriptor_set.space;
                break;
            }
        }
        return std::make_pair(uniform_set, uniform_binding);
    }

    /// Collects descriptor bindings using the supplied arguments and current state.
    ///
    /// @param reflection `reflection` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<ReflectedDescriptorBinding> collect_descriptor_bindings(
        const slang::ShaderReflection &reflection) {
        vector<ReflectedDescriptorBinding> descriptors;
        for (const slang::ShaderParameterReflection &parameter : reflection.global_parameters) {
            if (parameter.category == slang::ShaderParameterCategory::Uniform ||
                parameter.category == slang::ShaderParameterCategory::PushConstantBuffer) {
                continue;
            }
            for (const slang::ShaderBindingRangeReflection &range : parameter.binding_ranges) {
                const optional<RHI::BindingType> type = to_rhi_binding_type(range.type);
                if (!type) {
                    if (range.type != slang::ShaderBindingType::PushConstant) {
                        Foundation::log_warn(
                            "ReflectionBinding: skipping unsupported binding (set {}, register {}) — no RHI descriptor for this kind.",
                            parameter.binding_space, parameter.binding + range.binding);
                    }
                    continue;
                }
                descriptors.push_back(ReflectedDescriptorBinding{
                    .name = parameter.name,
                    .set = parameter.binding_space,
                    .binding = parameter.binding + range.binding,
                    .shader_register = parameter.binding + range.binding,
                    .type = *type,
                    .count = range.count,
                    .paired_binding = range.second_binding != std::numeric_limits<u32>::max()
                                           ? parameter.binding + range.second_binding
                                           : std::numeric_limits<u32>::max(),
                    .storage_format = *type == RHI::BindingType::StorageTexture
                                           ? to_rhi_storage_format(range.image_format)
                                           : RHI::Format::Undefined,
                    .storage_access = to_rhi_storage_access(range.access),
                    .sampler_is_comparison = static_cast<bool>(range.is_comparison_sampler),
                    .sampled_texture_is_depth = static_cast<bool>(range.is_depth_texture),
                    .sampled_texture_is_multisampled = static_cast<bool>(range.is_multisampled_texture),
                });
            }
        }

        std::stable_sort(descriptors.begin(), descriptors.end(), [](const auto &a, const auto &b) {
            return a.set < b.set;
        });


        for (usize begin = 0; begin < descriptors.size();) {
            const u32 set = descriptors[begin].set;
            usize end = begin + 1;
            while (end < descriptors.size() && descriptors[end].set == set) {
                ++end;
            }
            bool needs_flattening = false;
            for (usize i = begin; i < end && !needs_flattening; ++i) {
                for (usize j = i + 1; j < end; ++j) {
                    if (descriptors[i].shader_register == descriptors[j].shader_register) {
                        needs_flattening = true;
                        break;
                    }
                }
            }
            if (needs_flattening) {
                // Two different numbering rules, because the backends disagree about what a
                // register is.
                //
                // `binding` is sequential across the whole set: Vulkan addresses a descriptor by
                // its binding index, unique per set regardless of type.
                //
                // `shader_register` restarts per register class, because D3D12 gives constant
                // buffers, shader resources, unordered access and samplers their own register
                // spaces (b#, t#, u#, s#). Numbering it across the set instead — as this first did —
                // fixes textures colliding at t0 but then pushes a sampler declared after a texture
                // to s1 when the shader compiled it as s0, and the root signature no longer matches
                // the shader.
                u32 next_binding = 0;
                u32 next_register[4] = {0, 0, 0, 0};
                for (usize i = begin; i < end; ++i) {
                    const u32 old_binding = descriptors[i].binding;
                    descriptors[i].binding = next_binding++;
                    descriptors[i].shader_register = next_register[register_class_index(descriptors[i].type)]++;
                    // Preserve the paired binding's offset relative to its own entry rather than
                    // leaving it pointing at the pre-renumbering slot. This does not guard against
                    // the paired slot colliding with another entry's newly assigned binding --
                    // this whole branch is already a rare fallback for a Slang-side numbering
                    // collision (see the comment above), and a collision on top of a split
                    // combined-sampler within it is stacking two edge cases at once.
                    if (descriptors[i].paired_binding != std::numeric_limits<u32>::max()) {
                        const u32 offset = descriptors[i].paired_binding - old_binding;
                        descriptors[i].paired_binding = descriptors[i].binding + offset;
                    }
                }
            }
            begin = end;
        }

        // Slang's `parameter.binding` for explicit resource globals (textures, samplers, ...)
        // already accounts for the implicit global-uniform constant buffer's own slot — e.g. for
        // `Shaders/gbuffer_geometry.slang` (loose uniforms declared before its 5 textures), the
        // implicit block sits at binding 0 and `base_color_texture` reflects as binding 1, which
        // is exactly its real compiled SPIR-V binding. Bumping resource bindings by one here on
        // top of that double-counts the block's slot and desyncs the RHI bind-group layout from
        // the shader module's real layout — reproduced 100% as
        // `VUID-VkGraphicsPipelineCreateInfo-layout-07988` on this shader. No adjustment needed:
        // `generate_bind_group_layouts` inserts the implicit block's own entry at
        // `reflection.global_constant_buffer_binding` separately, using these bindings as-is.
        return descriptors;
    }

} // namespace

/// Performs the generate bind group layouts operation for `Renderer` using the supplied arguments.
///
/// @param reflection `reflection` value used by the operation.
/// @param visibility `visibility` value used by the operation.
/// @param bindless_array_max_count Number of elements or operations to process.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
vector<GeneratedBindGroupLayout> generate_bind_group_layouts(
        const slang::ShaderReflection &reflection,
        RHI::ShaderStage visibility,
        u32 bindless_array_max_count) {
        vector<GeneratedBindGroupLayout> layouts;
        layouts.reserve(reflection.descriptor_sets.size());
        for (const ReflectedDescriptorBinding &descriptor : collect_descriptor_bindings(reflection)) {
            // The reserved set is not a bind group the caller owns: on a target that emulates push
            // constants it holds the emulated block, which that backend binds itself, and on every
            // other target nothing is bound there at all. Reporting it would make callers build a
            // layout the backend then has to work around.
            if (descriptor.set == emulated_push_constant_set) {
                continue;
            }
            auto layout = std::ranges::find(layouts, descriptor.set, &GeneratedBindGroupLayout::set);
            if (layout == layouts.end()) {
                layouts.push_back(GeneratedBindGroupLayout{.set = descriptor.set});
                layout = std::prev(layouts.end());
            }
            const bool is_bindless = descriptor.count == 0 || descriptor.count == std::numeric_limits<u32>::max();
            layout->entries.push_back(RHI::BindGroupLayoutEntry{
                .binding = descriptor.binding,
                .shader_register = descriptor.shader_register,
                .type = descriptor.type,
                .visibility = visibility,
                .count = is_bindless ? bindless_array_max_count : descriptor.count,
                .flags = is_bindless
                             ? (RHI::BindingFlags::PartiallyBound | RHI::BindingFlags::UpdateAfterBind |
                                RHI::BindingFlags::VariableDescriptorCount)
                             : RHI::BindingFlags::None,
                .paired_binding = descriptor.paired_binding,
                .storage_format = descriptor.storage_format,
                .storage_access = descriptor.storage_access,
                .sampler_is_comparison = descriptor.sampler_is_comparison,
                .sampled_texture_is_depth = descriptor.sampled_texture_is_depth,
                .sampled_texture_is_multisampled = descriptor.sampled_texture_is_multisampled,
            });
        }
        if (const optional<std::pair<u32, u32>> uniform_location = find_global_constant_buffer_location(reflection)) {
            const auto [uniform_set, uniform_binding] = *uniform_location;

            auto layout = std::ranges::find(layouts, uniform_set, &GeneratedBindGroupLayout::set);
            if (layout == layouts.end()) {
                layouts.push_back(GeneratedBindGroupLayout{.set = uniform_set});
                layout = std::prev(layouts.end());
            }

            // On Vulkan, `uniform_binding` comes straight from this shader's own SPIR-V reflection
            // and cannot legitimately collide with another resource's real binding, so the common
            // case is a plain insert. D3D12 is the one that can reach the collision branch below:
            // CBVs, SRVs and samplers live in independent register namespaces there (b0/t0/s0 all
            // coexist), but `.binding` conflates them into one unified index, so the global
            // constant buffer's binding can legitimately equal a texture's after per-class
            // flattening (see `collect_descriptor_bindings`'s `needs_flattening` step).
            auto existing = std::ranges::find(layout->entries, uniform_binding, &RHI::BindGroupLayoutEntry::binding);
            if (existing == layout->entries.end() || existing->type == RHI::BindingType::UniformBuffer) {
                layout->entries.push_back(RHI::BindGroupLayoutEntry{
                    .binding = uniform_binding,
                    .shader_register = uniform_binding,
                    .type = RHI::BindingType::UniformBuffer,
                    .visibility = visibility,
                    .count = 1,
                    .flags = RHI::BindingFlags::None,
                });
            } else {
                // Only reachable on D3D12 (see above) — shifting `.binding` here is safe there
                // because that field is this RHI's internal bookkeeping index, not the real
                // register (`.shader_register`, untouched); the register-class flattening already
                // recorded the actual b#/t#/u#/s# assignment D3D12 binds to.
                for (RHI::BindGroupLayoutEntry &entry : layout->entries) {
                    if (entry.binding >= uniform_binding) {
                        ++entry.binding;
                    }
                }
                layout->entries.push_back(RHI::BindGroupLayoutEntry{
                    .binding = uniform_binding,
                    .shader_register = uniform_binding,
                    .type = RHI::BindingType::UniformBuffer,
                    .visibility = visibility,
                    .count = 1,
                    .flags = RHI::BindingFlags::None,
                });
            }
        }

        std::ranges::sort(layouts, {}, &GeneratedBindGroupLayout::set);
        for (GeneratedBindGroupLayout &layout : layouts) {
            std::ranges::sort(layout.entries, {}, &RHI::BindGroupLayoutEntry::binding);
        }
        return layouts;
    }

/// Performs the generate push constant ranges operation for `Renderer` using the supplied arguments.
///
/// @param reflection `reflection` value used by the operation.
/// @param stages `stages` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
vector<RHI::PushConstantRange> generate_push_constant_ranges(const slang::ShaderReflection &reflection, RHI::ShaderStage stages) {
        vector<RHI::PushConstantRange> ranges;
        for (const slang::ShaderParameterReflection &parameter : reflection.global_parameters) {
            if (parameter.category != slang::ShaderParameterCategory::PushConstantBuffer) {
                continue;
            }
            if (parameter.size == 0) {
                Foundation::log_warn(
                    "ReflectionBinding: push constant '{}' reflected a zero byte size — skipping rather than "
                    "emitting a bogus range (an unresolved generic/link-time size?).",
                    parameter.name);
                continue;
            }
            ranges.push_back(RHI::PushConstantRange{
                .stages = stages,
                .offset = static_cast<u32>(parameter.offset),
                .size = static_cast<u32>(parameter.size),
                .shader_register = parameter.binding,
                .register_space = parameter.binding_space,
            });
        }
        if (!ranges.empty()) {
            return ranges;
        }

        // Nothing reflected as a push constant. On a target that has them, that means the shader
        // genuinely declares none. On one that does not -- WGSL -- the same block was compiled
        // through the SFT_EMULATE_PUSH_CONSTANTS path into an ordinary uniform buffer in the
        // reserved set, and reflection reports it as the constant buffer it now is. Recovering the
        // range from there is what lets every caller keep asking for push constants and stay
        // unaware of which target it is on; the backend that emulates them puts the block back
        // together (see WebGpuDevicePushConstants.cpp).
        for (const slang::ShaderParameterReflection &parameter : reflection.global_parameters) {
            const bool is_reserved_uniform_block =
                parameter.binding_space == emulated_push_constant_set && parameter.size != 0 &&
                (parameter.category == slang::ShaderParameterCategory::ConstantBuffer ||
                 parameter.category == slang::ShaderParameterCategory::DescriptorTableSlot ||
                 parameter.category == slang::ShaderParameterCategory::Uniform);
            if (!is_reserved_uniform_block) {
                continue;
            }
            ranges.push_back(RHI::PushConstantRange{
                .stages = stages,
                .offset = 0,
                .size = static_cast<u32>(parameter.size),
                .shader_register = parameter.binding,
                .register_space = parameter.binding_space,
            });
        }
        return ranges;
    }

} // namespace SFT::Renderer

namespace SFT::Renderer::detail {

/// Reports whether numeric leaf holds for this `detail`.
///
/// @param type Type value to inspect, select, or convert.
///
/// @return Returns `true` when the stated condition holds; otherwise returns `false`.
/// @note This function does not throw exceptions.
bool is_numeric_leaf(const slang::ShaderTypeReflection &type) noexcept {
            return type.kind == slang::ShaderTypeKind::Scalar || type.kind == slang::ShaderTypeKind::Vector ||
                   type.kind == slang::ShaderTypeKind::Matrix;
        }

/// Collects uniform leaves using the supplied arguments and current state.
///
/// @param type Type value to inspect, select, or convert.
/// @param name Name used to identify or label the target.
/// @param base_offset Offset from the beginning of the relevant range or buffer.
/// @param out `out` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void collect_uniform_leaves(const slang::ShaderTypeReflection &type,
                                           const string &name,
                                           u64 base_offset,
                                           vector<ReflectedUniform> &out) {
            if (is_numeric_leaf(type)) {
                out.push_back(ReflectedUniform{
                    .name = name,
                    .offset = base_offset,
                    .size = type.size,
                    .scalar = type.scalar_type,
                    .rows = type.kind == slang::ShaderTypeKind::Matrix ? type.row_count
                            : type.kind == slang::ShaderTypeKind::Vector ? 1u
                                                                         : 1u,
                    .columns = type.kind == slang::ShaderTypeKind::Scalar
                                   ? 1u
                                   : (type.column_count != 0 ? type.column_count : type.row_count),
                });
                return;
            }
            if (type.kind == slang::ShaderTypeKind::Struct) {
                for (const slang::ShaderFieldReflection &field : type.fields) {
                    if (!field.type) {
                        continue;
                    }
                    const string child = name.empty() ? field.name : name + "." + field.name;
                    collect_uniform_leaves(*field.type, child, base_offset + field.offset, out);
                }
            }


        }

} // namespace SFT::Renderer::detail

namespace SFT::Renderer {

/// Collects uniform fields using the supplied arguments and current state.
///
/// @param reflection `reflection` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
vector<ReflectedUniform> collect_uniform_fields(const slang::ShaderReflection &reflection) {
        vector<ReflectedUniform> uniforms;
        for (const slang::ShaderParameterReflection &param : reflection.global_parameters) {
            if (param.category != slang::ShaderParameterCategory::Uniform || !param.type) {
                continue;
            }
            detail::collect_uniform_leaves(*param.type, param.name, param.offset, uniforms);
        }
        return uniforms;
    }

/// Collects resource bindings using the supplied arguments and current state.
///
/// @param reflection `reflection` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
vector<ReflectedResource> collect_resource_bindings(const slang::ShaderReflection &reflection) {
        vector<ReflectedResource> resources;
        for (const ReflectedDescriptorBinding &descriptor : collect_descriptor_bindings(reflection)) {
            resources.push_back(ReflectedResource{
                .name = descriptor.name,
                .set = descriptor.set,
                .binding = descriptor.binding,
                .type = descriptor.type,
            });
        }
        return resources;
    }

} // namespace SFT::Renderer
