#include <Animation/Graph.hpp>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace SFT::Animation {

    // ---- GraphDef / GraphBuilder -------------------------------------------------------------

    u32 GraphDef::find_param(const ustr &name) const noexcept {
        for (auto &&[i, param] : Foundation::iter(params).enumerate()) {
            if (param.name == name) {
                return static_cast<u32>(i);
            }
        }
        return no_param;
    }

    u32 GraphDef::find_state(const StateMachineDef &machine, const ustr &name) const noexcept {
        for (auto &&[i, state] : Foundation::iter(machine.states).enumerate()) {
            if (state.name == name) {
                return static_cast<u32>(i);
            }
        }
        return no_node;
    }

    namespace {
        u32 add_param(GraphDef &def, UString name, ParamType type, f32 value) {
            def.params.push_back(ParamDef{std::move(name), type, value});
            return static_cast<u32>(def.params.size() - 1);
        }
        NodeId add_node(GraphDef &def, GraphNode node, UString name) {
            node.name = std::move(name);
            def.nodes.push_back(std::move(node));
            return static_cast<NodeId>(def.nodes.size() - 1);
        }
    } // namespace

    u32 GraphBuilder::param_float(UString name, f32 v) { return add_param(def_, std::move(name), ParamType::Float, v); }
    u32 GraphBuilder::param_bool(UString name, bool v) { return add_param(def_, std::move(name), ParamType::Bool, v ? 1.0f : 0.0f); }
    u32 GraphBuilder::param_int(UString name, i32 v) { return add_param(def_, std::move(name), ParamType::Int, static_cast<f32>(v)); }
    u32 GraphBuilder::param_trigger(UString name) { return add_param(def_, std::move(name), ParamType::Trigger, 0.0f); }

    u32 GraphBuilder::add_clip(std::shared_ptr<const Clip> clip) {
        def_.clips.push_back(std::move(clip));
        return static_cast<u32>(def_.clips.size() - 1);
    }

    NodeId GraphBuilder::clip_node(u32 clip, bool loop, f32 speed, UString name) {
        GraphNode n;
        n.type = GraphNode::Type::Clip;
        n.clip = clip;
        n.loop = loop;
        n.speed = speed;
        return add_node(def_, std::move(n), std::move(name));
    }

    NodeId GraphBuilder::blend_1d(u32 param, std::vector<BlendChild1D> children, UString name) {
        GraphNode n;
        n.type = GraphNode::Type::Blend1D;
        n.param_x = param;
        std::stable_sort(children.begin(), children.end(),
                         [](const BlendChild1D &a, const BlendChild1D &b) { return a.threshold < b.threshold; });
        n.children_1d = std::move(children);
        return add_node(def_, std::move(n), std::move(name));
    }

    NodeId GraphBuilder::blend_2d(u32 px, u32 py, std::vector<BlendChild2D> children, UString name) {
        GraphNode n;
        n.type = GraphNode::Type::Blend2D;
        n.param_x = px;
        n.param_y = py;
        n.children_2d = std::move(children);
        return add_node(def_, std::move(n), std::move(name));
    }

    NodeId GraphBuilder::additive(NodeId base, NodeId delta, f32 weight, u32 weight_param, UString name) {
        GraphNode n;
        n.type = GraphNode::Type::Additive;
        n.a = base;
        n.b = delta;
        n.weight = weight;
        n.weight_param = weight_param;
        return add_node(def_, std::move(n), std::move(name));
    }

    NodeId GraphBuilder::mix(NodeId a, NodeId b, f32 weight, u32 weight_param, UString name) {
        GraphNode n;
        n.type = GraphNode::Type::Mix;
        n.a = a;
        n.b = b;
        n.weight = weight;
        n.weight_param = weight_param;
        return add_node(def_, std::move(n), std::move(name));
    }

    NodeId GraphBuilder::state_machine(StateMachineDef machine, UString name) {
        GraphNode n;
        n.type = GraphNode::Type::StateMachine;
        n.machine = std::move(machine);
        return add_node(def_, std::move(n), std::move(name));
    }

    void GraphBuilder::add_layer(Layer layer) { def_.layers.push_back(std::move(layer)); }

    void GraphBuilder::enable_root_motion(UString joint) {
        def_.root_motion = true;
        def_.root_joint = std::move(joint);
    }

    std::vector<f32> make_joint_mask(const Skeleton &skeleton, std::span<const UString> root_names,
                                     bool include_children, f32 value) {
        std::vector<f32> mask(skeleton.joint_count(), 0.0f);
        for (auto &&[j, joint_name] : Foundation::iter(skeleton.names).enumerate()) {
            const bool named = std::find(root_names.begin(), root_names.end(), joint_name) != root_names.end();
            const u32 parent = skeleton.parents[j];
            if (named || (include_children && parent != no_joint && mask[parent] > 0.0f)) {
                mask[j] = value;
            }
        }
        return mask;
    }

    // ---- validation --------------------------------------------------------------------------

    namespace {

        void children_of(const GraphNode &node, std::vector<NodeId> &out) {
            out.clear();
            switch (node.type) {
                case GraphNode::Type::Clip: break;
                case GraphNode::Type::Blend1D:
                    for (const auto &c : node.children_1d) out.push_back(c.node);
                    break;
                case GraphNode::Type::Blend2D:
                    for (const auto &c : node.children_2d) out.push_back(c.node);
                    break;
                case GraphNode::Type::Additive:
                case GraphNode::Type::Mix:
                    out.push_back(node.a);
                    out.push_back(node.b);
                    break;
                case GraphNode::Type::StateMachine:
                    for (const auto &s : node.machine.states) out.push_back(s.node);
                    break;
            }
        }

        // 0 = unvisited, 1 = on the current path, 2 = done
        bool has_cycle(const GraphDef &def, NodeId id, std::vector<u8> &mark) {
            if (mark[id] == 1) return true;
            if (mark[id] == 2) return false;
            mark[id] = 1;
            std::vector<NodeId> kids;
            children_of(def.nodes[id], kids);
            for (NodeId k : kids) {
                if (k < def.nodes.size() && has_cycle(def, k, mark)) return true;
            }
            mark[id] = 2;
            return false;
        }

    } // namespace

    UString validate_graph(const GraphDef &def) {
        const auto bad_param = [&](u32 p) { return p != no_param && p >= def.params.size(); };
        for (usize i = 0; i < def.nodes.size(); ++i) {
            const GraphNode &n = def.nodes[i];
            const UString where = std::format("node {}{}", i, n.name.empty() ? UString{} : UString{std::format(" '{}'", n.name)});
            std::vector<NodeId> kids;
            children_of(n, kids);
            for (NodeId k : kids) {
                if (k >= def.nodes.size()) return where + " references a missing node."_ustr;
            }
            if (bad_param(n.param_x) || bad_param(n.param_y) || bad_param(n.weight_param) || bad_param(n.speed_param)) {
                return where + " references a missing parameter."_ustr;
            }
            switch (n.type) {
                case GraphNode::Type::Clip:
                    if (n.clip >= def.clips.size()) return where + " references a missing clip."_ustr;
                    break;
                case GraphNode::Type::Blend1D:
                    if (n.children_1d.empty()) return where + " has no children."_ustr;
                    break;
                case GraphNode::Type::Blend2D:
                    if (n.children_2d.empty()) return where + " has no children."_ustr;
                    break;
                case GraphNode::Type::StateMachine: {
                    const auto &m = n.machine;
                    if (m.states.empty()) return where + " has no states."_ustr;
                    if (m.entry >= m.states.size()) return where + " has an invalid entry state."_ustr;
                    for (const Transition &t : m.transitions) {
                        if ((t.from != any_state && t.from >= m.states.size()) || t.to >= m.states.size()) {
                            return where + " has a transition naming a missing state."_ustr;
                        }
                        for (const Condition &c : t.conditions) {
                            if (c.param == no_param || c.param >= def.params.size()) {
                                return where + " has a condition naming a missing parameter."_ustr;
                            }
                        }
                    }
                    break;
                }
                default: break;
            }
        }
        std::vector<u8> mark(def.nodes.size(), 0);
        for (usize i = 0; i < def.nodes.size(); ++i) {
            if (has_cycle(def, static_cast<NodeId>(i), mark)) return "the graph contains a cycle.";
        }
        for (usize i = 0; i < def.layers.size(); ++i) {
            if (def.layers[i].root >= def.nodes.size()) return std::format("layer {} has no valid root node.", i);
            if (bad_param(def.layers[i].weight_param)) return std::format("layer {} references a missing parameter.", i);
        }
        return {};
    }

    // ---- GraphInstance -----------------------------------------------------------------------

    namespace {

        constexpr f32 kEpsilon = 1e-4f;

        [[nodiscard]] f32 smoothstep01(f32 t) {
            t = std::clamp(t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        [[nodiscard]] RootMotionDelta blend_root(const RootMotionDelta &a, const RootMotionDelta &b, f32 w) {
            RootMotionDelta r;
            r.translation = glm::mix(a.translation, b.translation, w);
            glm::quat qb = b.rotation;
            if (glm::dot(a.rotation, qb) < 0.0f) qb = -qb;
            r.rotation = glm::normalize(glm::quat(glm::mix(a.rotation.w, qb.w, w), glm::mix(a.rotation.x, qb.x, w),
                                                  glm::mix(a.rotation.y, qb.y, w), glm::mix(a.rotation.z, qb.z, w)));
            return r;
        }

        // Splits q into twist about +Y and the remaining swing: q = swing * twist.
        void twist_swing_y(const glm::quat &q, glm::quat &twist, glm::quat &swing) {
            const glm::quat t = glm::normalize(glm::quat(q.w, 0.0f, q.y, 0.0f));
            twist = glm::dot(t, t) > 0.0f && std::isfinite(t.w) ? t : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            swing = q * glm::inverse(twist);
        }

    } // namespace

    GraphInstance::GraphInstance(std::shared_ptr<const GraphDef> def, std::shared_ptr<const Skeleton> skeleton)
        : def_(std::move(def)), skeleton_(std::move(skeleton)) {
        reference_ = skeleton_->rest_pose;
        if (!def_->root_joint.empty()) {
            const u32 found = skeleton_->find_joint(def_->root_joint);
            root_joint_ = found == no_joint ? 0 : found;
        }
        states_.resize(def_->nodes.size());
        reset();
    }

    void GraphInstance::reset() {
        params_.resize(def_->params.size());
        for (usize i = 0; i < params_.size(); ++i) {
            params_[i] = def_->params[i].default_value;
        }
        for (usize i = 0; i < states_.size(); ++i) {
            states_[i] = NodeState{};
            if (def_->nodes[i].type == GraphNode::Type::StateMachine) {
                states_[i].current = def_->nodes[i].machine.entry;
            }
        }
        events_.clear();
        root_delta_ = {};
    }

    void GraphInstance::set_param(u32 index, f32 value) {
        if (index < params_.size()) params_[index] = value;
    }
    void GraphInstance::set_float(const ustr &name, f32 value) { set_param(def_->find_param(name), value); }
    void GraphInstance::set_bool(const ustr &name, bool value) { set_param(def_->find_param(name), value ? 1.0f : 0.0f); }
    void GraphInstance::set_int(const ustr &name, i32 value) { set_param(def_->find_param(name), static_cast<f32>(value)); }
    void GraphInstance::set_trigger(const ustr &name) { set_param(def_->find_param(name), 1.0f); }
    f32 GraphInstance::get_param(const ustr &name) const { return param_value(def_->find_param(name)); }

    f32 GraphInstance::param_value(u32 index) const { return index < params_.size() ? params_[index] : 0.0f; }

    bool GraphInstance::condition_holds(const Condition &c) const {
        const f32 v = param_value(c.param);
        switch (c.op) {
            case CompareOp::Greater: return v > c.value;
            case CompareOp::Less: return v < c.value;
            case CompareOp::GreaterEqual: return v >= c.value;
            case CompareOp::LessEqual: return v <= c.value;
            case CompareOp::Equal: return std::fabs(v - c.value) < kEpsilon;
            case CompareOp::NotEqual: return std::fabs(v - c.value) >= kEpsilon;
            case CompareOp::IsTrue:
            case CompareOp::Triggered: return v > 0.5f;
            case CompareOp::IsFalse: return v <= 0.5f;
        }
        return false;
    }

    void GraphInstance::consume_triggers(const Transition &t) {
        for (const Condition &c : t.conditions) {
            if (c.op == CompareOp::Triggered) set_param(c.param, 0.0f);
        }
    }

    const UString &GraphInstance::current_state_name() const {
        static const UString none;
        for (auto &&[i, n] : Foundation::iter(def_->nodes).enumerate()) {
            if (n.type == GraphNode::Type::StateMachine) {
                const NodeState &s = states_[i];
                const u32 index = s.next != no_node ? s.next : s.current;
                return index < n.machine.states.size() ? n.machine.states[index].name : none;
            }
        }
        return none;
    }

    bool GraphInstance::in_transition() const {
        for (usize i = 0; i < def_->nodes.size(); ++i) {
            if (def_->nodes[i].type == GraphNode::Type::StateMachine) return states_[i].next != no_node;
        }
        return false;
    }

    void GraphInstance::blend_weights(const GraphNode &node, std::vector<f32> &weights) const {
        if (node.type == GraphNode::Type::Blend1D) {
            const auto &c = node.children_1d;
            weights.assign(c.size(), 0.0f);
            const f32 x = param_value(node.param_x);
            if (x <= c.front().threshold) {
                weights.front() = 1.0f;
            } else if (x >= c.back().threshold) {
                weights.back() = 1.0f;
            } else {
                for (usize i = 0; i + 1 < c.size(); ++i) {
                    if (x >= c[i].threshold && x <= c[i + 1].threshold) {
                        const f32 span = c[i + 1].threshold - c[i].threshold;
                        const f32 t = span > kEpsilon ? (x - c[i].threshold) / span : 0.0f;
                        weights[i] = 1.0f - t;
                        weights[i + 1] = t;
                        break;
                    }
                }
            }
        } else {
            const auto &c = node.children_2d;
            weights.assign(c.size(), 0.0f);
            const glm::vec2 p{param_value(node.param_x), param_value(node.param_y)};
            f32 total = 0.0f;
            for (usize i = 0; i < c.size(); ++i) {
                const f32 d = glm::length(p - c[i].position);
                if (d < kEpsilon) {
                    std::fill(weights.begin(), weights.end(), 0.0f);
                    weights[i] = 1.0f;
                    return;
                }
                weights[i] = 1.0f / (d * d);
                total += weights[i];
            }
            for (f32 &w : weights) w /= total;
        }
    }

    // Playback rate in cycles per second of whatever the node plays (1 / duration at speed 1).
    f32 GraphInstance::node_rate(NodeId id) const {
        const GraphNode &node = def_->nodes[id];
        switch (node.type) {
            case GraphNode::Type::Clip: {
                const Clip &clip = *def_->clips[node.clip];
                const f32 speed = node.speed * (node.speed_param != no_param ? param_value(node.speed_param) : 1.0f);
                return clip.duration > kEpsilon ? std::fabs(speed) / clip.duration : 0.0f;
            }
            case GraphNode::Type::Blend1D:
            case GraphNode::Type::Blend2D: {
                std::vector<f32> w;
                blend_weights(node, w);
                f32 rate = 0.0f;
                for (usize i = 0; i < w.size(); ++i) {
                    const NodeId child = node.type == GraphNode::Type::Blend1D ? node.children_1d[i].node : node.children_2d[i].node;
                    if (w[i] > kEpsilon) rate += w[i] * node_rate(child);
                }
                return rate;
            }
            case GraphNode::Type::Additive: return node_rate(node.a);
            case GraphNode::Type::Mix: {
                const f32 w = node.weight_param != no_param ? param_value(node.weight_param) : node.weight;
                return glm::mix(node_rate(node.a), node_rate(node.b), std::clamp(w, 0.0f, 1.0f));
            }
            case GraphNode::Type::StateMachine: {
                const NodeState &s = states_[id];
                const u32 index = s.next != no_node ? s.next : s.current;
                return node_rate(node.machine.states[index].node);
            }
        }
        return 0.0f;
    }

    void GraphInstance::reset_subtree(NodeId id) {
        const GraphNode &node = def_->nodes[id];
        states_[id] = NodeState{};
        std::vector<NodeId> kids;
        switch (node.type) {
            case GraphNode::Type::Clip: break;
            case GraphNode::Type::Blend1D:
                for (const auto &c : node.children_1d) kids.push_back(c.node);
                break;
            case GraphNode::Type::Blend2D:
                for (const auto &c : node.children_2d) kids.push_back(c.node);
                break;
            case GraphNode::Type::Additive:
            case GraphNode::Type::Mix:
                kids = {node.a, node.b};
                break;
            case GraphNode::Type::StateMachine:
                states_[id].current = node.machine.entry;
                kids.push_back(node.machine.states[node.machine.entry].node);
                break;
        }
        for (NodeId k : kids) reset_subtree(k);
    }

    void GraphInstance::sample_root(const Clip &clip, f32 time, JointTransform &out) const {
        out = skeleton_->rest_pose[root_joint_];
        if (root_joint_ >= clip.channels.size()) return;
        const JointChannels &c = clip.channels[root_joint_];
        if (!c.translation.empty()) out.translation = sample_vec3(c.translation, time);
        if (!c.rotation.empty()) out.rotation = sample_quat(c.rotation, time);
    }

    GraphInstance::EvalResult GraphInstance::eval_clip(const GraphNode &node, NodeState &state,
                                                       const EvalContext &context, Pose &out) {
        EvalResult result;
        const Clip &clip = *def_->clips[node.clip];
        const f32 speed = node.speed * (node.speed_param != no_param ? param_value(node.speed_param) : 1.0f);
        f32 t = 0.0f;
        f32 previous = 0.0f;
        bool wrapped = false;
        if (context.has_phase) {
            t = context.phase * clip.duration;
            previous = state.previous_phase * clip.duration;
            wrapped = context.phase < state.previous_phase;
            state.previous_phase = context.phase;
        } else {
            previous = state.time;
            state.time += context.dt * speed;
            if (node.loop && clip.duration > kEpsilon) {
                const f32 w = std::fmod(state.time, clip.duration);
                wrapped = state.time >= clip.duration;
                state.time = w < 0.0f ? w + clip.duration : w;
            } else {
                state.time = std::clamp(state.time, 0.0f, clip.duration);
            }
            t = state.time;
        }
        sample_clip(*skeleton_, clip, t, false, out);

        if (context.dt > 0.0f && context.weight >= 0.5f) {
            for (const ClipEvent &e : clip.events) {
                const bool fires = wrapped ? (e.time > previous || e.time <= t) : (e.time > previous && e.time <= t);
                if (fires) events_.push_back(FiredEvent{e.name, e.time});
            }
        }

        if (def_->root_motion && root_joint_ < out.size()) {
            JointTransform now, before, origin;
            sample_root(clip, t, now);
            sample_root(clip, previous, before);
            sample_root(clip, 0.0f, origin);
            RootMotionDelta delta;
            if (wrapped) {
                JointTransform end;
                sample_root(clip, clip.duration, end);
                delta.translation = (end.translation - before.translation) + (now.translation - origin.translation);
                delta.rotation = (end.rotation * glm::inverse(before.rotation)) * (now.rotation * glm::inverse(origin.rotation));
            } else {
                delta.translation = now.translation - before.translation;
                delta.rotation = now.rotation * glm::inverse(before.rotation);
            }
            result.root = delta;
            // The pose keeps its vertical motion and its tilt; the horizontal travel and the yaw are the delta's.
            JointTransform &root = out[root_joint_];
            root.translation.x = origin.translation.x;
            root.translation.z = origin.translation.z;
            glm::quat twist_now, swing_now, twist_origin, swing_origin;
            twist_swing_y(root.rotation, twist_now, swing_now);
            twist_swing_y(origin.rotation, twist_origin, swing_origin);
            root.rotation = glm::normalize(swing_now * twist_origin);
        }
        return result;
    }

    GraphInstance::EvalResult GraphInstance::eval(NodeId id, const EvalContext &context, Pose &out) {
        const GraphNode &node = def_->nodes[id];
        NodeState &state = states_[id];
        EvalResult result;
        switch (node.type) {
            case GraphNode::Type::Clip: return eval_clip(node, state, context, out);

            case GraphNode::Type::Blend1D:
            case GraphNode::Type::Blend2D: {
                std::vector<f32> weights;
                blend_weights(node, weights);
                const bool one_d = node.type == GraphNode::Type::Blend1D;
                EvalContext child_context = context;
                if (node.sync_children) {
                    if (!context.has_phase) {
                        state.previous_phase = state.phase;
                        state.phase = std::fmod(state.phase + context.dt * node_rate(id), 1.0f);
                    }
                    child_context.has_phase = true;
                    child_context.phase = context.has_phase ? context.phase : state.phase;
                    if (!context.has_phase) {
                        // Children track the phase they were last given, so wrap detection stays per child.
                    }
                }
                bool first = true;
                f32 accumulated = 0.0f;
                Pose child_pose;
                for (usize i = 0; i < weights.size(); ++i) {
                    if (weights[i] <= kEpsilon) continue;
                    const NodeId child = one_d ? node.children_1d[i].node : node.children_2d[i].node;
                    EvalContext cc = child_context;
                    cc.weight = context.weight * weights[i];
                    if (first) {
                        const EvalResult r = eval(child, cc, out);
                        result.root = r.root;
                        first = false;
                        accumulated = weights[i];
                    } else {
                        const EvalResult r = eval(child, cc, child_pose);
                        const f32 w = weights[i] / (accumulated + weights[i]);
                        blend_poses(out, child_pose, w, out);
                        result.root = blend_root(result.root, r.root, w);
                        accumulated += weights[i];
                    }
                }
                if (first) out = skeleton_->rest_pose;
                return result;
            }

            case GraphNode::Type::Additive: {
                const EvalResult base = eval(node.a, context, out);
                Pose delta;
                EvalContext dc = context;
                dc.weight = 0.0f; // the delta layer never contributes events/root motion
                eval(node.b, dc, delta);
                Pose reference = skeleton_->rest_pose;
                const GraphNode &b = def_->nodes[node.b];
                if (b.type == GraphNode::Type::Clip) {
                    sample_clip(*skeleton_, *def_->clips[b.clip], 0.0f, false, reference);
                }
                const f32 w = node.weight_param != no_param ? param_value(node.weight_param) : node.weight;
                apply_additive(out, delta, reference, w, out);
                result.root = base.root;
                return result;
            }

            case GraphNode::Type::Mix: {
                const f32 w = std::clamp(node.weight_param != no_param ? param_value(node.weight_param) : node.weight, 0.0f, 1.0f);
                EvalContext ca = context, cb = context;
                ca.weight = context.weight * (1.0f - w);
                cb.weight = context.weight * w;
                const EvalResult ra = eval(node.a, ca, out);
                Pose other;
                const EvalResult rb = eval(node.b, cb, other);
                blend_poses(out, other, w, out);
                result.root = blend_root(ra.root, rb.root, w);
                return result;
            }

            case GraphNode::Type::StateMachine: {
                const StateMachineDef &m = node.machine;
                const f32 dt = context.dt;

                // Start a transition if one's conditions hold.
                if (state.next == no_node || state.interruptible) {
                    const u32 source = state.next != no_node ? state.next : state.current;
                    const NodeId source_node = m.states[source].node;
                    for (const Transition &t : m.transitions) {
                        if (t.to == source || (t.from != any_state && t.from != source)) continue;
                        if (t.has_exit_time) {
                            const f32 rate = node_rate(source_node);
                            if (state.state_elapsed * rate < t.exit_time) continue;
                        }
                        if (!std::all_of(t.conditions.begin(), t.conditions.end(),
                                         [this](const Condition &c) { return condition_holds(c); })) {
                            continue;
                        }
                        consume_triggers(t);
                        const bool interrupting = state.next != no_node;
                        state.snapshot_valid = interrupting && !state.last_pose.empty();
                        if (state.snapshot_valid) state.snapshot = state.last_pose;
                        if (interrupting) state.current = state.next; // the blend becomes the "from" side via snapshot
                        reset_subtree(m.states[t.to].node);
                        if (t.duration <= 0.0f) {
                            state.current = t.to;
                            state.next = no_node;
                            state.snapshot_valid = false;
                        } else {
                            state.next = t.to;
                        }
                        state.fade_time = 0.0f;
                        state.fade_duration = t.duration;
                        state.interruptible = t.can_interrupt;
                        state.state_elapsed = 0.0f;
                        break;
                    }
                }

                if (state.next == no_node) {
                    const EvalResult r = eval(m.states[state.current].node, context, out);
                    state.state_elapsed += dt;
                    state.last_pose = out;
                    state.last_root = r.root;
                    return r;
                }

                state.fade_time += dt;
                const f32 s = smoothstep01(state.fade_duration > kEpsilon ? state.fade_time / state.fade_duration : 1.0f);
                EvalContext from_context = context, to_context = context;
                from_context.weight = context.weight * (1.0f - s);
                to_context.weight = context.weight * s;
                EvalResult from_result;
                if (state.snapshot_valid) {
                    out = state.snapshot;
                } else {
                    from_result = eval(m.states[state.current].node, from_context, out);
                }
                Pose to_pose;
                const EvalResult to_result = eval(m.states[state.next].node, to_context, to_pose);
                blend_poses(out, to_pose, s, out);
                result.root = blend_root(from_result.root, to_result.root, s);
                state.state_elapsed += dt;
                if (state.fade_time >= state.fade_duration) {
                    state.current = state.next;
                    state.next = no_node;
                    state.snapshot_valid = false;
                }
                state.last_pose = out;
                state.last_root = result.root;
                return result;
            }
        }
        return result;
    }

    void GraphInstance::update(f32 delta_seconds, Pose &out) {
        events_.clear();
        root_delta_ = {};
        if (def_->layers.empty()) {
            out = skeleton_->rest_pose;
            return;
        }
        const f32 dt = std::max(delta_seconds, 0.0f);

        const auto layer_weight = [this](const Layer &l) {
            return std::clamp(l.weight_param != no_param ? param_value(l.weight_param) : l.weight, 0.0f, 1.0f);
        };

        EvalResult base = eval(def_->layers[0].root, EvalContext{.dt = dt}, out);
        const f32 base_weight = layer_weight(def_->layers[0]);
        if (base_weight < 1.0f) {
            blend_poses(skeleton_->rest_pose, out, base_weight, out);
        }
        root_delta_ = def_->root_motion ? base.root : RootMotionDelta{};

        Pose layer_pose;
        for (usize li = 1; li < def_->layers.size(); ++li) {
            const Layer &layer = def_->layers[li];
            const f32 w = layer_weight(layer);
            if (w <= 0.0f) {
                // Still advance the layer so it stays in time when it fades in.
                eval(layer.root, EvalContext{.dt = dt, .weight = 0.0f}, layer_pose);
                continue;
            }
            eval(layer.root, EvalContext{.dt = dt, .weight = w}, layer_pose);
            for (usize j = 0; j < out.size() && j < layer_pose.size(); ++j) {
                const f32 jw = w * (layer.mask.empty() ? 1.0f : (j < layer.mask.size() ? layer.mask[j] : 0.0f));
                if (jw <= 0.0f) continue;
                out[j] = layer.additive ? add_joint(out[j], layer_pose[j], reference_[j], jw)
                                        : blend(out[j], layer_pose[j], jw);
            }
        }
    }

} // namespace SFT::Animation
