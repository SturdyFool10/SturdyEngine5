module;
#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#pragma endregion

export module Sturdy.RHI:Adapter;

import :Error;
import :Features;
import :Extensions;
import :Queues;
import :Device; // BackendType, AdapterInfo, DeviceLimits, RhiDevice

using std::span;
using std::string_view;
using std::unique_ptr;
using std::vector;

export namespace SFT::RHI {

    // Parameters for bringing up a backend instance (a driver connection). The strings are borrowed —
    // they need only outlive the create-instance call. `enable_validation` turns on the API's
    // validation/debug layers (Vulkan validation layers, the D3D12 debug layer); `enable_debug_utils`
    // turns on object labeling and command-buffer debug markers (VK_EXT_debug_utils and equivalents)
    // so captures and validation messages name resources instead of raw handles.
    struct InstanceDesc {
        string_view application_name;
        u32 application_version = 0;
        string_view engine_name = "Sturdy";
        u32 engine_version = 0;
        bool enable_validation = false;
        bool enable_debug_utils = false;
        // Prefer a headless instance (no presentation/surface extensions) — for compute-only or
        // offscreen device creation. A backend ignores it when it cannot honor it.
        bool headless = false;
        const char *label = nullptr;
    };

    // What an application asks for when turning an adapter into a device. This is the one place the
    // three APIs' feature models are reconciled:
    //
    //  - `required_features` are hard requirements. `RhiAdapter::create_device()` fails with an
    //    `Unsupported` error (naming the missing features) if the adapter doesn't support them all.
    //    This is exactly how a title states "I rely on hardware ray tracing, so I do not run on GPUs
    //    without it": put `Feature::RayTracingPipeline` in `required_features` and a device simply
    //    won't be created on a machine that lacks it — the app reads the error and bails cleanly with
    //    a legible message instead of crashing deep in a draw call.
    //  - `optional_features` are enabled where supported and silently skipped where not. The app
    //    then reads `RhiDevice::enabled_features()` to see what it actually got and branches its
    //    render path accordingly ("shadows via ray query if enabled, else the raster fallback").
    //
    // On Vulkan the union of required+optional (intersected with support) becomes the explicit
    // feature/extension enable list at device creation — the mandatory step that API demands. On
    // D3D12/Metal, where features are query-then-use, the backend just records the same set as
    // enabled; no behavior is lost either way, which is what keeps this shape API-agnostic.
    struct DeviceRequest {
        FeatureSet required_features;
        FeatureSet optional_features;
        // Required extensions fail device creation if absent; optional extensions are enabled where
        // present and reported through RhiDevice::enabled_extensions(). Spans are consumed during
        // create_device(), matching the rest of the RHI descriptor style.
        span<const ExtensionId> required_extensions;
        span<const ExtensionId> optional_extensions;
        // Queue-lane requests let a renderer ask for dedicated/extra compute or transfer lanes while
        // still allowing portable fallback when the corresponding async feature is not required.
        span<const QueueRequest> queue_requests;
        const char *label = nullptr;
    };

    // A physical GPU (or software adapter) exposed by an instance — the pre-device seam where
    // capabilities are inspected. Mirrors VkPhysicalDevice / IDXGIAdapter / MTLDevice-as-adapter:
    // you read its info, supported features, feature properties, and limits to decide whether (and
    // how) to run, *then* commit to a full device via `create_device()`.
    //
    // An adapter need not outlive the device it creates — `create_device()` captures everything the
    // device needs, matching how a VkDevice outlives the transient VkPhysicalDevice handle.
    class RhiAdapter {
      public:
        virtual ~RhiAdapter() = default;

        RhiAdapter(const RhiAdapter &) = delete;
        RhiAdapter &operator=(const RhiAdapter &) = delete;
        RhiAdapter(RhiAdapter &&) = delete;
        RhiAdapter &operator=(RhiAdapter &&) = delete;

        [[nodiscard]] virtual const AdapterInfo &info() const noexcept = 0;

        // The full set of optional features this adapter can provide. Query this before building a
        // DeviceRequest — `supported_features().contains_all(my_required)` answers "can this GPU run
        // the game at all" without creating a device.
        [[nodiscard]] virtual const FeatureSet &supported_features() const noexcept = 0;

        // Graded values (max ray recursion depth, VRS tile size, ...) for whichever features above
        // are supported; fields for unsupported features are left zero.
        [[nodiscard]] virtual const FeatureProperties &feature_properties() const noexcept = 0;

        // API/vendor-specific extensions and queue topology visible before device creation, so a
        // renderer can choose between required, optional, and fallback paths deliberately.
        [[nodiscard]] virtual span<const ExtensionId> supported_extensions() const noexcept = 0;
        [[nodiscard]] virtual span<const QueueInfo> queue_infos() const noexcept = 0;

        [[nodiscard]] virtual const DeviceLimits &limits() const noexcept = 0;

        // Realizes a logical device with the requested features enabled. Returns an `Unsupported`
        // error (message naming the missing features via `feature_name`) if any of
        // `request.required_features` is absent from `supported_features()`.
        [[nodiscard]] virtual RhiExpected<unique_ptr<RhiDevice>> create_device(const DeviceRequest &request) = 0;

      protected:
        RhiAdapter() = default;
    };

    // The backend entry point: one graphics API's driver connection, from which adapters are
    // enumerated (VkInstance / IDXGIFactory / the set of MTLDevices). A concrete backend provides a
    // free factory returning one of these — e.g. `create_vulkan_instance(const InstanceDesc&) ->
    // RhiExpected<unique_ptr<RhiInstance>>` — living in the backend, not in this API-agnostic module.
    class RhiInstance {
      public:
        virtual ~RhiInstance() = default;

        RhiInstance(const RhiInstance &) = delete;
        RhiInstance &operator=(const RhiInstance &) = delete;
        RhiInstance(RhiInstance &&) = delete;
        RhiInstance &operator=(RhiInstance &&) = delete;

        [[nodiscard]] virtual BackendType backend_type() const noexcept = 0;

        // Every adapter this instance can see, ownership transferred to the caller. Typically the
        // caller scores them (discrete vs integrated) and against `supported_features()`, keeps the
        // one it wants, and lets the rest drop.
        [[nodiscard]] virtual RhiExpected<vector<unique_ptr<RhiAdapter>>> enumerate_adapters() = 0;

      protected:
        RhiInstance() = default;
    };

} // namespace SFT::RHI
