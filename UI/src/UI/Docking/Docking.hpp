#pragma once

// Docking workspace: dockable/tabbed/splittable/resizable panels, built entirely on UI::Context's
// own public API. See DockWorkspace.hpp's own top doc comment for the overall design (and why
// tear-off to a real OS window is deliberately *not* handled in this package — that's
// Engine::DockWindowCoordinator, layered on top).
#include "DockLayout.hpp"
#include "DockTypes.hpp"
#include "DockWorkspace.hpp"
