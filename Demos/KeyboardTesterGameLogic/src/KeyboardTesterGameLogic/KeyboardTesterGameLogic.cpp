#include <KeyboardTesterGameLogic/KeyboardTesterGameLogic.hpp>

#include <array>
#include <glm/ext/matrix_transform.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace RendererApi = SFT::Renderer;

namespace SFT::KeyboardTester {

    namespace {

        // Raw keycodes here are SDL's — the same "lowercase ASCII for a letter's unshifted
        // identity" convention RuntimeDemoGameLogic's own keyboard systems rely on (event.key ==
        // '['), which is why this table can stay platform-header-free. -1 marks a purely decorative
        // slot (Caps Lock, Shift): SDL's modifier keycodes live outside the ASCII range this table
        // otherwise uses, so those keys are drawn but can never match a real KeyboardEvent.
        struct KeyLayoutSlot {
            i32 key;
            f32 width_units;
            u32 row;
        };

        constexpr i32 kBackspace = 8;
        constexpr i32 kTab = 9;
        constexpr i32 kEnter = 13;
        constexpr i32 kSpace = 32;
        constexpr i32 kDecorative = -1;

        constexpr std::array<KeyLayoutSlot, 54> kLayout{{
            // Row 0: number row.
            {'`', 1.0f, 0}, {'1', 1.0f, 0}, {'2', 1.0f, 0}, {'3', 1.0f, 0}, {'4', 1.0f, 0},
            {'5', 1.0f, 0}, {'6', 1.0f, 0}, {'7', 1.0f, 0}, {'8', 1.0f, 0}, {'9', 1.0f, 0},
            {'0', 1.0f, 0}, {'-', 1.0f, 0}, {'=', 1.0f, 0}, {kBackspace, 2.0f, 0},
            // Row 1: qwerty row.
            {kTab, 1.5f, 1}, {'q', 1.0f, 1}, {'w', 1.0f, 1}, {'e', 1.0f, 1}, {'r', 1.0f, 1},
            {'t', 1.0f, 1}, {'y', 1.0f, 1}, {'u', 1.0f, 1}, {'i', 1.0f, 1}, {'o', 1.0f, 1},
            {'p', 1.0f, 1}, {'[', 1.0f, 1}, {']', 1.0f, 1}, {'\\', 1.5f, 1},
            // Row 2: home row.
            {kDecorative, 1.75f, 2}, {'a', 1.0f, 2}, {'s', 1.0f, 2}, {'d', 1.0f, 2}, {'f', 1.0f, 2},
            {'g', 1.0f, 2}, {'h', 1.0f, 2}, {'j', 1.0f, 2}, {'k', 1.0f, 2}, {'l', 1.0f, 2},
            {';', 1.0f, 2}, {'\'', 1.0f, 2}, {kEnter, 2.25f, 2},
            // Row 3: bottom letter row.
            {kDecorative, 2.25f, 3}, {'z', 1.0f, 3}, {'x', 1.0f, 3}, {'c', 1.0f, 3}, {'v', 1.0f, 3},
            {'b', 1.0f, 3}, {'n', 1.0f, 3}, {'m', 1.0f, 3}, {',', 1.0f, 3}, {'.', 1.0f, 3},
            {'/', 1.0f, 3}, {kDecorative, 2.25f, 3},
            // Row 4: space row.
            {kSpace, 6.25f, 4},
        }};

        constexpr f32 kUnitSize = 0.9f;
        constexpr f32 kGap = 0.12f;
        constexpr u32 kRowCount = 5;

        struct KeyPlacement {
            f32 world_x = 0.0f;
            f32 world_z = 0.0f;
            f32 world_width = kUnitSize;
            f32 world_depth = kUnitSize;
        };

