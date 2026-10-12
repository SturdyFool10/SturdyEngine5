//! Compiles Slang shader source to raw SPIR-V bytes for [`crate::rhi::Rhi::create_shader_module`].
//!
//! Distinct from [`crate::assets::Assets::load_shader`], which loads a `.slang` **file** into an
//! engine-managed [`crate::assets::Asset`] for the higher-level renderer/material system — there
//! is no bridge from that `Asset` back to a raw [`crate::rhi::ShaderModuleHandle`]. Use this
//! module when you need actual bytecode to hand to the low-level RHI pipeline-creation path
//! directly (e.g. a custom render pipeline outside the engine's own material system, or a test).

use sturdy_sys::shader_compiler::ffi;

/// Which shader stage a requested entry point targets. Mirrors the subset of `SFT::Core::Slang::
/// ShaderStage` this binding exposes; see [`sturdy_sys::shader_compiler::ffi::ShaderStage`] if a
/// future caller needs a stage not listed here.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShaderStage {
    Vertex,
    Fragment,
    Compute,
}

impl From<ShaderStage> for ffi::ShaderStage {
    fn from(stage: ShaderStage) -> Self {
        match stage {
            ShaderStage::Vertex => ffi::ShaderStage::Vertex,
            ShaderStage::Fragment => ffi::ShaderStage::Fragment,
            ShaderStage::Compute => ffi::ShaderStage::Compute,
        }
    }
}

/// One entry point to compile, e.g. `EntryPoint::new("vertexMain", ShaderStage::Vertex)`.
#[derive(Debug, Clone)]
pub struct EntryPoint {
    pub name: String,
    pub stage: ShaderStage,
}

impl EntryPoint {
    pub fn new(name: impl Into<String>, stage: ShaderStage) -> Self {
        Self { name: name.into(), stage }
    }
}

/// A Slang compile error: the compiler's own message, plus source diagnostics when available
/// (folded together into one string by the C++ side — see `sturdy-sys`'s `describe` helper).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompileError(pub String);

impl core::fmt::Display for CompileError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for CompileError {}

/// Compiles `source` (a self-contained Slang module — no `import`s; this binding passes no search
/// path for the compiler to resolve them against) targeting SPIR-V, returning each requested
/// entry point's bytecode in the same order as `entry_points`. `module_name` only affects
/// diagnostic messages and Slang's internal module cache key.
pub fn compile_spirv(
    source: &str,
    module_name: &str,
    entry_points: &[EntryPoint],
) -> Result<Vec<Vec<u8>>, CompileError> {
    let requests: Vec<ffi::EntryPointRequest> = entry_points
        .iter()
        .map(|e| ffi::EntryPointRequest { name: e.name.clone(), stage: e.stage.into() })
        .collect();
    let result = ffi::compile_slang_spirv(source, module_name, &requests);
    if !result.ok {
        return Err(CompileError(result.error));
    }
    Ok(result.entry_points.into_iter().map(|e| e.bytes).collect())
}

// -- The full compiler: any target, macros, search paths, reflection and variants -----------------

use sturdy_sys::shader_compiler::ffi as full;

/// Code-generation targets.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum TargetFormat {
    Spirv = 0,
    Dxil,
    Hlsl,
    Glsl,
    Metal,
    Wgsl,
}

impl TargetFormat {
    fn from_u8(v: u8) -> Self {
        match v {
            1 => Self::Dxil,
            2 => Self::Hlsl,
            3 => Self::Glsl,
            4 => Self::Metal,
            5 => Self::Wgsl,
            _ => Self::Spirv,
        }
    }
}

/// Every shader stage Slang knows.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Stage {
    Unknown = 0,
    Vertex,
    Hull,
    Domain,
    Geometry,
    Fragment,
    Compute,
    RayGeneration,
    Intersection,
    AnyHit,
    ClosestHit,
    Miss,
    Callable,
    Mesh,
    Amplification,
    Dispatch,
}

impl Stage {
    fn from_u8(v: u8) -> Self {
        const ALL: [Stage; 16] = [
            Stage::Unknown, Stage::Vertex, Stage::Hull, Stage::Domain, Stage::Geometry, Stage::Fragment, Stage::Compute,
            Stage::RayGeneration, Stage::Intersection, Stage::AnyHit, Stage::ClosestHit, Stage::Miss, Stage::Callable,
            Stage::Mesh, Stage::Amplification, Stage::Dispatch,
        ];
        ALL.get(v as usize).copied().unwrap_or(Stage::Unknown)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u8)]
