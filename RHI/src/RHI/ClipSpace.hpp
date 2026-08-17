#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/mat4x4.hpp>
#pragma endregion

#include "Device.hpp"

namespace SFT::RHI {

    // Bakes the main-path clip-space Y reconciliation (see Viewport's own doc comment in Types.hpp)
    // into an already-built matrix instead of relying on a shader-side sturdy_clip_position() call.
    // Vulkan's NDC is this engine's canonical convention, so `m` passes through unchanged there;
    // D3D12 (and, pre-emptively, Metal/WebGpu, which share D3D12's +Y-up NDC once implemented) get a
    // copy with row 1 negated across every column — the general form a post-hoc flip of an arbitrary,
    // already-combined matrix needs. This is deliberately NOT the single-entry `m[1][1] *= -1.0f`
    // shortcut Engine::Camera::flip_projection_y_ uses: that shortcut only works because it runs
    // before Camera::projection_matrix() adds its lens-shift/jitter terms into column 2 — a matrix
    // that has already been combined with a view matrix (or anything else) needs the full row
    // negated, or the flip silently only touches part of what it's supposed to.
    [[nodiscard]] glm::mat4 gpu_clip_flip(const glm::mat4 &m, BackendType backend) noexcept;

    // Scalar form of the same reconciliation for screen-space UI vertex shaders that build clip
    // position from scratch (pixel space -> NDC) rather than through an already-combined matrix —
    // +1.0 on Vulkan, -1.0 on D3D12/Metal/WebGpu. A shader multiplies its computed NDC.y by this
    // value (uploaded via the pass's existing push-constant/uniform block) instead of branching on
    // STURDY_CLIP_Y_SIGN.
    [[nodiscard]] f32 gpu_clip_y_sign(BackendType backend) noexcept;

} // namespace SFT::RHI
