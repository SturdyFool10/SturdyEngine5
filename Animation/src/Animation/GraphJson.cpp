#include <Animation/GraphJson.hpp>

#include <nlohmann/json.hpp>

#include <unordered_map>

namespace SFT::Animation {

    namespace {

        using Json = nlohmann::json;

        struct Failure {
            std::string message;
        };

        [[nodiscard]] std::string str(const Json &j, const char *key, std::string fallback = {}) {
            const auto it = j.find(key);
            return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
        }
        [[nodiscard]] f32 num(const Json &j, const char *key, f32 fallback) {
            const auto it = j.find(key);
            return it != j.end() && it->is_number() ? it->get<f32>() : fallback;
        }
        [[nodiscard]] bool flag(const Json &j, const char *key, bool fallback) {
            const auto it = j.find(key);
            return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
        }

        [[nodiscard]] bool parse_op(std::string_view s, CompareOp &op) {
            static constexpr std::pair<std::string_view, CompareOp> table[] = {
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

    std::expected<GraphDef, std::string> load_graph_json(std::string_view text,
                                                         std::span<const std::shared_ptr<const Clip>> clips,
                                                         const Skeleton &skeleton) {
        Json doc;
        try {
            doc = Json::parse(text.begin(), text.end(), nullptr, true, /*ignore_comments=*/true);
        } catch (const Json::exception &e) {
            return std::unexpected(std::string{"animation graph: invalid JSON: "} + e.what());
        }
        if (!doc.is_object()) {
            return std::unexpected("animation graph: the document must be an object.");
        }

        try {
            GraphDef def;

            for (const Json &p : doc.value("parameters", Json::array())) {
                const std::string name = str(p, "name");
                const std::string type = str(p, "type", "float");
                if (name.empty()) throw Failure{"a parameter has no name."};
                const f32 value = p.contains("default") && p["default"].is_boolean() ? (p["default"].get<bool>() ? 1.0f : 0.0f)
                                                                                       : num(p, "default", 0.0f);
                ParamType t = ParamType::Float;
                if (type == "bool") t = ParamType::Bool;
                else if (type == "int") t = ParamType::Int;
                else if (type == "trigger") t = ParamType::Trigger;
                else if (type != "float") throw Failure{"parameter '" + name + "' has unknown type '" + type + "'."};
                def.params.push_back(ParamDef{name, t, value});
            }
            const auto param = [&](const std::string &name, const char *what) -> u32 {
                const u32 i = def.find_param(name);
                if (i == no_param) throw Failure{std::string{what} + " names an unknown parameter '" + name + "'."};
                return i;
            };

            const Json &nodes = doc.contains("nodes") ? doc["nodes"] : Json::array();
            if (!nodes.is_array()) throw Failure{"'nodes' must be an array."};
            std::unordered_map<std::string, NodeId> ids;
            for (usize i = 0; i < nodes.size(); ++i) {
                const std::string id = str(nodes[i], "id");
                if (!id.empty() && !ids.emplace(id, static_cast<NodeId>(i)).second) {
                    throw Failure{"node id '" + id + "' is used twice."};
                }
            }
            const auto node_ref = [&](const Json &j, const char *key) -> NodeId {
                const std::string id = j.is_string() ? j.get<std::string>() : str(j, key);
                const auto it = ids.find(id);
                if (it == ids.end()) throw Failure{std::string{"unknown node '"} + id + "'."};
                return it->second;
            };
            const auto clip_index = [&](const std::string &name) -> u32 {
                for (usize i = 0; i < def.clips.size(); ++i) {
                    if (def.clips[i]->name == name) return static_cast<u32>(i);
                }
                for (const auto &c : clips) {
                    if (c->name == name) {
                        def.clips.push_back(c);
                        return static_cast<u32>(def.clips.size() - 1);
                    }
                }
                throw Failure{"unknown clip '" + name + "'."};
            };

            for (const Json &j : nodes) {
                GraphNode n;
                n.name = str(j, "id");
                const std::string type = str(j, "type");
                if (type == "clip") {
                    n.type = GraphNode::Type::Clip;
                    n.clip = clip_index(str(j, "clip"));
                    n.speed = num(j, "speed", 1.0f);
                    n.loop = flag(j, "loop", true);
                    if (j.contains("speed_param")) n.speed_param = param(str(j, "speed_param"), "a clip node");
                } else if (type == "blend1d") {
                    n.type = GraphNode::Type::Blend1D;
                    n.param_x = param(str(j, "param"), "a blend1d node");
                    n.sync_children = flag(j, "sync", true);
                    for (const Json &c : j.value("children", Json::array())) {
                        n.children_1d.push_back(BlendChild1D{num(c, "threshold", 0.0f), node_ref(c, "node")});
                    }
                    std::stable_sort(n.children_1d.begin(), n.children_1d.end(),
                                     [](const BlendChild1D &a, const BlendChild1D &b) { return a.threshold < b.threshold; });
                } else if (type == "blend2d") {
                    n.type = GraphNode::Type::Blend2D;
                    n.param_x = param(str(j, "param_x"), "a blend2d node");
                    n.param_y = param(str(j, "param_y"), "a blend2d node");
                    n.sync_children = flag(j, "sync", true);
                    for (const Json &c : j.value("children", Json::array())) {
                        n.children_2d.push_back(BlendChild2D{{num(c, "x", 0.0f), num(c, "y", 0.0f)}, node_ref(c, "node")});
                    }
                } else if (type == "additive") {
                    n.type = GraphNode::Type::Additive;
                    n.a = node_ref(j["base"], "base");
                    n.b = node_ref(j["delta"], "delta");
                    n.weight = num(j, "weight", 1.0f);
                    if (j.contains("weight_param")) n.weight_param = param(str(j, "weight_param"), "an additive node");
                } else if (type == "mix") {
                    n.type = GraphNode::Type::Mix;
                    n.a = node_ref(j["a"], "a");
                    n.b = node_ref(j["b"], "b");
                    n.weight = num(j, "weight", 0.5f);
                    if (j.contains("weight_param")) n.weight_param = param(str(j, "weight_param"), "a mix node");
                } else if (type == "state_machine") {
                    n.type = GraphNode::Type::StateMachine;
                    StateMachineDef &m = n.machine;
                    for (const Json &s : j.value("states", Json::array())) {
                        m.states.push_back(State{str(s, "name"), node_ref(s, "node")});
                    }
                    const std::string entry = str(j, "entry");
                    if (!entry.empty()) {
                        m.entry = def.find_state(m, entry);
                        if (m.entry == no_node) throw Failure{"state machine entry '" + entry + "' is not a state."};
                    }
                    for (const Json &t : j.value("transitions", Json::array())) {
                        Transition tr;
                        const std::string from = str(t, "from", "*");
                        tr.from = from == "*" ? any_state : def.find_state(m, from);
                        tr.to = def.find_state(m, str(t, "to"));
                        if ((from != "*" && tr.from == no_node) || tr.to == no_node) {
                            throw Failure{"a transition names a state that does not exist ('" + from + "' -> '" + str(t, "to") + "')."};
                        }
                        tr.duration = num(t, "duration", 0.2f);
                        tr.has_exit_time = t.contains("exit_time");
                        tr.exit_time = num(t, "exit_time", 1.0f);
                        tr.can_interrupt = flag(t, "can_interrupt", true);
                        for (const Json &c : t.value("conditions", Json::array())) {
                            Condition cond;
                            cond.param = param(str(c, "param"), "a transition condition");
                            if (!parse_op(str(c, "op", ">"), cond.op)) {
                                throw Failure{"unknown condition operator '" + str(c, "op") + "'."};
                            }
                            cond.value = num(c, "value", 0.0f);
                            tr.conditions.push_back(cond);
                        }
                        m.transitions.push_back(std::move(tr));
                    }
                } else {
                    throw Failure{"node '" + n.name + "' has unknown type '" + type + "'."};
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
                    std::vector<std::string> names;
                    for (const Json &n : l["mask"].value("joints", Json::array())) {
                        if (n.is_string()) names.push_back(n.get<std::string>());
                    }
                    for (const std::string &name : names) {
                        if (skeleton.find_joint(name) == no_joint) throw Failure{"mask names unknown joint '" + name + "'."};
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

            if (const std::string problem = validate_graph(def); !problem.empty()) {
                return std::unexpected("animation graph: " + problem);
            }
            return def;
        } catch (const Failure &f) {
            return std::unexpected("animation graph: " + f.message);
        } catch (const Json::exception &e) {
            return std::unexpected(std::string{"animation graph: malformed field: "} + e.what());
        }
    }

} // namespace SFT::Animation
