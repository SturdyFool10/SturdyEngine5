#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <glm/vec2.hpp>
#include <string>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>

#include <Renderer/UI/Style.hpp>

// Plain data types behind Context::custom_element() (Clay's CUSTOM render command,
// plans/clay-ui-renderer.md's Phase 3: shader-driven styling) — deliberately Clay-free, mirroring
// UiQuad.hpp's own separation, so UI::UiCustomElementPipeline (the GPU side) never needs to see a
// Clay type either.
namespace SFT::UI {

    // Clay has exactly one CUSTOM render command type, carrying a single opaque `void* customData`
    // payload — Context::custom_element() (CustomShaderRef), Context::stroke_paths() (UI/UiStroke.hpp's
    // StrokePolylineData), Context::fill_quads() (UI/UiFill.hpp's FillQuadListData),
    // Context::fill_sectors() (UI/UiSector.hpp's SectorListData), and Context::stroke_custom()
    // (CustomStrokeElementData, below) all route through it, since Clay's fixed set of render command
    // types has no native "arbitrary polyline"/"many overlapping rects"/"pie slice"/"shader-driven
    // line" command. This tag disambiguates them at resolve time (Context::finish_frame()'s
    // CLAY_RENDER_COMMAND_TYPE_CUSTOM case): it MUST be the first declared member of every payload
    // struct, so `static_cast<const UiCustomCommandKind *>` on the untyped `customData` pointer reads
    // the right struct's tag via the standard's common-initial-sequence/pointer-interconvertibility
    // guarantee for standard-layout types.
    enum class UiCustomCommandKind : u8 { Shader = 0, Stroke = 1, Fill = 2, Sector = 3, CustomStroke = 4 };

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
        // MUST stay the first member — see UiCustomCommandKind's own doc comment above.
        UiCustomCommandKind command_kind = UiCustomCommandKind::Shader;

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

    // The push-constant prefix a Context::stroke_custom() shader's own struct must start with —
    // CustomShaderRef reused as-is (same shader_path/module_name/fragment_entry_point/push_constants
    // shape), just paired with this prefix instead of UiElementConstants. A clean 48 bytes for the
    // same reason UiElementConstants is 32: a shader's first trailing field lands at a clean multiple
    // of 16 with no implicit HLSL-cbuffer-style alignment gap to account for.
    //
    // `vertexMain` MUST build its geometry via Shaders/sturdy_common.slang's
    // uiStrokeSegmentClipPosition() — the shared-vertex-helper counterpart of
    // uiQuadClipPosition()/UiElementConstants for quad custom elements — given these five fields.
    //
    // `arc_length_so_far` is this segment's starting distance along the *whole* polyline (same value
    // UiStrokeInstance::dash_phase carries for the built-in dashing) — a shader computing a gradient/
    // pattern along the line needs this rather than a per-segment-local parametrization, since two
    // adjacent segments' local [0,1] ranges don't correspond to a consistent position along the curve
    // (each segment's own orientation differs). Using local position instead produces a visibly
    // discontinuous "restarts every segment" look — found by rendering the demo shader
    // (Shaders/ui_stroke_custom_demo.slang) and seeing exactly that artifact.
    struct CustomStrokeElementConstants {
        glm::vec2 p0{0.0f};
        glm::vec2 p1{0.0f};
        f32 half_width = 0.5f;
        f32 feather_px = 0.0f;
        glm::vec2 viewport_size{0.0f};
        f32 arc_length_so_far = 0.0f;
        f32 _pad0 = 0.0f;
        f32 _pad1 = 0.0f;
        f32 _pad2 = 0.0f;
    };

    // A whole polyline drawn through a caller-supplied fragment shader instead of ui_stroke.slang —
    // the stroke-shaped sibling of Context::custom_element(). Unlike a batched StrokeDraw
    // (UI/UiStroke.hpp), this expands to one *unbatched* draw call per segment (points.size() - 1),
    // matching CustomDraw's own "expected to be rare, not per-widget" cost model: a caller flags a
    // specific data series or gridline for a custom shader/post-effect, not every line in a chart.
    struct CustomStrokeElementData {
        // MUST stay the first member — see UiCustomCommandKind's own doc comment above.
        UiCustomCommandKind command_kind = UiCustomCommandKind::CustomStroke;

        std::vector<glm::vec2> points;
        f32 half_width = 0.5f;
        f32 feather_px = 0.0f;
        CustomShaderRef shader;
    };

    // One resolved Context::stroke_custom() call.
    struct CustomStrokeDraw {
        std::vector<glm::vec2> points;
        f32 half_width = 0.5f;
        f32 feather_px = 0.0f;
        const CustomShaderRef *shader = nullptr;
        RHI::Rect2D scissor{};
        PaintKey paint{};
    };

} // namespace SFT::UI
