#include "EcsRendering.hpp"
#include "AssetManager.hpp"

#include <algorithm>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>

namespace SFT::Engine {

    namespace {
        // Matches SpotLight/DirectionalLight's own default local-space direction ({0,-1,0}, "down")
        // — an ECS light entity with an identity WorldTransform rotation shines straight down, same
        // as the hand-authored default.
        [[nodiscard]] glm::vec3 world_direction(const WorldTransform &transform) noexcept {
            return glm::normalize(glm::mat3{transform.value} * glm::vec3{0.0f, -1.0f, 0.0f});
        }

        [[nodiscard]] glm::vec3 world_position(const WorldTransform &transform) noexcept {
            return glm::vec3{transform.value[3]};
        }
    } // namespace

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

    std::shared_ptr<const RenderFrameRequests::RenderableList> RenderFrameRequests::finish_frame() const noexcept {
        if (!current_) {
            Ecs::Detail::contract_violation(
                "RenderFrameRequests::finish_frame() called before begin_frame().");
        }
        return current_;
    }

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

    std::shared_ptr<const RenderFrameRequests::RenderableList> RenderFrameRequests::finish_gizmo_frame() const noexcept {
        if (!current_gizmos_) {
            Ecs::Detail::contract_violation(
                "RenderFrameRequests::finish_gizmo_frame() called before begin_frame().");
        }
        return current_gizmos_;
    }

    void LightFrameRequests::begin_frame() {
        current_ = std::make_shared<ExtractedLights>();
    }

    void LightFrameRequests::submit(Ecs::Entity, const WorldTransform &transform,
                                    const DirectionalLightRenderer &light) noexcept {
        if (!current_) {
            Ecs::Detail::contract_violation(
                "LightFrameRequests::submit() requires Engine::prepare_render_frame() to begin extraction first.");
        }
        // Only one sun is meaningful to the renderer; the last entity submitted this frame wins.
        current_->sun = SFT::Renderer::DirectionalLight{
            .direction = world_direction(transform),
            .radiance = light.radiance,
            .angular_radius_degrees = light.angular_radius_degrees,
            .casts_shadows = light.casts_shadows,
        };
    }

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

    std::shared_ptr<const LightFrameRequests::ExtractedLights> LightFrameRequests::finish_frame() const noexcept {
        if (!current_) {
            Ecs::Detail::contract_violation(
                "LightFrameRequests::finish_frame() called before begin_frame().");
        }
        return current_;
    }

} // namespace SFT::Engine
