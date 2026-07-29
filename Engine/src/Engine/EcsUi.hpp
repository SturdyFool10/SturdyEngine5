#pragma once

#include <Foundation/src/Foundation.hpp>

#include "AssetManager.hpp"

#include <Core/Core.hpp>
#include <Ecs/src/Resource.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Renderer.hpp>
#include <UI/UI.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Engine-level ECS integration for Sturdy.UI: UI itself stays Ecs-agnostic (see UI.hpp's own doc
// comment), so this is the seam that lets an ECS app build UI trees through ordinary systems
// instead of hand-rolling the pointer-input plumbing and prepare()/draw() glue every consumer
// would otherwise duplicate — see plans/clay-ui-renderer.md's Phase 2 status note.
namespace SFT::Engine {

    // Accumulated primary-pointer state (position + primary-button-down), kept current by a
    // built-in system Engine registers in its own constructor (folding MouseMoveEvent/
    // MouseButtonEvent, the same streams RuntimeClient's own FlyCameraState reads). This is the
    // minimal slice of the still-unbuilt InputState (plans/ecs-engine-subsystem-access.md) that
    // UI hit-testing actually needs — not a general input-state resource. Read via
    // Ecs::ReadResource<UiPointerState>; hand `.state()` straight to UI::Context::begin_layout().
    class UiPointerState {
      public:
        void set_position(glm::vec2 position) noexcept { state_.position = position; }
        void set_down(bool down) noexcept { state_.down = down; }
        // MouseWheelEvent is a delta, not a position, so this accumulates (multiple wheel events can
        // land in one frame) rather than overwriting — cleared by UiContext::begin_layout() below
        // once it's been folded into Clay's scroll tracking, the same one-shot lifetime as an
        // ordinary Events<T> read.
        void add_scroll_delta(glm::vec2 delta) noexcept { state_.scroll_delta += delta; }
        void clear_scroll_delta() noexcept { state_.scroll_delta = glm::vec2{0.0f}; }
        [[nodiscard]] const UI::PointerState &state() const noexcept { return state_; }

        // Whether this frame's pointer position landed on any UI element, per
        // UI::Context::pointer_over_any() — set by UiContext::begin_layout() below once the UI tree
        // that owns this pointer has actually hit-tested it. Gameplay systems (fly-camera look,
        // click-to-interact, etc.) should check this before consuming raw mouse input, the same
        // input-precedence contract plans/clay-ui-renderer.md flags as previously missing. One-frame
        // stale like hovered()/clicked() themselves: it reflects whichever begin_layout() call ran
        // last, which for a typical app runs once per frame after gameplay input systems already
        // consumed this frame's raw events — see that ordering's own note in EngineImpl.cpp.
        void set_consumed(bool consumed) noexcept { consumed_ = consumed; }
        [[nodiscard]] bool consumed() const noexcept { return consumed_; }

      private:
        UI::PointerState state_{};
        bool consumed_ = false;
    };

    // Owns one UI::Context + UI::UiRenderer pair as an ordinary World resource
    // (Ecs::WriteResource<UiContext>), so any system with resource access can build/query a UI
    // tree — not just whichever code happens to own the Engine instance. Font registration stays
    // the caller's job (font choice/discovery is app policy, not Engine policy); this only owns
    // the two GPU-backed objects every UI consumer needs regardless of what it draws.
    class UiContext {
        struct UiRendererState {
            std::mutex mutex;
            std::optional<UI::UiRenderer> renderer;
        };

