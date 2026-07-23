#pragma once

#include <Ecs/src/Entity.hpp>
#include <Ecs/src/Resource.hpp>
#include <Renderer/Scene.hpp>

#include "Asset.hpp"
#include "Camera.hpp"
#include "RenderGraph.hpp"

#include <memory>
#include <optional>
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

    // An always-on debug marker (e.g. a small icosphere) at a light's position — visually distinct
    // from ModelRenderer because it's drawn through a separate, single-color-target forward pass
    // (Shaders/geometry_color.slang) rather than the deferred G-buffer pipeline, so it needs its own
    // component to avoid being submitted into the wrong renderable list.
    struct LightGizmoRenderer {
        Asset model{};
        bool visible = true;
    };

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

        // Gizmos: same shape as the main renderable list above, but a separate list entirely (see
        // LightGizmoRenderer's doc comment on why) — never pooled, since this list is always tiny.
        void submit_gizmo(Ecs::Entity entity, const WorldTransform &transform, const LightGizmoRenderer &renderer) noexcept;
        [[nodiscard]] std::shared_ptr<const RenderableList> finish_gizmo_frame() const noexcept;

      private:
        std::vector<std::shared_ptr<RenderableList>> buffers_;
        std::shared_ptr<RenderableList> current_;
        usize previous_high_watermark_ = 0;
        std::shared_ptr<RenderableList> current_gizmos_;
        AssetManager *assets_ = nullptr;
    };

    // Native game-facing light components. Position/direction are never stored here — they're
    // derived each frame from the entity's WorldTransform (translation, and local -Y rotated into
    // world space for direction), mirroring how ModelRenderer holds no world matrix of its own.
    struct DirectionalLightRenderer {
        glm::vec3 radiance{4.0f, 3.75f, 3.35f};
        f32 angular_radius_degrees = 0.27f;
        bool casts_shadows = true;
    };

    struct SpotLightRenderer {
        glm::vec3 radiance{1.0f};
        f32 range = 10.0f;
        f32 inner_cone_cos = 0.97f;
        f32 outer_cone_cos = 0.90f;
        f32 source_radius = 0.05f;
        bool casts_shadows = true;
    };

    struct PointLightRenderer {
        glm::vec3 radiance{1.0f};
        f32 range = 10.0f;
        f32 source_radius = 0.05f;
        bool casts_shadows = true;
    };

    // CPU-only light extraction target, structurally mirroring RenderFrameRequests. Unlike
    // renderables, light lists are always small (a handful of entities), so this skips
    // RenderFrameRequests' buffer-reuse pool and just allocates a fresh snapshot per frame.
    class LightFrameRequests {
      public:
        struct ExtractedLights {
            std::optional<SFT::Renderer::DirectionalLight> sun;
            std::vector<SFT::Renderer::SpotLight> spot_lights;
            std::vector<SFT::Renderer::PointLight> point_lights;
        };

        void begin_frame();
        void submit(Ecs::Entity entity, const WorldTransform &transform,
                   const DirectionalLightRenderer &light) noexcept;
        void submit(Ecs::Entity entity, const WorldTransform &transform, const SpotLightRenderer &light) noexcept;
        void submit(Ecs::Entity entity, const WorldTransform &transform, const PointLightRenderer &light) noexcept;
        [[nodiscard]] std::shared_ptr<const ExtractedLights> finish_frame() const noexcept;

      private:
        std::shared_ptr<ExtractedLights> current_;
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
        std::vector<SFT::Renderer::CustomPostProcessEffect> custom_post_processes;
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
        std::shared_ptr<const RenderFrameRequests::RenderableList> gizmo_renderables;
        RenderGraphDescription render_graph{};
        std::vector<SFT::Renderer::CustomPostProcessEffect> custom_post_processes;
        u32 visibility_mask = ~0u;
        UString debug_label;
    };

} // namespace SFT::Engine

SFT_ECS_COMPONENT(SFT::Engine::WorldTransform, "sturdy.engine.world_transform");
SFT_ECS_COMPONENT(SFT::Engine::ModelRenderer, "sturdy.engine.model_renderer");
SFT_ECS_COMPONENT(SFT::Engine::LightGizmoRenderer, "sturdy.engine.light_gizmo_renderer");
SFT_ECS_COMPONENT(SFT::Engine::DirectionalLightRenderer, "sturdy.engine.directional_light_renderer");
SFT_ECS_COMPONENT(SFT::Engine::SpotLightRenderer, "sturdy.engine.spot_light_renderer");
SFT_ECS_COMPONENT(SFT::Engine::PointLightRenderer, "sturdy.engine.point_light_renderer");
SFT_ECS_RESOURCE(SFT::Engine::RenderFrameRequests, "sturdy.engine.render_frame_requests");
SFT_ECS_RESOURCE(SFT::Engine::LightFrameRequests, "sturdy.engine.light_frame_requests");
