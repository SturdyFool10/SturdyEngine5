/// C ABI implementation of the raw native handle escape hatch.
///
/// This is the deliberate hole in the abstraction: a consumer that needs to do something the RHI
/// has no concept of — bind an interop surface, attach a vendor SDK, record with an extension the
/// engine does not wrap — can take the backend's own objects and go direct, without abandoning the
/// engine for everything else.
///
/// It is gated twice on purpose. `SturdyRuntimeConfig::enable_native_access` must have asked for
/// it, and the active backend must actually publish it, so nothing here can be reached by accident
/// and a build that never opts in cannot come to depend on native access. Everything handed out is
/// borrowed and lives outside the engine's synchronization; the header says so in the terms a
/// caller needs.

#include <Foundation/Foundation.hpp>

#include <Engine/Engine.hpp>
#include <RHI/RHI.hpp>

#if !defined(STURDY_PLATFORM_WEB)
#include <Core/Vulkan/Rhi/VulkanNativeAccessExtension.hpp>
#endif
#if defined(_WIN32)
#include <Core/D3D12/RHI/D3D12NativeAccessExtension.hpp>
#endif

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;

    /// Resolves an engine handle to its active RHI device.
    ///
    /// @param engine Handle supplied to a game-logic callback.
    /// @param out_device Receives the borrowed device on success.
    ///
    /// @return `STURDY_OK`, or the handle/availability failure encountered.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult resolve_device(SturdyEngine engine, SFT::RHI::RhiDevice **out_device) noexcept {
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::RHI::RhiDevice *device = resolved_engine->rhi_device();
        if (device == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "the engine has no active RHI device");
        }
        *out_device = device;
        return STURDY_OK;
    }

    /// Translates an ABI queue class to the engine's own enumeration.
    ///
    /// @param queue_class Value received from the caller.
    /// @param out_queue Receives the translated value.
    ///
    /// @return Returns `true` when `queue_class` is a value this build recognizes; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool translate_queue_class(SturdyQueueClass queue_class,
                                             SFT::RHI::QueueClass *out_queue) noexcept {
        switch (queue_class) {
        case STURDY_QUEUE_CLASS_GRAPHICS:
            *out_queue = SFT::RHI::QueueClass::Graphics;
            return true;
        case STURDY_QUEUE_CLASS_COMPUTE:
            *out_queue = SFT::RHI::QueueClass::Compute;
            return true;
        case STURDY_QUEUE_CLASS_TRANSFER:
            *out_queue = SFT::RHI::QueueClass::Transfer;
            return true;
        case STURDY_QUEUE_CLASS_SPARSE:
            *out_queue = SFT::RHI::QueueClass::Sparse;
            return true;
        case STURDY_QUEUE_CLASS_VIDEO_DECODE:
            *out_queue = SFT::RHI::QueueClass::VideoDecode;
            return true;
        case STURDY_QUEUE_CLASS_VIDEO_ENCODE:
            *out_queue = SFT::RHI::QueueClass::VideoEncode;
            return true;
        case STURDY_QUEUE_CLASS_FORCE_U32:
        default:
            return false;
        }
    }

#if !defined(STURDY_PLATFORM_WEB)
    /// Resolves the Vulkan native-access extension published by `device`.
    ///
    /// @param device Active RHI device.
    ///
    /// @return The extension, or null when this is not a Vulkan device or native access was not
    ///         enabled.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SFT::Core::Vulkan::VulkanNativeAccessExtension *vulkan_native_access(
        SFT::RHI::RhiDevice &device) noexcept {
        if (device.backend_type() != SFT::RHI::BackendType::Vulkan) {
            return nullptr;
        }
        return static_cast<SFT::Core::Vulkan::VulkanNativeAccessExtension *>(
            device.extension_interface(SFT::Core::Vulkan::VulkanNativeAccessExtension::id()));
    }
#endif

#if defined(_WIN32)
    /// Resolves the D3D12 native-access extension published by `device`.
    ///
    /// @param device Active RHI device.
    ///
    /// @return The extension, or null when this is not a D3D12 device or native access was not
    ///         enabled.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SFT::D3D12::D3D12NativeAccessExtension *d3d12_native_access(
        SFT::RHI::RhiDevice &device) noexcept {
        if (device.backend_type() != SFT::RHI::BackendType::D3D12) {
            return nullptr;
        }
        return static_cast<SFT::D3D12::D3D12NativeAccessExtension *>(
            device.extension_interface(SFT::D3D12::D3D12NativeAccessExtension::id()));
    }
