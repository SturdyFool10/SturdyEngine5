#pragma once

// The single include for every CxxApi function header (and, through them, every plain-data type).
// A `cxx` bridge `include!`s this one file; bindgen needs only `Types.hpp`.

#include <CxxApi/BodyRig.hpp>
#include <CxxApi/Camera.hpp>
#include <CxxApi/DebugDraw.hpp>
#include <CxxApi/Ecs.hpp>
#include <CxxApi/Engine.hpp>
#include <CxxApi/Log.hpp>
#include <CxxApi/Render.hpp>
#include <CxxApi/Runtime.hpp>
#include <CxxApi/Scene.hpp>
#include <CxxApi/Types.hpp>
#include <CxxApi/Ui.hpp>
#include <CxxApi/Window.hpp>
