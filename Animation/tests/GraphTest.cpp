#include <Animation/Graph.hpp>
#include <Animation/GraphJson.hpp>

#include <cmath>
#include <iostream>

using namespace SFT::Animation;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }
    bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

    std::shared_ptr<Skeleton> make_skeleton() {
        auto s = std::make_shared<Skeleton>();
        s->names = {"Hips", "Spine", "Arm"};
        s->parents = {no_joint, 0, 1};
        s->rest_pose = {JointTransform{}, JointTransform{.translation = {0, 1, 0}}, JointTransform{.translation = {0, 1, 0}}};
        s->inverse_bind.assign(3, glm::mat4(1.0f));
        return s;
    }

    // A clip whose Hips translation.x runs from `from` to `to` over `duration`, and whose Arm rotates about Z.
    std::shared_ptr<Clip> make_clip(const char *name, float from, float to, float duration, float arm_degrees = 0.0f) {
        auto c = std::make_shared<Clip>();
        c->name = name;
        c->channels.resize(3);
        auto &t = c->channels[0].translation;
        t.times = {0.0f, duration};
        t.values = {from, 0, 0, to, 0, 0};
        if (arm_degrees != 0.0f) {
            auto &r = c->channels[2].rotation;
            const float h = glm::radians(arm_degrees) * 0.5f;
            r.times = {0.0f, duration};
            r.values = {0, 0, 0, 1, 0, 0, std::sin(h), std::cos(h)};
        }
        c->recompute_duration();
        return c;
    }
} // namespace

