// Sturdy.UI — Clay-based immediate-mode UI layout + GPU rendering. See plans/clay-ui-renderer.md.
//
// Public surface: Style.hpp (Color/ElementDecl/TextStyle/...), Context.hpp (UI::Context, the
// layout-tree builder), UiRenderer.hpp (UI::UiRenderer, the batching GPU renderer). Clay's own C
// API (clay.h) never appears here — TextBridge.hpp/UiQuadPipeline.hpp are implementation details
// included directly by the .cpp files that need them, not re-exported through this header.
#pragma once

#include <Foundation/src/Foundation.hpp>

#include "Button.hpp"
#include "ColorPicker.hpp"
#include "Context.hpp"
#include "Docking/Docking.hpp"
#include "DragGesture.hpp"
#include "Dropdown.hpp"
#include "DocumentTextArea.hpp"
#include "Easing.hpp"
#include "Masonry.hpp"
#include "ScrollArea.hpp"
#include "Slider.hpp"
#include "Style.hpp"
#include "Svg/SvgIcon.hpp"
#include "TextArea.hpp"
#include "TextEdit.hpp"
#include "TextInput.hpp"
#include "Toggle.hpp"
#include "UiRenderer.hpp"
#include "WidgetComposition.hpp"
