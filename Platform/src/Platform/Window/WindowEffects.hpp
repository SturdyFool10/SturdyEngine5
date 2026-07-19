#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <string_view>
#pragma endregion

#include <Platform/Window/WindowError.hpp>
#include <Platform/Window/WindowConfig.hpp>
#include <Platform/Window/WindowEffect.hpp>

using std::expected;
using std::string_view;
using std::unexpected;

namespace SFT::Platform::Windowing {

    [[nodiscard]] OperatingSystem current_operating_system() noexcept;
    [[nodiscard]] bool operating_system_may_support_window_effect(WindowEffectKind effect) noexcept;
    [[nodiscard]] WindowEffectResult enable_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept;

    [[nodiscard]] expected<void, WindowError> window_result_from_effect_result(WindowEffectResult result) noexcept;

    [[nodiscard]] expected<void, WindowError> set_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept;

    [[nodiscard]] expected<void, WindowError> set_native_window_blur_enabled(NativeWindowHandle handle, bool enabled) noexcept;

} // namespace SFT::Platform::Windowing
