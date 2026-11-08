// PLY and STL meshes import as models (ASCII and binary, both byte orders for PLY), and a model can be rebuilt under
// the same handle. Needs a real Vulkan device; prints SKIP and succeeds without one.

#include <Async/Scheduler.hpp>
#include <Engine/EngineModule.hpp>
#include <Engine/MeshFileImport.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>
#include <WindowManager/WindowManager.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
    using namespace SFT;

    int failures = 0;

    bool check(bool condition, const std::string &message) {
        if (!condition) {
            Foundation::log_error("FAILED: {}", message);
            ++failures;
        }
        return condition;
    }

    std::vector<std::byte> bytes_of(const std::string &text) {
        std::vector<std::byte> out(text.size());
        std::memcpy(out.data(), text.data(), text.size());
        return out;
    }

    template <class T>
    void append(std::vector<std::byte> &out, T value) {
        const auto *p = reinterpret_cast<const std::byte *>(&value);
        out.insert(out.end(), p, p + sizeof(T));
    }
} // namespace

int main() {
    namespace fs = std::filesystem;

    WindowManager::WindowManager window_manager;
    WindowManager::WindowConfig window_config{};
    window_config.title = "MeshFileImportGpuTest";
    window_config.extent = {64, 64};
    window_config.visible = false;
    window_config.graphics_api = WindowManager::WindowGraphicsApi::Vulkan;
    auto window_id = window_manager.spawn_window<WindowManager::SDL3::SDL3Window>(window_config);
    if (!window_id) {
        Foundation::log_info("SKIP: could not create a window ({})", window_id.error().message);
        return 0;
    }
    auto engine = std::make_unique<Engine::Engine>();
    std::optional<Core::RenderSurfaceHandle> surface;
    std::string skip_reason;
    (void)window_manager.with_window(*window_id, [&](WindowManager::Window &window) {
        auto initialized = engine->initialize(window);
        if (!initialized) {
            skip_reason = initialized.error().message;
            return false;
        }
        surface = *initialized;
        return true;
    });
    if (!surface) {
        Foundation::log_info("SKIP: no Vulkan device available ({})", skip_reason);
        return 0;
    }

    auto shader = engine->assets().load_shader("Shaders/gbuffer_geometry.slang", UString{std::string_view{"mesh import shader"}});
    if (!check(shader.has_value(), "the shader must load")) return 1;

    // ASCII PLY: a quad (two triangles via one 4-gon) with vertex colours and no normals.
    {
        const auto ply = bytes_of(
            "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
            "property uchar red\nproperty uchar green\nproperty uchar blue\nelement face 1\n"
            "property list uchar int vertex_indices\nend_header\n"
            "0 0 0 255 0 0\n1 0 0 0 255 0\n1 1 0 0 0 255\n0 1 0 255 255 255\n4 0 1 2 3\n");
        auto scene = Engine::import_ply_bytes(engine->assets(), ply, "quad", *shader);
        if (check(scene.has_value(), "an ASCII PLY must import") && check(scene->models.size() == 1, "one model")) {
            const auto info = engine->assets().model_info(scene->models[0]);
            check(info && info->triangle_count == 2 && info->vertex_count == 4, "a 4-gon fans into two triangles");
        }
    }
    // Binary big-endian PLY: one triangle, float positions, with an extra element to skip.
    {
        std::string header = "ply\nformat binary_big_endian 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
                             "element face 1\nproperty list uchar uint vertex_indices\nend_header\n";
        std::vector<std::byte> ply = bytes_of(header);
        const auto be = [&](float f) {
            std::uint32_t v;
            std::memcpy(&v, &f, 4);
            for (int i = 3; i >= 0; --i) ply.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
        };
        for (float f : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f}) be(f);
        ply.push_back(std::byte{3});
        for (std::uint32_t index : {0u, 1u, 2u}) for (int i = 3; i >= 0; --i) ply.push_back(static_cast<std::byte>((index >> (8 * i)) & 0xff));
        auto scene = Engine::import_ply_bytes(engine->assets(), ply, "tri", *shader);
        check(scene.has_value(), "a big-endian binary PLY must import");
        if (scene) {
            const auto info = engine->assets().model_info(scene->models[0]);
            check(info && info->triangle_count == 1, "one triangle");
        }
        check(!Engine::import_ply_bytes(engine->assets(), bytes_of("not a ply"), "bad", *shader).has_value(), "garbage is refused");
    }
    // Binary STL: two triangles.
    {
        std::vector<std::byte> stl(80, std::byte{0});
        append<std::uint32_t>(stl, 2);
        for (int t = 0; t < 2; ++t) {
            for (float f : {0.f, 0.f, 1.f,  0.f, 0.f, 0.f,  1.f, 0.f, 0.f,  0.f, 1.f, float(t)}) append<float>(stl, f);
            append<std::uint16_t>(stl, 0);
        }
        auto scene = Engine::import_stl_bytes(engine->assets(), stl, "two", *shader);
        if (check(scene.has_value(), "a binary STL must import")) {
            const auto info = engine->assets().model_info(scene->models[0]);
            check(info && info->triangle_count == 2, "two triangles");
        }
    }
    // ASCII STL: one triangle.
    {
        const auto stl = bytes_of("solid t\n facet normal 0 0 1\n  outer loop\n   vertex 0 0 0\n   vertex 1 0 0\n   vertex 0 1 0\n  endloop\n endfacet\nendsolid t\n");
        auto scene = Engine::import_stl_bytes(engine->assets(), stl, "ascii", *shader);
        if (check(scene.has_value(), "an ASCII STL must import")) {
            const auto info = engine->assets().model_info(scene->models[0]);
            check(info && info->triangle_count == 1, "one ASCII triangle");
        }
    }
    // replace_model: the handle stays, the geometry changes.
    {
        auto model = engine->assets().create_model(Renderer::Mesh::cube(), *shader, glm::vec4{1.0f});
        if (check(model.has_value(), "a cube must be created")) {
            const auto before = engine->assets().model_info(*model);
            Engine::ModelAssetDesc desc;
            desc.label = UString{std::string_view{"rebuilt"}};
            desc.primitives.push_back(Engine::ModelPrimitiveDesc{.mesh = Renderer::Mesh::cube(), .shader = *shader});
            desc.primitives.push_back(Engine::ModelPrimitiveDesc{.mesh = Renderer::Mesh::cube(), .shader = *shader});
            check(engine->assets().replace_model(*model, std::move(desc)).has_value(), "replace_model must succeed");
            const auto after = engine->assets().model_info(*model);
            check(before && after && after->primitive_count == 2 && before->primitive_count == 1, "the same handle now has two primitives");
            Engine::ModelAssetDesc bad;
            check(!engine->assets().replace_model(*model, std::move(bad)).has_value(), "an empty description is refused");
            check(engine->assets().model_info(*model)->primitive_count == 2, "a failed replace leaves the model alone");
        }
    }

    if (failures == 0) std::puts("OK");
    return failures == 0 ? 0 : 1;
}
