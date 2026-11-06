/// A shader that cannot compile must be answered from memory on the next request instead of being recompiled: renderer features ask
/// for their shaders every frame, and recompiling a broken one each time looks like an endless loop inside shader compilation. Fixing
/// the source (or an explicit invalidate()) must still take effect at once.

#include <Core/Slang/ShaderVariant.hpp>

#include <chrono>
#include <string>

#include <Foundation/Foundation.hpp>

namespace {
    namespace slang = SFT::Core::Slang;

    constexpr const char *kBroken = R"(
[shader("compute")]
[numthreads(1, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) { this_name_does_not_exist = 1; }
)";

    constexpr const char *kFixed = R"(
[shader("compute")]
[numthreads(1, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {}
)";

    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            SFT::Foundation::log_error("FAILED: {}", message);
            ++failures;
        }
    }

    slang::ShaderCompileOptions options() {
        slang::ShaderCompileOptions result;
        result.entry_points.push_back(slang::ShaderEntryPointRequest{.name = "cs_main", .stage = slang::ShaderStage::Compute});
        return result;
    }

    double timed_ms(slang::ShaderVariantCache &cache, bool &ok, std::string &message) {
        const auto start = std::chrono::steady_clock::now();
        const auto shader = cache.get_or_compile_base();
        ok = shader.has_value();
        message = shader ? std::string{} : shader.error().message + shader.error().diagnostics;
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
} // namespace

int main() {
    bool ok = true;
    std::string first_message;
    std::string second_message;

    // Two separate caches, as the renderer's per-frame ensure_* functions build them.
    slang::ShaderVariantCache first{slang::ShaderSource::from_source("memo_probe", kBroken), options()};
    slang::ShaderVariantCache second{slang::ShaderSource::from_source("memo_probe", kBroken), options()};

    const double first_ms = timed_ms(first, ok, first_message);
    check(!ok, "a shader referencing an unknown name must fail to compile");
    check(!first_message.empty(), "the failure must say why");

    const double second_ms = timed_ms(second, ok, second_message);
    check(!ok, "the repeat request must report the same failure");
    check(second_message == first_message, "the remembered failure must carry the original reason");
    check(second_ms < first_ms, "the repeat request must not recompile");

    // Changed source is a different key, so it compiles straight away.
    slang::ShaderVariantCache fixed{slang::ShaderSource::from_source("memo_probe", kFixed), options()};
    std::string fixed_message;
    (void)timed_ms(fixed, ok, fixed_message);
    check(ok, "fixing the source must take effect immediately");

    // An explicit invalidate() is a request to try again.
    first.invalidate();
    (void)timed_ms(first, ok, first_message);
    check(!ok && !first_message.empty(), "after invalidate() the broken shader is compiled again and still fails with a reason");

    return failures == 0 ? 0 : 1;
}
