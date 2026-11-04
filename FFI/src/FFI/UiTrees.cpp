/// C ABI for UI trees (`Engine/UiTree.hpp`): entities whose component is a UI tree, laid out once per update.
///
/// A foreign builder runs with the tree's layout context bound as this thread's open UI frame (`ForeignUiContextBinding`), so it builds with
/// the same `sturdy_ui_begin_element` / `sturdy_ui_text` calls a window frame uses. The builder, its user pointer and the `release_user` hook
/// live in one shared record so the hook runs exactly once, when the last copy of the builder goes away.

#include <Foundation/Foundation.hpp>

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

#include <glm/vec2.hpp>

#include <Engine/Engine.hpp>
#include <Engine/ForeignUi.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    namespace E = SFT::Engine;

    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;

    [[nodiscard]] SturdyResult invalid(const char *what) noexcept { return set_error(STURDY_ERROR_INVALID_ARGUMENT, what); }

    [[nodiscard]] SFT::Ecs::Entity to_entity(SturdyEntity entity) noexcept { return SFT::Ecs::Entity{.index = entity.index, .generation = entity.generation}; }
    [[nodiscard]] SturdyEntity from_entity(SFT::Ecs::Entity entity) noexcept { return SturdyEntity{entity.index, entity.generation}; }

    /// A builder callback plus the user data it owns. Shared by every copy of the `std::function` the tree holds.
    struct BuilderRecord {
        SturdyUiTreeBuilder builder = nullptr;
        void *user = nullptr;
        void(STURDY_ABI_CALL *release_user)(void *) = nullptr;

        BuilderRecord() = default;
        BuilderRecord(const BuilderRecord &) = delete;
        BuilderRecord &operator=(const BuilderRecord &) = delete;
        ~BuilderRecord() {
            if (release_user != nullptr) {
                release_user(user);
            }
        }
    };

    [[nodiscard]] E::UiTreeBuilder make_builder(SturdyUiTreeBuilder builder, void *user, void(STURDY_ABI_CALL *release_user)(void *)) {
        auto record = std::make_shared<BuilderRecord>();
        record->builder = builder;
        record->user = user;
        record->release_user = release_user;
        return [record](E::UiTreeFrame &frame) {
            SturdyUiTreeFrame foreign{};
            foreign.struct_size = sizeof(SturdyUiTreeFrame);
            foreign.entity = from_entity(frame.entity);
            foreign.width = frame.extent.x;
            foreign.height = frame.extent.y;
            foreign.delta_seconds = frame.delta_seconds;
            foreign.build_index = frame.build_index;
            foreign.request_rebuild = STURDY_FALSE;

            // The tree lays out in pixels divided by its scale; the foreign UI session only needs a viewport to size full-window elements.
            E::ForeignUiContextBinding binding{frame.ui, frame.extent};
            if (binding.rejected()) {
                throw std::runtime_error("a UI frame is already open on this thread; a UI tree cannot be built inside it");
            }
            const SFT::Ffi::ScopedHandle engine_handle{SFT::Ffi::HandleKind::Engine, &frame.engine};
            record->builder(SturdyEngine{engine_handle.token()}, &foreign, record->user);
            if (!binding.balanced()) {
                throw std::runtime_error("the UI tree builder returned with an element still open");
            }
            if (foreign.request_rebuild != 0) {
                frame.request_rebuild();
            }
        };
    }

    constexpr E::UiRebuildPolicy to_native(SturdyUiRebuildPolicy policy, bool *ok) noexcept {
        *ok = true;
        switch (policy) {
        case STURDY_UI_REBUILD_EVERY_UPDATE:
            return E::UiRebuildPolicy::EveryUpdate;
        case STURDY_UI_REBUILD_ON_DIRTY:
            return E::UiRebuildPolicy::OnDirty;
        case STURDY_UI_REBUILD_FIXED_RATE:
            return E::UiRebuildPolicy::FixedRate;
        case STURDY_UI_REBUILD_FORCE_U32:
        default:
            *ok = false;
            return E::UiRebuildPolicy::EveryUpdate;
        }
    }

    [[nodiscard]] SturdyResult to_native(const SturdyUiTreeTarget &target, E::UiTreeTarget *out) noexcept {
        switch (target.kind) {
        case STURDY_UI_TREE_TARGET_WINDOW: {
            E::UiWindowTarget window;
            if (target.window != 0) {
                window.window = static_cast<SFT::WindowManager::WindowId>(target.window);
            }
            *out = window;
            return STURDY_OK;
        }
        case STURDY_UI_TREE_TARGET_OFFSCREEN:
            if (target.target.value == 0) {
                return invalid("an off-screen UI tree needs a render target");
            }
            *out = E::UiOffscreenTarget{E::RenderTargetHandle{target.target.value}};
            return STURDY_OK;
        case STURDY_UI_TREE_TARGET_HEADLESS:
            *out = E::UiHeadlessTarget{glm::uvec2{target.headless_width, target.headless_height}};
            return STURDY_OK;
        case STURDY_UI_TREE_TARGET_FORCE_U32:
        default:
            return invalid("unrecognized UI tree target kind");
        }
    }

    [[nodiscard]] SturdyUiTreeTarget to_foreign(const E::UiTreeTarget &target) noexcept {
        SturdyUiTreeTarget out{};
        if (const auto *window = std::get_if<E::UiWindowTarget>(&target)) {
            out.kind = STURDY_UI_TREE_TARGET_WINDOW;
            out.window = window->window.has_value() ? static_cast<uint64_t>(*window->window) : 0;
        } else if (const auto *offscreen = std::get_if<E::UiOffscreenTarget>(&target)) {
            out.kind = STURDY_UI_TREE_TARGET_OFFSCREEN;
            out.target.value = offscreen->target.value;
        } else if (const auto *headless = std::get_if<E::UiHeadlessTarget>(&target)) {
            out.kind = STURDY_UI_TREE_TARGET_HEADLESS;
            out.headless_width = headless->extent.x;
            out.headless_height = headless->extent.y;
        }
        return out;
    }

    /// Resolves the engine and copies out the entity's tree handle (a cheap shared handle, so no component borrow is held while it is used).
    template <class Body>
    [[nodiscard]] SturdyResult with_tree(SturdyEngine engine, SturdyEntity entity, Body &&body) noexcept {
        return guarded([&]() -> SturdyResult {
            E::Engine *resolved = nullptr;
            const SturdyResult result = resolve_engine(engine, &resolved);
            if (result != STURDY_OK) {
                return result;
            }
            E::UiTree tree;
            {
                auto borrow = resolved->ecs_world().get_component<E::UiTree>(to_entity(entity));
                if (!borrow || !borrow->valid()) {
                    return set_error(STURDY_ERROR_COMPONENT_MISSING, "the entity has no UI tree");
                }
                tree = *borrow;
            }
            return body(*resolved, tree);
        });
    }

    [[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_desc_init(SturdyUiTreeDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return invalid("desc must not be null");
        }
        const E::UiTreeDesc defaults;
        *desc = {};
        desc->struct_size = sizeof(SturdyUiTreeDesc);
        desc->name = nullptr;
        desc->target = to_foreign(defaults.target);
        desc->rebuild = STURDY_UI_REBUILD_EVERY_UPDATE;
        desc->rebuild_hz = defaults.rebuild_hz;
        desc->layer = defaults.layer;
        desc->scale_mode = STURDY_UI_SCALE_MODE_CONTENT_SCALE;
        desc->content_scale = defaults.content_scale;
        desc->hdr_reference_white_scale = defaults.hdr_reference_white_scale;
        desc->window_input = defaults.window_input ? STURDY_TRUE : STURDY_FALSE;
        for (int i = 0; i < 4; ++i) {
            desc->clear_color[i] = defaults.clear_color[i];
        }
        desc->build_budget_ms = defaults.build_budget_ms;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_spawn(SturdyEngine engine, const SturdyUiTreeDesc *desc, SturdyEntity *out_entity) {
    return guarded([&]() -> SturdyResult {
        E::Engine *resolved = nullptr;
        if (const SturdyResult result = resolve_engine(engine, &resolved); result != STURDY_OK) {
            return result;
        }
        if (desc == nullptr || out_entity == nullptr) {
            return invalid("desc and output pointer must not be null");
        }
        if (desc->struct_size != sizeof(SturdyUiTreeDesc)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyUiTreeDesc::struct_size does not match this build");
        }
        if (desc->builder == nullptr) {
            return invalid("a UI tree needs a builder");
        }
        if (!finite(desc->rebuild_hz) || !finite(desc->content_scale) || !finite(desc->hdr_reference_white_scale) || !finite(desc->build_budget_ms) ||
            desc->rebuild_hz < 0.0f || desc->content_scale <= 0.0f) {
            return invalid("rebuild_hz, content_scale, hdr_reference_white_scale and build_budget_ms must be finite (and the first two positive)");
        }
        E::UiTreeDesc native;
        if (desc->name != nullptr) {
            native.name = desc->name;
        }
        if (const SturdyResult target = to_native(desc->target, &native.target); target != STURDY_OK) {
            return target;
        }
        bool policy_ok = false;
        native.rebuild = to_native(desc->rebuild, &policy_ok);
        if (!policy_ok) {
            return invalid("unrecognized rebuild policy");
        }
        native.rebuild_hz = desc->rebuild_hz;
        native.layer = desc->layer;
        switch (desc->scale_mode) {
        case STURDY_UI_SCALE_MODE_CONTENT_SCALE:
            native.scale_mode = SFT::UI::UiScaleMode::ContentScale;
            break;
        case STURDY_UI_SCALE_MODE_RAW_PIXELS:
            native.scale_mode = SFT::UI::UiScaleMode::RawPixels;
            break;
        case STURDY_UI_SCALE_MODE_FORCE_U32:
        default:
            return invalid("unrecognized scale mode");
        }
        native.content_scale = desc->content_scale;
        native.hdr_reference_white_scale = desc->hdr_reference_white_scale;
        native.window_input = desc->window_input != 0;
        native.clear_color = {desc->clear_color[0], desc->clear_color[1], desc->clear_color[2], desc->clear_color[3]};
        native.build_budget_ms = desc->build_budget_ms;
        native.builder = make_builder(desc->builder, desc->user, desc->release_user);
        *out_entity = from_entity(resolved->spawn_ui_tree(std::move(native)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_set_builder(SturdyEngine engine, SturdyEntity entity, SturdyUiTreeBuilder builder, void *user,
                                                        void(STURDY_ABI_CALL *release_user)(void *user)) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        if (builder == nullptr) {
            return invalid("a UI tree needs a builder");
        }
        tree.set_builder(make_builder(builder, user, release_user));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_set_target(SturdyEngine engine, SturdyEntity entity, const SturdyUiTreeTarget *target) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        if (target == nullptr) {
            return invalid("target must not be null");
        }
        E::UiTreeTarget native;
        if (const SturdyResult translated = to_native(*target, &native); translated != STURDY_OK) {
            return translated;
        }
        tree.set_target(native);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_target(SturdyEngine engine, SturdyEntity entity, SturdyUiTreeTarget *out_target) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        if (out_target == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_target = to_foreign(tree.target());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_set_rebuild_policy(SturdyEngine engine, SturdyEntity entity, SturdyUiRebuildPolicy policy, float rebuild_hz) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        bool ok = false;
        const E::UiRebuildPolicy native = to_native(policy, &ok);
        if (!ok || !finite(rebuild_hz) || rebuild_hz < 0.0f) {
            return invalid("unrecognized policy, or rebuild_hz is not a finite non-negative number");
        }
        tree.set_rebuild_policy(native, rebuild_hz);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_set_layer(SturdyEngine engine, SturdyEntity entity, int32_t layer) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        tree.set_layer(layer);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_layer(SturdyEngine engine, SturdyEntity entity, int32_t *out_layer) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        if (out_layer == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_layer = tree.layer();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_request_rebuild(SturdyEngine engine, SturdyEntity entity) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        tree.request_rebuild();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_point_at(SturdyEngine engine, SturdyEntity entity, SturdyBool has_uv, float u, float v, SturdyBool down) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        if (has_uv != 0) {
            if (!finite(u) || !finite(v)) {
                return invalid("u and v must be finite");
            }
            tree.point_at(glm::vec2{u, v}, down != 0);
        } else {
            tree.point_at(std::nullopt, down != 0);
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_scroll(SturdyEngine engine, SturdyEntity entity, float dx, float dy) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        if (!finite(dx) || !finite(dy)) {
            return invalid("scroll deltas must be finite");
        }
        tree.with_input([&](SFT::UI::UiInput &input) { input.add_scroll(glm::vec2{dx, dy}); });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_add_text(SturdyEngine engine, SturdyEntity entity, const char *utf8) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        if (utf8 == nullptr) {
            return invalid("text must not be null");
        }
        const std::string_view text{utf8};
        tree.with_input([&](SFT::UI::UiInput &input) { input.add_text(text); });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_pointer_consumed(SturdyEngine engine, SturdyEntity entity, SturdyBool *out_consumed) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        if (out_consumed == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_consumed = tree.pointer_consumed() ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_stats(SturdyEngine engine, SturdyEntity entity, SturdyUiTreeStats *out_stats) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        if (out_stats == nullptr) {
            return invalid("output pointer must not be null");
        }
        const E::UiTreeStats stats = tree.stats();
        const glm::vec2 extent = tree.last_extent();
        *out_stats = {stats.builds,  stats.skipped,      stats.over_budget, stats.failed_builds, stats.target_renders,
                      stats.last_build_ms, stats.max_build_ms, extent.x,     extent.y};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_last_error(SturdyEngine engine, SturdyEntity entity, char *buffer, size_t capacity, size_t *out_length) {
    return with_tree(engine, entity, [&](E::Engine &, E::UiTree &tree) -> SturdyResult {
        return copy_string_out(tree.stats().last_error, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_count(SturdyEngine engine, uint64_t *out_count) {
    return guarded([&]() -> SturdyResult {
        E::Engine *resolved = nullptr;
        if (const SturdyResult result = resolve_engine(engine, &resolved); result != STURDY_OK) {
            return result;
        }
        if (out_count == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_count = resolved->ui_trees().tree_count();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_tree_render_targets(SturdyEngine engine, uint64_t *out_rendered) {
    return guarded([&]() -> SturdyResult {
        E::Engine *resolved = nullptr;
        if (const SturdyResult result = resolve_engine(engine, &resolved); result != STURDY_OK) {
            return result;
        }
        const SFT::usize rendered = resolved->render_ui_tree_targets();
        if (out_rendered != nullptr) {
            *out_rendered = rendered;
        }
        return STURDY_OK;
    });
}

} // extern "C"
