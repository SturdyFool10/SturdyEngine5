#include <Engine/RenderGraphBlueprint.hpp>

#include <Reflection/Json.hpp>

#include <algorithm>
#include <set>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] std::unexpected<BlueprintError> fail(BlueprintErrorCode code, std::string_view message, std::string node = {}) {
            return std::unexpected(BlueprintError{.code = code, .message = UString{message}, .node = std::move(node)});
        }

        [[nodiscard]] std::string string_of(const Reflection::Value &value) { return std::string{value.as_string().cpp_string_view()}; }

        constexpr u32 max_composite_depth = 16;

    } // namespace

    // ── BlueprintParams ───────────────────────────────────────────────────────────────────────

    BlueprintParams &BlueprintParams::set_bytes(std::string_view key, std::span<const std::byte> bytes) {
        Reflection::Value array = Reflection::Value::make_array();
        for (const std::byte byte : bytes) {
            array.push_back(Reflection::Value{static_cast<i64>(std::to_integer<unsigned>(byte))});
        }
        values_.set(key, std::move(array));
        return *this;
    }

    std::string BlueprintParams::string_or(std::string_view key, std::string_view fallback) const {
        const Reflection::Value *found = values_.find(key);
        return found != nullptr && found->is_string() ? string_of(*found) : std::string{fallback};
    }

    i64 BlueprintParams::int_or(std::string_view key, i64 fallback) const noexcept {
        const Reflection::Value *found = values_.find(key);
        return found != nullptr && found->is_number() ? static_cast<i64>(found->as_number()) : fallback;
    }

    f64 BlueprintParams::float_or(std::string_view key, f64 fallback) const noexcept {
        const Reflection::Value *found = values_.find(key);
        return found != nullptr && found->is_number() ? found->as_number() : fallback;
    }

    bool BlueprintParams::bool_or(std::string_view key, bool fallback) const noexcept {
        const Reflection::Value *found = values_.find(key);
        return found != nullptr && found->is_bool() ? found->as_bool() : fallback;
    }

    std::vector<std::byte> BlueprintParams::bytes_or_empty(std::string_view key) const {
        std::vector<std::byte> bytes;
        const Reflection::Value *found = values_.find(key);
        if (found == nullptr || !found->is_array()) {
            return bytes;
        }
        bytes.reserve(found->size());
        for (usize i = 0; i < found->size(); ++i) {
            bytes.push_back(static_cast<std::byte>(static_cast<unsigned>(found->at(i).as_number()) & 0xFFu));
        }
        return bytes;
    }

    BlueprintExpected<BlueprintParams> BlueprintParams::from_document(const Reflection::Value &value) {
        if (!value.is_object()) {
            return fail(BlueprintErrorCode::InvalidDocument, "node params must be an object");
        }
        BlueprintParams params;
        params.values_ = value;
        return params;
    }

    // ── RenderGraphBlueprint ──────────────────────────────────────────────────────────────────

    NodeRef RenderGraphBlueprint::add(std::string_view module, std::span<const NodeRef> inputs, BlueprintParams params,
                                      std::string_view id) {
        Node node;
        node.id = id.empty() ? "n" + std::to_string(next_auto_id_++) : std::string{id};
        node.module = std::string{module};
        for (const NodeRef &input : inputs) {
            node.inputs.push_back(input.id);
        }
        node.params = std::move(params);
        NodeRef ref{node.id};
        nodes_.push_back(std::move(node));
        return ref;
    }

    NodeRef RenderGraphBlueprint::scene(std::string_view id) { return add("deferred_scene", std::span<const NodeRef>{}, {}, id); }
    NodeRef RenderGraphBlueprint::anti_aliasing(const NodeRef &input, std::string_view id) { return add("anti_aliasing", {input}, {}, id); }
    NodeRef RenderGraphBlueprint::bloom(const NodeRef &input, std::string_view id) { return add("bloom", {input}, {}, id); }
    NodeRef RenderGraphBlueprint::tone_mapping(const NodeRef &input, std::string_view id) { return add("tone_mapping", {input}, {}, id); }
    NodeRef RenderGraphBlueprint::debug_overlay(const NodeRef &input, std::string_view id) { return add("debug_overlay", {input}, {}, id); }
    NodeRef RenderGraphBlueprint::present(const NodeRef &input, std::string_view id) { return add("present", {input}, {}, id); }

    NodeRef RenderGraphBlueprint::fullscreen_effect(const NodeRef &input, const FullscreenEffectDescription &effect,
                                                    std::span<const NodeRef> extra_inputs, std::string_view id) {
        BlueprintParams params;
        params.set("shader_path", effect.shader_path.string())
            .set("module_name", effect.module_name)
            .set("fragment_entry_point", effect.fragment_entry_point)
            .set("label", effect.label.cpp_string_view());
        if (!effect.push_constants.empty()) {
            params.set_bytes("push_constants", effect.push_constants);
        }
        std::vector<NodeRef> all{input};
        all.insert(all.end(), extra_inputs.begin(), extra_inputs.end());
        return add("fullscreen_effect", std::span<const NodeRef>{all}, std::move(params), id);
    }

    NodeRef RenderGraphBlueprint::compute_effect(const NodeRef &input, const ComputeEffectDescription &effect, std::string_view id) {
        BlueprintParams params;
        params.set("shader_path", effect.shader_path.string())
            .set("module_name", effect.module_name)
            .set("compute_entry_point", effect.compute_entry_point)
            .set("label", effect.label.cpp_string_view());
        if (!effect.push_constants.empty()) {
            params.set_bytes("push_constants", effect.push_constants);
        }
        return add("compute_effect", {input}, std::move(params), id);
    }

    NodeRef RenderGraphBlueprint::copy(const NodeRef &input, std::string_view label, std::string_view id) {
        BlueprintParams params;
        if (!label.empty()) {
            params.set("label", label);
        }
        return add("copy", {input}, std::move(params), id);
    }

    NodeRef RenderGraphBlueprint::declare_input(std::string_view name) {
        input_names_.emplace_back(name);
        return NodeRef{std::string{name}};
    }

    RenderGraphBlueprint &RenderGraphBlueprint::set_output(const NodeRef &node) {
        output_ = node.id;
        return *this;
    }

    RenderGraphBlueprint RenderGraphBlueprint::standard() {
        RenderGraphBlueprint blueprint;
        NodeRef color = blueprint.scene();
        color = blueprint.anti_aliasing(color);
        color = blueprint.bloom(color);
        color = blueprint.tone_mapping(color);
        color = blueprint.debug_overlay(color);
        (void)blueprint.present(color);
        return blueprint;
    }

    Reflection::Value RenderGraphBlueprint::to_document() const {
        Reflection::Value document = Reflection::Value::make_object();
        if (!input_names_.empty()) {
            Reflection::Value inputs = Reflection::Value::make_array();
            for (const std::string &name : input_names_) {
                inputs.push_back(Reflection::Value{std::string_view{name}});
            }
            document.set("inputs", std::move(inputs));
        }
        if (!output_.empty()) {
            document.set("output", Reflection::Value{std::string_view{output_}});
        }
        Reflection::Value nodes = Reflection::Value::make_array();
        for (const Node &node : nodes_) {
            Reflection::Value entry = Reflection::Value::make_object();
            entry.set("id", Reflection::Value{std::string_view{node.id}});
            entry.set("module", Reflection::Value{std::string_view{node.module}});
            if (!node.inputs.empty()) {
                Reflection::Value inputs = Reflection::Value::make_array();
                for (const std::string &input : node.inputs) {
                    inputs.push_back(Reflection::Value{std::string_view{input}});
                }
                entry.set("inputs", std::move(inputs));
            }
            if (node.params.document().size() != 0) {
                entry.set("params", node.params.document());
            }
            nodes.push_back(std::move(entry));
        }
        document.set("nodes", std::move(nodes));
        return document;
    }

    BlueprintExpected<RenderGraphBlueprint> RenderGraphBlueprint::from_document(const Reflection::Value &document) {
        if (!document.is_object()) {
            return fail(BlueprintErrorCode::InvalidDocument, "a blueprint document must be an object");
        }
        RenderGraphBlueprint blueprint;
        if (const Reflection::Value *inputs = document.find("inputs")) {
            if (!inputs->is_array()) {
                return fail(BlueprintErrorCode::InvalidDocument, "\"inputs\" must be an array of names");
            }
            for (usize i = 0; i < inputs->size(); ++i) {
                if (!inputs->at(i).is_string()) {
                    return fail(BlueprintErrorCode::InvalidDocument, "\"inputs\" must contain only strings");
                }
                blueprint.input_names_.push_back(string_of(inputs->at(i)));
            }
        }
        if (const Reflection::Value *output = document.find("output")) {
            if (!output->is_string()) {
                return fail(BlueprintErrorCode::InvalidDocument, "\"output\" must be a node id");
            }
            blueprint.output_ = string_of(*output);
        }
        const Reflection::Value *nodes = document.find("nodes");
        if (nodes == nullptr || !nodes->is_array()) {
            return fail(BlueprintErrorCode::InvalidDocument, "a blueprint document needs a \"nodes\" array");
        }
        for (usize i = 0; i < nodes->size(); ++i) {
            const Reflection::Value &entry = nodes->at(i);
            const Reflection::Value *module = entry.is_object() ? entry.find("module") : nullptr;
            if (module == nullptr || !module->is_string()) {
                return fail(BlueprintErrorCode::InvalidDocument, "every node needs a string \"module\"");
            }
            Node node;
            node.module = string_of(*module);
            if (const Reflection::Value *id = entry.find("id")) {
                if (!id->is_string()) {
                    return fail(BlueprintErrorCode::InvalidDocument, "a node \"id\" must be a string");
                }
                node.id = string_of(*id);
            } else {
                node.id = "n" + std::to_string(blueprint.next_auto_id_++);
            }
            if (const Reflection::Value *inputs = entry.find("inputs")) {
                if (!inputs->is_array()) {
                    return fail(BlueprintErrorCode::InvalidDocument, "a node's \"inputs\" must be an array of node ids", node.id);
                }
                for (usize input = 0; input < inputs->size(); ++input) {
                    if (!inputs->at(input).is_string()) {
                        return fail(BlueprintErrorCode::InvalidDocument, "a node's \"inputs\" must contain only strings", node.id);
                    }
                    node.inputs.push_back(string_of(inputs->at(input)));
                }
            }
            if (const Reflection::Value *params = entry.find("params")) {
                auto parsed = BlueprintParams::from_document(*params);
                if (!parsed) {
                    parsed.error().node = node.id;
                    return std::unexpected(parsed.error());
                }
                node.params = std::move(*parsed);
            }
            blueprint.nodes_.push_back(std::move(node));
        }
        return blueprint;
    }

    UString RenderGraphBlueprint::to_json(bool pretty) const { return Reflection::write_json(to_document(), pretty); }

    BlueprintExpected<RenderGraphBlueprint> RenderGraphBlueprint::from_json(std::string_view text) {
        auto parsed = Reflection::parse_json(text);
        if (!parsed) {
            return fail(BlueprintErrorCode::InvalidDocument, "not valid JSON");
        }
        return from_document(*parsed);
    }

    // ── RenderModuleRegistry ──────────────────────────────────────────────────────────────────

    namespace {

        [[nodiscard]] BlueprintExpected<ModuleOutput> build_fullscreen(ModuleBuildContext &context) {
            FullscreenEffectDescription effect;
            effect.shader_path = context.params.string_or("shader_path");
            effect.module_name = context.params.string_or("module_name");
            effect.fragment_entry_point = context.params.string_or("fragment_entry_point", "fragmentMain");
            effect.label = UString{context.params.string_or("label")};
            effect.push_constants = context.params.bytes_or_empty("push_constants");
            if (effect.shader_path.empty() || effect.module_name.empty()) {
                return fail(BlueprintErrorCode::InvalidParameter, "fullscreen_effect needs \"shader_path\" and \"module_name\"");
            }
            effect.extra_inputs.assign(context.inputs.begin() + 1, context.inputs.end());
            return context.graph.add_fullscreen_effect(context.inputs[0], effect);
        }

        [[nodiscard]] BlueprintExpected<ModuleOutput> build_compute(ModuleBuildContext &context) {
            ComputeEffectDescription effect;
            effect.shader_path = context.params.string_or("shader_path");
            effect.module_name = context.params.string_or("module_name");
            effect.compute_entry_point = context.params.string_or("compute_entry_point", "computeMain");
            effect.label = UString{context.params.string_or("label")};
            effect.push_constants = context.params.bytes_or_empty("push_constants");
            if (effect.shader_path.empty() || effect.module_name.empty()) {
                return fail(BlueprintErrorCode::InvalidParameter, "compute_effect needs \"shader_path\" and \"module_name\"");
            }
            return context.graph.add_compute_effect(context.inputs[0], effect);
        }

    } // namespace

    RenderModuleRegistry RenderModuleRegistry::with_builtins() {
        RenderModuleRegistry registry;
        const auto add = [&registry](ModuleInfo info, ModuleBuildFn build) { (void)registry.register_module(std::move(info), std::move(build)); };

        add({"deferred_scene", 0, 0, "The deferred scene renderer. Must be the graph's first node."},
            [](ModuleBuildContext &c) -> BlueprintExpected<ModuleOutput> { return c.graph.compose(RenderModules::DeferredScene{}); });
        add({"anti_aliasing", 1, 1, "Anti-aliasing resolve of the scene."},
            [](ModuleBuildContext &c) -> BlueprintExpected<ModuleOutput> { return c.graph.compose(RenderModules::AntiAliasing{.input = c.inputs[0]}); });
        add({"bloom", 1, 1, "Bloom."},
            [](ModuleBuildContext &c) -> BlueprintExpected<ModuleOutput> { return c.graph.compose(RenderModules::Bloom{.input = c.inputs[0]}); });
        add({"tone_mapping", 1, 1, "Scene-linear HDR to display-encoded."},
            [](ModuleBuildContext &c) -> BlueprintExpected<ModuleOutput> { return c.graph.compose(RenderModules::ToneMapping{.input = c.inputs[0]}); });
        add({"debug_overlay", 1, 1, "Engine debug text overlay."},
            [](ModuleBuildContext &c) -> BlueprintExpected<ModuleOutput> { return c.graph.compose(RenderModules::DebugOverlay{.input = c.inputs[0]}); });
        add({"present", 1, 1, "Presents its input. Produces no texture."},
            [](ModuleBuildContext &c) -> BlueprintExpected<ModuleOutput> {
                (void)c.graph.compose(RenderModules::Present{.input = c.inputs[0]});
                return ModuleOutput{};
            });
        add({"fullscreen_effect", 1, 64, "A fullscreen raster effect: input 0 is sourceTexture, the rest extraTexture0..N."}, build_fullscreen);
        add({"compute_effect", 1, 1, "A compute effect over one input."}, build_compute);
        add({"copy", 1, 1, "Exact copy of the input."},
            [](ModuleBuildContext &c) -> BlueprintExpected<ModuleOutput> {
                return c.graph.compose(RenderModules::Copy{.input = c.inputs[0], .copy = CopyDescription{.label = UString{c.params.string_or("label")}}});
            });
        return registry;
    }

    const RenderModuleRegistry &RenderModuleRegistry::builtins() {
        static const RenderModuleRegistry registry = with_builtins();
        return registry;
    }

    BlueprintExpected<void> RenderModuleRegistry::register_module(ModuleInfo info, ModuleBuildFn build) {
        if (info.name.empty() || !build) {
            return fail(BlueprintErrorCode::InvalidParameter, "a module needs a name and a builder");
        }
        if (entries_.contains(info.name)) {
            return fail(BlueprintErrorCode::DuplicateModule, "a module named '" + info.name + "' is already registered");
        }
        std::string name = info.name;
        entries_.emplace(std::move(name), Entry{std::move(info), std::move(build)});
        return {};
    }

    BlueprintExpected<void> RenderModuleRegistry::define_composite(std::string name, RenderGraphBlueprint blueprint,
                                                                   std::string description) {
        const u32 inputs = static_cast<u32>(blueprint.input_names().size());
        return register_module(
            ModuleInfo{.name = std::move(name), .min_inputs = inputs, .max_inputs = inputs, .description = std::move(description)},
            [blueprint = std::move(blueprint)](ModuleBuildContext &context) -> BlueprintExpected<ModuleOutput> {
                return context.registry.instantiate(blueprint, context.graph, context.inputs, context.depth + 1);
            });
    }

    BlueprintExpected<void> RenderModuleRegistry::define_composites_from_document(const Reflection::Value &modules) {
        if (!modules.is_object()) {
            return fail(BlueprintErrorCode::InvalidDocument, "\"modules\" must be an object of blueprints");
        }
        for (usize i = 0; i < modules.size(); ++i) {
            auto blueprint = RenderGraphBlueprint::from_document(modules.value_at(i));
            if (!blueprint) {
                return std::unexpected(blueprint.error());
            }
            if (auto defined = define_composite(std::string{modules.key_at(i).cpp_string_view()}, std::move(*blueprint)); !defined) {
                return defined;
            }
        }
        return {};
    }

    const ModuleInfo *RenderModuleRegistry::find(std::string_view name) const noexcept {
        const auto found = entries_.find(std::string{name});
        return found == entries_.end() ? nullptr : &found->second.info;
    }

    std::vector<ModuleInfo> RenderModuleRegistry::modules() const {
        std::vector<ModuleInfo> result;
        result.reserve(entries_.size());
        for (const auto &[name, entry] : entries_) {
            result.push_back(entry.info);
        }
        std::ranges::sort(result, {}, &ModuleInfo::name);
        return result;
    }

    BlueprintExpected<ModuleOutput> RenderModuleRegistry::instantiate(const RenderGraphBlueprint &blueprint, RenderGraph &graph,
                                                                       std::span<const RenderGraphTextureHandle> inputs,
                                                                       u32 depth) const {
        if (depth > max_composite_depth) {
            return fail(BlueprintErrorCode::TooDeep, "composite modules are nested too deeply (is one defined in terms of itself?)");
        }
        if (inputs.size() != blueprint.input_names().size()) {
            return fail(BlueprintErrorCode::WrongInputCount, "the blueprint declares a different number of inputs than it was given");
        }

        // What each name resolves to. `produces` records whether a node yields a texture at all.
        std::unordered_map<std::string, RenderGraphTextureHandle> textures;
        std::set<std::string> sinks;
        std::set<std::string> known;
        for (usize i = 0; i < inputs.size(); ++i) {
            const std::string &name = blueprint.input_names()[i];
            if (!known.insert(name).second) {
                return fail(BlueprintErrorCode::DuplicateNode, "duplicate input name", name);
            }
            textures.emplace(name, inputs[i]);
        }
        for (const RenderGraphBlueprint::Node &node : blueprint.nodes()) {
            if (!known.insert(node.id).second) {
                return fail(BlueprintErrorCode::DuplicateNode, "more than one node (or input) is named '" + node.id + "'", node.id);
            }
        }
        for (const RenderGraphBlueprint::Node &node : blueprint.nodes()) {
            for (const std::string &input : node.inputs) {
                if (!known.contains(input)) {
                    return fail(BlueprintErrorCode::UnknownNode, "node '" + node.id + "' consumes unknown node '" + input + "'", node.id);
                }
            }
        }

        // Build in declaration order, deferring any node whose inputs are not ready yet. This is a stable
        // topological order, so a graph written in dependency order lowers exactly as written.
        std::vector<bool> built(blueprint.nodes().size(), false);
        usize remaining = blueprint.nodes().size();
        std::string last_texture_node;
        while (remaining > 0) {
            bool progressed = false;
            for (usize index = 0; index < blueprint.nodes().size(); ++index) {
                if (built[index]) {
                    continue;
                }
                const RenderGraphBlueprint::Node &node = blueprint.nodes()[index];
                const bool ready = std::ranges::all_of(node.inputs, [&](const std::string &input) {
                    return textures.contains(input) || sinks.contains(input);
                });
                if (!ready) {
                    continue;
                }

                const auto entry = entries_.find(node.module);
                if (entry == entries_.end()) {
                    return fail(BlueprintErrorCode::UnknownModule, "no module named '" + node.module + "' is registered", node.id);
                }
                const ModuleInfo &info = entry->second.info;
                if (node.inputs.size() < info.min_inputs || node.inputs.size() > info.max_inputs) {
                    return fail(BlueprintErrorCode::WrongInputCount,
                                "module '" + node.module + "' takes " + std::to_string(info.min_inputs) +
                                    (info.min_inputs == info.max_inputs ? "" : ".." + std::to_string(info.max_inputs)) +
                                    " input(s), node '" + node.id + "' has " + std::to_string(node.inputs.size()),
                                node.id);
                }
                std::vector<RenderGraphTextureHandle> node_inputs;
                for (const std::string &input : node.inputs) {
                    const auto texture = textures.find(input);
                    if (texture == textures.end()) {
                        return fail(BlueprintErrorCode::InvalidGraph, "node '" + input + "' produces no texture to consume", node.id);
                    }
                    node_inputs.push_back(texture->second);
                }

                ModuleBuildContext context{.graph = graph, .inputs = node_inputs, .params = node.params, .registry = *this, .depth = depth};
                auto output = entry->second.build(context);
                if (!output) {
                    if (output.error().node.empty()) {
                        output.error().node = node.id;
                    }
                    return std::unexpected(output.error());
                }
                if (output->texture) {
                    textures.emplace(node.id, output->texture);
                    last_texture_node = node.id;
                } else {
                    sinks.insert(node.id);
                }
                built[index] = true;
                --remaining;
                progressed = true;
            }
            if (!progressed) {
                for (usize index = 0; index < blueprint.nodes().size(); ++index) {
                    if (!built[index]) {
                        return fail(BlueprintErrorCode::Cycle, "the nodes form a cycle", blueprint.nodes()[index].id);
                    }
                }
            }
        }

        const std::string &wanted = blueprint.output().empty() ? last_texture_node : blueprint.output();
        if (wanted.empty()) {
            return ModuleOutput{};
        }
        const auto result = textures.find(wanted);
        if (result == textures.end()) {
            return fail(BlueprintErrorCode::UnknownNode, "the blueprint's output '" + wanted + "' is not a node that produces a texture", wanted);
        }
        return ModuleOutput{result->second};
    }

    BlueprintExpected<RenderGraph> RenderModuleRegistry::build(const RenderGraphBlueprint &blueprint,
                                                               RenderGraphDescription description) const {
        RenderGraph graph = RenderGraph::empty(std::move(description));
        auto instantiated = instantiate(blueprint, graph, {}, 0);
        if (!instantiated) {
            return std::unexpected(instantiated.error());
        }
        if (RenderGraphResult valid = graph.validate(); !valid) {
            return std::unexpected(BlueprintError{.code = BlueprintErrorCode::InvalidGraph, .message = valid.error().message, .node = {}});
        }
        return graph;
    }

    BlueprintExpected<RenderGraphBlueprint> load_render_graph_json(std::string_view text, RenderModuleRegistry &registry) {
        auto parsed = Reflection::parse_json(text);
        if (!parsed) {
            return fail(BlueprintErrorCode::InvalidDocument, "not valid JSON");
        }
        if (!parsed->is_object()) {
            return fail(BlueprintErrorCode::InvalidDocument, "the document must be an object");
        }
        if (const Reflection::Value *modules = parsed->find("modules")) {
            if (auto defined = registry.define_composites_from_document(*modules); !defined) {
                return std::unexpected(defined.error());
            }
        }
        return RenderGraphBlueprint::from_document(*parsed);
    }

} // namespace SFT::Engine
