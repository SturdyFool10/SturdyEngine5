#include <Engine/EcsUi.hpp>


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
        AssetManager &assets, const std::filesystem::path &path,
        TextureColorSpace color_space) {
        const std::string key = path.string() + (color_space == TextureColorSpace::Linear ? "|L" : "|S");
        if (auto cached = by_key_.find(key); cached != by_key_.end()) {
            return cached->second.handle;
        }
        auto asset = assets.load_texture(path, color_space);
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

    /// Clears the stored state or contents.
    ///
    /// @return Returns the current clear value.
    /// @note This function does not throw exceptions.
    void UiImageCache::clear() noexcept { by_key_.clear(); }

    /// Resolves the requested value into the concrete value used by the engine.
    ///
    /// @param assets `assets` value used by the operation.
    /// @param path Filesystem path identifying the target resource.
    /// @param target_px `target_px` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::DecodeFailure`.
    AssetExpected<Renderer::TextureHandle> UiSvgCache::resolve(
        AssetManager &assets, const std::filesystem::path &path, f32 target_px) {
        const std::string key = path.string() + "|" + std::to_string(target_px);
        if (auto cached = by_key_.find(key); cached != by_key_.end()) {
            return cached->second;
        }

        std::optional<UI::Svg::RasterizedSvg> rasterized = UI::Svg::rasterize_svg_file(path, target_px);
        if (!rasterized) {
            return std::unexpected(AssetError{
                .code = AssetErrorCode::DecodeFailure,
                .message = UString{"Failed to load/rasterize SVG."_ustr},
                .source = path,
            });
        }

        auto asset = assets.create_texture(TextureAssetDesc{
            .width = rasterized->width,
            .height = rasterized->height,


            .color_space = TextureColorSpace::Srgb,
            .pixels = std::move(rasterized->rgba),
            .label = UString{"ui svg icon"_ustr},
        });
        if (!asset) {
            return std::unexpected(asset.error());
        }
        auto handle = assets.texture_handle(*asset);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        by_key_.emplace(key, *handle);
        return *handle;
    }

    /// Clears the stored state or contents.
    ///
    /// @return Returns the current clear value.
    /// @note This function does not throw exceptions.
    void UiSvgCache::clear() noexcept { by_key_.clear(); }

} // namespace SFT::Engine

