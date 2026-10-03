#include <Animation/GraphJson.hpp>

#include <nlohmann/json.hpp>

#include <format>
#include <unordered_map>

namespace SFT::Animation {

    namespace {

        using Json = nlohmann::json;

        struct Failure {
            UString message;
        };

        [[nodiscard]] UString str(const Json &j, const char *key, UString fallback = {}) {
            const auto it = j.find(key);
            return it != j.end() && it->is_string() ? UString{it->get<std::string>()} : fallback;
        }
        [[nodiscard]] f32 num(const Json &j, const char *key, f32 fallback) {
            const auto it = j.find(key);
            return it != j.end() && it->is_number() ? it->get<f32>() : fallback;
        }
        [[nodiscard]] bool flag(const Json &j, const char *key, bool fallback) {
            const auto it = j.find(key);
            return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
        }

        [[nodiscard]] bool parse_op(const UString &s, CompareOp &op) {
            static const std::pair<UString, CompareOp> table[] = {
                {">", CompareOp::Greater},     {"<", CompareOp::Less},     {">=", CompareOp::GreaterEqual},
                {"<=", CompareOp::LessEqual},  {"==", CompareOp::Equal},   {"!=", CompareOp::NotEqual},
                {"true", CompareOp::IsTrue},   {"false", CompareOp::IsFalse}, {"trigger", CompareOp::Triggered}};
            for (const auto &[name, value] : table) {
                if (name == s) {
                    op = value;
                    return true;
                }
            }
            return false;
        }

    } // namespace

