module;

#pragma region Imports
#include <optional>
#include <string>
#include <vector>
#pragma endregion

export module Sturdy.Renderer:ReflectionBinding;

import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.RHI;

using std::optional;
using std::string;
using std::vector;

export namespace SFT::Renderer {

    // ─── Reflection → RHI descriptor translation ─────────────────────────────────────────────────
    //
    // The concrete instance of the engine-wide rule "derive RHI descriptors from Slang reflection
    // rather than hand-writing them" (see plans/reflection-driven-binding-generation.md). Neither
    // Sturdy.RHI (API-agnostic) nor Core::Slang (graphics-API-agnostic) may depend on the other, so the
    // translator lives here in Sturdy.Renderer, which already imports both. Everything below is pure —
    // it turns a `Core::Slang::ShaderReflection` into owning descriptor data; creating the actual RHI
    // handles from that data is a device call the caller makes (see :Material).

    namespace slang = Core::Slang;

    // Maps a Slang pipeline stage onto the RHI's bitmask stage. Unknown/unsupported stages map to
    // ShaderStage::None (the caller can OR the results across a shader's entry points to get combined
    // visibility).
    [[nodiscard]] inline RHI::ShaderStage to_rhi_shader_stage(slang::ShaderStage stage) noexcept {
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

    // The OR of every entry point's stage — the visibility to give a material's bindings when a finer
    // per-binding analysis isn't warranted (a vertex+fragment material's uniforms are visible to both).
    [[nodiscard]] inline RHI::ShaderStage reflected_stage_mask(const slang::ShaderReflection &reflection) noexcept {
        RHI::ShaderStage mask = RHI::ShaderStage::None;
        for (const slang::ShaderEntryPointReflection &entry : reflection.entry_points) {
            mask = mask | to_rhi_shader_stage(entry.stage);
        }
        return mask == RHI::ShaderStage::None ? RHI::ShaderStage::AllGraphics : mask;
    }

    // Maps a reflected descriptor kind onto an RHI BindingType. Returns nullopt for kinds the RHI has
    // no descriptor for (push constants — handled as a PushConstantRange, not a descriptor — and the
    // exotic Slang-only kinds), so the caller logs and skips rather than emitting a bogus binding.
    [[nodiscard]] inline optional<RHI::BindingType> to_rhi_binding_type(slang::ShaderBindingType type) noexcept {
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

    // ─── Bind-group layouts ──────────────────────────────────────────────────────────────────────

    // One descriptor set's worth of layout, translated from reflection. Owns its `entries` (RHI's
    // BindGroupLayoutDesc holds a non-owning span, so the caller keeps this alive across the
    // create_bind_group_layout() call). `set` is the register space / set index it targets.
    struct GeneratedBindGroupLayout {
        u32 set = 0;
        vector<RHI::BindGroupLayoutEntry> entries;
    };

    // Translates every `descriptor_sets` entry of a reflection into a GeneratedBindGroupLayout, in set
    // order. `visibility` is the stage mask stamped on every entry (pass reflected_stage_mask() unless
    // a caller wants to restrict it). A range whose kind the RHI can't express is logged and skipped —
    // a shader with an unsupported binding still yields a usable layout for the rest. A range with
    // `count == 0` is a runtime-sized/bindless array: it gets a large count plus the descriptor-indexing
    // flags, so it lands on the bindless path instead of being emitted as a zero-length binding.
    [[nodiscard]] inline vector<GeneratedBindGroupLayout> generate_bind_group_layouts(
        const slang::ShaderReflection &reflection,
        RHI::ShaderStage visibility,
        u32 bindless_array_max_count = 4096) {
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

    // ─── Uniform parameters ──────────────────────────────────────────────────────────────────────

    // One scalar/vector/matrix uniform reflected out of a shader's default constant buffer: its
    // fully-qualified `name` (struct fields flattened with dotted paths), byte `offset`/`size` within
    // the UBO, and its numeric shape (`scalar` element type, `rows`/`columns`). The material layer turns
    // these into typed, named, settable parameters.
    struct ReflectedUniform {
        string name;
        u64 offset = 0;
        u64 size = 0;
        slang::ShaderScalarType scalar = slang::ShaderScalarType::None;
        u32 rows = 0;
        u32 columns = 0;
    };

    namespace detail {

        [[nodiscard]] inline bool is_numeric_leaf(const slang::ShaderTypeReflection &type) noexcept {
            return type.kind == slang::ShaderTypeKind::Scalar || type.kind == slang::ShaderTypeKind::Vector ||
                   type.kind == slang::ShaderTypeKind::Matrix;
        }

        // Depth-first walk of a uniform type, appending a ReflectedUniform for each numeric leaf.
        // `base_offset` accumulates struct-field offsets; `prefix` accumulates the dotted name path.
        inline void collect_uniform_leaves(const slang::ShaderTypeReflection &type,
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

    } // namespace detail

    // Flattens every module-scope uniform parameter (the fields of a shader's default constant buffer)
    // into a list of named, offset-addressed leaves. This is the source of a MaterialTemplate's
    // parameter descriptor. Resource parameters (textures/samplers/buffers) are ignored here — they
    // come through generate_bind_group_layouts()/collect_resource_bindings() instead.
    [[nodiscard]] inline vector<ReflectedUniform> collect_uniform_fields(const slang::ShaderReflection &reflection) {
        vector<ReflectedUniform> uniforms;
        for (const slang::ShaderParameterReflection &param : reflection.global_parameters) {
            if (param.category != slang::ShaderParameterCategory::Uniform || !param.type) {
                continue;
            }
            detail::collect_uniform_leaves(*param.type, param.name, param.offset, uniforms);
        }
        return uniforms;
    }

    // ─── Resource (texture/sampler/buffer) bindings ──────────────────────────────────────────────

    // A named, non-uniform shader resource and where it binds: `name` (what a material sets by),
    // `set`/`binding` register location, and the RHI descriptor `type`. Lets a MaterialInstance bind a
    // concrete texture/sampler to a slot by the shader's own parameter name.
    struct ReflectedResource {
        string name;
        u32 set = 0;
        u32 binding = 0;
        RHI::BindingType type = RHI::BindingType::SampledTexture;
    };

    // Collects every module-scope resource parameter (textures, samplers, buffers, acceleration
    // structures — anything that isn't plain uniform data or a push constant) with its name and binding
    // location, so materials can bind resources by name. Parameters whose kind the RHI can't express
    // are skipped.
    [[nodiscard]] inline vector<ReflectedResource> collect_resource_bindings(const slang::ShaderReflection &reflection) {
        vector<ReflectedResource> resources;
        for (const slang::ShaderParameterReflection &param : reflection.global_parameters) {
            if (param.category == slang::ShaderParameterCategory::Uniform ||
                param.category == slang::ShaderParameterCategory::PushConstantBuffer) {
                continue;
            }
            // Prefer the parameter's own binding ranges (they carry the descriptor kind); fall back to
            // the parameter-level binding/space when it has none listed.
            if (!param.binding_ranges.empty()) {
                for (const slang::ShaderBindingRangeReflection &range : param.binding_ranges) {
                    optional<RHI::BindingType> type = to_rhi_binding_type(range.type);
                    if (!type) {
                        continue;
                    }
                    resources.push_back(ReflectedResource{
                        .name = param.name,
                        .set = range.descriptor_set,
                        .binding = range.binding,
                        .type = *type,
                    });
                }
            }
        }
        return resources;
    }

} // namespace SFT::Renderer
