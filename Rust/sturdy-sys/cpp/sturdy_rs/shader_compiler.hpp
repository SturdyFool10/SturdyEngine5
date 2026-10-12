// Rust <-> SturdyEngine 5 Slang-shader-compiler shim. See src/shader_compiler.rs for why this
// exists (making the raw RHI pipeline path testable from Rust, distinct from
// sturdy::assets::Assets::load_shader's file-based, Asset-producing path).
#pragma once

#include <cstdint>

#include <memory>

#include <rust/cxx.h>

#include <Core/Slang/Shader.hpp>
#include <Core/Slang/ShaderVariant.hpp>

namespace sturdy_rs::shader_compiler {

    enum class ShaderStage : std::uint8_t;
    struct EntryPointRequest;
    struct ShaderCompileResult;

    struct TargetDesc;
    struct MacroDesc;
    struct EntryPointDesc;
    struct CompileOptionsDesc;
    struct FullCompileResult;
    using VariantCacheBox = SFT::Core::Slang::ShaderVariantCache;

    FullCompileResult compile_shader(bool from_file, rust::Str module_name, rust::Str path, rust::Str source,
                                     const CompileOptionsDesc &options);
    FullCompileResult reflect_shader(bool from_file, rust::Str module_name, rust::Str path, rust::Str source,
                                     const CompileOptionsDesc &options);
    std::unique_ptr<VariantCacheBox> variant_cache_new(bool from_file, rust::Str module_name, rust::Str path,
                                                       rust::Str source, const CompileOptionsDesc &options,
                                                       bool enable_disk_cache);
    FullCompileResult variant_cache_get(VariantCacheBox &cache, const rust::Vec<MacroDesc> &defines);
    std::uint64_t variant_cache_size(const VariantCacheBox &cache);
    bool variant_cache_contains(const VariantCacheBox &cache, const rust::Vec<MacroDesc> &defines);
    void variant_cache_invalidate(VariantCacheBox &cache);
    rust::String variant_key_canonical(const rust::Vec<MacroDesc> &defines);
    std::uint64_t variant_key_hash(const rust::Vec<MacroDesc> &defines);

    ShaderCompileResult compile_slang_spirv(rust::Str source, rust::Str module_name,
                                            rust::Slice<const EntryPointRequest> entry_points);

} // namespace sturdy_rs::shader_compiler
