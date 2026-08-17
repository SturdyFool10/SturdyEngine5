#include <D3D12/D3D12Common.hpp>

#pragma region Imports
#include <cstdio>
#include <vector>
#pragma endregion

namespace SFT::D3D12 {

    /// Performs the error code from hresult operation for `D3D12` using the supplied arguments.
    ///
    /// @param hr `hr` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::DeviceLost`, `RhiErrorCode::OutOfMemory`, `RhiErrorCode::InvalidArgument`, `RhiErrorCode::Unsupported`, `RhiErrorCode::NotReady`, `RhiErrorCode::SurfaceLost` among others.
    /// @note This function does not throw exceptions.
    rhi::RhiErrorCode error_code_from_hresult(HRESULT hr) noexcept {
        switch (hr) {
            case DXGI_ERROR_DEVICE_REMOVED:
            case DXGI_ERROR_DEVICE_RESET:
            case DXGI_ERROR_DEVICE_HUNG:
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
                return rhi::RhiErrorCode::DeviceLost;
            case E_OUTOFMEMORY:
                return rhi::RhiErrorCode::OutOfMemory;
            case E_INVALIDARG:
            case E_POINTER:
                return rhi::RhiErrorCode::InvalidArgument;
            case E_NOTIMPL:
            case DXGI_ERROR_UNSUPPORTED:
                return rhi::RhiErrorCode::Unsupported;
            case DXGI_STATUS_OCCLUDED:
            case DXGI_ERROR_WAS_STILL_DRAWING:
                return rhi::RhiErrorCode::NotReady;


            case DXGI_ERROR_ACCESS_LOST:
                return rhi::RhiErrorCode::SurfaceLost;


            case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
                return rhi::RhiErrorCode::FullScreenExclusiveLost;
            default:
                return rhi::RhiErrorCode::OperationFailed;
        }
    }

    /// Returns a human-readable name for the supplied hresult value.
    ///
    /// @param hr `hr` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::string hresult_name(HRESULT hr) {
        switch (hr) {
            case S_OK: return "S_OK";
            case S_FALSE: return "S_FALSE";
            case E_FAIL: return "E_FAIL";
            case E_INVALIDARG: return "E_INVALIDARG";
            case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
            case E_NOTIMPL: return "E_NOTIMPL";
            case E_NOINTERFACE: return "E_NOINTERFACE";
            case E_POINTER: return "E_POINTER";
            case E_ACCESSDENIED: return "E_ACCESSDENIED";
            case E_HANDLE: return "E_HANDLE";
            case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
            case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
            case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
            case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
            case DXGI_ERROR_NOT_FOUND: return "DXGI_ERROR_NOT_FOUND";
            case DXGI_ERROR_MORE_DATA: return "DXGI_ERROR_MORE_DATA";
            case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
            case DXGI_ERROR_ACCESS_LOST: return "DXGI_ERROR_ACCESS_LOST";
            case DXGI_ERROR_ACCESS_DENIED: return "DXGI_ERROR_ACCESS_DENIED";
            case DXGI_ERROR_WAIT_TIMEOUT: return "DXGI_ERROR_WAIT_TIMEOUT";
            case DXGI_ERROR_WAS_STILL_DRAWING: return "DXGI_ERROR_WAS_STILL_DRAWING";
            case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
            case DXGI_ERROR_FRAME_STATISTICS_DISJOINT: return "DXGI_ERROR_FRAME_STATISTICS_DISJOINT";
            case DXGI_STATUS_OCCLUDED: return "DXGI_STATUS_OCCLUDED";
            case DXGI_STATUS_MODE_CHANGED: return "DXGI_STATUS_MODE_CHANGED";
            case D3D12_ERROR_ADAPTER_NOT_FOUND: return "D3D12_ERROR_ADAPTER_NOT_FOUND";
            case D3D12_ERROR_DRIVER_VERSION_MISMATCH: return "D3D12_ERROR_DRIVER_VERSION_MISMATCH";
            default: break;
        }
        char buffer[16]{};
        std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
        return std::string(buffer);
    }

    /// Creates an error result describing the supplied hresult failure.
    ///
    /// @param hr `hr` value used by the operation.
    /// @param operation `operation` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    std::unexpected<rhi::RhiError> hresult_error(HRESULT hr, std::string_view operation) {
        return rhi::rhi_error(error_code_from_hresult(hr),
                              std::string(operation) + " failed: " + hresult_name(hr) + ".");
    }

    /// Performs the invalid argument operation for `D3D12` using the supplied arguments.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`.
    std::unexpected<rhi::RhiError> invalid_argument(std::string message) {
        return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, std::move(message));
    }

    /// Performs the unsupported operation for `D3D12` using the supplied arguments.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`.
    std::unexpected<rhi::RhiError> unsupported(std::string message) {
        return rhi::rhi_error(rhi::RhiErrorCode::Unsupported, std::move(message));
    }

    /// Performs the operation failed operation for `D3D12` using the supplied arguments.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::OperationFailed`.
    std::unexpected<rhi::RhiError> operation_failed(std::string message) {
        return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, std::move(message));
    }

    namespace {


        /// Performs the widen operation for `D3D12` using the supplied arguments.
        ///
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::vector<wchar_t> widen(const char *label) {
            std::vector<wchar_t> wide;
            for (const char *cursor = label; *cursor != '\0'; ++cursor) {
                wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*cursor)));
            }
            wide.push_back(L'\0');
            return wide;
        }

    } // namespace

    /// Returns a human-readable name for the supplied set debug value.
    ///
    /// @param object `object` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void set_debug_name(ID3D12Object *object, const char *label) noexcept {
        if (object == nullptr || label == nullptr || *label == '\0') {
            return;
        }
        const std::vector<wchar_t> wide = widen(label);
        (void)object->SetName(wide.data());
    }

    /// Returns a human-readable name for the supplied set debug value.
    ///
    /// @param object `object` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void set_debug_name(IDXGIObject *object, const char *label) noexcept {
        if (object == nullptr || label == nullptr || *label == '\0') {
            return;
        }
        (void)object->SetPrivateData(WKPDID_D3DDebugObjectName,
                                     static_cast<UINT>(std::char_traits<char>::length(label)), label);
    }

    /// Computes the fnv1a bytes required by the supplied values.
    ///
    /// @param hash `hash` value used by the operation.
    /// @param data Data consumed or referenced by the operation.
    /// @param size Requested or available size for the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    u64 fnv1a_bytes(u64 hash, const void *data, usize size) noexcept {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (usize i = 0; i < size; ++i) {
            hash = (hash ^ bytes[i]) * 0x100000001b3ull;
        }
        return hash;
    }

} // namespace SFT::D3D12
