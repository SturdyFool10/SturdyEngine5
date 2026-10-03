#pragma once

#include <Animation/Clip.hpp>
#include <Animation/Skeleton.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

/// Data-driven animation graphs: parameters feed clip, blend, additive and state-machine nodes; layers stack
/// the results with optional per-joint masks. A `GraphDef` is immutable and shared; each animated character owns a
/// `GraphInstance` holding its parameters and playback state. Graphs can be built in code (`GraphBuilder`) or loaded
/// from a JSON document (`load_graph_json`), so authoring tools in any language can produce them.
namespace SFT::Animation {

    using NodeId = u32;
    inline constexpr NodeId no_node = 0xFFFFFFFFu;
    inline constexpr u32 no_param = 0xFFFFFFFFu;
    /// `Transition::from` value meaning "from any state".
    inline constexpr u32 any_state = 0xFFFFFFFFu;

    enum class ParamType : u8 { Float, Bool, Int, Trigger };

    struct ParamDef {
        UString name;
        ParamType type = ParamType::Float;
        f32 default_value = 0.0f;
    };

    enum class CompareOp : u8 { Greater, Less, GreaterEqual, LessEqual, Equal, NotEqual, IsTrue, IsFalse, Triggered };

    struct Condition {
        u32 param = no_param;
        CompareOp op = CompareOp::Greater;
        f32 value = 0.0f;
    };

    struct Transition {
        u32 from = any_state;
        u32 to = 0;
        /// All of these must hold (an empty list holds trivially, which with `has_exit_time` gives "play out, then go").
        std::vector<Condition> conditions;
        bool has_exit_time = false;
        /// Fraction of the source state's duration after which the transition may fire.
        f32 exit_time = 1.0f;
        /// Cross-fade length in seconds.
        f32 duration = 0.2f;
        /// May this transition start while another fade is still running (the running blend is frozen into a
        /// snapshot and faded out).
        bool can_interrupt = true;
    };

    struct State {
        UString name;
        NodeId node = no_node;
    };

    struct StateMachineDef {
        std::vector<State> states;
        u32 entry = 0;
        std::vector<Transition> transitions;
    };

    struct BlendChild1D {
        f32 threshold = 0.0f;
        NodeId node = no_node;
    };

    struct BlendChild2D {
        glm::vec2 position{0.0f};
        NodeId node = no_node;
    };

    struct GraphNode {
        enum class Type : u8 { Clip, Blend1D, Blend2D, Additive, Mix, StateMachine };
        Type type = Type::Clip;
        UString name;

        // Clip
        u32 clip = 0;
        f32 speed = 1.0f;
        bool loop = true;
        u32 speed_param = no_param; // multiplies `speed` when set

        // Blend1D / Blend2D (children are synchronised on a shared normalised phase)
        u32 param_x = no_param;
        u32 param_y = no_param;
        std::vector<BlendChild1D> children_1d;
        std::vector<BlendChild2D> children_2d;
        bool sync_children = true;

        // Additive (base + delta of `b` against its own first frame) and Mix (lerp a -> b)
        NodeId a = no_node;
        NodeId b = no_node;
        f32 weight = 1.0f;
        u32 weight_param = no_param;

        StateMachineDef machine;
    };

    struct Layer {
        NodeId root = no_node;
        /// Per-joint influence in [0, 1]; empty means every joint.
        std::vector<f32> mask;
        bool additive = false;
        f32 weight = 1.0f;
        u32 weight_param = no_param;
    };

    struct GraphDef {
        std::vector<ParamDef> params;
        std::vector<GraphNode> nodes;
        std::vector<Layer> layers;
        /// Clips every `Clip` node indexes into; bound to the skeleton the graph is instanced with.
        std::vector<std::shared_ptr<const Clip>> clips;
        /// Root-motion extraction: the root joint's horizontal translation and yaw are removed from the pose and
        /// reported as a per-update delta instead.
        bool root_motion = false;
        UString root_joint; // empty = joint 0

        [[nodiscard]] u32 find_param(const ustr &name) const noexcept;
        [[nodiscard]] u32 find_state(const StateMachineDef &machine, const ustr &name) const noexcept;
    };

    /// Checks node/param/state/clip indices and rejects cycles; empty on success, otherwise a description of the
    /// first problem. `GraphInstance` assumes a valid graph.
    [[nodiscard]] UString validate_graph(const GraphDef &def);

    /// Fluent construction of a `GraphDef`. Every `add_*`/`param_*` returns the new id.
    class GraphBuilder {
      public:
        u32 param_float(UString name, f32 default_value = 0.0f);
        u32 param_bool(UString name, bool default_value = false);
        u32 param_int(UString name, i32 default_value = 0);
        u32 param_trigger(UString name);

