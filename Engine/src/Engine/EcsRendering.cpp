#include "EcsRendering.hpp"
#include "AssetManager.hpp"

#include <algorithm>

namespace SFT::Engine {

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

} // namespace SFT::Engine
