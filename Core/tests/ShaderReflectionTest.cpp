/// reflect() must describe the entry points a caller asks for, and a ConstantBuffer<T> parameter must
/// expose T's members through ShaderTypeReflection::element_type.

#include <Core/Slang/Shader.hpp>

#include <iostream>

namespace {
    namespace slang = SFT::Core::Slang;

    constexpr const char *kSource = R"(
struct Params {
    float4 tint;
    float scale;
};
ConstantBuffer<Params> params;

[shader("compute")]
[numthreads(8, 4, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {}
)";

    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }
} // namespace

int main() {
    slang::ShaderCompiler compiler;
    slang::ShaderCompileOptions options;
    options.entry_points.push_back(slang::ShaderEntryPointRequest{.name = "cs_main", .stage = slang::ShaderStage::Compute});

    const auto reflected = compiler.reflect(slang::ShaderSource::from_source("reflection_test", kSource), options);
    check(reflected.has_value(), "reflect() must succeed");
    if (!reflected) {
        return 1;
    }

    check(reflected->entry_points.size() == 1 && reflected->entry_points[0].name == "cs_main",
          "reflect() must report the requested entry point");
    check(!reflected->entry_points.empty() && reflected->entry_points[0].compute_thread_group_size[0] == 8,
          "reflect() must report the entry point's thread group size");

    const slang::ShaderParameterReflection *params = nullptr;
    for (const auto &parameter : reflected->global_parameters) {
        if (parameter.name == "params") {
            params = &parameter;
        }
    }
    check(params != nullptr && params->type != nullptr, "the params constant buffer must be reflected");
    if (params != nullptr && params->type != nullptr) {
        check(params->type->element_type != nullptr, "ConstantBuffer<Params> must expose its element type");
        if (params->type->element_type != nullptr) {
            const auto &fields = params->type->element_type->fields;
            check(fields.size() == 2 && fields[0].name == "tint" && fields[1].name == "scale",
                  "the element type must list the buffer's members");
        }
    }

    const auto without_request = compiler.reflect(slang::ShaderSource::from_source("reflection_test", kSource), slang::ShaderCompileOptions{});
    check(without_request.has_value(), "reflect() without requested entry points must still succeed");
    return failures == 0 ? 0 : 1;
}
