# Animation

Everything needed to take animation from any authoring pipeline and apply it to game objects.

## 1. Getting content in

`Engine::import_model(assets, path, shader)` picks the importer from the file extension and returns an
`ImportedScene` (models, node instances, lights, skins, clips). `Engine::import_animations(assets, path)` loads only
the motion from a file.

| You use | Export | Notes |
|---|---|---|
| **Blender** | File > Export > glTF 2.0 (`.glb`). Enable *Animation* (all actions / NLA tracks), *Skinning*, *Shape Keys*. | Y-up is the default. Constraints/drivers are baked by the exporter. 4 influences per vertex. |
| **Maya / 3ds Max / Cinema 4D / Houdini** | FBX 2020 binary, *Y up*, *Bake animation*, *Skins*, *Blend shapes*. | Units and axes are converted on import (metres, right-handed Y-up). One FBX *take* becomes one clip. |
| **Mixamo** | FBX binary. "With Skin" for the character, "Without Skin" for extra animations. | Load animation-only files with `import_animations` and `adapt_clips` onto the character. |
| **Unity / Unreal / Godot** | Their glTF or FBX exporters. | |
| **VRM / Sketchfab / marketplace** | `.glb` or `.fbx`. | Humanoid bones are recognised by name for retargeting. |
| **Blockbench** | Export glTF. | |
| **Mocap (CMU, Rokoko, Xsens, Perception Neuron)** | `.bvh` | Centimetres are converted to metres automatically; `BvhOptions::z_up` for Z-up files. |
| **Wavefront** | `.obj` (+`.mtl`) | Static meshes. |

Supported content: skinned meshes (4 influences/vertex), blend shapes / morph targets (sparse, with weight
animation), node-hierarchy animation (doors, props, rigid parts), STEP / LINEAR / CUBICSPLINE interpolation (FBX is baked
to 30 Hz linear), several clips per file, clip event markers, PBR materials and their textures.

Not supported yet: FBX metallic/roughness/AO texture maps (the material factors are used), glTF
`KHR_animation_pointer`, USD/Alembic, FBX per-mesh bind poses that disagree on a shared skeleton.

## 2. Putting it on screen

```cpp
auto scene = Engine::import_model(assets, "hero.fbx", shader);
Engine::SpawnedModel hero = Engine::spawn_imported(world, assets, *scene, {.transform = place_at});
```

`spawn_imported` handles every case: skinned characters get a private deformable copy and a `SkeletonAnimator`,
blend-shape meshes the same without a skeleton, and plain animated nodes follow a shared `HierarchyRig`. Skinning and
blend shapes run on the GPU (compute pass `mesh_skinning`); there is a CPU fallback when compute is unavailable.

Playback by name, from game code or the C ABI (`sturdy_animation_*`):

```cpp
Engine::animation_play(world, hero.animators[0], "Run", /*loop=*/true, /*speed=*/1.0f);
```

## 3. Animations from other files

```cpp
auto extra = Engine::import_animations(assets, "mixamo_walk.fbx");     // or .bvh / .glb
Engine::animation_adopt_clips(world, hero.animators[0], *extra);       // retargets onto the hero's skeleton
Engine::animation_play(world, hero.animators[0], "mixamo_walk");
```

`Animation::adapt_clip` chooses by-name remapping when the rigs share a layout and a humanoid retarget otherwise
(Mixamo, Unreal, 3ds Max Biped, Rigify, VRM bone names are recognised; hips travel is scaled by body size; T-pose vs
A-pose differences are handled through world-space rotation deltas).

## 4. Animation graphs

Parameters feed clip, blend (1D/2D), additive, mix and state-machine nodes; layers add masked overrides (upper body)
or additive layers (breathing, leaning); clips can carry events; the root joint's travel can be extracted as root
motion and applied to the entity.

```json
{
  "parameters": [{"name": "speed", "type": "float"}, {"name": "jump", "type": "trigger"}],
  "nodes": [
    {"id": "idle", "type": "clip", "clip": "Idle"},
    {"id": "walk", "type": "clip", "clip": "Walk"},
    {"id": "run",  "type": "clip", "clip": "Run"},
    {"id": "loco", "type": "blend1d", "param": "speed",
     "children": [{"threshold": 0, "node": "idle"}, {"threshold": 1, "node": "walk"}, {"threshold": 3, "node": "run"}]},
    {"id": "jumpclip", "type": "clip", "clip": "Jump", "loop": false},
    {"id": "machine", "type": "state_machine", "entry": "Locomotion",
     "states": [{"name": "Locomotion", "node": "loco"}, {"name": "Jump", "node": "jumpclip"}],
     "transitions": [
       {"from": "Locomotion", "to": "Jump", "duration": 0.15, "conditions": [{"param": "jump", "op": "trigger"}]},
       {"from": "Jump", "to": "Locomotion", "exit_time": 0.9, "duration": 0.2}]}
  ],
  "layers": [{"node": "machine"}],
  "root_motion": {"enabled": true, "joint": "Hips"}
}
```

```cpp
Engine::animation_attach_graph_json(world, hero.animators[0], json_text);
Engine::animation_set_float(world, hero.animators[0], "speed", 2.0f);
Engine::animation_set_trigger(world, hero.animators[0], "jump");
// Events arrive as Engine::AnimationEvent through the ECS event stream.
```

Graphs can also be built in code with `Animation::GraphBuilder`; one `GraphDef` can drive any number of characters.

## 5. Inverse kinematics

`TwoBoneIk` (legs, arms, with a pole vector), `LookAtIk` (head tracking with an angle limit) and `ChainIk` (FABRIK for
spines, tails, ropes) are solved after the clip or graph every tick: add them to the animator's `two_bone_iks` /
`look_ats` / `chain_iks` lists (or `animation_add_two_bone_ik` / `sturdy_animation_ik_add_two_bone`) and update their
`target` and `weight` each frame from gameplay (ground raycasts for feet, a camera for the head).

## 6. Layout

| File | Purpose |
|---|---|
| `Skeleton`, `Clip`, `Morph` | data types, sampling, blending, skinning matrices, sparse blend shapes |
| `Graph`, `GraphJson` | animation graphs and their JSON format |
| `Retarget` | humanoid mapping, retargeting, remap by name |
| `Ik` | two-bone, look-at and FABRIK solvers |
| `Bvh` | mocap parser |
| `Engine/FbxImport`, `GltfImport`, `ModelImport` | importers and the front door |
| `Engine/EcsAnimation`, `SpawnImported` | components, systems, control functions, spawning |
| `Renderer/RendererSkinning`, `Shaders/mesh_skinning.slang` | GPU skinning and blend shapes |
