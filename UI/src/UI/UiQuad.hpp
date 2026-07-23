#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#pragma endregion

namespace SFT::UI {

    enum class UiQuadKind : u32 { Rect = 0, Image = 1 };

    // GPU-visible per-instance data for the UI quad pipeline (Shaders/ui_quad.slang) — one instance
    // draws one rectangle/border/image render command from Clay's output. `kind` distinguishes only
    // solid-vs-textured (Rect covers plain rects AND bordered rects: `border_width` of all zero is
    // simply "no border to draw", so a run of mixed bordered/unbordered rects still batches as one
    // draw call — see UiRenderer.cpp).
    //
    // Field order is deliberate, not cosmetic: it must byte-match Shaders/ui_quad.slang's
    // UiQuadInstance exactly under Slang/std430 StructuredBuffer rules (vec2 aligned to 8, vec4
    // aligned to 16, whole struct padded to a multiple of its largest member's alignment) — same
    // discipline as Renderer::GlyphInstance (Renderer/TextInstance.hpp). Two vec2 pairs (16 bytes),
    // then four vec4s (64 bytes, each already 16-aligned), then `kind` plus three padding floats to
    // round the struct back up to a 16-byte multiple, lay out identically under C++'s natural
    // alignment and std430 with zero gaps.
    struct UiQuadInstance {
        glm::vec2 position{0.0f}; // top-left, pixel space
        glm::vec2 size{0.0f};
        // Per-corner radius: (topLeft, topRight, bottomLeft, bottomRight), in pixels.
        glm::vec4 corner_radius{0.0f};
        // Per-side border width: (left, right, top, bottom), in pixels.
        glm::vec4 border_width{0.0f};
        glm::vec4 fill_color{1.0f};
        glm::vec4 border_color{0.0f};
        glm::vec2 uv_min{0.0f};
        glm::vec2 uv_max{1.0f};
        f32 kind = 0.0f; // UiQuadKind, encoded as float to match the storage-buffer layout uniformly.
        f32 _pad0 = 0.0f;
        f32 _pad1 = 0.0f;
        f32 _pad2 = 0.0f;
    };

} // namespace SFT::UI