pub enum Optimization {
    None = 0,
    #[default]
    Default,
    High,
    Maximal,
}

/// Where the shader text comes from.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Source {
    /// Module text; `path` labels diagnostics and anchors relative imports.
    Text { module_name: String, source: String, path: String },
    /// A file on disk (module name defaults to the file's).
    File { path: String, module_name: String },
}

impl Source {
    pub fn text(module_name: impl Into<String>, source: impl Into<String>) -> Self {
        Self::Text { module_name: module_name.into(), source: source.into(), path: String::new() }
    }

    pub fn file(path: impl Into<String>) -> Self {
        Self::File { path: path.into(), module_name: String::new() }
    }

    fn parts(&self) -> (bool, &str, &str, &str) {
        match self {
            Self::Text { module_name, source, path } => (false, module_name, path, source),
            Self::File { path, module_name } => (true, module_name, path, ""),
        }
    }
}

/// Everything that shapes a compile.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompileOptions {
    /// `(format, profile)`; an empty profile uses the target's default (`spirv_1_5` for SPIR-V).
    pub targets: Vec<(TargetFormat, String)>,
    /// Entry points to compile; empty picks up `[shader(...)]`-attributed ones.
    pub entry_points: Vec<(String, Stage)>,
    pub search_paths: Vec<String>,
    pub macros: Vec<(String, String)>,
    pub optimization: Optimization,
    pub allow_glsl_syntax: bool,
    pub skip_spirv_validation: bool,
    pub enable_effect_annotations: bool,
}

impl Default for CompileOptions {
    fn default() -> Self {
        Self {
            targets: vec![(TargetFormat::Spirv, String::new())],
            entry_points: Vec::new(),
            search_paths: Vec::new(),
            macros: Vec::new(),
            optimization: Optimization::Default,
            allow_glsl_syntax: false,
            skip_spirv_validation: false,
            enable_effect_annotations: false,
        }
    }
}

impl CompileOptions {
    #[must_use]
    pub fn target(mut self, format: TargetFormat) -> Self {
        self.targets = vec![(format, String::new())];
        self
    }

    #[must_use]
    pub fn also_target(mut self, format: TargetFormat) -> Self {
        self.targets.push((format, String::new()));
        self
    }

    #[must_use]
    pub fn entry(mut self, name: impl Into<String>, stage: Stage) -> Self {
        self.entry_points.push((name.into(), stage));
        self
    }

    #[must_use]
    pub fn search_path(mut self, path: impl Into<String>) -> Self {
        self.search_paths.push(path.into());
        self
    }

    #[must_use]
    pub fn define(mut self, name: impl Into<String>, value: impl Into<String>) -> Self {
        self.macros.push((name.into(), value.into()));
        self
    }

    fn to_ffi(&self) -> full::CompileOptionsDesc {
        full::CompileOptionsDesc {
            targets: self.targets.iter().map(|(f, p)| full::TargetDesc { format: *f as u8, profile: p.clone() }).collect(),
            entry_points: self.entry_points.iter().map(|(n, s)| full::EntryPointDesc { name: n.clone(), stage: *s as u8 }).collect(),
            search_paths: self.search_paths.clone(),
            macros: self.macros.iter().map(|(n, v)| full::MacroDesc { name: n.clone(), value: v.clone() }).collect(),
            optimization: self.optimization as u8,
            allow_glsl_syntax: self.allow_glsl_syntax,
            skip_spirv_validation: self.skip_spirv_validation,
            enable_effect_annotations: self.enable_effect_annotations,
        }
    }
}

/// One binding range of a parameter.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BindingRange {
    /// Slang's `ShaderBindingType` ordinal (0 unknown, 1 sampler, 2 texture, 3 constant buffer, ...).
    pub binding_type: u8,
    pub category: u8,
    pub descriptor_set: u32,
    pub binding: u32,
    pub count: u32,
    pub access: u8,
    pub is_comparison_sampler: bool,
    pub is_depth_texture: bool,
    pub is_multisampled_texture: bool,
}

/// A field of a struct-typed parameter.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Field {
    pub name: String,
    pub type_name: String,
    pub offset: u64,
    pub size: u64,
}

