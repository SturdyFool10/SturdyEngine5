// Managed textures look after themselves: shrunk while decoding, reloaded in place when their file changes, released
// when over the residency limit and brought back when drawn again. Needs a real Vulkan device; prints SKIP without one.

#include <Async/Scheduler.hpp>
#include <Engine/EngineModule.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>
#include <WindowManager/WindowManager.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
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

    void be32(std::vector<std::uint8_t> &out, std::uint32_t v) { for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    std::uint32_t crc32(const std::uint8_t *d, std::size_t n) {
        std::uint32_t c = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < n; ++i) { c ^= d[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
        return ~c;
    }
    void chunk(std::vector<std::uint8_t> &png, const char *type, const std::vector<std::uint8_t> &body) {
        be32(png, static_cast<std::uint32_t>(body.size()));
        const std::size_t start = png.size();
        for (int i = 0; i < 4; ++i) png.push_back(static_cast<std::uint8_t>(type[i]));
        png.insert(png.end(), body.begin(), body.end());
        be32(png, crc32(png.data() + start, png.size() - start));
    }
    /// A solid-colour RGBA PNG (stored deflate), `size` x `size`.
    void write_png(const std::filesystem::path &path, std::uint32_t size, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        std::vector<std::uint8_t> ihdr;
        be32(ihdr, size); be32(ihdr, size);
        for (std::uint8_t v : {8, 6, 0, 0, 0}) ihdr.push_back(v);
        chunk(png, "IHDR", ihdr);
        std::vector<std::uint8_t> raw;
        for (std::uint32_t y = 0; y < size; ++y) { raw.push_back(0); for (std::uint32_t x = 0; x < size; ++x) { raw.push_back(r); raw.push_back(g); raw.push_back(b); raw.push_back(255); } }
        std::vector<std::uint8_t> z = {0x78, 0x01};
        for (std::size_t at = 0; at < raw.size(); at += 65535) {
            const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
            z.push_back(at + n >= raw.size() ? 1 : 0);
            z.push_back(n & 0xff); z.push_back(n >> 8); z.push_back(~n & 0xff); z.push_back((~n >> 8) & 0xff);
            z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        }
        std::uint32_t a = 1, b2 = 0;
        for (std::uint8_t v : raw) { a = (a + v) % 65521; b2 = (b2 + a) % 65521; }
        be32(z, (b2 << 16) | a);
        chunk(png, "IDAT", z);
        chunk(png, "IEND", {});
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
    }

    template <class Pred>
    bool wait_until(Engine::Engine &engine, std::chrono::seconds limit, Pred done) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            engine.assets().pump_texture_streaming();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }
} // namespace

int main() {
    namespace fs = std::filesystem;

    WindowManager::WindowManager window_manager;
    WindowManager::WindowConfig window_config{};
    window_config.title = "AssetManagedTextureGpuTest";
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

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sturdy-managed-texture-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto &assets = engine->assets();
    const auto size_of = [&](Engine::Asset a) { const auto p = assets.texture_progress(a); return p ? p->width : 0u; };
    const auto detail_of = [&](Engine::Asset a) { const auto p = assets.texture_progress(a); return p ? p->detail : Engine::TextureDetail::Failed; };

    // Shrunk while decoding.
    write_png(dir / "big.png", 16, 255, 0, 0);
    auto big = assets.load_texture_managed(dir / "big.png", Engine::TextureLoadOptions{.max_dimension = 4});
    if (!check(big.has_value(), "a managed texture must be created")) return 1;
    check(wait_until(*engine, std::chrono::seconds(10), [&] { return detail_of(*big) == Engine::TextureDetail::Complete; }), "it must finish loading");
    check(size_of(*big) == 4, "a 16px image limited to 4px must be 4px");
    auto again = assets.load_texture_managed(dir / "big.png", Engine::TextureLoadOptions{.max_dimension = 4});
    check(again && *again == *big, "the same file and options return the same asset");

    // Reload when the file changes (the size is capped, so the texture's version is what shows it).
    const u64 version_before = assets.texture_progress(*big)->version;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    write_png(dir / "big.png", 4, 0, 0, 255);
    check(wait_until(*engine, std::chrono::seconds(10), [&] { return assets.texture_progress(*big)->version > version_before; }), "a changed file must reload the same asset");
    check(*assets.texture_handle(*big) == *assets.texture_handle(*again), "the handle never changes");

    // A file that does not exist yet loads when it appears.
    auto late = assets.load_texture_managed(dir / "late.png");
    check(late.has_value(), "a missing file still gives an asset");
    check(wait_until(*engine, std::chrono::seconds(5), [&] { return detail_of(*late) == Engine::TextureDetail::Failed; }), "a missing file fails");
    write_png(dir / "late.png", 8, 255, 255, 0);
    check(wait_until(*engine, std::chrono::seconds(10), [&] { return detail_of(*late) == Engine::TextureDetail::Complete; }), "the file appearing loads it");
    check(size_of(*late) == 8, "with its own size");

    // Residency: over the limit the idle ones are released, and drawing one brings it back.
    write_png(dir / "a.png", 8, 1, 2, 3);
    write_png(dir / "b.png", 8, 4, 5, 6);
    auto a = assets.load_texture_managed(dir / "a.png");
    auto b = assets.load_texture_managed(dir / "b.png");
    check(wait_until(*engine, std::chrono::seconds(10), [&] { return detail_of(*a) == Engine::TextureDetail::Complete && detail_of(*b) == Engine::TextureDetail::Complete; }), "a and b load");
    assets.set_texture_residency_limit(1);
    check(wait_until(*engine, std::chrono::seconds(15), [&] {
        return detail_of(*a) == Engine::TextureDetail::Placeholder || detail_of(*b) == Engine::TextureDetail::Placeholder;
    }), "over the limit something is released");
    check(size_of(*a) == 8 && size_of(*b) == 8, "released textures keep their logical size");
    const Engine::Asset released = detail_of(*a) == Engine::TextureDetail::Placeholder ? *a : *b;
    const auto handle = *assets.texture_handle(released);
    const std::pair<Renderer::TextureHandle, f32> drawn[1] = {{handle, 64.0f}};
    engine->renderer()->note_texture_demand(drawn);
    check(wait_until(*engine, std::chrono::seconds(10), [&] { return detail_of(released) == Engine::TextureDetail::Complete; }), "drawing a released texture brings it back");

    fs::remove_all(dir);
    if (failures == 0) std::puts("OK");
    return failures == 0 ? 0 : 1;
}