      public:
        // Lazily creates the UI::Context/UI::UiRenderer pair against `device`/`color_format`.
        // Needs a live RHI device, which isn't necessarily up before the first frame — safe to
        // call every frame; a cheap no-op once ready() (or once creation has already failed once,
        // to avoid retrying a hard failure every single frame).
        [[nodiscard]] bool ensure_ready(RHI::RhiDevice &device, RHI::Format color_format) {
            {
                const std::lock_guard lock{renderer_state_->mutex};
                if (renderer_state_->renderer.has_value()) {
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
                const std::lock_guard lock{renderer_state_->mutex};
                renderer_state_->renderer = std::move(*renderer);
            }
            return true;
        }

        [[nodiscard]] bool ready() const {
            const std::lock_guard lock{renderer_state_->mutex};
            return renderer_state_->renderer.has_value();
        }
        [[nodiscard]] UI::Context &context() noexcept { return context_; }

        // Thin wrapper over UI::Context::begin_layout() that also keeps `pointer_state`'s consumed
        // flag and scroll accumulator in sync — folding both into this one call (rather than leaving
        // callers to remember them, the way the raw two-arg Context::begin_layout() would) is what
        // makes the input-precedence contract structural instead of convention-based: any system that
        // builds a UI frame through here automatically updates what gameplay input-precedence checks
        // read. Callers should prefer this over calling context().begin_layout() directly whenever
        // `pointer_state` came from Engine's own UiPointerState resource.
        void begin_layout(glm::vec2 viewport_size, UiPointerState &pointer_state, f32 delta_seconds) {
            context_.begin_layout(viewport_size, pointer_state.state(), delta_seconds);
            pointer_state.set_consumed(context_.pointer_over_any());
            pointer_state.clear_scroll_delta();
        }

        // Packages an already-finished FrameSnapshot (UI::Context::finish_frame()'s result) into
        // the RenderGraph's UiOverlayHooks seam (Renderer::Scene.hpp) — the exact prepare()/draw()
        // glue every consumer of this UI package would otherwise write by hand. `snapshot` is
        // shared (not moved) since the returned hooks' `prepare`/`draw` closures both need to
        // outlive this call, holding it alive until the frame graph actually runs them.
        [[nodiscard]] Renderer::UiOverlayHooks build_overlay_hooks(std::shared_ptr<UI::FrameSnapshot> snapshot,
                                                                    Renderer::Renderer *texture_resolver) {
            Renderer::UiOverlayHooks hooks;
            const std::shared_ptr<UiRendererState> renderer_state = renderer_state_;
            {
                const std::lock_guard lock{renderer_state->mutex};
                if (!renderer_state->renderer.has_value() || !snapshot) {
                    return hooks;
                }
            }
            hooks.prepare = [renderer_state, snapshot, texture_resolver](
                                RHI::RhiDevice &device, RHI::CommandEncoder &encoder, glm::vec2 viewport_size,
                                std::vector<RHI::BufferHandle> &transient_buffers,
                                Renderer::TextAtlasRetiredResources &retired_atlas_resources) -> Core::RendererResult {
                const Core::Extent2D layout_extent = snapshot->viewport_extent();
                if (viewport_size.x != static_cast<f32>(layout_extent.width) ||
                    viewport_size.y != static_cast<f32>(layout_extent.height)) {
                    return Core::graphics_backend_error(
                        Core::GraphicsBackendErrorCode::OperationFailed,
                        "UI snapshot extent does not match the selected render endpoint; call begin_layout() "
                        "with the off-screen target's absolute extent.");
                }
                const std::lock_guard lock{renderer_state->mutex};
                if (!renderer_state->renderer) {
                    return Core::graphics_backend_error(
                        Core::GraphicsBackendErrorCode::OperationFailed,
                        "UI renderer was destroyed before overlay preparation.");
                }
                return renderer_state->renderer->prepare(
                    device, encoder, *snapshot, texture_resolver, transient_buffers,
                    retired_atlas_resources);
            };
            hooks.draw = [renderer_state](RHI::RenderPassEncoder &pass,
                                          glm::vec2 viewport_size) -> Core::RendererResult {
                const std::lock_guard lock{renderer_state->mutex};
                if (!renderer_state->renderer) {
                    return Core::graphics_backend_error(
                        Core::GraphicsBackendErrorCode::OperationFailed,
                        "UI renderer was destroyed before overlay drawing.");
                }
                return renderer_state->renderer->draw(pass, viewport_size);
            };
            return hooks;
        }

        void destroy(RHI::RhiDevice &device) noexcept {
            {
                const std::lock_guard lock{renderer_state_->mutex};
                if (renderer_state_->renderer) {
                    renderer_state_->renderer->destroy(device);
                    renderer_state_->renderer.reset();
                }
            }
            context_.destroy();
            create_attempted_ = false;
        }

      private:
        UI::Context context_{};
        std::shared_ptr<UiRendererState> renderer_state_ = std::make_shared<UiRendererState>();
        bool create_attempted_ = false;
    };