/// A shader parameter (global, entry-point input, or result) with its layout.
#[derive(Debug, Clone, PartialEq)]
pub struct Parameter {
    pub name: String,
    pub type_name: String,
    /// Slang's `ShaderTypeKind` ordinal (struct 1, array 2, matrix 3, vector 4, scalar 5, constant
    /// buffer 6, resource 7, sampler 8, ...).
    pub type_kind: u8,
    pub scalar_type: u8,
    pub resource_shape: u8,
    pub rows: u32,
    pub columns: u32,
    pub element_count: u64,
    pub category: u8,
    pub stage: Stage,
    pub binding: u32,
    pub binding_space: u32,
    pub offset: u64,
    pub size: u64,
    pub stride: u64,
    pub semantic_name: String,
    pub semantic_index: u32,
    pub binding_ranges: Vec<BindingRange>,
    /// The parameter's struct fields, one level deep. Empty for a `ConstantBuffer<T>`/`ParameterBlock<T>`
    /// wrapper (the engine's reflection doesn't unwrap the element type): read `T`'s members from
    /// [`Reflection::json`] instead.
    pub fields: Vec<Field>,
}

impl From<full::ParameterDesc> for Parameter {
    fn from(p: full::ParameterDesc) -> Self {
        Self {
            name: p.name,
            type_name: p.type_name,
            type_kind: p.type_kind,
            scalar_type: p.scalar_type,
            resource_shape: p.resource_shape,
            rows: p.row_count,
            columns: p.column_count,
            element_count: p.element_count,
            category: p.category,
            stage: Stage::from_u8(p.stage),
            binding: p.binding,
            binding_space: p.binding_space,
            offset: p.offset,
            size: p.size,
            stride: p.stride,
            semantic_name: p.semantic_name,
            semantic_index: p.semantic_index,
            binding_ranges: p
                .binding_ranges
                .into_iter()
                .map(|r| BindingRange {
                    binding_type: r.binding_type,
                    category: r.category,
                    descriptor_set: r.descriptor_set,
                    binding: r.binding,
                    count: r.count,
                    access: r.access,
                    is_comparison_sampler: r.is_comparison_sampler,
                    is_depth_texture: r.is_depth_texture,
                    is_multisampled_texture: r.is_multisampled_texture,
                })
                .collect(),
            fields: p.fields.into_iter().map(|f| Field { name: f.name, type_name: f.type_name, offset: f.offset, size: f.size }).collect(),
        }
    }
}

/// One entry point's reflection.
#[derive(Debug, Clone, PartialEq)]
pub struct EntryPointReflection {
    pub name: String,
    pub stage: Stage,
    /// Compute thread-group size (`[0, 0, 0]` for non-compute stages).
    pub thread_group_size: [u32; 3],
    pub wave_size: u32,
    pub uses_sample_rate_input: bool,
    pub has_default_constant_buffer: bool,
    pub parameters: Vec<Parameter>,
    pub results: Vec<Parameter>,
}

/// One descriptor range in a set.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DescriptorRange {
    pub binding_type: u8,
    pub category: u8,
    pub binding: u32,
    pub count: u32,
}

/// Everything the compiler learned about a shader's interface.
#[derive(Debug, Clone, PartialEq)]
pub struct Reflection {
    pub global_parameters: Vec<Parameter>,
    pub entry_points: Vec<EntryPointReflection>,
    /// `(space, ranges)` per descriptor set.
    pub descriptor_sets: Vec<(u32, Vec<DescriptorRange>)>,
    pub global_constant_buffer_binding: u32,
    pub global_constant_buffer_size: u64,
    /// The descriptor space used for bindless resources, if any.
    pub bindless_space_index: Option<u32>,
    /// Slang's own reflection JSON.
    pub json: String,
}

impl From<full::ReflectionDesc> for Reflection {
    fn from(r: full::ReflectionDesc) -> Self {
        Self {
            global_parameters: r.global_parameters.into_iter().map(Parameter::from).collect(),
            entry_points: r
                .entry_points
                .into_iter()
                .map(|e| EntryPointReflection {
                    name: e.name,
                    stage: Stage::from_u8(e.stage),
                    thread_group_size: e.thread_group_size,
                    wave_size: e.wave_size,
                    uses_sample_rate_input: e.uses_sample_rate_input,
                    has_default_constant_buffer: e.has_default_constant_buffer,
                    parameters: e.parameters.into_iter().map(Parameter::from).collect(),
                    results: e.results.into_iter().map(Parameter::from).collect(),
                })
                .collect(),
            descriptor_sets: r
                .descriptor_sets
                .into_iter()
                .map(|s| {
                    (
                        s.space,
                        s.ranges
                            .into_iter()
                            .map(|d| DescriptorRange { binding_type: d.binding_type, category: d.category, binding: d.binding, count: d.count })
                            .collect(),
                    )
                })
                .collect(),
            global_constant_buffer_binding: r.global_constant_buffer_binding,
            global_constant_buffer_size: r.global_constant_buffer_size,
            bindless_space_index: (r.bindless_space_index >= 0).then_some(r.bindless_space_index as u32),
            json: r.json,
        }
    }
}

