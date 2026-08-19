#pragma once


#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <windows.h>


#ifdef small
#undef small
#endif


#include <directx/d3d12.h>
#include <directx/d3dx12.h>
#include <dxgi1_6.h>

#include <wrl/client.h>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#pragma endregion

#include <RHI/RHI.hpp>


#if defined(__clang__)
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#endif

namespace SFT::D3D12 {

    namespace rhi = SFT::RHI;

    using Microsoft::WRL::ComPtr;


    /// Performs the error code from hresult operation using the supplied arguments.
    ///
    /// @param hr `hr` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::DeviceLost`, `RhiErrorCode::OutOfMemory`, `RhiErrorCode::InvalidArgument`, `RhiErrorCode::Unsupported`, `RhiErrorCode::NotReady`, `RhiErrorCode::SurfaceLost` among others.
    /// @note This function does not throw exceptions.
    [[nodiscard]] rhi::RhiErrorCode error_code_from_hresult(HRESULT hr) noexcept;


    /// Returns a human-readable name for the supplied hresult value.
    ///
    /// @param hr `hr` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] std::string hresult_name(HRESULT hr);


    /// Performs the hresult error operation using the supplied arguments.
    ///
    /// @param hr `hr` value used by the operation.
    /// @param operation `operation` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] std::unexpected<rhi::RhiError> hresult_error(HRESULT hr, std::string_view operation);

    /// Performs the invalid argument operation using the supplied arguments.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] std::unexpected<rhi::RhiError> invalid_argument(std::string message);

    /// Performs the unsupported operation using the supplied arguments.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] std::unexpected<rhi::RhiError> unsupported(std::string message);

    /// Performs the operation failed operation using the supplied arguments.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] std::unexpected<rhi::RhiError> operation_failed(std::string message);


    /// Returns the current or globally available device not ready value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    template <typename Value>
    [[nodiscard]] rhi::RhiExpected<Value> device_not_ready(std::string_view operation) {
        return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                              std::string("D3D12 backend cannot run ") + std::string(operation) +
                                  ": device resources are not ready.");
    }


    /// Returns a human-readable name for the supplied set debug value.
    ///
    /// @param object `object` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void set_debug_name(ID3D12Object *object, const char *label) noexcept;
    /// Returns a human-readable name for the supplied set debug value.
    ///
    /// @param object `object` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void set_debug_name(IDXGIObject *object, const char *label) noexcept;


    /// Performs the align up operation using the supplied arguments.
    ///
    /// @param value Value consumed by the operation.
    /// @param alignment `alignment` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr u64 align_up(u64 value, u64 alignment) noexcept {
        return (value + alignment - 1) & ~(alignment - 1);
    }


    inline constexpr u64 fnv1a_offset_basis = 0xcbf29ce484222325ull;

    /// Computes the fnv1a bytes required by the supplied values.
    ///
    /// @param hash `hash` value used by the operation.
    /// @param data Data consumed or referenced by the operation.
    /// @param size Requested or available size for the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u64 fnv1a_bytes(u64 hash, const void *data, usize size) noexcept;

    /// Performs the fnv1a operation for `D3D12` using the supplied arguments.
    ///
    /// @param hash `hash` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] u64 fnv1a(u64 hash, const T &value) noexcept {
        return fnv1a_bytes(hash, &value, sizeof(T));
    }

} // namespace SFT::D3D12
