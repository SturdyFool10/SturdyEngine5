/// What the shader disk cache and the DXC bundle must get right: an entry made for other targets or by another Slang is never served,
/// an edit to a module that a shader only imports takes effect, unsigned DXIL is recognised, and shader paths survive a trip through UTF-8.

#include <Core/Slang/BundledDxc.hpp>
#include <Core/Slang/ShaderCache.hpp>
#include <Core/Slang/Shader.hpp>
#include <Core/Slang/ShaderError.hpp>
#include <Core/Slang/ShaderPath.hpp>
#include <Core/Slang/ShaderVariant.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <string>
#include <thread>

#include <Foundation/Foundation.hpp>

namespace {
    namespace slang = SFT::Core::Slang;
    namespace fs = std::filesystem;

    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            SFT::Foundation::log_error("FAILED: {}", message);
            ++failures;
        }
    }

    void write(const fs::path &path, const std::string &text) {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out << text;
    }

    constexpr const char *kModuleGood = "float scale_value(float v) { return v * 2.0; }\n";
    constexpr const char *kModuleBroken = "float renamed_value(float v) { return v * 2.0; }\n";
    constexpr const char *kShader = R"(
import shared_math;
[shader("compute")]
[numthreads(1, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) { float x = scale_value(1.0); }
)";

    slang::ShaderCompileOptions options() {
        slang::ShaderCompileOptions result;
        result.entry_points.push_back(slang::ShaderEntryPointRequest{.name = "cs_main", .stage = slang::ShaderStage::Compute});
        return result;
    }

    void test_signature_detection() {
        std::array<std::byte, 24> unsigned_dxil{};
        unsigned_dxil[0] = std::byte{'D'};
        unsigned_dxil[1] = std::byte{'X'};
        unsigned_dxil[2] = std::byte{'B'};
        unsigned_dxil[3] = std::byte{'C'};
        check(!slang::dxil_container_is_signed(unsigned_dxil), "a container with an all-zero hash is unsigned");
        std::array<std::byte, 24> signed_dxil = unsigned_dxil;
        signed_dxil[9] = std::byte{0x5a};
        check(slang::dxil_container_is_signed(signed_dxil), "a container with a hash is signed");
        std::array<std::byte, 24> not_dxil = signed_dxil;
        not_dxil[0] = std::byte{'X'};
        check(!slang::dxil_container_is_signed(not_dxil), "something that is not a DXBC container is not signed DXIL");
        check(!slang::dxil_container_is_signed({}), "empty bytes are not signed DXIL");
    }

    void test_cache_key_covers_targets() {
        slang::ShaderCompileOptions spirv = options();
        spirv.targets = {slang::ShaderTarget{.format = slang::ShaderTargetFormat::Spirv, .profile = "spirv_1_5"}};
        slang::ShaderCompileOptions dxil = spirv;
        dxil.targets = {slang::ShaderTarget{.format = slang::ShaderTargetFormat::Dxil, .profile = "sm_6_6"}};
        slang::ShaderCompileOptions other_profile = spirv;
        other_profile.targets = {slang::ShaderTarget{.format = slang::ShaderTargetFormat::Spirv, .profile = "spirv_1_6"}};
        const auto key = [](const slang::ShaderCompileOptions &o) { return slang::compute_shader_cache_key("m", "text", "", o); };
        check(key(spirv) == key(spirv), "the key is stable");
        check(key(spirv) != key(dxil), "different target formats must not share a cache entry");
        check(key(spirv) != key(other_profile), "different profiles must not share a cache entry");
    }

    void test_imported_module_edit_invalidates() {
        const fs::path root = fs::temp_directory_path() / "sturdy-shader-cache-test";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "src");
        const fs::path cache = root / "cache";
        write(root / "src" / "shared_math.slang", kModuleGood);
        write(root / "src" / "consumer.slang", kShader);
        const std::string path = slang::shader_path_to_utf8(root / "src" / "consumer.slang");

        {
            slang::ShaderVariantCache first{slang::ShaderSource::from_file(path, "consumer"), options(), {}, true, cache};
            check(first.get_or_compile_base().has_value(), "the shader compiles against its module");
        }
        // The module is replaced by one that no longer defines what the shader calls; the shader's own text is unchanged. The cached bytecode
        // of the old module must not be served.
        // The directory scan behind the key is memoised for a second (a person editing a file is slower than that).
        std::this_thread::sleep_for(std::chrono::milliseconds{1200});
        write(root / "src" / "shared_math.slang", kModuleBroken);
        {
            slang::ShaderVariantCache second{slang::ShaderSource::from_file(path, "consumer"), options(), {}, true, cache};
            check(!second.get_or_compile_base().has_value(), "an edit to an imported module must not be hidden by the disk cache");
        }
        fs::remove_all(root, ec);
    }

    void test_dxc_status_reporting() {
        const slang::DxcStatus status = slang::bundled_dxc_status();
        check(!status.summary.empty(), "the DXC status must always say something");
        check(status.describe().find(status.summary) == 0, "describe() starts with the summary");
#if defined(_WIN32)
        check(status.health != slang::DxcHealth::NotApplicable, "on Windows DXC is relevant");
        check(status.health == slang::DxcHealth::Ok || !status.details.empty() || !status.summary.empty(),
              "anything that is not Ok explains itself");
#else
        check(status.health == slang::DxcHealth::NotApplicable, "off Windows DXC plays no part");
        check(!slang::bundled_dxc().available(), "off Windows nothing is bundled");
        check(!slang::bundled_dxc().unavailable_reason.empty(), "an absent bundle says why it is absent");
#endif
        const slang::DxilAvailability &dxil = slang::dxil_availability();
        check(dxil.available || !dxil.message.empty(), "when DXIL is unavailable the result must say why");
        check(dxil.available == (dxil.issue == slang::DxilIssue::None || dxil.issue == slang::DxilIssue::Degraded),
              "the issue code must agree with the availability flag");
        check(slang::dxil_generation_available() == dxil.available, "the bool form agrees with the detailed result");
    }

    void test_cache_reports_why() {
        const fs::path root = fs::temp_directory_path() / "sturdy-shader-cache-issues";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);

        auto missing = slang::load_shader_cache_entry(root, 1);
        check(!missing && missing.error().code == slang::ShaderCacheCode::Missing && missing.error().routine(),
              "a key with no entry is reported as Missing, a routine outcome");

        write(root / "garbage", "x");
        // A file that is not an entry: put one where the loader will look for key 2.
        slang::ShaderCacheEntry entry;
        entry.module_name = "m";
        check(slang::store_shader_cache_entry(root, 2, entry).has_value(), "an entry can be stored");
        check(slang::load_shader_cache_entry(root, 2).has_value(), "and loaded again");
        // Damage it: keep the file, destroy its header.
        for (const auto &file : fs::directory_iterator(root)) {
            if (file.path().filename() != "garbage") {
                write(file.path(), "this is not a shader cache entry at all");
            }
        }
        auto damaged = slang::load_shader_cache_entry(root, 2);
        check(!damaged && !damaged.error().routine() && !damaged.error().message.empty(),
              "a damaged entry is reported as a real problem, with the file named");

        // A cache folder that cannot be created (its parent is a file).
        auto blocked = slang::store_shader_cache_entry(root / "garbage" / "sub", 3, entry);
        check(!blocked && blocked.error().code == slang::ShaderCacheCode::DirectoryFailed && !blocked.error().message.empty(),
              "a cache folder that cannot be created is reported as DirectoryFailed with the reason");
        fs::remove_all(root, ec);
    }

    void test_error_messages_carry_the_cause() {
        const auto error = slang::shader_error(slang::ShaderErrorCode::CompilationFailed, "Failed to load Slang shader module: x",
                                               "x.slang(3): error 30015: undefined identifier 'nope'\nsecond line\n");
        check(error.error().message.find("undefined identifier 'nope'") != std::string::npos,
              "a compile error's message names what the compiler complained about, not only that it failed");
        check(error.error().diagnostics.find("second line") != std::string::npos, "the full diagnostics are kept");

        slang::ShaderCompileOptions bad = options();
        bad.targets = {slang::ShaderTarget{.format = slang::ShaderTargetFormat::Spirv, .profile = "spirv_9_9_not_real"}};
        slang::ShaderCompiler compiler;
        const auto result = compiler.compile(slang::ShaderSource::from_source("p", kModuleGood), bad);
        check(!result.has_value(), "an unknown SPIR-V profile is an error, not a silent change of output");
        if (!result) {
            check(result.error().message.find("spirv_9_9_not_real") != std::string::npos, "the error names the unknown profile");
        }

        const auto missing_file = compiler.compile(slang::ShaderSource::from_file("/definitely/not/here/nothing_like_this.slang"), options());
        check(!missing_file.has_value(), "a missing shader file is an error");
        if (!missing_file) {
            check(missing_file.error().message.find("does not exist") != std::string::npos, "and says that it does not exist");
        }
    }

    void test_utf8_paths() {
        const std::string text = "Shaders/z\xc3\xb6\xc3\xab/shader.slang";
        check(slang::shader_path_to_utf8(slang::shader_path_from_utf8(text)) == text, "a UTF-8 shader path survives a round trip");
    }
} // namespace

int main() {
    test_signature_detection();
    test_cache_key_covers_targets();
    test_imported_module_edit_invalidates();
    test_dxc_status_reporting();
    test_cache_reports_why();
    test_error_messages_carry_the_cause();
    test_utf8_paths();
    return failures == 0 ? 0 : 1;
}