#endif

    /// Builds the failure returned when native access was requested but is not published.
    ///
    /// @return Always `STURDY_ERROR_NOT_AVAILABLE`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult native_unavailable() noexcept {
        return set_error(STURDY_ERROR_NOT_AVAILABLE,
                         "native access is not published; enable SturdyRuntimeConfig::enable_native_access "
                         "and check that the active backend matches the handles being requested");
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_native_available(SturdyEngine engine, SturdyBool *out_available) {
    return guarded([&]() -> SturdyResult {
        if (out_available == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        bool available = false;
#if !defined(STURDY_PLATFORM_WEB)
        available = available || vulkan_native_access(*device) != nullptr;
#endif
#if defined(_WIN32)
        available = available || d3d12_native_access(*device) != nullptr;
#endif
        *out_available = available ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_native_vulkan(SturdyEngine engine, SturdyVulkanHandles *out_handles) {
    return guarded([&]() -> SturdyResult {
        if (out_handles == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

#if defined(STURDY_PLATFORM_WEB)
        (void)device;
        return native_unavailable();
#else
        auto *access = vulkan_native_access(*device);
        if (access == nullptr) {
            return native_unavailable();
        }

        *out_handles = SturdyVulkanHandles{};
        out_handles->struct_size = static_cast<uint32_t>(sizeof(SturdyVulkanHandles));
        out_handles->instance = access->native_instance();
        out_handles->physical_device = access->native_physical_device();
        out_handles->device = access->native_device();
        out_handles->graphics_queue = access->native_graphics_queue();
        return STURDY_OK;
#endif
    });
}

SturdyResult STURDY_ABI_CALL sturdy_native_vulkan_queue(SturdyEngine engine,
                                                        SturdyQueueClass queue_class,
                                                        uint32_t lane_index,
                                                        void **out_queue,
                                                        uint32_t *out_queue_family_index) {
    return guarded([&]() -> SturdyResult {
        SFT::RHI::QueueClass engine_queue{};
        if (!translate_queue_class(queue_class, &engine_queue)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized queue class");
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

#if defined(STURDY_PLATFORM_WEB)
        (void)device;
        (void)lane_index;
        (void)out_queue;
        (void)out_queue_family_index;
        return native_unavailable();
#else
        auto *access = vulkan_native_access(*device);
        if (access == nullptr) {
            return native_unavailable();
        }

        const SFT::RHI::QueueLane lane{engine_queue, lane_index};
        if (out_queue != nullptr) {
            *out_queue = access->native_queue(lane);
        }
        if (out_queue_family_index != nullptr) {
            *out_queue_family_index = access->native_queue_family(lane);
        }
        return STURDY_OK;
#endif
    });
}

SturdyResult STURDY_ABI_CALL sturdy_native_d3d12(SturdyEngine engine, SturdyD3D12Handles *out_handles) {
    return guarded([&]() -> SturdyResult {
        if (out_handles == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

#if !defined(_WIN32)
        (void)device;
        // D3D12 sources are excluded from non-Windows builds entirely, so this is not a runtime
        // capability question — the backend does not exist in this binary.
        return native_unavailable();
#else
        auto *access = d3d12_native_access(*device);
        if (access == nullptr) {
            return native_unavailable();
        }

        *out_handles = SturdyD3D12Handles{};
        out_handles->struct_size = static_cast<uint32_t>(sizeof(SturdyD3D12Handles));
        out_handles->factory = access->native_factory();
        out_handles->adapter = access->native_adapter();
        out_handles->device = access->native_device();
        out_handles->graphics_queue = access->native_graphics_queue();
        return STURDY_OK;
#endif
    });
}

SturdyResult STURDY_ABI_CALL sturdy_native_d3d12_queue(SturdyEngine engine,
                                                       SturdyQueueClass queue_class,
                                                       uint32_t lane_index,
                                                       void **out_queue) {
    return guarded([&]() -> SturdyResult {
        if (out_queue == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::RHI::QueueClass engine_queue{};
        if (!translate_queue_class(queue_class, &engine_queue)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized queue class");
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

#if !defined(_WIN32)
        (void)device;
        (void)lane_index;
        return native_unavailable();
#else
        auto *access = d3d12_native_access(*device);
        if (access == nullptr) {
            return native_unavailable();
        }
        *out_queue = access->native_queue(SFT::RHI::QueueLane{engine_queue, lane_index});
        return STURDY_OK;
#endif
    });
}

} // extern "C"
