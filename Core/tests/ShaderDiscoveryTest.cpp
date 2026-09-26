/// Runs the engine's own shader discovery over the shipped Shaders/ directory and requires that every
/// .slang file reflects cleanly.
///
/// Why this is a test and not something a build catches: discover_shaders() reflects *every* file in
/// the directory at each engine start, and a file that fails is logged and skipped rather than
/// failing the build — so a new or edited shader with a diagnostic (a bad import, an unsupported
/// stage capability, a signature mismatch against a module it imports) ships silently, costs startup
/// time re-failing on every run, and only surfaces as a missing material template much later.
/// slangc is not a substitute: it rejects library modules ("no exported symbols") that the engine
/// accepts, and accepts flags (-capability, -profile) the engine's reflect() does not pass.

#include <Core/Slang/ShaderDiscovery.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>

namespace {

    namespace fs = std::filesystem;
    namespace slang = SFT::Core::Slang;

    /// Repository-relative Shaders/ directory, derived from this file's own location so the test needs no
    /// build-system plumbing: Core/tests/<this file> -> <repo>/Shaders.
    fs::path shaders_directory() { return fs::path(__FILE__).parent_path().parent_path().parent_path() / "Shaders"; }

} // namespace

int main() {
    const fs::path directory = shaders_directory();
    if (!fs::is_directory(directory)) {
        std::cout << "skipped: no Shaders directory next to the sources (" << directory << ")\n";
        return 0;
    }

    std::unordered_set<std::string> on_disk;
    for (const fs::directory_entry &entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == slang::shader_file_extension) {
            on_disk.insert(entry.path().stem().string());
        }
    }

    slang::ShaderCompiler compiler;
    const auto discovered = slang::discover_shaders(directory, compiler, slang::ShaderCompileOptions{}, false);

    std::unordered_set<std::string> reflected;
    for (const slang::UnCompiledShader &shader : discovered) {
        reflected.insert(std::string(shader.module_name()));
    }

    int failures = 0;
    for (const std::string &name : on_disk) {
        if (!reflected.contains(name)) {
            std::cerr << "FAILED: Shaders/" << name << ".slang did not reflect (see the diagnostic above)\n";
            ++failures;
        }
    }
    if (failures != 0) {
        return 1;
    }
    std::cout << "ok: " << on_disk.size() << " shader file(s) reflected\n";
    return 0;
}
