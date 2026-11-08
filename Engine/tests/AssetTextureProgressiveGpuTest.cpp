/// Textures are managed assets whose handle never changes while their pixels do:
///   * `Renderer::replace_texture` gives a handle new contents, size and format; `update_texture_regions` rewrites
///     rectangles of it; the texture's `revision` moves so caches keyed on its view notice;
///   * an SVG asset is re-rasterised in place when a larger size is asked for;
///   * `load_texture_progressive` / `create_texture_progressive_from_bytes` return at once and then deliver the image
///     (a JPEG XL through its low-detail pass first), ending `Complete` at the full size on the same handle.
/// Needs a real Vulkan device; prints SKIP and succeeds without one.

#include <Async/Scheduler.hpp>
#include <Engine/EngineModule.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>
#include <WindowManager/WindowManager.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
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

    /// The base level of a single-mip 8-bit RGBA texture as bytes, read back from the GPU.
    std::optional<std::vector<u8>> read_rgba(Engine::Engine &engine, Renderer::TextureHandle handle) {
        RHI::RhiDevice *device = engine.rhi_device();
        const Renderer::TextureResource *resource = engine.renderer()->texture(handle);
        if (device == nullptr || resource == nullptr) return std::nullopt;
        const u32 width = resource->width, height = resource->height;
        const u64 row_pitch = ((static_cast<u64>(width) * 4u + 255u) / 256u) * 256u;
        auto buffer = device->create_buffer(RHI::BufferDesc{.size = row_pitch * height,
                                                            .usage = RHI::BufferUsage::TransferDst,
                                                            .memory = RHI::MemoryLocation::HostReadback,
                                                            .label = "texture asset test readback"});
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "texture asset test readback"});
        if (!buffer || !encoder) return std::nullopt;
        RHI::TextureBarrier to_transfer{
            .texture = resource->texture,
            .src_stage = RHI::PipelineStage::FragmentShader | RHI::PipelineStage::ComputeShader,
            .src_access = RHI::AccessFlags::ShaderRead,
            .dst_stage = RHI::PipelineStage::Transfer,
            .dst_access = RHI::AccessFlags::TransferRead,
            .old_layout = RHI::TextureLayout::ShaderReadOnly,
            .new_layout = RHI::TextureLayout::TransferSrc,
        };
        (*encoder)->barrier({}, {}, std::span<const RHI::TextureBarrier>{&to_transfer, 1});
        (*encoder)->copy_texture_to_buffer(resource->texture, *buffer,
                                           RHI::BufferTextureCopy{.buffer_offset = 0,
                                                                  .buffer_row_length = static_cast<u32>(row_pitch / 4u),
                                                                  .buffer_image_height = height,
                                                                  .mip_level = 0,
                                                                  .base_array_layer = 0,
                                                                  .array_layer_count = 1,
                                                                  .texture_offset = {0, 0, 0},
                                                                  .texture_extent = {width, height, 1}});
        RHI::TextureBarrier back = to_transfer;
        std::swap(back.src_stage, back.dst_stage);
        std::swap(back.src_access, back.dst_access);
        std::swap(back.old_layout, back.new_layout);
        (*encoder)->barrier({}, {}, std::span<const RHI::TextureBarrier>{&back, 1});
        auto command_buffer = (*encoder)->finish();
        auto fence = device->create_fence(RHI::FenceDesc{.label = "texture asset test fence"});
        if (!command_buffer || !fence) return std::nullopt;
        const RHI::CommandBufferHandle buffers[1] = {*command_buffer};
        if (!device->submit(RHI::SubmitDesc{.command_buffers = std::span<const RHI::CommandBufferHandle>{buffers, 1},
                                            .fence = *fence,
                                            .flags = RHI::SubmitFlags::OneShot,
                                            .label = "texture asset test submit"})) {
            return std::nullopt;
        }
        auto waited = device->wait_fences(std::span<const RHI::FenceHandle>{&*fence, 1}, true);
        if (!waited || !*waited) return std::nullopt;
        auto mapped = device->map_buffer(*buffer);
        if (!mapped) return std::nullopt;
        std::vector<u8> out(static_cast<usize>(width) * height * 4u);
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(out.data() + static_cast<usize>(y) * width * 4u,
                        reinterpret_cast<const u8 *>(mapped->data()) + y * row_pitch, static_cast<usize>(width) * 4u);
        }
        device->unmap_buffer(*buffer);
        device->destroy_fence(*fence);
        device->destroy_command_buffer(*command_buffer);
        device->destroy_buffer(*buffer);
        return out;
    }

    std::vector<std::byte> solid(u32 width, u32 height, u8 r, u8 g, u8 b, u8 a = 255) {
        std::vector<std::byte> pixels(static_cast<usize>(width) * height * 4u);
        for (usize i = 0; i < pixels.size(); i += 4) {
            pixels[i] = std::byte{r};
            pixels[i + 1] = std::byte{g};
            pixels[i + 2] = std::byte{b};
            pixels[i + 3] = std::byte{a};
        }
        return pixels;
    }

    /// Pumps the asset manager until the texture's detail reaches `wanted` (or a few seconds pass).
    bool wait_for(Engine::Engine &engine, Engine::Asset asset, Engine::TextureDetail wanted) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            engine.assets().pump_texture_streaming();
            const auto progress = engine.assets().texture_progress(asset);
            if (progress && progress->detail == wanted) return true;
            if (progress && progress->detail == Engine::TextureDetail::Failed) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
} // namespace

