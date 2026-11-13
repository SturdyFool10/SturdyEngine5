// Compile-only check of Shaders/ui_quad.slang for every target the UI quad pipeline can run on (SPIR-V, WGSL, DXIL),
// with and without SFT_UI_BINDLESS. A device-less test: it catches a shader that one backend's front end rejects, which a
// GPU test on Vulkan alone never would. DXIL needs the DirectX compiler, which is not available everywhere, so a DXIL
// failure is reported but does not fail the test.

#include <Core/Slang/Shader.hpp>
#include <Core/Slang/ShaderVariant.hpp>

#include <filesystem>
#include <iostream>
#include <string>

namespace {
    namespace slang = SFT::Core::Slang;

    int failures = 0;

    void compile(const char *name, slang::ShaderTargetFormat format, const char *profile, bool bindless, bool required) {
        slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{.format = format, .profile = profile}},
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        if (bindless) {
            options.macros.push_back(slang::ShaderMacro{.name = "SFT_UI_BINDLESS", .value = "1"});
        }
        slang::ShaderVariantCache cache{slang::ShaderSource::from_file("Shaders/ui_quad.slang", "ui_quad"), options,
                                        slang::ShaderCompiler{}, false};
        std::string problem;
        auto shader = cache.get_or_compile_base();
        if (!shader) {
            problem = shader.error().message + "\n" + shader.error().diagnostics;
        } else {
            for (const char *entry : {"vertexMain", "fragmentMain"}) {
                auto code = shader->entry_point_code(entry, format);
                if (!code || code->bytes.empty()) {
                    problem = std::string{entry} + ": " + (code ? "no code" : code.error().message);
                    break;
                }
            }
        }
        if (problem.empty()) {
            std::cout << name << (bindless ? " (bindless)" : "") << ": ok\n";
        } else if (required) {
            std::cerr << "FAILED: " << name << (bindless ? " (bindless)" : "") << ": " << problem << '\n';
            ++failures;
        } else {
            std::cout << "note: " << name << (bindless ? " (bindless)" : "") << " did not compile here: " << problem << '\n';
        }
    }
} // namespace

int main() {
    std::filesystem::current_path(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path());
    for (const bool bindless : {false, true}) {
        compile("spirv", slang::ShaderTargetFormat::Spirv, "spirv_1_5", bindless, true);
        // HLSL source is what DXIL is made from; generating it exercises the D3D12 front end without needing dxc.
        compile("hlsl", slang::ShaderTargetFormat::Hlsl, "sm_6_6", bindless, true);
        compile("dxil", slang::ShaderTargetFormat::Dxil, "sm_6_6", bindless, false);
    }
    // WebGPU has no descriptor arrays: only the plain variant is ever built for it.
    compile("wgsl", slang::ShaderTargetFormat::Wgsl, "", false, true);
    return failures == 0 ? 0 : 1;
}
