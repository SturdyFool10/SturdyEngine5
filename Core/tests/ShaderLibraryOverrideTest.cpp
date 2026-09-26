/// An override registered with override_shader_module() must be served for `import <name>;` in place of
/// (or without) a file, must change what compiles when it is replaced, and must stop applying once removed.

#include <Core/Slang/Shader.hpp>
#include <Core/Slang/ShaderLibrary.hpp>

#include <iostream>

namespace {
    namespace slang = SFT::Core::Slang;

    constexpr const char *kSource = R"(
import override_probe_lib;
ConstantBuffer<ProbeParams> params;

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {}
)";

    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }

    /// Field names of ProbeParams as reflected, or empty when compilation fails.
    std::vector<std::string> probe_fields(slang::ShaderCompiler &compiler, bool &ok) {
        slang::ShaderCompileOptions options;
        options.entry_points.push_back(slang::ShaderEntryPointRequest{.name = "cs_main", .stage = slang::ShaderStage::Compute});
        const auto reflected = compiler.reflect(slang::ShaderSource::from_source("override_probe", kSource), options);
        ok = reflected.has_value();
        std::vector<std::string> names;
        if (!reflected) {
            return names;
        }
        for (const auto &parameter : reflected->global_parameters) {
            if (parameter.name == "params" && parameter.type && parameter.type->element_type) {
                for (const auto &field : parameter.type->element_type->fields) {
                    names.push_back(std::string{field.name});
                }
            }
        }
        return names;
    }
} // namespace

int main() {
    slang::ShaderCompiler compiler;
    bool ok = false;

    (void)probe_fields(compiler, ok);
    check(!ok, "importing a module that exists nowhere must fail before an override is registered");
    check(slang::shader_override_fingerprint() == 0, "no overrides means fingerprint 0");

    slang::override_shader_module("override_probe_lib.slang", "struct ProbeParams { float alpha; };");
    check(slang::find_shader_module_override("override_probe_lib").has_value(), "a trailing .slang must be ignored");
    const u64 first_fingerprint = slang::shader_override_fingerprint();
    check(first_fingerprint != 0, "a registered override must change the fingerprint");
    auto fields = probe_fields(compiler, ok);
    check(ok && fields.size() == 1 && fields[0] == "alpha", "the override must be served for the import");

    slang::override_shader_module("override_probe_lib", "struct ProbeParams { float beta; float gamma; };");
    check(slang::shader_override_fingerprint() != first_fingerprint, "replacing the source must change the fingerprint");
    fields = probe_fields(compiler, ok);
    check(ok && fields.size() == 2 && fields[0] == "beta", "a replaced override must take effect on the next compile");

    check(slang::remove_shader_module_override("override_probe_lib"), "removal must report success");
    check(!slang::remove_shader_module_override("override_probe_lib"), "removing twice must report false");
    (void)probe_fields(compiler, ok);
    check(!ok, "after removal the import must fail again");
    check(slang::shader_override_fingerprint() == 0, "no overrides means fingerprint 0 again");
    return failures == 0 ? 0 : 1;
}