        u32 add_clip(std::shared_ptr<const Clip> clip);

        NodeId clip_node(u32 clip, bool loop = true, f32 speed = 1.0f, UString name = {});
        NodeId blend_1d(u32 param, std::vector<BlendChild1D> children, UString name = {});
        NodeId blend_2d(u32 param_x, u32 param_y, std::vector<BlendChild2D> children, UString name = {});
        NodeId additive(NodeId base, NodeId delta, f32 weight = 1.0f, u32 weight_param = no_param, UString name = {});
        NodeId mix(NodeId a, NodeId b, f32 weight = 0.5f, u32 weight_param = no_param, UString name = {});
        NodeId state_machine(StateMachineDef machine, UString name = {});

        void add_layer(Layer layer);
        void enable_root_motion(UString joint = {});

        [[nodiscard]] GraphDef build() const { return def_; }
        [[nodiscard]] GraphDef &def() noexcept { return def_; }

      private:
        GraphDef def_;
    };

    struct FiredEvent {
        UString name;
        /// Time within the clip that fired it.
        f32 clip_time = 0.0f;
    };

    /// Movement the extracted root made this update, in the root joint's parent space.
    struct RootMotionDelta {
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    /// Per-joint weights for a layer: 1 for the named joints (and, with `include_children`, everything below them), 0
    /// elsewhere.
    [[nodiscard]] std::vector<f32> make_joint_mask(const Skeleton &skeleton, std::span<const UString> root_names,
                                                   bool include_children = true, f32 value = 1.0f);

    /// Runtime state of one character's graph.
    class GraphInstance {
      public:
        GraphInstance(std::shared_ptr<const GraphDef> def, std::shared_ptr<const Skeleton> skeleton);

        // Parameters, by name or index (names that do not exist are ignored / read as zero).
        void set_float(const ustr &name, f32 value);
        void set_bool(const ustr &name, bool value);
        void set_int(const ustr &name, i32 value);
        void set_trigger(const ustr &name);
        void set_param(u32 index, f32 value);
        [[nodiscard]] f32 get_param(const ustr &name) const;

        /// Advances every active node by `delta_seconds` and writes the final pose.
        void update(f32 delta_seconds, Pose &out);

        /// Events fired by the most recent `update`.
        [[nodiscard]] const std::vector<FiredEvent> &events() const noexcept { return events_; }
        /// Root motion produced by the most recent `update` (identity when extraction is off).
        [[nodiscard]] const RootMotionDelta &root_motion() const noexcept { return root_delta_; }

        /// Name of the state the first state machine node is in (or fading toward); empty without one.
        [[nodiscard]] const UString &current_state_name() const;
        /// True while a state machine is cross-fading.
        [[nodiscard]] bool in_transition() const;

        /// Restarts every node and the state machines at their entry states; parameters are kept.
        void reset();

        [[nodiscard]] const GraphDef &def() const noexcept { return *def_; }

      private:
        struct NodeState {
            f32 time = 0.0f;
            f32 phase = 0.0f;
            f32 previous_time = 0.0f;
            f32 previous_phase = 0.0f;
            // State machine
            u32 current = 0;
            u32 next = no_node;
            f32 fade_time = 0.0f;
            f32 fade_duration = 0.0f;
            f32 state_elapsed = 0.0f;
            bool interruptible = true;
            bool snapshot_valid = false;
            Pose snapshot;
            Pose last_pose;
            RootMotionDelta last_root;
        };
        struct EvalContext {
            f32 dt = 0.0f;
            bool has_phase = false;
            f32 phase = 0.0f;
            f32 weight = 1.0f;
        };
        struct EvalResult {
            RootMotionDelta root;
            f32 phase = 0.0f;
        };

        EvalResult eval(NodeId id, const EvalContext &context, Pose &out);
        EvalResult eval_clip(const GraphNode &node, NodeState &state, const EvalContext &context, Pose &out);
        void reset_subtree(NodeId id);
        [[nodiscard]] f32 node_rate(NodeId id) const;
        void blend_weights(const GraphNode &node, std::vector<f32> &weights) const;
        [[nodiscard]] f32 param_value(u32 index) const;
        [[nodiscard]] bool condition_holds(const Condition &condition) const;
        void consume_triggers(const Transition &transition);
        void sample_root(const Clip &clip, f32 time, JointTransform &out) const;

        std::shared_ptr<const GraphDef> def_;
        std::shared_ptr<const Skeleton> skeleton_;
        std::vector<f32> params_;
        std::vector<NodeState> states_;
        std::vector<FiredEvent> events_;
        RootMotionDelta root_delta_;
        u32 root_joint_ = 0;
        Pose scratch_a_;
        Pose scratch_b_;
        Pose reference_;
    };

} // namespace SFT::Animation