    // UI-facing image cache: UI::Context::image() (UI/) needs a raw Renderer::TextureHandle,
    // resolved fresh every frame an app declares an image element — since Sturdy.UI is immediate-
    // mode and rebuilds its whole tree every frame, calling AssetManager::load_texture() directly
    // from a UI-building system would decode and re-upload the same file from disk on every single
    // frame. This cache makes a repeat resolve() call for the same (path, color_space) an O(1)
    // lookup instead of a decode — the concrete "large UI tree" cost this exists to avoid.
    //
    // Synchronous: a large image's first resolve() call still blocks the calling frame on disk I/O +
    // decode. An async/streamed path (matching plans/texture-streaming research's direction) isn't
    // built here since no concrete need for it has come up yet.
    class UiImageCache {
      public:
        [[nodiscard]] AssetExpected<Renderer::TextureHandle> resolve(
            AssetManager &assets, const std::filesystem::path &path,
            TextureColorSpace color_space = TextureColorSpace::Srgb) {
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

        // Drops every cached entry without unloading the underlying AssetManager assets — callers
        // that also own the AssetManager decide asset lifetime themselves; this only forgets the
        // path->handle mapping (e.g. useful for a hot-reload/asset-changed-on-disk workflow).
        void clear() noexcept { by_key_.clear(); }

      private:
        struct Entry {
            Asset asset{};
            Renderer::TextureHandle handle{};
        };
        std::unordered_map<std::string, Entry> by_key_;
    };

    // UI-facing SVG icon cache: loads+rasterizes a source file once per distinct (path, target_px)
    // tuple (UI::Svg::rasterize_svg_file() — a thin wrapper over the vendored lunasvg library, see
    // its own doc comment for why this package no longer hand-rolls SVG parsing) and uploads the
    // result once — repeat resolve() calls are an O(1) lookup, the same "immediate-mode UI rebuilds
    // its tree every frame" reasoning UiImageCache documents. Deliberately shaped identically to
    // UiImageCache::resolve() (AssetManager-backed, same AssetExpected<TextureHandle> return) now
    // that a rasterized SVG is just another RGBA8 bitmap asset, not a special SDF case.
    class UiSvgCache {
      public:
        [[nodiscard]] AssetExpected<Renderer::TextureHandle> resolve(
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
                // SVG fill/stop-color values are sRGB-encoded, same as a PNG icon asset — matches
                // UiImageCache::resolve()'s own default color space.
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

        // Drops every cached path->handle entry — same non-destructive contract as
        // UiImageCache::clear() (the underlying AssetManager asset isn't unloaded here).
        void clear() noexcept { by_key_.clear(); }

      private:
        std::unordered_map<std::string, Renderer::TextureHandle> by_key_;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::UiPointerState, "sturdy.engine.ui_pointer_state");
SFT_ECS_RESOURCE(SFT::Engine::UiContext, "sturdy.engine.ui_context");
SFT_ECS_RESOURCE(SFT::Engine::UiImageCache, "sturdy.engine.ui_image_cache");
SFT_ECS_RESOURCE(SFT::Engine::UiSvgCache, "sturdy.engine.ui_svg_cache");
