#include "ReflectionBinding.hpp"

namespace SFT::Renderer {

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

RHI::ShaderStage reflected_stage_mask(const slang::ShaderReflection &reflection) noexcept {
        RHI::ShaderStage mask = RHI::ShaderStage::None;
        for (const slang::ShaderEntryPointReflection &entry : reflection.entry_points) {
            mask = mask | to_rhi_shader_stage(entry.stage);
        }
        return mask == RHI::ShaderStage::None ? RHI::ShaderStage::AllGraphics : mask;
    }

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

vector<GeneratedBindGroupLayout> generate_bind_group_layouts(
        const slang::ShaderReflection &reflection,
        RHI::ShaderStage visibility,
        u32 bindless_array_max_count) {
        vector<GeneratedBindGroupLayout> layouts;
        layouts.reserve(reflection.descriptor_sets.size());
        for (const slang::ShaderDescriptorSetReflection &set : reflection.descriptor_sets) {
            GeneratedBindGroupLayout generated;
            generated.set = set.space;
            for (const slang::ShaderDescriptorRangeReflection &range : set.ranges) {
                optional<RHI::BindingType> binding_type = to_rhi_binding_type(range.type);
                if (!binding_type) {
                    if (range.type != slang::ShaderBindingType::PushConstant) {
                        Foundation::log_warn("ReflectionBinding: skipping unsupported binding (set {}, binding {}) — no RHI descriptor for this kind.",
                                              set.space, range.binding);
                    }
                    continue;
                }
                const bool is_bindless = range.count == 0;
                generated.entries.push_back(RHI::BindGroupLayoutEntry{
                    .binding = range.binding,
                    .type = *binding_type,
                    .visibility = visibility,
                    .count = is_bindless ? bindless_array_max_count : range.count,
                    .flags = is_bindless
                                 ? (RHI::BindingFlags::PartiallyBound | RHI::BindingFlags::UpdateAfterBind |
                                    RHI::BindingFlags::VariableDescriptorCount)
                                 : RHI::BindingFlags::None,
                });
            }
            if (!generated.entries.empty()) {
                layouts.push_back(std::move(generated));
            }
        }
        return layouts;
    }

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

bool is_numeric_leaf(const slang::ShaderTypeReflection &type) noexcept {
            return type.kind == slang::ShaderTypeKind::Scalar || type.kind == slang::ShaderTypeKind::Vector ||
                   type.kind == slang::ShaderTypeKind::Matrix;
        }

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
            // Arrays of numerics and other kinds are intentionally not flattened here — the material
            // model targets named scalar/vector/matrix parameters. Extend when an array-of-uniforms
            // material parameter is actually needed.
        }

} // namespace SFT::Renderer::detail

namespace SFT::Renderer {

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

vector<ReflectedResource> collect_resource_bindings(const slang::ShaderReflection &reflection) {
        vector<ReflectedResource> resources;
        for (const slang::ShaderParameterReflection &param : reflection.global_parameters) {
            if (param.category == slang::ShaderParameterCategory::Uniform ||
                param.category == slang::ShaderParameterCategory::PushConstantBuffer) {
                continue;
            }
            // `range.descriptor_set`/`range.binding` are relative to the *parameter's own* type
            // layout (a standalone resource type has exactly one binding range, always at local
            // offset 0), not the shader's global descriptor space — using them directly collapses
            // every simple resource parameter onto set 0/binding 0. The parameter's own
            // `binding_space`/`binding` (VariableLayoutReflection::getBindingSpace()/
            // getBindingIndex()) is the absolute location; the range only contributes a further
            // local sub-offset for aggregate/array parameters.
            if (!param.binding_ranges.empty()) {
                for (const slang::ShaderBindingRangeReflection &range : param.binding_ranges) {
                    optional<RHI::BindingType> type = to_rhi_binding_type(range.type);
                    if (!type) {
                        continue;
                    }
                    resources.push_back(ReflectedResource{
                        .name = param.name,
                        .set = param.binding_space,
                        .binding = param.binding + range.binding,
                        .type = *type,
                    });
                }
            }
        }
        return resources;
    }

} // namespace SFT::Renderer
