#include <WindowManager/WindowEffects.hpp>

#include <algorithm>
#include <cmath>

#include <tracy/Tracy.hpp>

namespace SFT::WindowManager {

/// Performs the window result from effect result operation for `Windowing` using the supplied arguments.
///
/// @param result `result` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
expected<void, WindowError> window_result_from_effect_result(WindowEffectResult result) noexcept {
        ZoneScopedN("Windowing::window_result_from_effect_result");
        if (result.succeeded()) [[likely]] {
            return {};
        }

        return unexpected(WindowError{WindowErrorCode::OperationFailed, result.details.empty() ? string_view{"Window effect failed."} : result.details});
    }

/// Sets the native window effect for this `Windowing`.
///
/// @param handle Handle identifying the target object or resource.
/// @param effect `effect` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
expected<void, WindowError> set_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
        ZoneScopedN("Windowing::set_native_window_effect");
        return window_result_from_effect_result(enable_native_window_effect(handle, effect));
    }

/// Sets the native window blur enabled for this `Windowing`.
///
/// @param handle Handle identifying the target object or resource.
/// @param enabled Whether the associated behavior is enabled.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
std::vector<EffectRect> resolve_blur_region(const WindowEffectOptions &options) {
        std::vector<EffectRect> out;
        const bool rounded = options.corner_radius > 0.0f;
        if (options.blur_rect_count == 0 && !rounded) {
            return out;
        }

        std::vector<EffectRect> sources;
        if (options.blur_rect_count == 0) {
            if (options.window_width == 0 || options.window_height == 0) {
                return out; // size unknown: fall back to the whole window rather than guessing
            }
            sources.push_back(EffectRect{0, 0, static_cast<i32>(options.window_width), static_cast<i32>(options.window_height)});
        } else {
            sources.assign(options.blur_rects, options.blur_rects + options.blur_rect_count);
        }

        for (const EffectRect &rect : sources) {
            if (rect.width <= 0 || rect.height <= 0) {
                continue;
            }
            const i32 radius = static_cast<i32>(std::min({std::ceil(options.corner_radius), static_cast<f32>(rect.width) / 2.0f,
                                                          static_cast<f32>(rect.height) / 2.0f}));
            if (!rounded || radius <= 0) {
                out.push_back(rect);
                continue;
            }
            // One rectangle per scanline in the corner bands, one for the straight middle. The inset of scanline `row`
            // (counted from the edge) is how far the circle of `radius` is from the edge at that row's centre.
            for (i32 row = 0; row < radius; ++row) {
                const f32 dy = static_cast<f32>(radius - row) - 0.5f;
                const f32 dx = std::sqrt(std::max(0.0f, static_cast<f32>(radius * radius) - dy * dy));
                const i32 inset = static_cast<i32>(std::ceil(static_cast<f32>(radius) - dx));
                const i32 width = rect.width - 2 * inset;
                if (width <= 0) {
                    continue;
                }
                out.push_back(EffectRect{rect.x + inset, rect.y + row, width, 1});
                out.push_back(EffectRect{rect.x + inset, rect.y + rect.height - 1 - row, width, 1});
            }
            if (rect.height - 2 * radius > 0) {
                out.push_back(EffectRect{rect.x, rect.y + radius, rect.width, rect.height - 2 * radius});
            }
        }
        return out;
    }

expected<void, WindowError> set_native_window_blur_enabled(NativeWindowHandle handle, bool enabled) noexcept {
        ZoneScopedN("Windowing::set_native_window_blur_enabled");
        return set_native_window_effect(handle, WindowEffect::blur(enabled));
    }

} // namespace SFT::WindowManager
