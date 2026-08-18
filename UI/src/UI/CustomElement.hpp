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

    // Every custom-element shader's single push-constant struct must begin with exactly these five
    // fields, in this order and byte layout (see Shaders/sturdy_common.slang's uiQuadClipPosition(),
    // which a custom shader's own vertexMain calls with these five fields) —
    // UiCustomElementPipeline writes them, then appends CustomShaderRef::push_constants right after.
    // Deliberately a clean 32 bytes (two full 16-byte cbuffer registers, explicitly padded rather
    // than left to Slang's implicit next-16-byte-boundary rounding — see CustomShaderRef's own doc
    // comment) so a trailing float4 field in a concrete shader always starts immediately at byte 32
    // with no extra gap to account for on the C++ packing side. `_reserved0`/`_reserved1` carry no
    // meaning today (there used to be a per-backend clip-space sign here — no longer needed, see
    // RHI::Viewport's own doc comment) but stay as explicit padding so this struct's size doesn't
    // change and every existing concrete shader's trailing-field offset stays valid.
    struct UiElementConstants {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
        glm::vec2 viewport_size{0.0f};
        f32 _reserved0 = 0.0f;
        f32 _reserved1 = 0.0f;
    };

    // A Slang shader an app supplies for one Context::custom_element() call — the shader-driven-
    // styling seam Clay's CUSTOM render command exists for. Both entry points must live in
    // `shader_path`'s own file (same convention Renderer::CustomPostProcessEffect already uses):
    // `vertexMain` MUST build its quad via Shaders/sturdy_common.slang's uiQuadClipPosition()/
    // uiQuadCorner(), given a `[[push_constant]] ConstantBuffer<T>` whose first four fields match
    // UiElementConstants exactly (see its own doc comment).
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
        // 16-byte alignment) is pushed to the next 16-byte boundary. UiElementConstants' 32-byte
        // prefix (see its own doc comment) is deliberately already a multiple of 16, so a shader's
        // first trailing float4 field lands immediately at byte 32 with no implicit gap to account
        // for — unlike the legacy 24-byte (three-vec2) prefix, which needed an explicit 8-byte gap.
        // Compute the real layout from what the shader actually reflects (log
        // Renderer::generate_push_constant_ranges()'s range while iterating, or just note the size
        // UiRenderer's error message reports) rather than assuming your C++ struct's natural
        // sizeof/alignof matches.
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