        // Lays every kLayout slot out left-to-right within its row (row order top-to-bottom becomes
        // -Z-to-+Z, so a camera looking straight down with world_up={0,0,-1} — see the constructor
        // below — sees the number row at the top of the screen and the space bar at the bottom,
        // matching a physical keyboard), then centers the whole grid on the origin on both axes.
        [[nodiscard]] std::vector<KeyPlacement> compute_key_placements() {
            std::vector<KeyPlacement> placements(kLayout.size());

            std::array<f32, kRowCount> row_width{};
            for (u32 row = 0; row < kRowCount; ++row) {
                f32 cursor_x = 0.0f;
                for (usize i = 0; i < kLayout.size(); ++i) {
                    if (kLayout[i].row != row) continue;
                    const f32 width = kLayout[i].width_units * kUnitSize;
                    placements[i].world_x = cursor_x + width * 0.5f;
                    placements[i].world_width = width;
                    cursor_x += width + kGap;
                }
                row_width[row] = cursor_x > 0.0f ? cursor_x - kGap : 0.0f;
            }

            f32 max_row_width = 0.0f;
            for (f32 width : row_width) max_row_width = std::max(max_row_width, width);

            const f32 row_step = kUnitSize + kGap;
            const f32 grid_depth = static_cast<f32>(kRowCount) * row_step - kGap;
            for (usize i = 0; i < kLayout.size(); ++i) {
                placements[i].world_x -= max_row_width * 0.5f;
                placements[i].world_z =
                    static_cast<f32>(kLayout[i].row) * row_step - grid_depth * 0.5f + kUnitSize * 0.5f;
            }
            return placements;
        }

    } // namespace

