#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <optional>
#include <string>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

using std::optional;
using std::string;
using std::vector;

namespace SFT::Renderer {

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
    [[nodiscard]] RHI::ShaderStage to_rhi_shader_stage(slang::ShaderStage stage) noexcept;

    // The OR of every entry point's stage — the visibility to give a material's bindings when a finer
    // per-binding analysis isn't warranted (a vertex+fragment material's uniforms are visible to both).
    [[nodiscard]] RHI::ShaderStage reflected_stage_mask(const slang::ShaderReflection &reflection) noexcept;

    // Maps a reflected descriptor kind onto an RHI BindingType. Returns nullopt for kinds the RHI has
    // no descriptor for (push constants — handled as a PushConstantRange, not a descriptor — and the
    // exotic Slang-only kinds), so the caller logs and skips rather than emitting a bogus binding.
    [[nodiscard]] optional<RHI::BindingType> to_rhi_binding_type(slang::ShaderBindingType type) noexcept;

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
    [[nodiscard]] vector<GeneratedBindGroupLayout> generate_bind_group_layouts(
        const slang::ShaderReflection &reflection,
        RHI::ShaderStage visibility,
        u32 bindless_array_max_count = 4096);

    // ─── Push-constant ranges ────────────────────────────────────────────────────────────────────

    // Derives push-constant ranges from every reflected `[[push_constant]] ConstantBuffer<T>`
    // module-scope global — the offset/size a caller would otherwise hand-maintain in a C++ mirror
    // struct (`sizeof(SceneDrawConstants)`, `sizeof(TextViewConstantsGpu)`, ...) that can silently
    // drift out of sync with the actual `.slang` declaration after an edit. `stages` is NOT derived
    // from reflection: Slang's per-entry-point parameter lists cover only that stage's own varying
    // I/O, not module-scope globals, so a push constant's reflected `stage` is always Unknown
    // regardless of which entry point actually reads it — the caller must still supply the same
    // `RHI::ShaderStage` it will later pass to `set_push_constants()`/`pass.set_push_constants()`
    // for this range, since Vulkan requires those to agree exactly (mismatched stageFlags between a
    // pipeline layout's declared range and a push call over the same bytes is invalid usage).
    // Skips a parameter whose element size reflection couldn't resolve (0 — an unresolved generic
    // or link-time-constant size) rather than emitting a bogus empty range.
    [[nodiscard]] vector<RHI::PushConstantRange> generate_push_constant_ranges(const slang::ShaderReflection &reflection,
                                                                                RHI::ShaderStage stages);

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

        [[nodiscard]] bool is_numeric_leaf(const slang::ShaderTypeReflection &type) noexcept;

        // Depth-first walk of a uniform type, appending a ReflectedUniform for each numeric leaf.
        // `base_offset` accumulates struct-field offsets; `prefix` accumulates the dotted name path.
        void collect_uniform_leaves(const slang::ShaderTypeReflection &type,
                                           const string &name,
                                           u64 base_offset,
                                           vector<ReflectedUniform> &out);

    } // namespace detail

    // Flattens every module-scope uniform parameter (the fields of a shader's default constant buffer)
    // into a list of named, offset-addressed leaves. This is the source of a MaterialTemplate's
    // parameter descriptor. Resource parameters (textures/samplers/buffers) are ignored here — they
    // come through generate_bind_group_layouts()/collect_resource_bindings() instead.
    [[nodiscard]] vector<ReflectedUniform> collect_uniform_fields(const slang::ShaderReflection &reflection);

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
    [[nodiscard]] vector<ReflectedResource> collect_resource_bindings(const slang::ShaderReflection &reflection);

} // namespace SFT::Renderer
