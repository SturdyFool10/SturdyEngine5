#pragma once

#include <Foundation/Foundation.hpp>

#include <Engine/RenderGraph.hpp>
#include <Reflection/Document.hpp>

#include <expected>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// Data-driven, fully composable description of a render graph.
///
/// A `RenderGraphBlueprint` is plain data: named nodes, each naming a *module kind* and the nodes whose
/// output it consumes, plus a bag of parameters. A `RenderModuleRegistry` turns kinds into real
/// `RenderGraph` passes. Because a kind is just a name, the same description can be written in C++, loaded
/// from JSON, or produced by another language, and because a blueprint can itself be registered as a kind
/// (a *composite*), modules compose without limit: an application builds `grade` out of three effects,
/// then uses `grade` like any built-in.
///
/// ```cpp
/// RenderGraphBlueprint blueprint;
/// const NodeRef scene = blueprint.scene();
/// const NodeRef aa = blueprint.anti_aliasing(scene);
/// const NodeRef color = blueprint.tone_mapping(blueprint.bloom(aa));
/// const NodeRef graded = blueprint.fullscreen_effect(color, {.shader_path = "Shaders/grade.slang",
///                                                           .module_name = "grade"});
/// blueprint.present(graded);
///
/// auto graph = build_render_graph(blueprint);   // std::expected<RenderGraph, BlueprintError>
/// ```
///
/// Adding a module kind:
///
/// ```cpp
/// RenderModuleRegistry registry = RenderModuleRegistry::with_builtins();
/// registry.register_module({.name = "vignette", .min_inputs = 1, .max_inputs = 1},
///                          [](ModuleBuildContext &context) {
///                              return context.graph.add_fullscreen_effect(context.inputs[0], {...});
///                          });
/// ```
///
/// Graph-wide settings (bloom, shadows, ...) are not part of the blueprint; they live in the
/// `RenderGraphDescription` handed to `build`.
namespace SFT::Engine {

    enum class BlueprintErrorCode : u8 {
        UnknownModule,
        UnknownNode,
        DuplicateNode,
        Cycle,
        WrongInputCount,
        InvalidParameter,
        InvalidGraph,
        InvalidDocument,
        DuplicateModule,
        TooDeep,
    };

    struct BlueprintError {
        BlueprintErrorCode code = BlueprintErrorCode::InvalidGraph;
        UString message;
        /// Id of the node the problem was found at, when there is one.
        std::string node;
    };

    template <class Value>
    using BlueprintExpected = std::expected<Value, BlueprintError>;

    /// Reference to a node of a blueprint (or to a composite's declared input). Cheap to copy.
    struct NodeRef {
        std::string id;
    };

    /// Named parameters of a node. Backed by a `Reflection::Value` object, so they round-trip through
    /// JSON unchanged. Setters chain.
    class BlueprintParams {
      public:
        BlueprintParams() : values_(Reflection::Value::make_object()) {}

        BlueprintParams &set(std::string_view key, std::string_view value) {
            values_.set(key, Reflection::Value{value});
            return *this;
        }
        BlueprintParams &set(std::string_view key, const char *value) { return set(key, std::string_view{value}); }
        BlueprintParams &set(std::string_view key, const std::string &value) { return set(key, std::string_view{value}); }
        BlueprintParams &set(std::string_view key, bool value) {
            values_.set(key, Reflection::Value{value});
            return *this;
        }
        template <class Number>
            requires(std::is_arithmetic_v<Number> && !std::is_same_v<Number, bool>)
        BlueprintParams &set(std::string_view key, Number value) {
            if constexpr (std::is_floating_point_v<Number>) {
                values_.set(key, Reflection::Value{static_cast<f64>(value)});
            } else {
                values_.set(key, Reflection::Value{static_cast<i64>(value)});
            }
            return *this;
        }
        /// Stores raw bytes (push constants) as an array of integers.
        BlueprintParams &set_bytes(std::string_view key, std::span<const std::byte> bytes);
        /// Stores a trivially copyable value's bytes.
        template <class Constants>
            requires std::is_trivially_copyable_v<Constants>
        BlueprintParams &set_bytes(std::string_view key, const Constants &constants) {
            return set_bytes(key, std::as_bytes(std::span<const Constants>{&constants, 1}));
        }

