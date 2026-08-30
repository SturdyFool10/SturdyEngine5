#include <Engine/EcsUi.hpp>


namespace SFT::Engine {

    /// Sets the position for this `Engine`.
    ///
    /// @param position `position` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void UiPointerState::set_position(glm::vec2 position) noexcept { state_.position = position; }

    /// Sets the down for this `Engine`.
    ///
    /// @param down `down` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Cancels the outstanding operation when cancellation is still possible.
    ///
    /// @return Returns the current cancel value.
    /// @note This function does not throw exceptions.
    void UiPointerState::cancel() noexcept {
        state_.cancelled = true;
        state_.down = false;
    }

    /// Clears transitions.
    ///
    /// @return Returns the current clear transitions value.
    /// @note This function does not throw exceptions.
    void UiPointerState::clear_transitions() noexcept {
        state_.pressed = false;
        state_.press_position.reset();
        state_.released = false;
        state_.cancelled = false;
    }

    /// Adds scroll delta using the supplied arguments and current state.
    ///
    /// @param delta `delta` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void UiPointerState::add_scroll_delta(glm::vec2 delta) noexcept { state_.scroll_delta += delta; }

    /// Clears scroll delta.
    ///
    /// @return Returns the current clear scroll delta value.
    /// @note This function does not throw exceptions.
    void UiPointerState::clear_scroll_delta() noexcept { state_.scroll_delta = glm::vec2{0.0f}; }

    /// Returns the current or globally available state value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const UI::PointerState &UiPointerState::state() const noexcept { return state_; }

    /// Sets the consumed for this `Engine`.
    ///
    /// @param consumed `consumed` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void UiPointerState::set_consumed(bool consumed) noexcept { consumed_ = consumed; }

    /// Returns the current or globally available consumed value.
    ///
    /// @return Returns the current consumed value.
    /// @note This function does not throw exceptions.
    bool UiPointerState::consumed() const noexcept { return consumed_; }

    /// Finds or creates the ready required by the operation.
    ///
    /// @param device Device used or affected by the operation.
    /// @param color_format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
            Foundation::log_error("Engine::UiContext: failed to create UI::Context: {}",
                                  context.error().message);
            return false;
        }
        context_ = std::move(*context);

        auto renderer = UI::UiRenderer::create(device, color_format);
        if (!renderer) {
            // Carry the backend's reason: without it this reports only that UI is unavailable, and
            // the actual cause — a pipeline or format the device rejected — is discarded at the one
            // point where it was still known.
            Foundation::log_error("Engine::UiContext: failed to create UI::UiRenderer: {}",
                                  renderer.error().message);
            context_.destroy();
            return false;
        }
        {
            auto guard = renderer_state_->renderer.lock();
            *guard = std::move(*renderer);
        }
        return true;
    }

    /// Reads the requested data from the associated source.
    ///
    /// @return Returns the current ready value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool UiContext::ready() const {
        auto guard = renderer_state_->renderer.lock();
        return guard->has_value();
    }

    /// Returns the current or globally available context value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    UI::Context &UiContext::context() noexcept { return context_; }

    /// Performs the begin layout operation for `Engine` using the supplied arguments.
    ///
    /// @param viewport_size Requested or available size for the operation.
    /// @param pointer_state `pointer_state` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void UiContext::begin_layout(glm::vec2 viewport_size, UiPointerState &pointer_state, f32 delta_seconds) {
        context_.begin_layout(viewport_size, pointer_state.state(), delta_seconds);
        pointer_state.set_consumed(context_.pointer_over_any() || context_.pointer_captured());
        pointer_state.clear_scroll_delta();
        pointer_state.clear_transitions();
    }

    /// Builds overlay hooks.
    ///
    /// @param snapshot `snapshot` value used by the operation.
    /// @param texture_resolver Texture used or affected by the operation.
    ///
    /// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
                            RHI::RhiDevice &device, RHI::CommandEncoder &encoder, Renderer::RenderGraph &render_graph,
                            glm::vec2 viewport_size, Core::RenderSurfaceHandle surface, u32 frame_resource_index,
                            std::vector<RHI::BufferHandle> &transient_buffers,
                            Renderer::TextAtlasRetiredResources &retired_atlas_resources,
                            std::vector<RHI::BindGroupHandle> &transient_bind_groups,
                            std::vector<Renderer::RenderGraphTextureHandle> &glow_bloom_outputs) -> Core::RendererResult {
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
                device, encoder, render_graph, *snapshot, texture_resolver, surface, frame_resource_index,
                transient_buffers, retired_atlas_resources, transient_bind_groups, glow_bloom_outputs);
        };
        hooks.draw = [renderer_state](RHI::RenderPassEncoder &pass, glm::vec2 viewport_size,
                                      Core::RenderSurfaceHandle surface, u32 frame_resource_index) -> Core::RendererResult {
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

    /// Destroys or releases the `Engine` resource represented by the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Clears the stored state or contents.
    ///
    /// @return Returns the current clear value.
    /// @note This function does not throw exceptions.
    void UiSvgCache::clear() noexcept { by_key_.clear(); }

} // namespace SFT::Engine

