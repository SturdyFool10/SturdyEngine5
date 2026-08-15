#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <glm/vec2.hpp>
#include <string>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>

#include "Style.hpp"

// Plain data types behind Context::custom_element() (Clay's CUSTOM render command,
// plans/clay-ui-renderer.md's Phase 3: shader-driven styling) — deliberately Clay-free, mirroring
// UiQuad.hpp's own separation, so UI::UiCustomElementPipeline (the GPU side) never needs to see a
// Clay type either.
namespace SFT::UI {

    // Every custom-element shader's single push-constant struct must begin with exactly these three
    // fields, in this order and byte layout (see Shaders/sturdy_common.slang's uiQuadClipPosition(),
    // which a custom shader's own vertexMain calls with these three fields) —
    // UiCustomElementPipeline writes them, then appends CustomShaderRef::push_constants right after.
    struct UiElementConstants {
        glm::vec2 position{0.0f}; // pixel-space top-left, Y down (Clay's convention)
        glm::vec2 size{0.0f}; // pixel-space width/height
        glm::vec2 viewport_size{0.0f};
    };

    // A Slang shader an app supplies for one Context::custom_element() call — the shader-driven-
    // styling seam Clay's CUSTOM render command exists for. Both entry points must live in
    // `shader_path`'s own file (same convention Renderer::CustomPostProcessEffect already uses):
    // `vertexMain` MUST build its quad via Shaders/sturdy_common.slang's uiQuadClipPosition()/
    // uiQuadCorner(), given a `[[push_constant]] ConstantBuffer<T>` whose first three fields match
    // UiElementConstants exactly (see its own doc comment).
    //
    // uiQuadClipPosition() is not just a convenience: it applies the engine's clip-space ABI (see
    // RHI::Viewport), which reconciles Vulkan's NDC with D3D12's. A vertexMain that writes
    // SV_Position directly renders correctly on Vulkan and vertically mirrored on D3D12. If a custom
    // shader must compute clip space itself, pass the result through sturdy_clip_position().
    //
    // v1 scope: push-constant parameters only, no texture/buffer bindings — a shader that declares
    // one fails at UiRenderer::prepare() time with a clear error rather than silently drawing wrong.
    // No concrete need for a textured custom element has come up yet (plans/clay-ui-renderer.md);
    // this can grow to support resource bindings if/when one does.
    struct CustomShaderRef {
        std::string shader_path;
        std::string module_name;
        std::string fragment_entry_point = "fragmentMain";
        // Appended immediately after the host-written UiElementConstants prefix in the shader's
        // push-constant block — total bytes sent is UiElementConstants' size plus this size, which
        // must equal the shader's own reflected push-constant struct size exactly (UiRenderer's
        // prepare()/draw() validates this and fails loudly on a mismatch rather than drawing wrong).
        //
        // Gotcha: Slang packs push-constant structs HLSL-cbuffer-style — a float4 (or anything with
        // 16-byte alignment) is pushed to the next 16-byte boundary rather than packed tightly right
        // after UiElementConstants' 24 bytes, and the whole struct is padded up to a multiple of 16.
        // Compute the real layout from what the shader actually reflects (log
        // Renderer::generate_push_constant_ranges()'s range while iterating, or just note the size
        // UiRenderer's error message reports) rather than assuming your C++ struct's natural
        // sizeof/alignof matches (e.g. a trailing float4 lands at byte 32, not 24, once the 24-byte
        // UiElementConstants prefix is accounted for).
        std::vector<std::byte> push_constants;
    };

    // One resolved CLAY_RENDER_COMMAND_TYPE_CUSTOM command, positioned/clipped the same way a
    // QuadDraw is (Context.hpp) but drawn through UiCustomElementPipeline instead of the generic
    // rect/image pipeline — an arbitrary user fragment shader can't share that pipeline's fixed
    // fragment stage. One draw call per instance, no batching: custom-shaded elements are expected
    // to be rare (a handful of animated panels), not per-widget.
    struct CustomDraw {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
        const CustomShaderRef *shader = nullptr;
        RHI::Rect2D scissor{};
        PaintKey paint{};
    };

} // namespace SFT::UI
