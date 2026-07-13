// `Sturdy.RHI` — the engine's render-hardware-interface.
//
// A single API-agnostic vocabulary for *describing* GPU work: opaque typed resource handles
// (:Handles), descriptor structs and their enums for buffers/textures/samplers (:Resources),
// shaders and vertex layouts (:Shader), the binding model (:Binding), pipeline state (:Pipeline),
// ray-tracing descriptors (:RayTracing), queue/timeline submission (:Queues/:Execution), command
// recording via encoder interfaces (:Command), and presentation (:Swapchain) — all tied together by
// the abstract `RhiDevice` (:Device), which a concrete backend implements. Optional hardware
// capabilities are negotiated through `:Features` (the `Feature` enum + `FeatureSet`), API/vendor
// extension IDs through `:Extensions`, and the `:Adapter` seam (`RhiInstance` → `RhiAdapter` →
// `RhiDevice`), where an app states its hard requirements and reads back what a GPU actually
// supports. `:Flags` supplies the bitmask-operator machinery the flag enums opt into; `:Error` the
// exception-free `RhiResult`/`RhiExpected` shape; `:Types` the shared formats and geometry PODs.
//
// It depends on nothing but `Sturdy.Foundation` (scalar types, logging) and GLM (vector math for
// colors/clears) — deliberately no Vulkan, no Platform, no Core. That direction matters: the RHI is
// the neutral contract, and the graphics backend (Core's Vulkan backend today; Metal/D3D12/WebGPU
// later) is what implements it. Higher layers describe rendering purely in these terms and never
// name a graphics-API symbol; swapping the backend never touches a line of that description.
//
// This is the interface only — no backend lives here. Each backend exposes its own factory (outside
// this module — `RhiInstance` → `RhiAdapter` → `RhiDevice`) returning a `std::unique_ptr<RhiInstance>`.
//
// ─── Design law: the maximal feature set ─────────────────────────────────────────────────────────
//
// This RHI is deliberately a *maximal union*, not a minimal intersection. The goal is that anything
// any target graphics API can do is expressible here — if one backend has a capability, the RHI can
// describe it. That is the opposite of WebGPU/wgpu/sokol (which expose only the common subset) and
// the same stance as The Forge / Diligent / NVRHI / Godot's RenderingDevice.
//
// Three tiers keep that from meaning "unportable chaos":
//   1. Base — capabilities every backend guarantees (dynamic rendering, timeline-style sync,
//      compute, indexed/instanced draws, a common format set). Always available, never queried.
//   2. Gated — optional capabilities named in `Feature` (:Features) and negotiated per-adapter. The
//      command/descriptor vocabulary to *use* each one lives here too (e.g. ray tracing, mesh
//      shaders, render bundles, multi-draw-indirect-count); code guards it behind
//      `RhiDevice::is_enabled(...)`.
//   3. Extended — API/vendor-specific features use `ExtensionId` (:Extensions) so Metal/D3D/Vulkan
//      one-offs can still be expressed, required, optionally enabled, or safely skipped.
//   4. Explicit — the RHI is an explicit-synchronization interface (barriers in :Barrier plus
//      timeline submission in :Execution), not an auto-tracked one, because auto-tracking cannot
//      express everything the underlying APIs can.
//
// Growing toward "complete" is a method, not a single commit: every new capability is added as base,
// or as a `Feature` + its vocabulary, and adding a new graphics API only ever *appends* enum values
// and backend mappings — the shapes above never have to change to accommodate it.
module;
#include <Foundation/Foundation.hpp>

export module Sturdy.RHI;

export import :Error;
export import :Flags;
export import :Types;
export import :Handles;
export import :Features;
export import :Extensions;
export import :Queues;
export import :Resources;
export import :Shader;
export import :Binding;
export import :Pipeline;
export import :RayTracing;
export import :Barrier;
export import :Queries;
export import :Execution;
export import :Command;
export import :Swapchain;
export import :HdrDisplay;
export import :Device;
export import :Adapter;
export import :Backend;
export import :Selection;