    KeyboardTesterGameLogic::KeyboardTesterGameLogic() {
        camera_ = Engine::Camera::orthographic(9.0f, 16.0f / 9.0f, 0.05f, 100.0f);
        // Looking straight down (-Y): world_up must not be parallel to that forward direction (the
        // default {0,1,0} would be), so pick an axis in the ground plane instead — see
        // compute_key_placements()'s doc comment for how this choice maps rows to screen position.
        camera_.set_position({0.0f, 12.0f, 0.0f});
        camera_.look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});

        render_graph_ = Engine::RenderGraph::standard();
        render_graph_.set_resolution_scale(1.0f);
    }

    Engine::GameLogicResult KeyboardTesterGameLogic::on_engine_initialized(Engine::Engine &engine) {
        if (Engine::AssetResult models = create_key_models(engine); !models) {
            return std::unexpected(Engine::GameLogicError{.message = models.error().message});
        }
        configure_render_extraction(engine);
        configure_keyboard_tracking(engine);
        spawn_key_entities(engine);
        return {};
    }

    Engine::AssetResult KeyboardTesterGameLogic::create_key_models(Engine::Engine &engine) {
        Engine::AssetManager &assets = engine.assets();
        auto shader = assets.load_shader(Engine::ShaderAssetDesc{
            .source = "Shaders/gbuffer_geometry.slang",
            .label = UString{"keyboard tester key shader"_ustr},
            .depth_only_fragment_entry_point = UString{"depthOnlyMain"_ustr},
        });
        if (!shader) {
            return std::unexpected(shader.error());
        }
        key_shader_ = *shader;

        const std::vector<KeyPlacement> placements = compute_key_placements();
        key_models_.reserve(kLayout.size());
        for (usize i = 0; i < kLayout.size(); ++i) {
            const KeyPlacement &placement = placements[i];
            auto model = assets.create_model(Engine::ModelAssetDesc{
                .label = UString{"keyboard tester key"_ustr},
                .primitives = {
                    Engine::ModelPrimitiveDesc{
                        .mesh = RendererApi::Mesh::plane(
                            {.width = placement.world_width, .depth = placement.world_depth,
                             .width_segments = 1, .depth_segments = 1},
                            "keyboard tester key"),
                        .shader = key_shader_,
                    },
                },
            });
            if (!model) {
                return std::unexpected(model.error());
            }

            // Idle appearance: a dim, faintly emissive gray so every key is visible against the
            // background even under low ambient light. apply_key_color_changes() below brightens
            // this to a saturated color the instant a real key press matches this slot.
            Engine::AssetResult configured = assets.set_model_vec4(
                *model, 0, "base_color_factor", glm::vec4{0.08f, 0.09f, 0.11f, 1.0f});
            if (configured) configured = assets.set_model_float(*model, 0, "metallic_factor", 0.0f);
            if (configured) configured = assets.set_model_float(*model, 0, "roughness_factor", 0.9f);
            if (configured) {
                configured = assets.set_model_vec4(
                    *model, 0, "emissive_factor", glm::vec4{0.05f, 0.07f, 0.09f, 0.0f});
            }
            if (configured) configured = assets.set_model_float(*model, 0, "emissive_strength", 1.0f);
            if (!configured) {
                return configured;
            }

            key_models_.push_back(*model);
        }

        grid_state_.pressed.assign(kLayout.size(), false);
        grid_state_last_applied_.assign(kLayout.size(), false);
        return {};
    }

    void KeyboardTesterGameLogic::configure_render_extraction(Engine::Engine &engine) {
        engine.ecs_world().bind_resource(engine.render_frame_requests());
        engine.render_extraction_schedule().add_system(
            [](Ecs::Entity entity,
               const Engine::WorldTransform &transform,
               const Engine::ModelRenderer &model_renderer,
               Ecs::WriteResource<Engine::RenderFrameRequests> render) noexcept {
                render->submit(entity, transform, model_renderer);
            });
    }

    void KeyboardTesterGameLogic::configure_keyboard_tracking(Engine::Engine &engine) {
        engine.ecs_world().bind_resource(grid_state_);
        engine.update_schedule().add_system(
            [](Ecs::WriteResource<KeyboardGridState> state,
               Ecs::EventReader<Engine::KeyboardEvent> keyboard) noexcept {
                for (const Engine::KeyboardEvent &event : keyboard.read()) {
                    for (usize i = 0; i < kLayout.size(); ++i) {
                        if (kLayout[i].key == event.key && kLayout[i].key != kDecorative) {
                            state->pressed[i] = event.pressed();
                        }
                    }
                }
            });
    }

    void KeyboardTesterGameLogic::spawn_key_entities(Engine::Engine &engine) {
        const std::vector<KeyPlacement> placements = compute_key_placements();
        for (usize i = 0; i < kLayout.size(); ++i) {
            const KeyPlacement &placement = placements[i];
            (void)engine.ecs_world().spawn(
                Engine::WorldTransform{
                    .value = glm::translate(
                        glm::mat4{1.0f}, glm::vec3{placement.world_x, 0.0f, placement.world_z}),
                },
                Engine::ModelRenderer{.model = key_models_[i]});
        }
    }

    void KeyboardTesterGameLogic::apply_key_color_changes(Engine::Engine &engine) {
        Engine::AssetManager &assets = engine.assets();
        constexpr glm::vec4 kIdleEmissive{0.05f, 0.07f, 0.09f, 0.0f};
        constexpr glm::vec4 kPressedEmissive{0.15f, 1.4f, 0.55f, 0.0f};
        for (usize i = 0; i < grid_state_.pressed.size(); ++i) {
            const bool pressed = grid_state_.pressed[i];
            if (pressed == grid_state_last_applied_[i]) continue;
            (void)assets.set_model_vec4(
                key_models_[i], 0, "emissive_factor", pressed ? kPressedEmissive : kIdleEmissive);
            grid_state_last_applied_[i] = pressed;
        }
    }

    std::optional<Engine::RenderFrameParameters> KeyboardTesterGameLogic::request_render_frame(
        Engine::Engine &engine,
        Core::RenderSurfaceHandle /*surface*/,
        const Core::FrameInput &frame) {
        apply_key_color_changes(engine);
        camera_.set_viewport_size(frame.framebuffer_width, frame.framebuffer_height);

        Engine::RenderFrameParameters parameters{
            .camera = camera_,
            .lighting = Engine::SceneLighting{
                .ambient_radiance = {0.12f, 0.13f, 0.16f},
                .exposure = 1.0f,
            },
            .render_graph = render_graph_,
            .debug_label = UString{"Keyboard tester scene"_ustr},
        };
        camera_.commit_frame();
        return parameters;
    }

    void KeyboardTesterGameLogic::on_shutdown(Engine::Engine & /*engine*/) noexcept {}

    std::unique_ptr<Engine::GameLogic> create_keyboard_tester_game_logic() {
        return std::make_unique<KeyboardTesterGameLogic>();
    }

} // namespace SFT::KeyboardTester
