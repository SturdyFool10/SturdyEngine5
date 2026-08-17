#pragma once

// Shared plumbing for the Direct3D 12 backend: the Windows/D3D12/DXGI include order, the COM smart
// pointer every other file uses, and the HRESULT -> RhiError translation that keeps the rest of the
// backend free of raw `FAILED(hr)` branches.
//
// Include order matters here and is deliberate: NOMINMAX/WIN32_LEAN_AND_MEAN must be defined before
// anything drags <windows.h> in, or <windef.h>'s min/max macros break every std::min/std::max and
// std::numeric_limits<T>::max() in a translation unit that only *transitively* included a Windows
// header. Every D3D12 source file includes this header first for exactly that reason.

#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <windows.h>

// rpcndr.h, included transitively by the Windows SDK, defines `small` as a C macro. It corrupts
// ordinary C++ identifiers in every consumer of this public backend header, so contain it here.
#ifdef small
#undef small
#endif

// Microsoft's open-source DirectX-Headers rather than whatever d3d12.h the installed Windows SDK
// happens to ship (see cmake/SturdyDependencies.cmake's sturdy_fetch_directx_headers()) — enhanced
// barriers (ID3D12GraphicsCommandList7), DXR 1.1, mesh shaders, and D3D12_FEATURE_D3D12_OPTIONS12+
// all live in declarations newer than several supported SDK versions.
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

// IID_PPV_ARGS expands to __uuidof, which clang correctly reports as a Microsoft language extension.
// It is also the only way to obtain an interface's IID from the Windows SDK headers, so every COM
// call in this backend would otherwise carry the same warning. Suppressed once here (this header is
// included first by every D3D12 translation unit) rather than at a few hundred call sites — the same
// choice graphicsPlatform/Source/Windows/CompositionPresent.cpp already makes for the same reason.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#endif

namespace SFT::D3D12 {

    namespace rhi = SFT::RHI;

    using Microsoft::WRL::ComPtr;

    // ─── Error translation ───────────────────────────────────────────────────────

    // The RHI error code an HRESULT maps to. The distinctions that matter to a caller are the ones
    // RhiErrorCode already draws: DXGI_ERROR_DEVICE_REMOVED/RESET is the D3D12 spelling of "the
    // logical device is gone, rebuild everything" (RhiErrorCode::DeviceLost), out-of-memory is
    // retryable-after-freeing, and everything else is an unclassified operation failure.
    [[nodiscard]] rhi::RhiErrorCode error_code_from_hresult(HRESULT hr) noexcept;

    // A human-readable HRESULT: the symbolic name for the ones this backend actually encounters,
    // falling back to `0x........` for anything else. FormatMessage is deliberately not used — it is
    // localized, frequently returns nothing useful for DXGI/D3D12 facility codes, and a log line that
    // says "DXGI_ERROR_DEVICE_REMOVED" is worth more than one that says "The parameter is incorrect."
    [[nodiscard]] std::string hresult_name(HRESULT hr);

    // `operation` names the call site ("create_buffer", "CreateCommittedResource", ...); the resulting
    // message reads "<operation> failed: <hresult_name>". Use for every failed COM call so a log line
    // identifies both what was attempted and why it failed.
    [[nodiscard]] std::unexpected<rhi::RhiError> hresult_error(HRESULT hr, std::string_view operation);

    [[nodiscard]] std::unexpected<rhi::RhiError> invalid_argument(std::string message);

    [[nodiscard]] std::unexpected<rhi::RhiError> unsupported(std::string message);

    [[nodiscard]] std::unexpected<rhi::RhiError> operation_failed(std::string message);

    // The "this device never finished coming up" guard every create_*/record path opens with, mirroring
    // the Vulkan bridge's own device_not_ready(). A template because it has to produce an
    // RhiExpected<T> for whatever T the caller returns.
    template <typename Value>
    [[nodiscard]] rhi::RhiExpected<Value> device_not_ready(std::string_view operation) {
        return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                              std::string("D3D12 backend cannot run ") + std::string(operation) +
                                  ": device resources are not ready.");
    }

    // ─── Debug names ─────────────────────────────────────────────────────────────

    // Attaches `label` to `object` as its D3D12 debug name, so PIX captures, the debug layer, and
    // DRED breadcrumbs all name the resource instead of printing a bare pointer. A null/empty label
    // is a no-op. SetName takes UTF-16, so the ASCII label is widened here rather than at every call
    // site (the RHI's descriptors carry `const char *label` throughout).
    void set_debug_name(ID3D12Object *object, const char *label) noexcept;
    void set_debug_name(IDXGIObject *object, const char *label) noexcept;

    // ─── Small numeric helpers ───────────────────────────────────────────────────

    // Rounds `value` up to the next multiple of `alignment` (a power of two). The D3D12 constant-
    // buffer (256B), texture-copy row-pitch (256B), and placed-resource alignment rules all need this.
    [[nodiscard]] constexpr u64 align_up(u64 value, u64 alignment) noexcept {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    // ─── Content hashing ─────────────────────────────────────────────────────────

    // FNV-1a (same constants Core::Slang::ShaderCache.cpp mixes its own keys with). Used to derive a
    // pipeline-state-object cache name from *content* — shader bytecode bytes, root-signature bytes,
    // blend/rasterizer/depth-stencil state — rather than a process-local pointer or handle, so the
    // name D3D12DevicePipelines.cpp stores a PSO under in the on-disk ID3D12PipelineLibrary cache
    // matches across runs whenever the pipeline's actual content does.
    inline constexpr u64 fnv1a_offset_basis = 0xcbf29ce484222325ull;

    [[nodiscard]] u64 fnv1a_bytes(u64 hash, const void *data, usize size) noexcept;

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] u64 fnv1a(u64 hash, const T &value) noexcept {
        return fnv1a_bytes(hash, &value, sizeof(T));
    }

} // namespace SFT::D3D12