        [[nodiscard]] bool contains(std::string_view key) const noexcept { return values_.find(key) != nullptr; }
        [[nodiscard]] std::string string_or(std::string_view key, std::string_view fallback = {}) const;
        [[nodiscard]] i64 int_or(std::string_view key, i64 fallback = 0) const noexcept;
        [[nodiscard]] f64 float_or(std::string_view key, f64 fallback = 0.0) const noexcept;
        [[nodiscard]] bool bool_or(std::string_view key, bool fallback = false) const noexcept;
        [[nodiscard]] std::vector<std::byte> bytes_or_empty(std::string_view key) const;

        [[nodiscard]] const Reflection::Value &document() const noexcept { return values_; }
        [[nodiscard]] static BlueprintExpected<BlueprintParams> from_document(const Reflection::Value &value);

      private:
        Reflection::Value values_;
    };

    class RenderGraphBlueprint {
      public:
        struct Node {
            std::string id;
            std::string module;
            std::vector<std::string> inputs;
            BlueprintParams params;
        };

        /// Adds a node of any registered kind. `id` may be left empty for an automatic one (`n0`, `n1`, ...).
        NodeRef add(std::string_view module, std::span<const NodeRef> inputs = {}, BlueprintParams params = {},
                    std::string_view id = {});
        NodeRef add(std::string_view module, std::initializer_list<NodeRef> inputs, BlueprintParams params = {},
                    std::string_view id = {}) {
            return add(module, std::span<const NodeRef>{inputs.begin(), inputs.size()}, std::move(params), id);
        }

        // Built-in kinds, so ordinary graphs read naturally. Each is exactly `add("<kind>", ...)`.
        NodeRef scene(std::string_view id = {});
        NodeRef anti_aliasing(const NodeRef &input, std::string_view id = {});
        NodeRef bloom(const NodeRef &input, std::string_view id = {});
        NodeRef tone_mapping(const NodeRef &input, std::string_view id = {});
        NodeRef present(const NodeRef &input, std::string_view id = {});
        /// A fullscreen raster effect. `extra_inputs` are further textures it samples as `extraTexture0..N`
        /// (the description's own handle-based `extra_inputs` are ignored: blueprints refer to nodes).
        NodeRef fullscreen_effect(const NodeRef &input, const FullscreenEffectDescription &effect,
                                  std::span<const NodeRef> extra_inputs = {}, std::string_view id = {});
        NodeRef compute_effect(const NodeRef &input, const ComputeEffectDescription &effect, std::string_view id = {});
        NodeRef copy(const NodeRef &input, std::string_view label = {}, std::string_view id = {});

        /// For a blueprint used as a composite module: declares an input port and returns a reference to it
        /// that nodes can consume. Ports are matched to the caller's inputs in declaration order.
        NodeRef declare_input(std::string_view name);
        /// For a composite: which node's output the module produces. Defaults to the last node.
        RenderGraphBlueprint &set_output(const NodeRef &node);

        [[nodiscard]] const std::vector<Node> &nodes() const noexcept { return nodes_; }
        [[nodiscard]] const std::vector<std::string> &input_names() const noexcept { return input_names_; }
        [[nodiscard]] const std::string &output() const noexcept { return output_; }

        /// The engine's default chain: scene, anti-aliasing, bloom, tone mapping, present.
        [[nodiscard]] static RenderGraphBlueprint standard();

        [[nodiscard]] Reflection::Value to_document() const;
        [[nodiscard]] static BlueprintExpected<RenderGraphBlueprint> from_document(const Reflection::Value &document);
        [[nodiscard]] UString to_json(bool pretty = true) const;
        [[nodiscard]] static BlueprintExpected<RenderGraphBlueprint> from_json(std::string_view text);