    std::expected<GraphDef, UString> load_graph_json(const ustr &text,
                                                         std::span<const std::shared_ptr<const Clip>> clips,
                                                         const Skeleton &skeleton) {
        Json doc;
        try {
            const std::string_view bytes = text.cpp_string_view();
            doc = Json::parse(bytes.begin(), bytes.end(), nullptr, true, /*ignore_comments=*/true);
        } catch (const Json::exception &e) {
            return std::unexpected(UString{std::format("animation graph: invalid JSON: {}", e.what())});
        }
        if (!doc.is_object()) {
            return std::unexpected("animation graph: the document must be an object.");
        }

        try {
            GraphDef def;

            for (const Json &p : doc.value("parameters", Json::array())) {
                const UString name = str(p, "name");
                const UString type = str(p, "type", "float");
                if (name.empty()) throw Failure{"a parameter has no name."};
                const f32 value = p.contains("default") && p["default"].is_boolean() ? (p["default"].get<bool>() ? 1.0f : 0.0f)
                                                                                       : num(p, "default", 0.0f);
                ParamType t = ParamType::Float;
                if (type == "bool"_ustr) t = ParamType::Bool;
                else if (type == "int"_ustr) t = ParamType::Int;
                else if (type == "trigger"_ustr) t = ParamType::Trigger;
                else if (type != "float"_ustr) throw Failure{std::format("parameter '{}' has unknown type '{}'.", name, type)};
                def.params.push_back(ParamDef{name, t, value});
            }
            const auto param = [&](const UString &name, const char *what) -> u32 {
                const u32 i = def.find_param(name);
                if (i == no_param) throw Failure{std::format("{} names an unknown parameter '{}'.", what, name)};
                return i;
            };

            const Json &nodes = doc.contains("nodes") ? doc["nodes"] : Json::array();
            if (!nodes.is_array()) throw Failure{"'nodes' must be an array."};
            std::unordered_map<UString, NodeId> ids;
            for (auto &&[i, node] : Foundation::iter(nodes).enumerate()) {
                const UString id = str(node, "id");
                if (!id.empty() && !ids.emplace(id, static_cast<NodeId>(i)).second) {
                    throw Failure{std::format("node id '{}' is used twice.", id)};
                }
            }
            const auto node_ref = [&](const Json &j, const char *key) -> NodeId {
                const UString id = j.is_string() ? UString{j.get<std::string>()} : str(j, key);
                const auto it = ids.find(id);
                if (it == ids.end()) throw Failure{std::format("unknown node '{}'.", id)};
                return it->second;
            };
            const auto clip_index = [&](const UString &name) -> u32 {
                for (auto &&[i, clip] : Foundation::iter(def.clips).enumerate()) {
                    if (clip->name == name) return static_cast<u32>(i);
                }
                for (const auto &c : clips) {
                    if (c->name == name) {
                        def.clips.push_back(c);
                        return static_cast<u32>(def.clips.size() - 1);
                    }
                }
                throw Failure{std::format("unknown clip '{}'.", name)};
            };

            for (const Json &j : nodes) {
                GraphNode n;
                n.name = str(j, "id");
                const UString type = str(j, "type");
                if (type == "clip"_ustr) {
                    n.type = GraphNode::Type::Clip;
                    n.clip = clip_index(str(j, "clip"));
                    n.speed = num(j, "speed", 1.0f);
                    n.loop = flag(j, "loop", true);
                    if (j.contains("speed_param")) n.speed_param = param(str(j, "speed_param"), "a clip node");
                } else if (type == "blend1d"_ustr) {
                    n.type = GraphNode::Type::Blend1D;
                    n.param_x = param(str(j, "param"), "a blend1d node");
                    n.sync_children = flag(j, "sync", true);
                    for (const Json &c : j.value("children", Json::array())) {
                        n.children_1d.push_back(BlendChild1D{num(c, "threshold", 0.0f), node_ref(c, "node")});
                    }
                    std::stable_sort(n.children_1d.begin(), n.children_1d.end(),
                                     [](const BlendChild1D &a, const BlendChild1D &b) { return a.threshold < b.threshold; });
                } else if (type == "blend2d"_ustr) {
                    n.type = GraphNode::Type::Blend2D;
                    n.param_x = param(str(j, "param_x"), "a blend2d node");
                    n.param_y = param(str(j, "param_y"), "a blend2d node");
                    n.sync_children = flag(j, "sync", true);
                    for (const Json &c : j.value("children", Json::array())) {
                        n.children_2d.push_back(BlendChild2D{{num(c, "x", 0.0f), num(c, "y", 0.0f)}, node_ref(c, "node")});
                    }
                } else if (type == "additive"_ustr) {
                    n.type = GraphNode::Type::Additive;
                    n.a = node_ref(j["base"], "base");
                    n.b = node_ref(j["delta"], "delta");
                    n.weight = num(j, "weight", 1.0f);
                    if (j.contains("weight_param")) n.weight_param = param(str(j, "weight_param"), "an additive node");
                } else if (type == "mix"_ustr) {
                    n.type = GraphNode::Type::Mix;
                    n.a = node_ref(j["a"], "a");
                    n.b = node_ref(j["b"], "b");
                    n.weight = num(j, "weight", 0.5f);
                    if (j.contains("weight_param")) n.weight_param = param(str(j, "weight_param"), "a mix node");
                } else if (type == "state_machine"_ustr) {
                    n.type = GraphNode::Type::StateMachine;
                    StateMachineDef &m = n.machine;
                    for (const Json &s : j.value("states", Json::array())) {
                        m.states.push_back(State{str(s, "name"), node_ref(s, "node")});
                    }
                    const UString entry = str(j, "entry");
                    if (!entry.empty()) {
                        m.entry = def.find_state(m, entry);
                        if (m.entry == no_node) throw Failure{std::format("state machine entry '{}' is not a state.", entry)};
                    }
                    for (const Json &t : j.value("transitions", Json::array())) {
                        Transition tr;
                        const UString from = str(t, "from", "*");
                        tr.from = from == "*"_ustr ? any_state : def.find_state(m, from);
                        const UString to = str(t, "to");
                        tr.to = def.find_state(m, to);
                        if ((from != "*"_ustr && tr.from == no_node) || tr.to == no_node) {
                            throw Failure{std::format("a transition names a state that does not exist ('{}' -> '{}').", from, to)};
                        }
                        tr.duration = num(t, "duration", 0.2f);
                        tr.has_exit_time = t.contains("exit_time");
                        tr.exit_time = num(t, "exit_time", 1.0f);
                        tr.can_interrupt = flag(t, "can_interrupt", true);
                        for (const Json &c : t.value("conditions", Json::array())) {
                            Condition cond;
                            cond.param = param(str(c, "param"), "a transition condition");
                            if (!parse_op(str(c, "op", ">"), cond.op)) {
                                throw Failure{std::format("unknown condition operator '{}'.", str(c, "op"))};
                            }
                            cond.value = num(c, "value", 0.0f);
                            tr.conditions.push_back(cond);
                        }
                        m.transitions.push_back(std::move(tr));
                    }
                } else {
                    throw Failure{std::format("node '{}' has unknown type '{}'.", n.name, type)};
                }
                def.nodes.push_back(std::move(n));
            }

            for (const Json &l : doc.value("layers", Json::array())) {
                Layer layer;
                layer.root = node_ref(l, "node");
                layer.additive = flag(l, "additive", false);
                layer.weight = num(l, "weight", 1.0f);
                if (l.contains("weight_param")) layer.weight_param = param(str(l, "weight_param"), "a layer");
                if (l.contains("mask")) {
                    std::vector<UString> names;
                    for (const Json &n : l["mask"].value("joints", Json::array())) {
                        if (n.is_string()) names.emplace_back(n.get<std::string>());
                    }
                    for (const UString &name : names) {
                        if (skeleton.find_joint(name) == no_joint) throw Failure{std::format("mask names unknown joint '{}'.", name)};
                    }
                    layer.mask = make_joint_mask(skeleton, names, flag(l["mask"], "include_children", true),
                                                 num(l["mask"], "weight", 1.0f));
                }
                def.layers.push_back(std::move(layer));
            }

            if (doc.contains("root_motion")) {
                const Json &r = doc["root_motion"];
                def.root_motion = flag(r, "enabled", true);
                def.root_joint = str(r, "joint");
            }

            if (const UString problem = validate_graph(def); !problem.empty()) {
                return std::unexpected(UString{std::format("animation graph: {}", problem)});
            }
            return def;
        } catch (const Failure &f) {
            return std::unexpected(UString{std::format("animation graph: {}", f.message)});
        } catch (const Json::exception &e) {
            return std::unexpected(UString{std::format("animation graph: malformed field: {}", e.what())});
        }
    }

} // namespace SFT::Animation
