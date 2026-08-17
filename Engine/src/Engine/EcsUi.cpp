#include <Engine/src/Engine/EcsUi.hpp>


namespace SFT::Engine {

    void UiPointerState::set_position(glm::vec2 position) noexcept { state_.position = position; }

    void UiPointerState::set_down(bool down) noexcept {
        if (down != state_.down) {
            state_.pressed = state_.pressed || down;
            state_.released = state_.released || !down;
            if (down) {
                state_.press_position = state_.position;
            }
        }
        state_.down = down;
    }

    void UiPointerState::cancel() noexcept {
        state_.cancelled = true;
        state_.down = false;
    }

    void UiPointerState::clear_transitions() noexcept {
        state_.pressed = false;
        state_.press_position.reset();
        state_.released = false;
        state_.cancelled = false;
    }

    void UiPointerState::add_scroll_delta(glm::vec2 delta) noexcept { state_.scroll_delta += delta; }

    void UiPointerState::clear_scroll_delta() noexcept { state_.scroll_delta = glm::vec2{0.0f}; }

    const UI::PointerState &UiPointerState::state() const noexcept { return state_; }

    void UiPointerState::set_consumed(bool consumed) noexcept { consumed_ = consumed; }

    bool UiPointerState::consumed() const noexcept { return consumed_; }

    bool UiContext::ensure_ready(RHI::RhiDevice &device, RHI::Format color_format) {
        {
            auto guard = renderer_state_->renderer.lock();
            if (guard->has_value()) {
                return true;
            }
        }
        if (create_attempted_) {
            return false;
        }
        create_attempted_ = true;

        auto context = UI::Context::create(UI::Context::Config{});
        if (!context) {
            Foundation::log_error("Engine::UiContext: failed to create UI::Context.");
            return false;
        }
        context_ = std::move(*context);

        auto renderer = UI::UiRenderer::create(device, color_format);
        if (!renderer) {
            Foundation::log_error("Engine::UiContext: failed to create UI::UiRenderer.");
            context_.destroy();
            return false;
        }
        {
            auto guard = renderer_state_->renderer.lock();
            *guard = std::move(*renderer);
        }
        return true;
    }

    bool UiContext::ready() const {
        auto guard = renderer_state_->renderer.lock();
        return guard->has_value();
    }

    UI::Context &UiContext::context() noexcept { return context_; }

    void UiContext::begin_layout(glm::vec2 viewport_size, UiPointerState &pointer_state, f32 delta_seconds) {
        context_.begin_layout(viewport_size, pointer_state.state(), delta_seconds);
        pointer_state.set_consumed(context_.pointer_over_any() || context_.pointer_captured());
        pointer_state.clear_scroll_delta();
        pointer_state.clear_transitions();
    }

    Renderer::UiOverlayHooks UiContext::build_overlay_hooks(std::shared_ptr<UI::FrameSnapshot> snapshot,
                                                                Renderer::Renderer *texture_resolver) {
        Renderer::UiOverlayHooks hooks;
        const std::shared_ptr<UiRendererState> renderer_state = renderer_state_;
        {
            auto guard = renderer_state->renderer.lock();
            if (!guard->has_value() || !snapshot) {
                return hooks;
            }
        }
        hooks.prepare = [renderer_state, snapshot, texture_resolver](
                            RHI::RhiDevice &device, RHI::CommandEncoder &encoder, glm::vec2 viewport_size,
                            Core::RenderSurfaceHandle surface, u32 frame_resource_index,
                            std::vector<RHI::BufferHandle> &transient_buffers,
                            Renderer::TextAtlasRetiredResources &retired_atlas_resources) -> Core::RendererResult {
            const Core::Extent2D layout_extent = snapshot->viewport_extent();
            if (viewport_size != glm::vec2{layout_extent}) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "UI snapshot extent does not match the selected render endpoint; call begin_layout() "
                    "with the off-screen target's absolute extent.");
            }
            auto guard = renderer_state->renderer.lock();
            if (!*guard) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "UI renderer was destroyed before overlay preparation.");
            }
            return (*guard)->prepare(
                device, encoder, *snapshot, texture_resolver, surface, frame_resource_index,
                transient_buffers, retired_atlas_resources);
        };
        hooks.draw = [renderer_state](RHI::RenderPassEncoder &pass, glm::vec2 viewport_size,
                                      Core::RenderSurfaceHandle surface,
                                      u32 frame_resource_index) -> Core::RendererResult {
            auto guard = renderer_state->renderer.lock();
            if (!*guard) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "UI renderer was destroyed before overlay drawing.");
            }
            return (*guard)->draw(pass, viewport_size, surface, frame_resource_index);
        };
        return hooks;
    }

    void UiContext::destroy(RHI::RhiDevice &device) noexcept {
        {
            auto guard = renderer_state_->renderer.lock();
            if (*guard) {
                (*guard)->destroy(device);
                guard->reset();
            }
        }
        context_.destroy();
        create_attempted_ = false;
    }

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

    void UiImageCache::clear() noexcept { by_key_.clear(); }

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
            .rgba8 = std::move(rasterized->rgba),
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

    void UiSvgCache::clear() noexcept { by_key_.clear(); }

} // namespace SFT::Engine