/// Generated code for one entry point on one target.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Code {
    pub target: TargetFormat,
    pub profile: String,
    pub entry_point: String,
    /// SPIR-V/DXIL bytecode, or UTF-8 source text for HLSL/GLSL/Metal/WGSL.
    pub bytes: Vec<u8>,
}

/// The result of a full compile.
#[derive(Debug, Clone, PartialEq)]
pub struct Compiled {
    pub module_name: String,
    pub reflection: Reflection,
    /// One [`Code`] per entry point per target (empty for [`reflect`]).
    pub code: Vec<Code>,
}

impl Compiled {
    /// The code for `entry_point` on `target`.
    pub fn code_for(&self, entry_point: &str, target: TargetFormat) -> Option<&Code> {
        self.code.iter().find(|c| c.entry_point == entry_point && c.target == target)
    }
}

fn compiled(r: full::FullCompileResult) -> Result<Compiled, CompileError> {
    if !r.ok {
        return Err(CompileError(r.error));
    }
    Ok(Compiled {
        module_name: r.module_name,
        reflection: r.reflection.into(),
        code: r
            .code
            .into_iter()
            .map(|c| Code { target: TargetFormat::from_u8(c.target), profile: c.profile, entry_point: c.entry_point, bytes: c.bytes })
            .collect(),
    })
}

/// Compiles a shader for the requested targets and returns generated code plus reflection.
pub fn compile(source: &Source, options: &CompileOptions) -> Result<Compiled, CompileError> {
    let (from_file, module, path, text) = source.parts();
    compiled(full::compile_shader(from_file, module, path, text, &options.to_ffi()))
}

/// Reflection only: the shader's interface without generating code.
pub fn reflect(source: &Source, options: &CompileOptions) -> Result<Compiled, CompileError> {
    let (from_file, module, path, text) = source.parts();
    compiled(full::reflect_shader(from_file, module, path, text, &options.to_ffi()))
}

/// Compiles a source once per macro set, caching results in memory (and optionally on disk).
pub struct VariantCache {
    inner: cxx::UniquePtr<full::VariantCacheBox>,
}

impl VariantCache {
    pub fn new(source: &Source, options: &CompileOptions, disk_cache: bool) -> Self {
        let (from_file, module, path, text) = source.parts();
        Self { inner: full::variant_cache_new(from_file, module, path, text, &options.to_ffi(), disk_cache) }
    }

    fn defines(defines: &[(&str, &str)]) -> Vec<full::MacroDesc> {
        defines.iter().map(|(n, v)| full::MacroDesc { name: (*n).to_owned(), value: (*v).to_owned() }).collect()
    }

    /// The compiled variant for `defines` (compiling it on first request).
    pub fn get(&mut self, defines: &[(&str, &str)]) -> Result<Compiled, CompileError> {
        compiled(full::variant_cache_get(self.inner.pin_mut(), &Self::defines(defines)))
    }

    pub fn contains(&self, defines: &[(&str, &str)]) -> bool {
        full::variant_cache_contains(&self.inner, &Self::defines(defines))
    }

    /// How many variants are cached.
    pub fn len(&self) -> usize {
        full::variant_cache_size(&self.inner) as usize
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Drops every cached variant (after the source changed).
    pub fn invalidate(&mut self) {
        full::variant_cache_invalidate(self.inner.pin_mut());
    }
}

/// The canonical string a macro set is keyed by in the variant cache.
pub fn variant_key(defines: &[(&str, &str)]) -> String {
    full::variant_key_canonical(&VariantCache::defines(defines))
}

/// The hash of a macro set's variant key (order-independent).
pub fn variant_key_hash(defines: &[(&str, &str)]) -> u64 {
    full::variant_key_hash(&VariantCache::defines(defines))
}
