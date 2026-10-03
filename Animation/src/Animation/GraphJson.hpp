#pragma once

#include <Animation/Graph.hpp>

#include <expected>
#include <memory>
#include <span>
#include <string>

namespace SFT::Animation {

    /// Parses an animation graph document.
    ///
    /// ```json
    /// {
    ///   "parameters": [{"name": "speed", "type": "float", "default": 0}, {"name": "jump", "type": "trigger"}],
    ///   "nodes": [
    ///     {"id": "idle", "type": "clip", "clip": "Idle"},
    ///     {"id": "walk", "type": "clip", "clip": "Walk", "speed": 1.0, "loop": true},
    ///     {"id": "loco", "type": "blend1d", "param": "speed",
    ///      "children": [{"threshold": 0, "node": "idle"}, {"threshold": 1, "node": "walk"}]},
    ///     {"id": "machine", "type": "state_machine", "entry": "Locomotion",
    ///      "states": [{"name": "Locomotion", "node": "loco"}, {"name": "Jump", "node": "jumpclip"}],
    ///      "transitions": [{"from": "Locomotion", "to": "Jump", "duration": 0.15,
    ///                       "conditions": [{"param": "jump", "op": "trigger"}]},
    ///                      {"from": "Jump", "to": "Locomotion", "exit_time": 0.9, "duration": 0.2}]}
    ///   ],
    ///   "layers": [{"node": "machine"},
    ///              {"node": "wave", "additive": true, "weight": 0.5,
    ///               "mask": {"joints": ["UpperArm.R"], "include_children": true}}],
    ///   "root_motion": {"enabled": true, "joint": "Hips"}
    /// }
    /// ```
    ///
    /// Node types: `clip`, `blend1d`, `blend2d` (children carry `x`/`y`, params `param_x`/`param_y`), `additive`
    /// (`base`, `delta`), `mix` (`a`, `b`), `state_machine`. `from` may be "*" or omitted for any state. Condition ops:
    /// `>`, `<`, `>=`, `<=`, `==`, `!=`, `true`, `false`, `trigger`. `//` and `/* */` comments are accepted.
    /// Clips are looked up by name in `clips`; layer masks need the skeleton.
    [[nodiscard]] std::expected<GraphDef, UString> load_graph_json(
        const ustr &text, std::span<const std::shared_ptr<const Clip>> clips, const Skeleton &skeleton);

} // namespace SFT::Animation
