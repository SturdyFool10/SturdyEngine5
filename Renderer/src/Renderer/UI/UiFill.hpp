#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <vector>
#pragma endregion

#include <Renderer/UI/CustomElement.hpp>
#include <Renderer/UI/Style.hpp>

using std::vector;

namespace SFT::UI {

    // One axis-aligned filled (optionally rounded) rect within a Context::fill_quads() call —
    // deliberately the same shape UiQuadInstance already draws, since a fill batch is rendered by
    // reusing UiQuadPipeline directly rather than a new pipeline (see UiRenderer.cpp's
    // PaintEntry::Kind::Fill handling). `position`/`size` are in the filled element's own local space
    // until Context::finish_frame()'s resolve pass offsets them by the element's resolved bounding box.
    struct FillQuad {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
        Color color{1.0, 1.0, 1.0, 1.0};
        CornerRadius corner_radius{};
    };

    // The Clay CUSTOM-render-command payload behind Context::fill_quads() — see
    // UiCustomCommandKind's own doc comment (CustomElement.hpp) for why `command_kind` must stay the
    // first member. Many quads share one Clay leaf element (one bounding box, one scissor/paint order)
    // for the same reason StrokePolylineData batches multiple paths: a chart's bar/area fills all need
    // to coexist within one plot rect, which Clay's ordinary box-model layout can't do for separate
    // sibling elements (see StrokePolylineData's own doc comment for the full reasoning).
    struct FillQuadListData {
        UiCustomCommandKind command_kind = UiCustomCommandKind::Fill;

        vector<FillQuad> quads;
    };

} // namespace SFT::UI