      private:
        std::vector<Node> nodes_;
        std::vector<std::string> input_names_;
        std::string output_;
        u32 next_auto_id_ = 0;
    };

    class RenderModuleRegistry;

    /// What a module's builder is given.
    struct ModuleBuildContext {
        RenderGraph &graph;
        /// The textures produced by this node's input nodes, in order.
        std::span<const RenderGraphTextureHandle> inputs;
        const BlueprintParams &params;
        const RenderModuleRegistry &registry;
        /// Composite nesting depth (0 for the top-level blueprint).
        u32 depth = 0;
    };

    /// What a module produces. Implicitly built from the texture handle a `RenderGraph::add_*` returns; a
    /// module that only consumes (Present) returns `ModuleOutput{}`.
    struct ModuleOutput {
        RenderGraphTextureHandle texture{};
        ModuleOutput() = default;
        ModuleOutput(RenderGraphTextureHandle produced) : texture(produced) {}
    };

    using ModuleBuildFn = std::function<BlueprintExpected<ModuleOutput>(ModuleBuildContext &)>;

    struct ModuleInfo {
        std::string name;
        u32 min_inputs = 0;
        u32 max_inputs = 0;
        std::string description;
    };

    class RenderModuleRegistry {
      public:
        RenderModuleRegistry() = default;

        /// A registry holding the engine's built-in kinds: deferred_scene, anti_aliasing, bloom,
        /// tone_mapping, present, fullscreen_effect, compute_effect, copy.
        [[nodiscard]] static RenderModuleRegistry with_builtins();
        /// Shared read-only registry of just the built-ins.
        [[nodiscard]] static const RenderModuleRegistry &builtins();

        /// Registers a kind implemented in code. Fails if the name is taken.
        BlueprintExpected<void> register_module(ModuleInfo info, ModuleBuildFn build);
        /// Registers a kind defined by a blueprint, composed of other kinds (resolved when a graph is built,
        /// so definition order does not matter). Its input ports become the module's inputs.
        BlueprintExpected<void> define_composite(std::string name, RenderGraphBlueprint blueprint, std::string description = {});
        /// Registers every entry of a `{"name": <blueprint document>}` object as a composite.
        BlueprintExpected<void> define_composites_from_document(const Reflection::Value &modules);

        [[nodiscard]] const ModuleInfo *find(std::string_view name) const noexcept;
        [[nodiscard]] std::vector<ModuleInfo> modules() const;

        /// Builds a complete graph from a blueprint and validates it.
        [[nodiscard]] BlueprintExpected<RenderGraph> build(const RenderGraphBlueprint &blueprint,
                                                           RenderGraphDescription description = {}) const;
        /// Instantiates a blueprint's nodes into an existing graph (what composites do); returns the texture of
        /// the blueprint's output node.
        [[nodiscard]] BlueprintExpected<ModuleOutput> instantiate(const RenderGraphBlueprint &blueprint, RenderGraph &graph,
                                                                  std::span<const RenderGraphTextureHandle> inputs,
                                                                  u32 depth = 0) const;

      private:
        struct Entry {
            ModuleInfo info;
            ModuleBuildFn build;
        };
        std::unordered_map<std::string, Entry> entries_;
    };

    /// Builds a graph from a blueprint with the built-in kinds (or the registry you pass).
    [[nodiscard]] inline BlueprintExpected<RenderGraph> build_render_graph(
        const RenderGraphBlueprint &blueprint, RenderGraphDescription description = {},
        const RenderModuleRegistry &registry = RenderModuleRegistry::builtins()) {
        return registry.build(blueprint, std::move(description));
    }

    /// Loads a JSON document of the form
    /// `{"modules": {"name": {<blueprint>}, ...}, "inputs": [...], "output": "...", "nodes": [...]}`,
    /// registering `modules` into `registry` and returning the top-level blueprint.
    [[nodiscard]] BlueprintExpected<RenderGraphBlueprint> load_render_graph_json(std::string_view text,
                                                                                 RenderModuleRegistry &registry);

} // namespace SFT::Engine
