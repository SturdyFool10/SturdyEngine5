#include <Engine/EcsRendering.hpp>
#include <Engine/AssetManager.hpp>

#include <algorithm>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>

namespace SFT::Engine {

    namespace {


        /// Performs the world direction operation for `Engine` using the supplied arguments.
        ///
        /// @param transform `transform` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec3 world_direction(const WorldTransform &transform) noexcept {
            return glm::normalize(glm::mat3{transform.value} * glm::vec3{0.0f, -1.0f, 0.0f});
        }

        /// Performs the world position operation for `Engine` using the supplied arguments.
        ///
        /// @param transform `transform` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec3 world_position(const WorldTransform &transform) noexcept {
            return glm::vec3{transform.value[3]};
        }
    } // namespace

    /// Returns the current or globally available begin frame value.
    ///
    /// @return Returns the current begin frame value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void RenderFrameRequests::begin_frame() {
        previous_high_watermark_ = std::max(previous_high_watermark_, current_ ? current_->size() : usize{0});
        current_.reset();

        for (const auto &candidate : buffers_) {
            if (candidate.use_count() == 1) {
                current_ = candidate;
                break;
            }
        }
        if (!current_) {
            current_ = std::make_shared<RenderableList>();
            buffers_.push_back(current_);
        }

        current_->clear();
        if (current_->capacity() < previous_high_watermark_) {
            current_->reserve(previous_high_watermark_);
        }

        current_gizmos_ = std::make_shared<RenderableList>();
    }

    /// Submits the requested work.
    ///
    /// @param entity Entity used or affected by the operation.
    /// @param transform `transform` value used by the operation.
    /// @param renderer Renderer used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void RenderFrameRequests::submit(Ecs::Entity entity,
                                     const WorldTransform &transform,
                                     const ModelRenderer &renderer) noexcept {
        if (!renderer.visible || !renderer.model) {
            return;
        }
        if (!current_) {
            Ecs::Detail::contract_violation(
                "RenderFrameRequests::submit() requires Engine::prepare_render_frame() to begin extraction first.");
        }

        const u64 stable_id = (static_cast<u64>(entity.generation) << 32u) | entity.index;
        (void)assets_->append_model_renderables(renderer.model, transform.value, stable_id,
                                                renderer.visibility_mask, renderer.sort_key, *current_);
    }

    /// Returns the current or globally available finish frame value.
    ///
    /// @return Returns the current finish frame value.
    /// @note This function does not throw exceptions.
    std::shared_ptr<const RenderFrameRequests::RenderableList> RenderFrameRequests::finish_frame() const noexcept {
        if (!current_) {
            Ecs::Detail::contract_violation(
                "RenderFrameRequests::finish_frame() called before begin_frame().");
        }
        return current_;
    }

    /// Submits gizmo.
    ///
    /// @param entity Entity used or affected by the operation.
    /// @param transform `transform` value used by the operation.
    /// @param renderer Renderer used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void RenderFrameRequests::submit_gizmo(Ecs::Entity entity, const WorldTransform &transform,
                                           const LightGizmoRenderer &renderer) noexcept {
        if (!renderer.visible || !renderer.model) {
            return;
        }
        if (!current_gizmos_) {
            Ecs::Detail::contract_violation(
                "RenderFrameRequests::submit_gizmo() requires Engine::prepare_render_frame() to begin extraction first.");
        }

        const u64 stable_id = (static_cast<u64>(entity.generation) << 32u) | entity.index;
        (void)assets_->append_model_renderables(renderer.model, transform.value, stable_id,
                                                ~0u, 0u, *current_gizmos_);
    }

    /// Returns the current or globally available finish gizmo frame value.
    ///
    /// @return Returns the current finish gizmo frame value.
    /// @note This function does not throw exceptions.
    std::shared_ptr<const RenderFrameRequests::RenderableList> RenderFrameRequests::finish_gizmo_frame() const noexcept {
        if (!current_gizmos_) {
            Ecs::Detail::contract_violation(
                "RenderFrameRequests::finish_gizmo_frame() called before begin_frame().");
        }
        return current_gizmos_;
    }

    /// Returns the current or globally available begin frame value.
    ///
    /// @return Returns the current begin frame value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void LightFrameRequests::begin_frame() {
        current_ = std::make_shared<ExtractedLights>();
    }

    /// Submits the requested work.
    ///
    /// @param transform `transform` value used by the operation.
    /// @param light `light` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void LightFrameRequests::submit(Ecs::Entity, const WorldTransform &transform,
                                    const DirectionalLightRenderer &light) noexcept {
        if (!current_) {
            Ecs::Detail::contract_violation(
                "LightFrameRequests::submit() requires Engine::prepare_render_frame() to begin extraction first.");
        }

        current_->sun = SFT::Renderer::DirectionalLight{
            .direction = world_direction(transform),
            .radiance = light.radiance,
            .angular_radius_degrees = light.angular_radius_degrees,
            .casts_shadows = light.casts_shadows,
        };
    }

    /// Submits the requested work.
    ///
    /// @param transform `transform` value used by the operation.
    /// @param light `light` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void LightFrameRequests::submit(Ecs::Entity, const WorldTransform &transform,
                                    const SpotLightRenderer &light) noexcept {
        if (!current_) {
            Ecs::Detail::contract_violation(
                "LightFrameRequests::submit() requires Engine::prepare_render_frame() to begin extraction first.");
        }
        current_->spot_lights.push_back(SFT::Renderer::SpotLight{
            .position = world_position(transform),
            .direction = world_direction(transform),
            .radiance = light.radiance,
            .range = light.range,
            .inner_cone_cos = light.inner_cone_cos,
            .outer_cone_cos = light.outer_cone_cos,
            .source_radius = light.source_radius,
            .casts_shadows = light.casts_shadows,
        });
    }

    /// Submits the requested work.
    ///
    /// @param transform `transform` value used by the operation.
    /// @param light `light` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void LightFrameRequests::submit(Ecs::Entity, const WorldTransform &transform,
                                    const PointLightRenderer &light) noexcept {
        if (!current_) {
            Ecs::Detail::contract_violation(
                "LightFrameRequests::submit() requires Engine::prepare_render_frame() to begin extraction first.");
        }
        current_->point_lights.push_back(SFT::Renderer::PointLight{
            .position = world_position(transform),
            .radiance = light.radiance,
            .range = light.range,
            .source_radius = light.source_radius,
            .casts_shadows = light.casts_shadows,
        });
    }

    /// Returns the current or globally available finish frame value.
    ///
    /// @return Returns the current finish frame value.
    /// @note This function does not throw exceptions.
    std::shared_ptr<const LightFrameRequests::ExtractedLights> LightFrameRequests::finish_frame() const noexcept {
        if (!current_) {
            Ecs::Detail::contract_violation(
                "LightFrameRequests::finish_frame() called before begin_frame().");
        }
        return current_;
    }

} // namespace SFT::Engine

namespace SFT::Engine {

    /// Renders the requested content using the current rendering state.
    ///
    /// @param assets `assets` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    RenderFrameRequests::RenderFrameRequests(AssetManager &assets) noexcept : assets_(&assets) {}

} // namespace SFT::Engine

