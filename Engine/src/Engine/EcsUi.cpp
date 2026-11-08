#include <Engine/EcsUi.hpp>

#include <algorithm>
#include <cmath>


namespace SFT::Engine {

    /// Resolves the requested value into the concrete value used by the engine.
    ///
    /// @param assets `assets` value used by the operation.
    /// @param path Filesystem path identifying the target resource.
    /// @param color_space `color_space` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    AssetExpected<Renderer::TextureHandle> UiImageCache::resolve(
        AssetManager &assets, const std::filesystem::path &path, TextureColorSpace color_space) {
        const std::string key = path.string() + (color_space == TextureColorSpace::Linear ? "|L" : "|S");
        if (auto cached = by_key_.find(key); cached != by_key_.end()) {
            return cached->second.handle;
        }
        // Never blocks: the handle is valid at once, shows nothing until the first pass of the image is decoded and then
        // sharpens by itself (`AssetManager::load_texture_progressive`).
        auto asset = assets.load_texture_progressive(path, color_space);
        if (!asset) {
            return std::unexpected(asset.error());
        }
        auto handle = assets.texture_handle(*asset);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        by_key_.emplace(key, Entry{.asset = *asset, .handle = *handle});
        return *handle;
    }

    void UiImageCache::clear() noexcept { by_key_.clear(); }

    AssetExpected<Renderer::TextureHandle> UiSvgCache::resolve(
        AssetManager &assets, const std::filesystem::path &path, f32 target_px) {
        // The same key the asset manager rounds to, so two requests it would treat alike cost one lookup here.
        const u32 rounded = static_cast<u32>(std::ceil(std::clamp(target_px, 4.0f, 4096.0f) / 4.0f)) * 4u;
        const std::string key = path.string() + "|" + std::to_string(rounded);
        if (auto cached = by_key_.find(key); cached != by_key_.end()) {
            return cached->second;
        }
        // Creates the asset, or grows its raster when this is the biggest size asked for so far.
        auto asset = assets.load_svg(path, target_px);
        if (!asset) {
            return std::unexpected(asset.error());
        }
        Entry &entry = by_path_[path.string()];
        if (entry.epoch != epoch_) {
            // First request since sizes were reset: take exactly this size, shrinking the raster if it is bigger.
            entry.epoch = epoch_;
            if (auto sized = assets.set_svg_size(*asset, target_px); !sized) {
                return std::unexpected(sized.error());
            }
        }
        entry.asset = *asset;
        auto handle = assets.texture_handle(*asset);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        by_key_.emplace(key, *handle);
        return *handle;
    }

    AssetExpected<Renderer::TextureHandle> UiSvgCache::resolve_data(
        AssetManager &assets, std::string_view name, std::string_view svg, f32 target_px) {
        // The same key the asset manager rounds to, so two requests it would treat alike cost one lookup here.
        const u32 rounded = static_cast<u32>(std::ceil(std::clamp(target_px, 4.0f, 4096.0f) / 4.0f)) * 4u;
        const std::string key = "data|" + std::string{name} + "|" + std::to_string(rounded);
        if (auto cached = by_key_.find(key); cached != by_key_.end()) {
            return cached->second;
        }
        // Creates the asset, or grows its raster when this is the biggest size asked for so far.
        auto asset = assets.load_svg_data(name, svg, target_px);
        if (!asset) {
            return std::unexpected(asset.error());
        }
        Entry &entry = by_path_["data|" + std::string{name}];
        if (entry.epoch != epoch_) {
            // First request since sizes were reset: take exactly this size, shrinking the raster if it is bigger.
            entry.epoch = epoch_;
            if (auto sized = assets.set_svg_size(*asset, target_px); !sized) {
                return std::unexpected(sized.error());
            }
        }
        entry.asset = *asset;
        auto handle = assets.texture_handle(*asset);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        by_key_.emplace(key, *handle);
        return *handle;
    }

    void UiSvgCache::clear() noexcept {
        by_key_.clear();
        by_path_.clear();
    }

    void UiSvgCache::clear_and_unload(AssetManager &) noexcept {
        ++epoch_;
        by_key_.clear();
    }

} // namespace SFT::Engine

