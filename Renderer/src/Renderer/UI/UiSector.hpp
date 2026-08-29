#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <vector>
#pragma endregion

#include <Renderer/UI/CustomElement.hpp>
#include <Renderer/UI/Style.hpp>

using std::vector;

namespace SFT::UI {

    struct SectorStyle {
        Color color{1.0, 1.0, 1.0, 1.0};

        /// Same feather-padding convention as StrokeStyle::feather_px — 0 derives AA purely from
        /// fwidth() (crisp), a caller-set value gives a wider, stylized soft edge.
        f32 feather_px = 1.0f;
    };

    // One annular sector ("pie slice") within a Context::fill_sectors() call. `center`/`inner_radius`/
    // `outer_radius` are in the sectored element's own local space until Context::finish_frame()'s
    // resolve pass offsets `center` by the element's resolved bounding box. `start_angle`/`end_angle`
    // are radians in the same convention sectorSDF() (sturdy_common.slang) documents.
    struct Sector {
        glm::vec2 center{0.0f};
        f32 inner_radius = 0.0f;
        f32 outer_radius = 1.0f;
        f32 start_angle = 0.0f;
        f32 end_angle = 0.0f;
        SectorStyle style{};
    };

    // Field order must byte-match Shaders/ui_sector.slang's UiSectorInstance exactly (see the comment
    // there).
    struct UiSectorInstance {
        glm::vec2 center{0.0f};
        f32 inner_radius = 0.0f;
        f32 outer_radius = 1.0f;
        f32 start_angle = 0.0f;
        f32 end_angle = 0.0f;
        f32 feather_px = 1.0f;
        f32 _pad0 = 0.0f;
        glm::vec4 color{1.0f};
    };

    // The Clay CUSTOM-render-command payload behind Context::fill_sectors() — see
    // UiCustomCommandKind's own doc comment (CustomElement.hpp) for why `command_kind` must stay the
    // first member, and StrokePolylineData's own doc comment for why multiple sectors share one call
    // (a pie chart's slices all need to coexist within one plot rect).
    struct SectorListData {
        UiCustomCommandKind command_kind = UiCustomCommandKind::Sector;

        vector<Sector> sectors;
    };

} // namespace SFT::UI
