#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

#include <Core/WebGPU/RHI/WebGpuConvert.hpp>

#include <string>
#include <utility>

namespace SFT::Core::WebGpu {

    /// Builds the RHI error returned when a caller asks this backend for something WebGPU does not
    /// have.
    ///
    /// @param what The capability the caller asked for.
    ///
    /// @return Returns the error alternative describing why the operation failed.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    RHI::RhiError unsupported_by_webgpu(std::string_view what) {
        return RHI::RhiError{
            .code = RHI::RhiErrorCode::Unsupported,
            .message = std::string{what} + " is not part of the WebGPU API, so the WebGPU backend "
                                           "cannot provide it on any platform.",
        };
    }

    /// Builds the RHI error for a WebGPU call that failed at runtime.
    ///
    /// @param what The operation that failed.
    /// @param detail Backend-supplied detail, if any.
    ///
    /// @return Returns the error alternative describing why the operation failed.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    RHI::RhiError webgpu_error(std::string_view what, std::string_view detail) {
        std::string message = "WebGPU: " + std::string{what} + " failed";
        if (!detail.empty()) {
            message += ": ";
            message += detail;
        } else {
            message += ".";
        }
        return RHI::RhiError{.code = RHI::RhiErrorCode::OperationFailed, .message = std::move(message)};
    }

    /// Constructs a device around an already-created Dawn device.
    ///
    /// @param instance The Dawn instance the device came from.
    /// @param adapter The Dawn adapter `device` was requested from.
    /// @param device The Dawn device, whose ownership passes to this object.
    /// @param info Adapter description reported upward.
    /// @param limits Device limits reported upward.
    /// @param features Features negotiated at creation.
    /// @param enable_native_access Whether to publish `WebGpuNativeAccessExtension`.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    WebGpuDevice::WebGpuDevice(WGPUInstance instance, WGPUAdapter adapter, WGPUDevice device, rhi::AdapterInfo info,
                               rhi::DeviceLimits limits, rhi::FeatureSet features, bool enable_native_access)
        : instance_(instance),
          adapter_(adapter),
          device_(device),
          queue_(wgpuDeviceGetQueue(device)),
          adapter_info_(std::move(info)),
          limits_(limits),
          enabled_features_(features) {
        wgpuInstanceAddRef(instance_);
        // The adapter that requested `device` is otherwise only owned by the `RhiAdapter` wrapper
        // (`WebGpuAdapter`), which the backend that called this constructor does not keep around
        // past `create_device()` returning (see WebGpuBackend::initialize()'s local `adapters`
        // vector) -- without this addref, the handle stored here (and handed out through
        // `WebGpuNativeAccessExtension`) would dangle the moment that vector goes out of scope.
        if (adapter_ != nullptr) {
            wgpuAdapterAddRef(adapter_);
        }
        if (enable_native_access) {
            native_access_extension_.emplace(instance_, adapter_, device_, queue_);
        }

        // WebGPU exposes exactly one queue, which accepts render, compute, and copy work alike.
        // Reported as a single entry that claims all three classes so the renderer's queue
        // selection resolves everything to it rather than failing to find a transfer queue.
        queue_infos_.push_back(rhi::QueueInfo{
            .queue = rhi::QueueClass::Graphics,
            .capabilities = rhi::QueueCapability::Graphics | rhi::QueueCapability::Compute |
                            rhi::QueueCapability::Transfer,
            .lane_count = 1,
        });

        negotiation_report_.supported_features = features;
        negotiation_report_.requested_required_features = features;
        negotiation_report_.enabled_required_features = features;
    }

    /// Destroys the `WebGpuDevice` and releases every WebGPU object it still owns.
    ///
    /// @note This function does not throw exceptions.
    WebGpuDevice::~WebGpuDevice() {
        // Nothing may be released while the GPU could still be reading it. Dawn drains the queue
        // itself when the device is destroyed, but that happens at the *end* of this function --
        // long after the loop below has dropped the last reference to resources still in flight,
        // which leaves Dawn's deferred deleter walking objects that are already gone.
        // Not just a drain: Dawn queues its deferred deletions against the *pending* command
        // serial, so a submission has to happen before they can retire (see
        // flush_deferred_deletions). Anything still queued when the device tears down is deleted
        // from inside Dawn's own destruction path, which for a swapchain means talking to a window
        // system that may already be gone.
        flush_deferred_deletions();

        // Drained in rough reverse dependency order. Dawn reference-counts everything, so the
        // order only matters for keeping validation quiet, not for correctness.
        query_sets_.drain([](WGPUQuerySet &q) { wgpuQuerySetRelease(q); });
        swapchains_.drain([](rhi::SurfaceHandle &) {});
        surfaces_.drain([](SurfaceEntry &entry) {
            if (entry.surface != nullptr) {
                // A surface still configured when its last reference goes keeps swapchain images
                // alive that the device is about to tear down underneath them.
                if (entry.configured) {
                    wgpuSurfaceUnconfigure(entry.surface);
                }
                wgpuSurfaceRelease(entry.surface);
            }
        });
        render_bundles_.drain([](WGPURenderBundle &b) { wgpuRenderBundleRelease(b); });
        command_buffers_.drain([](WGPUCommandBuffer &c) { wgpuCommandBufferRelease(c); });
        compute_pipelines_.drain([](WGPUComputePipeline &p) { wgpuComputePipelineRelease(p); });
        render_pipelines_.drain([](WGPURenderPipeline &p) { wgpuRenderPipelineRelease(p); });
        pipeline_layouts_.drain([](WGPUPipelineLayout &l) { wgpuPipelineLayoutRelease(l); });
        bind_groups_.drain([](WGPUBindGroup &g) { wgpuBindGroupRelease(g); });
        bind_group_layouts_.drain([](BindGroupLayoutRecord &record) { wgpuBindGroupLayoutRelease(record.layout); });
        shader_modules_.drain([](WGPUShaderModule &m) { wgpuShaderModuleRelease(m); });
        samplers_.drain([](WGPUSampler &s) { wgpuSamplerRelease(s); });
        texture_views_.drain([](TextureViewEntry &v) { wgpuTextureViewRelease(v.view); });
        textures_.drain([](TextureEntry &entry) {
            // Released whether or not the surface owns the storage: the reference is this backend's
            // either way (see acquire_next_texture).
            if (entry.texture != nullptr) {
                wgpuTextureRelease(entry.texture);
            }
        });
        buffers_.drain([](BufferEntry &entry) {
            if (entry.readback != nullptr) {
                wgpuBufferRelease(entry.readback);
            }
            if (entry.buffer != nullptr) {
                wgpuBufferRelease(entry.buffer);
            }
        });

        // Push-constant emulation state (see WebGpuDevicePushConstants.cpp).
        destroy_push_constant_state();

        if (queue_ != nullptr) {
            wgpuQueueRelease(queue_);
        }
        if (device_ != nullptr) {
            wgpuDeviceRelease(device_);
        }
        if (adapter_ != nullptr) {
            wgpuAdapterRelease(adapter_);
        }
        if (instance_ != nullptr) {
            wgpuInstanceRelease(instance_);
        }
    }

    /// Blocks until `future` completes, pumping the Dawn instance while it waits.
    ///
    /// @param future `future` value used by the operation.
    ///
    /// @return Returns `true` when the future completed; `false` on timeout.
    /// @note This function does not throw exceptions.
    bool WebGpuDevice::wait_for(WGPUFuture future) noexcept {
        WGPUFutureWaitInfo wait{.future = future, .completed = 0};
        // UINT64_MAX is "no timeout". Dawn still needs the instance pumped for the callback to run,
        // which wgpuInstanceWaitAny does internally.
        const WGPUWaitStatus status = wgpuInstanceWaitAny(instance_, 1, &wait, UINT64_MAX);
        return status == WGPUWaitStatus_Success && wait.completed != 0;
    }

    /// Returns the current or globally available backend type value.
    ///
    /// @return Returns the current backend type value.
    /// @note This function does not throw exceptions.
    rhi::BackendType WebGpuDevice::backend_type() const noexcept { return rhi::BackendType::WebGpu; }

    /// Returns the adapter description.
    ///
    /// @return Returns a read-only reference to the requested state.
    /// @note This function does not throw exceptions.
    const rhi::AdapterInfo &WebGpuDevice::adapter_info() const noexcept { return adapter_info_; }

    /// Returns the device limits.
    ///
    /// @return Returns a read-only reference to the requested state.
    /// @note This function does not throw exceptions.
    const rhi::DeviceLimits &WebGpuDevice::limits() const noexcept { return limits_; }

    /// Returns the feature negotiation report.
    ///
    /// @return Returns a read-only reference to the requested state.
    /// @note This function does not throw exceptions.
    const rhi::FeatureNegotiationReport &WebGpuDevice::feature_negotiation_report() const noexcept {
        return negotiation_report_;
    }

    /// Returns the features enabled on this device.
    ///
    /// @return Returns a read-only reference to the requested state.
    /// @note This function does not throw exceptions.
    const rhi::FeatureSet &WebGpuDevice::enabled_features() const noexcept { return enabled_features_; }

    /// Returns the properties of the enabled features.
    ///
    /// @return Returns a read-only reference to the requested state.
    /// @note This function does not throw exceptions.
    const rhi::FeatureProperties &WebGpuDevice::feature_properties() const noexcept {
        return feature_properties_;
    }

    /// Returns the queues this device exposes.
    ///
    /// @return Returns a non-owning view of the underlying data.
    /// @note This function does not throw exceptions.
    span<const rhi::QueueInfo> WebGpuDevice::queue_infos() const noexcept { return queue_infos_; }

    /// Returns the extensions enabled on this device.
    ///
    /// WebGPU has no extension mechanism of its own (the kind Vulkan/D3D12 negotiate at device
    /// creation) -- the one entry this can ever report is this engine's own native-access escape
    /// hatch, published only when `enable_native_access` was set at construction. Everything else
    /// this RHI models as a backend-specific extension (full-screen exclusive, composition, ...)
    /// genuinely has no WebGPU counterpart and stays unreported.
    ///
    /// @return Returns a non-owning view of the underlying data.
    /// @note This function does not throw exceptions.
    span<const rhi::ExtensionId> WebGpuDevice::enabled_extensions() const noexcept {
        static constexpr rhi::ExtensionId native_access_id = WebGpuNativeAccessExtension::id();
        return native_access_extension_.has_value() ? span<const rhi::ExtensionId>{&native_access_id, 1}
                                                     : span<const rhi::ExtensionId>{};
    }

    /// Returns the interface for a device extension.
    ///
    /// The one real hatch here is `WebGpuNativeAccessExtension` (raw `WGPUInstance`/`WGPUAdapter`/
    /// `WGPUDevice`/`WGPUQueue`), gated by `enable_native_access` the same way Vulkan/D3D12 gate
    /// their own -- see WebGpuNativeAccessExtension.hpp for why this deliberately stops at the
    /// WebGPU objects rather than reaching beneath them to whatever native API Dawn chose.
    ///
    /// @param extension `extension` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    rhi::RhiDeviceExtension *WebGpuDevice::extension_interface(rhi::ExtensionId extension) noexcept {
        if (native_access_extension_.has_value() &&
            rhi::extension_matches(WebGpuNativeAccessExtension::id(), extension)) {
            return &*native_access_extension_;
        }
        return nullptr;
    }

    /// Waits until the device has finished all submitted work.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::wait_idle() noexcept {
        // WebGPU has no device-wide wait. The equivalent is an on-submitted-work-done callback on
        // the queue, which fires once everything queued so far has completed.
        struct WorkDone {
            bool done = false;
        } state;

        WGPUQueueWorkDoneCallbackInfo info{};
        info.mode = WGPUCallbackMode_WaitAnyOnly;
        info.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void *user_data, void *) {
            static_cast<WorkDone *>(user_data)->done = true;
        };
        info.userdata1 = &state;
        (void)wait_for(wgpuQueueOnSubmittedWorkDone(queue_, info));
    }

} // namespace SFT::Core::WebGpu
