//! Raw `cxx` bindings for the engine's Slang shader compiler (`SFT::Core::Slang::ShaderCompiler`),
//! independent of any `Engine`/`RhiDevice` — it's a standalone, on-demand compiler object with no
//! device dependency, only ever asked to target a specific `ShaderTargetFormat` (SPIR-V here).
//!
//! ## Why this exists, distinct from `sturdy::assets::Assets::load_shader`
//!
//! `Assets::load_shader` compiles a `.slang` **file** into an engine-managed `Asset` for the
//! higher-level renderer/material system — there is no bridge from that `Asset` back to a raw
//! `sturdy::rhi::ShaderModuleHandle`. Exercising the low-level RHI pipeline-creation path
//! (`Rhi::create_shader_module`/`create_render_pipeline`) from Rust needs actual SPIR-V bytes, and
//! nothing else in this crate produces them. This wraps `ShaderCompiler::compile` + `Shader::
//! entry_point_code` directly: source text in, one `Vec<u8>` of SPIR-V per requested entry point
//! out. It exists to make the RHI surface actually testable end to end (a real pipeline, a real
//! draw), not to duplicate the asset system's own shader-loading path.

#[cxx::bridge(namespace = "sturdy_rs::shader_compiler")]
pub mod ffi {
    /// Mirrors the subset of `SFT::Core::Slang::ShaderStage` a hand-written test/demo shader would
    /// plausibly use; add more variants here if a future caller needs them; the C++ side's `map`
    /// covers the engine's full enum regardless.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum ShaderStage {
        Vertex,
        Fragment,
        Compute,
    }

    #[derive(Debug, Clone)]
    struct EntryPointRequest {
        name: String,
        stage: ShaderStage,
    }

    /// One requested entry point's compiled SPIR-V, parallel to the `EntryPointRequest` slice
    /// passed to [`compile_slang_spirv`].
    #[derive(Debug, Clone, Default)]
    struct CompiledEntryPoint {
        bytes: Vec<u8>,
    }

    #[derive(Debug, Clone, Default)]
    struct ShaderCompileResult {
        ok: bool,
        /// Populated when `!ok`: `ShaderError::message`, plus `diagnostics` (the compiler's own
        /// error text, e.g. line/column-annotated syntax errors) when non-empty.
        error: String,
        /// Exactly `entry_points.len()` entries (the request slice), in the same order, when `ok`.
        entry_points: Vec<CompiledEntryPoint>,
    }

    // ---- The full compiler surface: any target, macros, search paths, reflection, variants ------

    /// `format`: 0 SPIR-V, 1 DXIL, 2 HLSL, 3 GLSL, 4 Metal, 5 WGSL.
    #[derive(Debug, Clone)]
    struct TargetDesc {
        format: u8,
        profile: String,
    }

    #[derive(Debug, Clone)]
    struct MacroDesc {
        name: String,
        value: String,
    }

    /// `stage` is the `ShaderStage` ordinal (0 unknown, 1 vertex, 2 hull, 3 domain, 4 geometry,
    /// 5 fragment, 6 compute, 7 ray generation, 8 intersection, 9 any hit, 10 closest hit, 11 miss,
    /// 12 callable, 13 mesh, 14 amplification, 15 dispatch).
    #[derive(Debug, Clone)]
    struct EntryPointDesc {
        name: String,
        stage: u8,
    }

    #[derive(Debug, Clone)]
    struct CompileOptionsDesc {
        targets: Vec<TargetDesc>,
        entry_points: Vec<EntryPointDesc>,
        search_paths: Vec<String>,
        macros: Vec<MacroDesc>,
        /// 0 none, 1 default, 2 high, 3 maximal.
        optimization: u8,
        allow_glsl_syntax: bool,
        skip_spirv_validation: bool,
        enable_effect_annotations: bool,
    }

    /// `SFT::Core::Slang::ShaderBindingRangeReflection`.
    #[derive(Debug, Clone, Default)]
    struct BindingRangeDesc {
        /// `ShaderBindingType` ordinal.
        binding_type: u8,
        /// `ShaderParameterCategory` ordinal.
        category: u8,
        descriptor_set: u32,
        binding: u32,
        count: u32,
        /// `ShaderResourceAccess` ordinal.
        access: u8,
        is_comparison_sampler: bool,
        is_depth_texture: bool,
        is_multisampled_texture: bool,
    }

    #[derive(Debug, Clone, Default)]
    struct FieldDesc {
        name: String,
        type_name: String,
        offset: u64,
        size: u64,
    }

    /// `SFT::Core::Slang::ShaderParameterReflection` with its type summarized one level deep.
    #[derive(Debug, Clone, Default)]
    struct ParameterDesc {
        name: String,
        type_name: String,
        /// `ShaderTypeKind` ordinal.
        type_kind: u8,
        /// `ShaderScalarType` ordinal.
        scalar_type: u8,
        /// `ShaderResourceShape` ordinal.
        resource_shape: u8,
        row_count: u32,
        column_count: u32,
        element_count: u64,
        category: u8,
        stage: u8,
        binding: u32,
        binding_space: u32,
        offset: u64,
        size: u64,
        stride: u64,
        semantic_name: String,
        semantic_index: u32,
        binding_ranges: Vec<BindingRangeDesc>,
        fields: Vec<FieldDesc>,
    }

    #[derive(Debug, Clone, Default)]
    struct EntryPointReflectionDesc {
        name: String,
        stage: u8,
        thread_group_size: [u32; 3],
        wave_size: u32,
        uses_sample_rate_input: bool,
        has_default_constant_buffer: bool,
        parameters: Vec<ParameterDesc>,
        results: Vec<ParameterDesc>,
    }

    #[derive(Debug, Clone, Default)]
    struct DescriptorRangeDesc {
        binding_type: u8,
        category: u8,
        binding: u32,
        count: u32,
    }

    #[derive(Debug, Clone, Default)]
    struct DescriptorSetDesc {
        space: u32,
        ranges: Vec<DescriptorRangeDesc>,
    }

    #[derive(Debug, Clone, Default)]
    struct ReflectionDesc {
        global_parameters: Vec<ParameterDesc>,
        entry_points: Vec<EntryPointReflectionDesc>,
        descriptor_sets: Vec<DescriptorSetDesc>,
        global_constant_buffer_binding: u32,
        global_constant_buffer_size: u64,
        bindless_space_index: i32,
        /// Slang's own reflection JSON.
        json: String,
    }

    #[derive(Debug, Clone, Default)]
    struct BytecodeDesc {
        target: u8,
        profile: String,
        entry_point: String,
        bytes: Vec<u8>,
    }

    #[derive(Debug, Clone, Default)]
    struct FullCompileResult {
        ok: bool,
        /// Compiler message plus diagnostics when `!ok`.
        error: String,
        module_name: String,
        reflection: ReflectionDesc,
        /// One entry per entry point per target (empty for `reflect_shader`).
        code: Vec<BytecodeDesc>,
    }

    unsafe extern "C++" {
        include!("sturdy_rs/shader_compiler.hpp");

        /// `SFT::Core::Slang::ShaderVariantCache`: compiles a source once per macro set, cached in
        /// memory and optionally on disk.
        type VariantCacheBox;

        /// Compiles a shader. `from_file` reads `path`; otherwise `source` is the module text
        /// (`path` only labels diagnostics/imports). Returns every requested entry point for every
        /// target, plus reflection.
        fn compile_shader(
            from_file: bool,
            module_name: &str,
            path: &str,
            source: &str,
            options: &CompileOptionsDesc,
        ) -> FullCompileResult;
        /// Reflection only (no code generation).
        fn reflect_shader(
            from_file: bool,
            module_name: &str,
            path: &str,
            source: &str,
            options: &CompileOptionsDesc,
        ) -> FullCompileResult;

        fn variant_cache_new(
            from_file: bool,
            module_name: &str,
            path: &str,
            source: &str,
            options: &CompileOptionsDesc,
            enable_disk_cache: bool,
        ) -> UniquePtr<VariantCacheBox>;
        fn variant_cache_get(cache: Pin<&mut VariantCacheBox>, defines: &Vec<MacroDesc>) -> FullCompileResult;
        fn variant_cache_size(cache: &VariantCacheBox) -> u64;
        fn variant_cache_contains(cache: &VariantCacheBox, defines: &Vec<MacroDesc>) -> bool;
        fn variant_cache_invalidate(cache: Pin<&mut VariantCacheBox>);
        /// Canonical string and hash of a macro set, as the cache keys variants.
        fn variant_key_canonical(defines: &Vec<MacroDesc>) -> String;
        fn variant_key_hash(defines: &Vec<MacroDesc>) -> u64;

        /// Compiles `source` (a self-contained Slang module — no `import`s, since this passes no
        /// search path for the compiler to resolve them against) targeting SPIR-V, and returns
        /// each requested entry point's bytecode in request order. `module_name` only affects
        /// diagnostic messages and Slang's internal module cache key.
        fn compile_slang_spirv(source: &str, module_name: &str, entry_points: &[EntryPointRequest]) -> ShaderCompileResult;
    }
}
