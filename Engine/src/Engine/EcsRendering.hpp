#pragma once

#include <Ecs/Entity.hpp>
#include <Ecs/Resource.hpp>
#include <Renderer/Scene.hpp>

#include "Asset.hpp"
#include "Camera.hpp"
#include "RenderGraph.hpp"

#include <memory>
#include <vector>

namespace SFT::Engine {

    class AssetManager;

    // Native game-facing render components. They contain opaque assets rather than GPU/RHI handles,
    // so they remain cheap archetype data and are safe to inspect on Async workers.
    struct WorldTransform {
        glm::mat4 value{1.0f};
    };

    struct ModelRenderer {
        Asset model{};
        u32 visibility_mask = ~0u;
        u32 sort_key = 0;
        bool visible = true;
    };

    // Source compatibility for early native consumers; both names describe the same high-level,
    // asset-backed component and neither exposes a GPU handle.
    using MeshRenderer = ModelRenderer;

    // CPU-only extraction target bound to the ECS World. A WriteResource<RenderFrameRequests>
    // system can request draws without receiving Engine, Renderer, RHI, or render-thread-affined
    // state. Schedule serializes mutable-resource chunks, then Engine publishes the completed
    // buffer as an immutable snapshot for the render thread.
    class RenderFrameRequests {
      public:
        using RenderableList = std::vector<SFT::Renderer::SceneRenderable>;

        explicit RenderFrameRequests(AssetManager &assets) noexcept : assets_(&assets) {}

        void begin_frame();
        void submit(Ecs::Entity entity, const WorldTransform &transform, const ModelRenderer &renderer) noexcept;
        [[nodiscard]] std::shared_ptr<const RenderableList> finish_frame() const noexcept;

      private:
        std::vector<std::shared_ptr<RenderableList>> buffers_;
        std::shared_ptr<RenderableList> current_;
        usize previous_high_watermark_ = 0;
        AssetManager *assets_ = nullptr;
    };

    struct SceneLighting {
        glm::vec3 ambient_radiance{0.02f, 0.02f, 0.02f};
        f32 exposure = 1.0f;
    };

    // Consumer-owned per-view policy. Engine supplies no camera, lighting, or labeling
    // behavior of its own; Runtime/game/editor code provides these when requesting a frame.
    struct RenderFrameParameters {
        Camera camera{};
        SceneLighting lighting{};
        RenderGraph render_graph{};
        UString debug_label;
    };

    // Fully owned CPU snapshot. Application builds this before queuing the frame, so ECS may start
    // preparing later frames while the dedicated render thread consumes this one.
    struct PreparedRenderFrame {
        Core::RenderSurfaceHandle surface{};
        Core::FrameInput frame{};
        SFT::Renderer::CameraView camera{};
        SFT::Renderer::SceneLighting lighting{};
        SFT::Renderer::DeferredTargetFormats deferred_formats{};
        std::shared_ptr<const RenderFrameRequests::RenderableList> renderables;
        RenderGraphDescription render_graph{};
        u32 visibility_mask = ~0u;
        UString debug_label;
    };

} // namespace SFT::Engine

SFT_ECS_COMPONENT(SFT::Engine::WorldTransform, "sturdy.engine.world_transform");
SFT_ECS_COMPONENT(SFT::Engine::ModelRenderer, "sturdy.engine.model_renderer");
SFT_ECS_RESOURCE(SFT::Engine::RenderFrameRequests, "sturdy.engine.render_frame_requests");