int main() {
    namespace fs = std::filesystem;

    WindowManager::WindowManager window_manager;
    WindowManager::WindowConfig window_config{};
    window_config.title = "AssetTextureProgressiveGpuTest";
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

    // ---- replace_texture: same handle, new size and pixels, moving revision ----
    {
        Renderer::Renderer &renderer = *engine->renderer();
        const auto red = solid(8, 8, 255, 0, 0);
        auto handle = renderer.create_texture(8, 8, RHI::Format::RGBA8Unorm, red, "replace test");
        if (!check(handle.has_value(), "a texture must be created")) return 1;
        const u64 first_revision = renderer.texture(*handle)->revision;
        const auto green = solid(16, 4, 0, 255, 0);
        check(renderer.replace_texture(*handle, 16, 4, RHI::Format::RGBA8Unorm, green).has_value(),
              "replacing a texture with a different size must succeed");
        const Renderer::TextureResource *resource = renderer.texture(*handle);
        check(resource != nullptr && resource->width == 16 && resource->height == 4,
              "the same handle must report the new size");
        check(resource != nullptr && resource->revision > first_revision, "a replacement must move the revision");
        const auto pixels = read_rgba(*engine, *handle);
        check(pixels && pixels->size() == 16 * 4 * 4 && (*pixels)[0] == 0 && (*pixels)[1] == 255,
              "the replaced texture must hold the new pixels");

        // ---- update_texture_regions: only the named rectangle changes ----
        const std::array<Renderer::Renderer::TextureRegion, 1> regions{
            Renderer::Renderer::TextureRegion{.x = 4, .y = 1, .width = 2, .height = 2}};
        const auto blue = solid(2, 2, 0, 0, 255);
        check(renderer.update_texture_regions(*handle, regions, blue).has_value(), "a region update must succeed");
        const auto patched = read_rgba(*engine, *handle);
        const auto texel = [&](u32 x, u32 y) { return patched ? (*patched).data() + (static_cast<usize>(y) * 16 + x) * 4 : nullptr; };
        check(patched && texel(4, 1)[2] == 255 && texel(5, 2)[2] == 255 && texel(0, 0)[1] == 255 && texel(6, 1)[1] == 255,
              "a region update must change the region and nothing around it");
        const std::array<Renderer::Renderer::TextureRegion, 1> outside{
            Renderer::Renderer::TextureRegion{.x = 15, .y = 0, .width = 2, .height = 1}};
        check(!renderer.update_texture_regions(*handle, outside, solid(2, 1, 1, 1, 1)).has_value(),
              "a region outside the texture must be refused");

        // Destroyed textures keep a revision that can never match a live one.
        renderer.destroy_texture(*handle);
        check(renderer.texture(*handle) == nullptr, "a destroyed texture must not resolve");
    }

    // ---- asset replace_texture keeps the asset and its renderer handle ----
    {
        auto asset = engine->assets().create_texture(Engine::TextureAssetDesc{
            .width = 4, .height = 4, .pixels = solid(4, 4, 9, 9, 9), .label = UString{std::string_view{"asset replace"}},
            .allow_compression = false, .generate_mipmaps = false});
        if (!check(asset.has_value(), "an asset texture must be created")) return 1;
        const auto handle_before = engine->assets().texture_handle(*asset);
        check(engine->assets()
                  .replace_texture(*asset, Engine::TextureAssetDesc{.width = 8, .height = 2, .pixels = solid(8, 2, 1, 2, 3),
                                                                      .allow_compression = false, .generate_mipmaps = false})
                  .has_value(),
              "replacing an asset texture must succeed");
        const auto handle_after = engine->assets().texture_handle(*asset);
        const auto info = engine->assets().texture_info(*asset);
        check(handle_before && handle_after && *handle_before == *handle_after, "the renderer handle must not change");
        check(info && info->width == 8 && info->height == 2, "the asset must report the new size");
    }

    // ---- SVG assets: one asset per file, re-rasterised in place when a larger size is asked for ----
    {
        const fs::path directory = fs::temp_directory_path() / "sturdy_asset_texture_progressive_test";
        fs::create_directories(directory);
        const fs::path svg = directory / "square.svg";
        {
            std::ofstream out(svg);
            out << R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect width="24" height="24" fill="#ffffff"/></svg>)";
        }
        auto small = engine->assets().load_svg(svg, 16.0f);
        if (!check(small.has_value(), "an SVG asset must load")) return 1;
        const auto small_handle = engine->assets().texture_handle(*small);
        const auto small_info = engine->assets().texture_info(*small);
        auto large = engine->assets().load_svg(svg, 64.0f);
        check(large && *large == *small, "the same SVG must be the same asset at another size");
        const auto large_handle = engine->assets().texture_handle(*small);
        const auto large_info = engine->assets().texture_info(*small);
        check(small_handle && large_handle && *small_handle == *large_handle, "the renderer handle must survive a re-rasterisation");
        check(small_info && large_info && large_info->width > small_info->width, "a larger request must grow the raster");
        auto smaller = engine->assets().load_svg(svg, 16.0f);
        check(smaller && engine->assets().texture_info(*small)->width == large_info->width,
              "a smaller request must not shrink the raster");
        check(engine->assets().set_svg_size(*small, 16.0f).has_value() && engine->assets().texture_info(*small)->width == small_info->width,
              "set_svg_size must shrink it back exactly");
        fs::remove_all(directory);
    }

    // ---- progressive loads: return at once, finish Complete at the full size on the same handle ----
    {
        static constexpr u8 kProgressiveJxl[] = {
                        0xFF, 0x0A, 0xF8, 0x1F, 0xFC, 0x64, 0x02, 0x0A, 0x28, 0x18, 0x01, 0x40, 0x09, 0x00, 0x34,
                        0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x50, 0x00, 0x50, 0x9A,
                        0x0F, 0xCC, 0x1E, 0x50, 0xC2, 0x95, 0xEC, 0x13, 0xA8, 0xCB, 0x00, 0xC0, 0xA1, 0x38, 0x19,
                        0x43, 0x39, 0x8E, 0x71, 0x43, 0x55, 0x45, 0x87, 0xBD, 0xE8, 0x60, 0x5F, 0x67, 0x14, 0x3E,
                        0x71, 0x26, 0x54, 0x5C, 0x17, 0x5D, 0x7A, 0x48, 0x7D, 0x78, 0xAA, 0x7F, 0x07, 0x0C, 0xC7,
                        0xD6, 0x92, 0x42, 0x3A, 0xBC, 0xAC, 0x51, 0x10, 0x38, 0x93, 0x59, 0x6D, 0xF3, 0x4F, 0x8C,
                        0x9C, 0x9E, 0xB4, 0x81, 0x95, 0x53, 0x09, 0x9F, 0xF6, 0x83, 0x0F, 0xAB, 0x00, 0xF8, 0x6D,
                        0x72, 0x57, 0xF8, 0xB8, 0x6A, 0x10, 0xB5, 0x50, 0xA8, 0x0C, 0xDB, 0x18, 0x6F, 0x0A, 0xBC,
                        0x9F, 0x43, 0xDD, 0x1E, 0xE4, 0x96, 0x2C, 0x1B, 0x4C, 0x01, 0x5A, 0x51, 0x76, 0x10, 0x82,
                        0xA1, 0xC2, 0xD2, 0x42, 0x96, 0xA2, 0xA5, 0x63, 0x84, 0x91, 0xA5, 0xA5, 0x47, 0x63, 0x9C,
                        0xB0, 0xBF, 0x1B, 0x77, 0x77, 0x63, 0x5C, 0xBC, 0x3D, 0x06, 0x4B, 0xC7, 0x60, 0x4F, 0x8E,
                        0xC1, 0x85, 0x8E, 0xC1, 0x55, 0x8E, 0xC1, 0xC2, 0x11, 0x31, 0x58, 0x34, 0x06, 0x85, 0x31,
                        0xC6, 0xC6, 0x18, 0x1D, 0x1D, 0x83, 0x1A, 0x4D, 0x1F, 0x14, 0x83, 0x05, 0x62, 0x70, 0x81,
                        0x63, 0x70, 0x20, 0x06, 0xB3, 0x10, 0x83, 0x13, 0x31, 0xB8, 0x14, 0x31, 0x58, 0x7B, 0x31,
                        0x74, 0x88, 0xAC, 0x0B, 0x3C, 0x3E, 0x46, 0xC5, 0x18, 0xC3, 0x18, 0x63, 0x18, 0x83, 0x09,
                        0x8A, 0xC1, 0x77, 0x86, 0x31, 0x08, 0xB4, 0xB9, 0x18, 0x43, 0xC5, 0x60, 0x5E, 0x18, 0x83,
                        0x8B, 0x31, 0x0E, 0x8C, 0xC1, 0x81, 0x08, 0xDA, 0x7F, 0x8C, 0x31, 0x86, 0x31, 0x8C, 0xC1,
                        0xAE, 0x31, 0xAA, 0x88, 0x08, 0x46, 0xF2, 0x98, 0x80, 0x80, 0xB6, 0x1B, 0x1D, 0x83, 0x0D,
                        0x03, 0xBB, 0x62, 0x6C, 0x8C, 0x31, 0x06, 0x62, 0x70, 0x41, 0xA0, 0x6A, 0x47, 0x40, 0x38,
                        0x01, 0x86, 0x16, 0x07, 0x81, 0xF5, 0x6F, 0xF7, 0xBA, 0xAE, 0xEB, 0x57, 0xFB, 0xBA, 0xAE,
                        0x8C, 0xF2, 0x3F, 0xC3, 0x30, 0x4C, 0xEE, 0x3F, 0xC3, 0x30, 0xEF, 0x3B, 0xB8, 0xFE, 0xBE,
                        0xEF, 0xFB, 0xBE, 0x55, 0x55, 0x89, 0xE0, 0x56, 0x55, 0x55, 0x55, 0x41, 0x10, 0x04, 0x9D,
                        0x5B, 0x41, 0x10, 0x04, 0xF7, 0xDE, 0x66, 0x9A, 0x7B, 0xEF, 0xBD, 0x67, 0x3D, 0x39, 0xE7,
                        0x9C, 0xB9, 0x4C, 0x73, 0xCE, 0x45, 0x91, 0xC2, 0x5D, 0x14, 0x45, 0x01, 0xCE, 0x50, 0x14,
                        0x6A, 0xDF, 0xE6, 0x74, 0x1C, 0x6B, 0x73, 0xA7, 0xAA, 0x02, 0xFC, 0xAA, 0xEA, 0x9C, 0xC9,
                        0x89, 0x73, 0x1E, 0x2A, 0xE0, 0x9F, 0x66, 0x1E, 0xDD, 0x3F, 0x11, 0xD1, 0x03, 0x11, 0x63,
                        0x8C, 0xED, 0x2E, 0xBA, 0xAE, 0x9B, 0x0E, 0xEB, 0xD2, 0x5D, 0xDB, 0xB9, 0x77, 0x45, 0x2B,
                        0x2B, 0x0A, 0x7F, 0xC3, 0xFF, 0xFF, 0xDF, 0xFF, 0x47, 0xFE, 0x33, 0x32, 0xFC, 0xCE, 0x4B,
                        0xC1, 0x98, 0x9C, 0x20, 0x9F, 0xA4, 0x44, 0x14, 0x01, 0x24, 0x02, 0x00, 0x70, 0x02, 0x00,
                        0x1E, 0x00, 0x39, 0x1C, 0x7C, 0xE7, 0x01, 0xFC, 0xBB, 0xE5, 0xF6, 0x1E, 0x46, 0xFD, 0x03,
                        0x0B, 0x4C, 0x04, 0xB6, 0xA4, 0xDB, 0x26, 0xEC, 0xBE, 0x6D, 0x4B, 0xDA, 0xC4, 0xB3, 0x06,
                        0x28, 0x5B, 0xDA, 0xF7, 0xD0, 0xD7, 0xE5, 0x9E, 0xCE, 0xCD, 0x64, 0x76, 0xBD, 0x09, 0x86,
                        0xE0, 0x3A, 0x1E, 0x66, 0x84, 0x25, 0xFE, 0x53, 0xE1, 0x19, 0xBB, 0x2E, 0xFC, 0xFF, 0xB8,
                        0x32, 0xEF, 0x0F, 0xC0, 0x80, 0x23, 0x00, 0xD8, 0xBE, 0x03, 0x24, 0x99, 0x89, 0xEB, 0x0C,
                        0x5C, 0x60, 0x70, 0x1E, 0x4F, 0xBD, 0x4E, 0x69, 0x0A, 0xF1, 0x0B, 0xCA, 0x97, 0x81, 0x66,
                        0xFB, 0xCF, 0xBC, 0x08, 0x76, 0xCA, 0xF7, 0x24, 0x20, 0xF0, 0x38, 0x43, 0x91, 0x25, 0x0C,
                        0x8E, 0xB9, 0x60, 0x55, 0xA4, 0xD1, 0x00, 0xD8, 0xE0, 0x03, 0xA5, 0x60, 0x84, 0xA4, 0x84,
                        0x42, 0x0B, 0xE1, 0xBB, 0x82, 0x05, 0x0B, 0x06, 0x30, 0x01, 0x13, 0x1C, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x01, 0x79, 0x14, 0x00, 0x80, 0x0A, 0x0D, 0x49, 0x02, 0x0A, 0x06, 0x00, 0x00, 0xB0,
                        0x24, 0x91, 0x82, 0xA8, 0x1B, 0xCD, 0xD6, 0x77, 0xBF, 0x02, 0xCA, 0xCA, 0x17, 0xFF, 0x6C,
                        0x0E, 0x1C, 0x74, 0x3A, 0xBD, 0xBD, 0x00, 0x00, 0xD0, 0x67, 0xA7, 0xCF, 0x8E, 0x40, 0x20,
                        0x10, 0x00, 0x00, 0x80, 0x42, 0x84, 0x80, 0x20, 0x29, 0x28, 0x28, 0x90, 0x86, 0x38, 0xF1,
                        0xE3, 0x7F, 0x0C, 0xC6, 0xC5, 0xF9, 0x89, 0x13, 0x63, 0x9C, 0x9D, 0x2D, 0xE0, 0x84, 0xA4,
                        0x1F, 0x00, 0x3F, 0x30, 0x81, 0xB0, 0x5E, 0x80, 0x79, 0x61, 0xEC, 0xB1, 0x2F, 0xE7, 0x43,
                        0x05, 0x5C, 0xD8, 0xF6, 0x01, 0xE8, 0x60, 0x54, 0x46, 0xA1, 0x00, 0x88, 0x61, 0x01, 0x24,
                        0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24,
                        0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x3C, 0xF3, 0xF7, 0x64, 0x0D, 0xD4, 0xF8,
                        0x2B, 0x3D, 0x20, 0xA4, 0x57, 0xD1, 0xA1, 0xD0, 0x94, 0xFD, 0x93, 0x2E, 0x18, 0x4E, 0xB8,
                        0xD2, 0x76, 0x02, 0x51, 0x92, 0xA0, 0x24, 0x41, 0x49, 0x02
                    };
        std::vector<std::byte> encoded(sizeof(kProgressiveJxl));
        std::memcpy(encoded.data(), kProgressiveJxl, sizeof(kProgressiveJxl));
        auto asset = engine->assets().create_texture_progressive_from_bytes(std::move(encoded));
        if (!check(asset.has_value(), "a progressive load must return an asset")) return 1;
        const auto handle_at_start = engine->assets().texture_handle(*asset);
        const auto started = engine->assets().texture_progress(*asset);
        check(started && (started->detail == Engine::TextureDetail::Placeholder || started->detail == Engine::TextureDetail::Partial ||
                          started->detail == Engine::TextureDetail::Complete),
              "a progressive asset must report a detail level straight away");
        check(wait_for(*engine, *asset, Engine::TextureDetail::Complete), "the image must finish decoding");
        const auto done = engine->assets().texture_progress(*asset);
        const auto handle_at_end = engine->assets().texture_handle(*asset);
        check(done && done->width == 512 && done->height == 512 && done->refined_fraction == 1.0f,
              "the finished image must be its full size");
        check(handle_at_start && handle_at_end && *handle_at_start == *handle_at_end,
              "the renderer handle must not change while the image loads");

        // The older streamed entry point is the same pipeline now: a usable asset at once, finished by the engine.
        const fs::path directory = fs::temp_directory_path() / "sturdy_asset_streamed_alias_test";
        fs::create_directories(directory);
        const fs::path file = directory / "flat.jxl";
        {
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char *>(kProgressiveJxl), sizeof(kProgressiveJxl));
        }
        auto streamed = engine->assets().load_texture_streamed(file);
        check(streamed.has_value() && wait_for(*engine, *streamed, Engine::TextureDetail::Complete) &&
                  engine->assets().texture_info(*streamed)->width == 512,
              "a streamed load must finish through the progressive pipeline");
        fs::remove_all(directory);

        std::vector<std::byte> garbage(64, std::byte{7});
        auto bad = engine->assets().create_texture_progressive_from_bytes(std::move(garbage));
        check(bad.has_value() && wait_for(*engine, *bad, Engine::TextureDetail::Complete) == false &&
                  engine->assets().texture_progress(*bad)->detail == Engine::TextureDetail::Failed,
              "an undecodable image must end Failed, not hang");
    }

    if (failures != 0) {
        Foundation::log_error("{} check(s) failed", failures);
        return 1;
    }
    Foundation::log_info("AssetTextureProgressiveGpuTest passed");
    return 0;
}
