#include <KeyboardTesterGameLogic/KeyboardTesterGameLogic.hpp>

#include <array>
#include <glm/ext/matrix_transform.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace RendererApi = SFT::Renderer;

namespace SFT::KeyboardTester {

    namespace {


        struct KeyLayoutSlot {
            Engine::KeyboardKey key;
            f32 width_units;
            u32 row;
        };

        using enum Engine::KeyboardKey;

        constexpr std::array<KeyLayoutSlot, 61> kLayout{{

            {GraveAccent, 1.0f, 0}, {Digit1, 1.0f, 0}, {Digit2, 1.0f, 0}, {Digit3, 1.0f, 0},
            {Digit4, 1.0f, 0}, {Digit5, 1.0f, 0}, {Digit6, 1.0f, 0}, {Digit7, 1.0f, 0},
            {Digit8, 1.0f, 0}, {Digit9, 1.0f, 0}, {Digit0, 1.0f, 0}, {Minus, 1.0f, 0},
            {Equal, 1.0f, 0}, {Backspace, 2.0f, 0},

            {Tab, 1.5f, 1}, {Q, 1.0f, 1}, {W, 1.0f, 1}, {E, 1.0f, 1}, {R, 1.0f, 1},
            {T, 1.0f, 1}, {Y, 1.0f, 1}, {U, 1.0f, 1}, {I, 1.0f, 1}, {O, 1.0f, 1},
            {P, 1.0f, 1}, {LeftBracket, 1.0f, 1}, {RightBracket, 1.0f, 1}, {Backslash, 1.5f, 1},

            {CapsLock, 1.75f, 2}, {A, 1.0f, 2}, {S, 1.0f, 2}, {D, 1.0f, 2}, {F, 1.0f, 2},
            {G, 1.0f, 2}, {H, 1.0f, 2}, {J, 1.0f, 2}, {K, 1.0f, 2}, {L, 1.0f, 2},
            {Semicolon, 1.0f, 2}, {Apostrophe, 1.0f, 2}, {Enter, 2.25f, 2},

            {LeftShift, 2.25f, 3}, {Z, 1.0f, 3}, {X, 1.0f, 3}, {C, 1.0f, 3}, {V, 1.0f, 3},
            {B, 1.0f, 3}, {N, 1.0f, 3}, {M, 1.0f, 3}, {Comma, 1.0f, 3}, {Period, 1.0f, 3},
            {Slash, 1.0f, 3}, {RightShift, 2.25f, 3},

            {LeftControl, 1.25f, 4}, {LeftSuper, 1.25f, 4}, {LeftAlt, 1.25f, 4},
            {Space, 6.25f, 4}, {RightAlt, 1.25f, 4}, {RightSuper, 1.25f, 4},
            {Menu, 1.25f, 4}, {RightControl, 1.25f, 4},
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


        /// Computes key placements using the supplied arguments and current state.
        ///
        /// @return Returns the current compute key placements value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Performs the keyboard tester game logic operation for `KeyboardTester` using the supplied arguments.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    KeyboardTesterGameLogic::KeyboardTesterGameLogic() {
        camera_ = Engine::Camera::orthographic(9.0f, 16.0f / 9.0f, 0.05f, 100.0f);


        camera_.set_position({0.0f, 12.0f, 0.0f});
        camera_.look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});

        render_graph_ = Engine::RenderGraph::standard();
        render_graph_.set_resolution_scale(1.0f);
    }

    /// Handles the on engine initialized callback and updates the associated platform state.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Engine::GameLogicResult KeyboardTesterGameLogic::on_engine_initialized(Engine::Engine &engine) {
        if (Engine::AssetResult models = create_key_models(engine); !models) {
            return std::unexpected(Engine::GameLogicError{.message = models.error().message});
        }
        configure_render_extraction(engine);
        configure_keyboard_tracking(engine);
        spawn_key_entities(engine);
        return {};
    }

    /// Creates a key models from the supplied parameters.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
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

    /// Configures render extraction using the supplied arguments and current state.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Configures keyboard tracking using the supplied arguments and current state.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void KeyboardTesterGameLogic::configure_keyboard_tracking(Engine::Engine &engine) {
        engine.ecs_world().bind_resource(grid_state_);
        engine.update_schedule().add_system(
            [](Ecs::WriteResource<KeyboardGridState> state,
               Ecs::EventReader<Engine::KeyboardEvent> keyboard) noexcept {
                for (const Engine::KeyboardEvent &event : keyboard.read()) {
                    for (usize i = 0; i < kLayout.size(); ++i) {
                        if (kLayout[i].key == event.key_code) {
                            state->pressed[i] = event.pressed();
                        }
                    }
                }
            });
    }

    /// Spawns key entities.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Applies key color changes using the supplied arguments and current state.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Requests render frame using the supplied arguments and current state.
    ///
    /// @param engine `engine` value used by the operation.
    /// @param frame `frame` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    std::optional<Engine::RenderFrameParameters> KeyboardTesterGameLogic::request_render_frame(
        Engine::Engine &engine,
        Core::RenderSurfaceHandle            ,
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

    /// Handles the on shutdown callback and updates the associated platform state.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void KeyboardTesterGameLogic::on_shutdown(Engine::Engine &           ) noexcept {}

    /// Creates a keyboard tester game logic from the supplied parameters.
    ///
    /// @return Returns the current create keyboard tester game logic value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::unique_ptr<Engine::GameLogic> create_keyboard_tester_game_logic() {
        return std::make_unique<KeyboardTesterGameLogic>();
    }

} // namespace SFT::KeyboardTester