int main() {
    const auto skeleton = make_skeleton();
    const auto idle = make_clip("Idle", 0, 0, 1.0f);
    const auto walk = make_clip("Walk", 0, 2, 1.0f);
    const auto run = make_clip("Run", 0, 6, 1.0f);

    // ---- 1D blend between clips -------------------------------------------------------------
    {
        GraphBuilder b;
        const u32 speed = b.param_float("speed");
        const u32 ci = b.add_clip(idle), cw = b.add_clip(walk), cr = b.add_clip(run);
        const NodeId blend = b.blend_1d(speed, {{0.0f, b.clip_node(ci)}, {1.0f, b.clip_node(cw)}, {2.0f, b.clip_node(cr)}});
        b.add_layer(Layer{.root = blend});
        auto def = std::make_shared<GraphDef>(b.build());
        check(validate_graph(*def).empty(), "valid graph validates");

        GraphInstance g(def, skeleton);
        Pose pose;
        g.set_float("speed", 0.5f);
        g.update(0.5f, pose);
        // Half walk / half idle, sampled at phase 0.5: hips x = 0.5 * 1 (walk at t=.5) + 0.5 * 0.
        check(near(pose[0].translation.x, 0.5f), "blend between idle and walk");
        g.set_float("speed", 1.5f);
        g.update(0.0f, pose);
        check(near(pose[0].translation.x, 0.5f * 1.0f + 0.5f * 3.0f), "blend between walk and run at same phase");
        g.set_float("speed", 99.0f);
        g.update(0.0f, pose);
        check(near(pose[0].translation.x, 3.0f), "parameter beyond the last threshold clamps");
    }

    // ---- state machine: trigger transition with cross-fade ----------------------------------
    {
        GraphBuilder b;
        const u32 go = b.param_trigger("go");
        const NodeId n_idle = b.clip_node(b.add_clip(idle), true, 1.0f, "idle");
        const NodeId n_run = b.clip_node(b.add_clip(run), true, 1.0f, "run");
        StateMachineDef m;
        m.states = {{"Idle", n_idle}, {"Run", n_run}};
        m.transitions.push_back(Transition{.from = 0, .to = 1, .conditions = {{go, CompareOp::Triggered, 0}}, .duration = 0.5f});
        const NodeId sm = b.state_machine(m);
        b.add_layer(Layer{.root = sm});
        auto def = std::make_shared<GraphDef>(b.build());
        check(validate_graph(*def).empty(), "state machine graph validates");

        GraphInstance g(def, skeleton);
        Pose pose;
        g.update(0.1f, pose);
        check(g.current_state_name() == "Idle" && !g.in_transition(), "starts in the entry state");
        g.set_trigger("go");
        g.update(0.25f, pose);
        check(g.in_transition() && g.current_state_name() == "Run", "trigger starts a transition");
        check(g.get_param("go") == 0.0f, "trigger is consumed");
        // Half way through the fade: smoothstep(0.5) = 0.5, run has advanced 0.25 -> x = 1.5; idle 0.
        check(near(pose[0].translation.x, 0.5f * 1.5f), "mid-fade pose blends both states");
        g.update(0.5f, pose);
        check(!g.in_transition(), "fade completes");
        g.update(0.0f, pose);
        check(near(pose[0].translation.x, 6.0f * 0.75f), "after the fade only the new state plays");
    }

    // ---- exit time ---------------------------------------------------------------------------
    {
        GraphBuilder b;
        const NodeId a = b.clip_node(b.add_clip(walk), false, 1.0f, "a");
        const NodeId c = b.clip_node(b.add_clip(idle), true, 1.0f, "c");
        StateMachineDef m;
        m.states = {{"A", a}, {"B", c}};
        m.transitions.push_back(Transition{.from = 0, .to = 1, .has_exit_time = true, .exit_time = 0.8f, .duration = 0.0f});
        b.add_layer(Layer{.root = b.state_machine(m)});
        GraphInstance g(std::make_shared<GraphDef>(b.build()), skeleton);
        Pose pose;
        g.update(0.5f, pose);
        check(g.current_state_name() == "A", "exit time not reached yet");
        g.update(0.4f, pose);
        g.update(0.0f, pose);
        check(g.current_state_name() == "B", "exit time reached, instant transition");
    }

    // ---- events ------------------------------------------------------------------------------
    {
        auto stepping = make_clip("Step", 0, 1, 1.0f);
        stepping->events = {{0.25f, "left_foot"}, {0.75f, "right_foot"}};
        GraphBuilder b;
        b.add_layer(Layer{.root = b.clip_node(b.add_clip(stepping))});
        GraphInstance g(std::make_shared<GraphDef>(b.build()), skeleton);
        Pose pose;
        g.update(0.5f, pose);
        check(g.events().size() == 1 && g.events()[0].name == "left_foot", "event fires when crossed");
        g.update(0.4f, pose);
        check(g.events().size() == 1 && g.events()[0].name == "right_foot", "second event");
        g.update(0.4f, pose); // 0.9 -> 1.3 (wraps to 0.3): crosses the end and 0.25
        check(g.events().size() == 1 && g.events()[0].name == "left_foot", "event across the loop wrap");
        g.update(0.0f, pose);
        check(g.events().empty(), "no events without time passing");
    }

    // ---- root motion -------------------------------------------------------------------------
    {
        GraphBuilder b;
        b.add_layer(Layer{.root = b.clip_node(b.add_clip(walk))});
        b.enable_root_motion("Hips");
        GraphInstance g(std::make_shared<GraphDef>(b.build()), skeleton);
        Pose pose;
        g.update(0.25f, pose);
        check(near(g.root_motion().translation.x, 0.5f), "root motion delta");
        check(near(pose[0].translation.x, 0.0f), "root translation removed from the pose");
        g.update(0.5f, pose);
        check(near(g.root_motion().translation.x, 1.0f), "second delta");
        g.update(0.5f, pose); // crosses the loop end at t=1
        check(near(g.root_motion().translation.x, 1.0f), "delta across the loop wrap (0.75->1 plus 0->0.25)");
    }

    // ---- masked override and additive layers -------------------------------------------------
    {
        const auto wave = make_clip("Wave", 0, 0, 1.0f, 90.0f);
        GraphBuilder b;
        const NodeId base = b.clip_node(b.add_clip(walk));
        const NodeId upper = b.clip_node(b.add_clip(wave));
        b.add_layer(Layer{.root = base});
        const std::vector<std::string> names{"Arm"};
        b.add_layer(Layer{.root = upper, .mask = make_joint_mask(*skeleton, names)});
        GraphInstance g(std::make_shared<GraphDef>(b.build()), skeleton);
        Pose pose;
        g.update(0.9f, pose);
        check(glm::degrees(glm::angle(pose[2].rotation)) > 80.0f, "masked layer overrides the arm");
        check(near(glm::degrees(glm::angle(pose[1].rotation)), 0.0f), "unmasked joints keep the base layer");

        GraphBuilder add;
        const NodeId add_base = add.clip_node(add.add_clip(idle));
        const NodeId add_delta = add.clip_node(add.add_clip(wave));
        add.add_layer(Layer{.root = add_base});
        add.add_layer(Layer{.root = add_delta, .additive = true, .weight = 0.5f});
        GraphInstance ga(std::make_shared<GraphDef>(add.build()), skeleton);
        ga.update(0.9f, pose);
        check(near(glm::degrees(glm::angle(pose[2].rotation)), 40.5f, 1.0f), "additive layer at half weight");
    }

    // ---- JSON --------------------------------------------------------------------------------
    {
        const std::vector<std::shared_ptr<const Clip>> library{idle, walk, run};
        const char *document = R"({
            // comments are allowed
            "parameters": [{"name": "speed", "type": "float"}, {"name": "go", "type": "trigger"}],
            "nodes": [
                {"id": "idle", "type": "clip", "clip": "Idle"},
                {"id": "walk", "type": "clip", "clip": "Walk"},
                {"id": "loco", "type": "blend1d", "param": "speed",
                 "children": [{"threshold": 0, "node": "idle"}, {"threshold": 1, "node": "walk"}]},
                {"id": "run", "type": "clip", "clip": "Run"},
                {"id": "sm", "type": "state_machine", "entry": "Loco",
                 "states": [{"name": "Loco", "node": "loco"}, {"name": "Run", "node": "run"}],
                 "transitions": [{"to": "Run", "duration": 0.1, "conditions": [{"param": "go", "op": "trigger"}]}]}
            ],
            "layers": [{"node": "sm"}],
            "root_motion": {"enabled": true, "joint": "Hips"}
        })";
        auto loaded = load_graph_json(document, library, *skeleton);
        check(loaded.has_value(), "JSON graph loads");
        if (!loaded) std::cerr << loaded.error() << '\n';
        if (loaded) {
            check(loaded->root_motion && loaded->nodes.size() == 5 && loaded->layers.size() == 1, "JSON structure");
            GraphInstance g(std::make_shared<GraphDef>(*loaded), skeleton);
            Pose pose;
            g.set_float("speed", 1.0f);
            g.update(0.5f, pose);
            check(g.current_state_name() == "Loco", "loaded graph runs");
            g.set_trigger("go");
            g.update(0.01f, pose);
            check(g.current_state_name() == "Run", "loaded transition fires from any state");
        }
        check(!load_graph_json("{ not json", library, *skeleton).has_value(), "syntax error is reported");
        auto missing = load_graph_json(R"({"nodes":[{"id":"x","type":"clip","clip":"Nope"}]})", library, *skeleton);
        check(!missing && missing.error().find("Nope") != std::string::npos, "unknown clip named in the error");
        auto cyclic = load_graph_json(
            R"({"nodes":[{"id":"a","type":"mix","a":"b","b":"b"},{"id":"b","type":"mix","a":"a","b":"a"}],"layers":[{"node":"a"}]})",
            library, *skeleton);
        check(!cyclic && cyclic.error().find("cycle") != std::string::npos, "cycles are rejected");
    }

    return failures == 0 ? 0 : 1;
}
