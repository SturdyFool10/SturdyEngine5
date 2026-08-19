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

    struct ReflectedDescriptorBinding {
        string name;
        u32 set = 0;
        u32 binding = 0;
        u32 shader_register = 0;
        RHI::BindingType type = RHI::BindingType::SampledTexture;
        u32 count = 1;
    };

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
                for (usize i = begin; i < end; ++i) {
                    descriptors[i].binding = static_cast<u32>(i - begin);
                }
            }
            begin = end;
        }
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
            });
        }
        const bool has_global_constant_buffer =
            reflection.global_constant_buffer_size != 0 &&
            reflection.global_constant_buffer_size != slang::shader_unbounded_size &&
            reflection.global_constant_buffer_size != slang::shader_unknown_size;
        if (has_global_constant_buffer) {
            u32 uniform_set = 0;
            const u32 uniform_binding = reflection.global_constant_buffer_binding;
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

            auto layout = std::ranges::find(layouts, uniform_set, &GeneratedBindGroupLayout::set);
            if (layout == layouts.end()) {
                layouts.push_back(GeneratedBindGroupLayout{.set = uniform_set});
                layout = std::prev(layouts.end());
            }

            auto existing = std::ranges::find(layout->entries, uniform_binding, &RHI::BindGroupLayoutEntry::binding);
            if (existing == layout->entries.end()) {
                layout->entries.push_back(RHI::BindGroupLayoutEntry{
                    .binding = uniform_binding,
                    .shader_register = uniform_binding,
                    .type = RHI::BindingType::UniformBuffer,
                    .visibility = visibility,
                    .count = 1,
                    .flags = RHI::BindingFlags::None,
                });
            } else if (existing->type != RHI::BindingType::UniformBuffer) {
                Foundation::log_warn(
                    "ReflectionBinding: global constant buffer collides with reflected set {} binding {} of another descriptor type.",
                    uniform_set, uniform_binding);
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
