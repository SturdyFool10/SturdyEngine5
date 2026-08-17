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


    struct WorldTransform {
        glm::mat4 value{1.0f};
    };

    struct ModelRenderer {
        Asset model{};
        u32 visibility_mask = ~0u;
        u32 sort_key = 0;
        bool visible = true;
    };


    using MeshRenderer = ModelRenderer;


    struct LightGizmoRenderer {
        Asset model{};
        bool visible = true;
    };


    class RenderFrameRequests {
      public:
        using RenderableList = std::vector<SFT::Renderer::SceneRenderable>;

        /// Constructs a `RenderFrameRequests` from the supplied initialization values.
        ///
        /// @param assets `assets` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit RenderFrameRequests(AssetManager &assets) noexcept;

        /// Performs the begin frame operation for `RenderFrameRequests` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void begin_frame();
        /// Submits the requested work.
        ///
        /// @param entity Entity used or affected by the operation.
        /// @param transform `transform` value used by the operation.
        /// @param renderer Renderer used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void submit(Ecs::Entity entity, const WorldTransform &transform, const ModelRenderer &renderer) noexcept;
        /// Returns the current or globally available finish frame value.
        ///
        /// @return Returns the current finish frame value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::shared_ptr<const RenderableList> finish_frame() const noexcept;


        /// Submits gizmo.
        ///
        /// @param entity Entity used or affected by the operation.
        /// @param transform `transform` value used by the operation.
        /// @param renderer Renderer used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void submit_gizmo(Ecs::Entity entity, const WorldTransform &transform, const LightGizmoRenderer &renderer) noexcept;
        /// Returns the current or globally available finish gizmo frame value.
        ///
        /// @return Returns the current finish gizmo frame value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::shared_ptr<const RenderableList> finish_gizmo_frame() const noexcept;

      private:
        std::vector<std::shared_ptr<RenderableList>> buffers_;
        std::shared_ptr<RenderableList> current_;
        usize previous_high_watermark_ = 0;
        std::shared_ptr<RenderableList> current_gizmos_;
        AssetManager *assets_ = nullptr;
    };


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


    class LightFrameRequests {
      public:
        struct ExtractedLights {
            std::optional<SFT::Renderer::DirectionalLight> sun;
            std::vector<SFT::Renderer::SpotLight> spot_lights;
            std::vector<SFT::Renderer::PointLight> point_lights;
        };

        /// Performs the begin frame operation for `LightFrameRequests` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void begin_frame();
        /// Submits the requested work.
        ///
        /// @param entity Entity used or affected by the operation.
        /// @param transform `transform` value used by the operation.
        /// @param light `light` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void submit(Ecs::Entity entity, const WorldTransform &transform,
                   const DirectionalLightRenderer &light) noexcept;
        /// Submits the requested work.
        ///
        /// @param entity Entity used or affected by the operation.
        /// @param transform `transform` value used by the operation.
        /// @param light `light` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void submit(Ecs::Entity entity, const WorldTransform &transform, const SpotLightRenderer &light) noexcept;
        /// Submits the requested work.
        ///
        /// @param entity Entity used or affected by the operation.
        /// @param transform `transform` value used by the operation.
        /// @param light `light` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void submit(Ecs::Entity entity, const WorldTransform &transform, const PointLightRenderer &light) noexcept;
        /// Returns the current or globally available finish frame value.
        ///
        /// @return Returns the current finish frame value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::shared_ptr<const ExtractedLights> finish_frame() const noexcept;

      private:
        std::shared_ptr<ExtractedLights> current_;
    };

    struct SceneLighting {
        glm::vec3 ambient_radiance{0.02f, 0.02f, 0.02f};
        f32 exposure = 1.0f;
    };


    struct RenderFrameParameters {
        Camera camera{};
        SceneLighting lighting{};
        RenderGraph render_graph{};


        SFT::Renderer::UiOverlayHooks ui_overlay;
        UString debug_label;
    };


    struct PreparedRenderFrame {
        Core::RenderSurfaceHandle surface{};
        Core::FrameInput frame{};
        SFT::Renderer::CameraView camera{};
        SFT::Renderer::SceneLighting lighting{};
        SFT::Renderer::DeferredTargetFormats deferred_formats{};
        std::shared_ptr<const RenderFrameRequests::RenderableList> renderables;
        std::shared_ptr<const RenderFrameRequests::RenderableList> gizmo_renderables;
        RenderGraph render_graph{};
        SFT::Renderer::UiOverlayHooks ui_overlay;
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
