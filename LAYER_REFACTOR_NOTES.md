# SE5 Layer Refactor Notes

## Resulting first-party layers

The primary dependency direction is:

`Foundation / Async / Ecs -> WindowManager + RHI -> Core -> Renderer -> Engine -> ApplicationHost / Runtime`

`RHI` remains a first-class API-neutral layer. It does not depend on Core, WindowManager, Renderer, or Engine.

### WindowManager

- Replaces the old `Platform` package for windows, input/event pumping, native window effects, and window-provider contracts.
- SDL3 remains the built-in provider at `WindowManager/src/WindowManager/Providers/SDL3`.
- The optional GLFW provider is now a nested package at `WindowManager/Providers/GLFW` and builds as `Sturdy::WindowManagerGLFW`.
- Windowing namespace use was normalized from `SFT::Platform::Windowing` to `SFT::WindowManager`.

### RHI

- Remains its own package/layer at `RHI/src/RHI`.
- Contains only API-neutral RHI vocabulary/interfaces and neutral HDR/display result types.
- OS/display implementation bridging was removed from RHI and placed in Core.

### Core

- Owns concrete graphics/API implementation and low-level graphics-platform integration.
- Existing Vulkan implementation remains under `Core/src/Core/Vulkan`.
- D3D12 RHI implementation moved from the old top-level `D3D12` package to `Core/src/Core/D3D12/RHI`.
- The old `graphicsPlatform` package moved to `Core/src/Core/GraphicsPlatform` and now uses `SFT::Core::GraphicsPlatform`.
- Backend inventory registration and Vulkan WSI-extension discovery are owned by Core rather than Engine.
- Accelerated platform file I/O is hidden behind `Core/StreamingIo.hpp`; DirectStorage and io_uring implementations live under `Core/src/Core/IO` so Engine does not include platform/backend implementation headers.

### Renderer

- Owns the previous standalone Text and UI packages.
- Text is at `Renderer/src/Renderer/Text` and retains the `SFT::Text` namespace.
- UI is at `Renderer/src/Renderer/UI` and retains the `SFT::UI` namespace.
- Platform font discovery moved with Text into Renderer.
- Text/UI tests were moved into `Renderer/tests/Text` and `Renderer/tests/UI`.

### Engine

- The canonical build target is now `Sturdy::Engine` / `Engine`; the intermediate `EngineComponents` target name is removed.
- Engine depends on the Renderer/Core/RHI-facing abstractions and no longer registers concrete Vulkan/D3D12 inventory providers itself.

## Include convention

First-party source trees now expose only their `src` directory as the build include root. Code uses the consistent form:

`#include <Package/fileRelativeToSrc>`

Examples:

- `#include <Foundation/UString.hpp>`
- `#include <WindowManager/Window.hpp>`
- `#include <RHI/RHI.hpp>`
- `#include <Core/Vulkan/VulkanBackend.hpp>`
- `#include <Renderer/Text/Text.hpp>`
- `#include <Renderer/UI/UI.hpp>`
- `#include <Engine/Engine.hpp>`

The normalized packages use `Package/src/Package/...`, including Foundation, Async, Ecs, ApplicationHost, WindowManager, RHI, Core, Renderer, and Engine.

## UI Workbench changes

- `Composition` was renamed to `Settings` (including its panel id and builder naming).
- The scrolling controls were moved from `Docking Guide` into `Settings`.
- Scroll behavior is applied every frame, so changing tabs does not gate the active scroll configuration.
- Initial workspace creation puts Settings, Slider Lab, Color Studio, Docking Guide, Text Lab, and Performance into one tab leaf across the top; it no longer creates initial right/bottom splits.
- GLFW is now a real optional Workbench dependency. With `STURDY_BUILD_GLFW_WINDOW_PROVIDER=OFF`, the window-provider selector exposes SDL3 only; when enabled, GLFW is added.

## Required cleanup when applying over an existing dirty tree

Archive extraction/overwrite does not delete files that moved to new directories. Before extracting this refactor over an existing checkout, delete these old package directories:

- `Platform/`
- `GlfwWindowProvider/`
- `Text/`
- `UI/`
- `D3D12/`
- `graphicsPlatform/`

Also delete these old source directories before extraction, because their files were normalized one level deeper and leaving the originals would make the recursive CMake source glob see duplicates:

- `Foundation/src/`
- `Async/src/`
- `Ecs/src/`
- `ApplicationHost/src/`

If present, also remove the dirty-tree temporary file:

- `Core/CMakeLists.txt.tmp.14928.fe21cf84da53`

Then extract the archive at the repository root and allow replacement of conflicting files.

## Validation performed

- Verified every first-party angle-bracket include resolves against a package `src` root: zero missing first-party include paths.
- Verified no first-party include still contains `/src/`.
- Verified no active CMake reference remains to the removed `Sturdy::Text`, `Sturdy::UI`, `Sturdy::Platform`, `Sturdy::graphicsPlatform`, `Sturdy::D3D12`, or `EngineComponents` targets.
- Verified RHI has no upward dependency on Core/WindowManager/Renderer/Engine.
- Verified WindowManager has no dependency on RHI/Core/Renderer/Engine.
- Verified Core has no upward dependency on Renderer/Engine.
- Verified Renderer has no upward dependency on Engine.
- Verified Engine contains no direct includes/references to concrete Vulkan, D3D12, graphics-platform, DirectStorage, or io_uring implementation headers.
- A native Linux CMake configure was attempted. It stopped during dependency discovery because the execution environment has the Vulkan runtime library but does not have the Vulkan 1.4 development headers/SDK (`find_package(Vulkan 1.4 REQUIRED)` failed). The configure therefore could not proceed to compilation in this environment.
