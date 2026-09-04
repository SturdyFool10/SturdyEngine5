#include <Core/WebGPU/RHI/WebGpuAdapter.hpp>

#include <Core/WebGPU/RHI/WebGpuCommon.hpp>
#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

#include <memory>
#include <vector>

namespace SFT::Core::WebGpu {

    namespace rhi = ::SFT::RHI;

    namespace {

        /// Blocks until `future` resolves on `instance`.
        ///
        /// @param instance `instance` value used by the operation.
        /// @param future `future` value used by the operation.
        ///
        /// @return Returns `true` when the future completed.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool wait_on(WGPUInstance instance, WGPUFuture future) noexcept {
            WGPUFutureWaitInfo wait{.future = future, .completed = 0};
            return wgpuInstanceWaitAny(instance, 1, &wait, UINT64_MAX) == WGPUWaitStatus_Success &&
                   wait.completed != 0;
        }

        /// Maps a WebGPU adapter type onto the RHI's device classification.
        ///
        /// @param type `type` value used by the operation.
        ///
        /// @return Returns the value converted to the RHI representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] rhi::DeviceType to_rhi_device_type(WGPUAdapterType type) noexcept {
            switch (type) {
                case WGPUAdapterType_DiscreteGPU: return rhi::DeviceType::DiscreteGpu;
                case WGPUAdapterType_IntegratedGPU: return rhi::DeviceType::IntegratedGpu;
                case WGPUAdapterType_CPU: return rhi::DeviceType::Cpu;
                default: return rhi::DeviceType::Other;
            }
        }

        /// Fills the RHI's device limits from a WebGPU adapter's reported limits.
        ///
        /// Several RHI limits have no WebGPU counterpart and are left at values that describe what
        /// the API guarantees rather than what the driver could do: WebGPU has no push constants,
        /// no timestamp period query, and no way to ask which sample counts an attachment supports
        /// beyond the fixed 1 and 4 the specification requires.
        ///
        /// @param limits `limits` value used by the operation.
        ///
        /// @return Returns the value converted to the RHI representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] rhi::DeviceLimits to_rhi_limits(const WGPULimits &limits) noexcept {
            rhi::DeviceLimits out{};
            out.max_texture_dimension_2d = limits.maxTextureDimension2D;
            out.max_texture_array_layers = limits.maxTextureArrayLayers;
            out.max_bind_groups = limits.maxBindGroups;
            // WebGPU has no push constants, but this backend emulates them with a dynamic-offset
            // uniform buffer (WebGpuDevicePushConstants.cpp), so the limit reported is the block
            // size that emulation supports rather than zero -- a caller checking this is asking
            // whether it can use the RHI's push-constant entry points, and it can.
            out.max_push_constants_size = 256;
            out.max_vertex_buffers = limits.maxVertexBuffers;
            out.max_vertex_attributes = limits.maxVertexAttributes;
            out.max_color_attachments = limits.maxColorAttachments;
            // WebGPU requires 1x and 4x multisampling and offers nothing to query beyond that.
            out.max_framebuffer_sample_count = 4;
            out.framebuffer_sample_counts = 1u | 4u;
            out.supports_minimum_depth_resolve = false;
            out.supports_bc_texture_compression = false; // Set by the caller from the feature list.
            out.max_compute_workgroup_size_x = limits.maxComputeWorkgroupSizeX;
            out.max_compute_workgroup_size_y = limits.maxComputeWorkgroupSizeY;
            out.max_compute_workgroup_size_z = limits.maxComputeWorkgroupSizeZ;
            out.min_uniform_buffer_offset_alignment = limits.minUniformBufferOffsetAlignment;
            out.min_storage_buffer_offset_alignment = limits.minStorageBufferOffsetAlignment;
            // WebGPU timestamps are already in nanoseconds, so the period is exactly 1.
            out.timestamp_period_ns = 1.0f;
            out.timestamp_valid_bits = 64;
            return out;
        }

        /// A WebGPU adapter.
        class WebGpuAdapter final : public rhi::RhiAdapter {
          public:
            /// Constructs an adapter around a Dawn adapter handle.
            ///
            /// @param instance The instance the adapter came from.
            /// @param adapter The Dawn adapter, whose ownership passes to this object.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            WebGpuAdapter(WGPUInstance instance, WGPUAdapter adapter)
                : instance_(instance), adapter_(adapter) {
                wgpuInstanceAddRef(instance_);

                WGPUAdapterInfo info{};
                if (wgpuAdapterGetInfo(adapter_, &info) == WGPUStatus_Success) {
                    info_.name = to_string(info.device);
                    info_.vendor = to_string(info.vendor);
                    info_.driver_version = to_string(info.description);
                    info_.vendor_id = info.vendorID;
                    info_.device_id = info.deviceID;
                    info_.device_type = to_rhi_device_type(info.adapterType);
                    info_.is_discrete = info.adapterType == WGPUAdapterType_DiscreteGPU;
                    // Names which of the three enabled native APIs Dawn actually chose, which is the
                    // single most useful thing to know when a WebGPU-only bug appears.
                    switch (info.backendType) {
                        case WGPUBackendType_Vulkan: info_.api_version = "WebGPU (Vulkan)"; break;
                        case WGPUBackendType_Metal: info_.api_version = "WebGPU (Metal)"; break;
                        case WGPUBackendType_D3D12: info_.api_version = "WebGPU (D3D12)"; break;
                        default: info_.api_version = "WebGPU"; break;
                    }
                    wgpuAdapterInfoFreeMembers(info);
                }
                info_.backend = rhi::BackendType::WebGpu;
                // Deliberately left empty: WebGPU exposes no device UUID or adapter LUID, and the
                // PCI vendor/device pair is not unique across two identical cards. The inventory
                // has a fallback for exactly this case (see find_physical_gpu in RHI/Inventory.cpp);
                // inventing an ID here would defeat it and list this GPU twice.
                info_.physical_device_id.clear();

                WGPULimits limits{};
                if (wgpuAdapterGetLimits(adapter_, &limits) == WGPUStatus_Success) {
                    limits_ = to_rhi_limits(limits);
                }
                limits_.supports_bc_texture_compression =
                    wgpuAdapterHasFeature(adapter_, WGPUFeatureName_TextureCompressionBC) != 0;
                populate_features();

                queue_infos_.push_back(rhi::QueueInfo{
                    .queue = rhi::QueueClass::Graphics,
                    .capabilities = rhi::QueueCapability::Graphics | rhi::QueueCapability::Compute |
                                    rhi::QueueCapability::Transfer,
                    .lane_count = 1,
                });
            }

            /// Destroys the adapter.
            ///
            /// @note This function does not throw exceptions.
            ~WebGpuAdapter() override {
                if (adapter_ != nullptr) {
                    wgpuAdapterRelease(adapter_);
                }
                if (instance_ != nullptr) {
                    wgpuInstanceRelease(instance_);
                }
            }

            [[nodiscard]] const rhi::AdapterInfo &info() const noexcept override { return info_; }
            [[nodiscard]] const rhi::FeatureSet &supported_features() const noexcept override {
                return features_;
            }
            [[nodiscard]] const rhi::FeatureProperties &feature_properties() const noexcept override {
                return feature_properties_;
            }
            [[nodiscard]] span<const rhi::ExtensionId> supported_extensions() const noexcept override {
                return {};
            }
            [[nodiscard]] span<const rhi::QueueInfo> queue_infos() const noexcept override {
                return queue_infos_;
            }
            [[nodiscard]] const rhi::DeviceLimits &limits() const noexcept override { return limits_; }

            /// Creates a device on this adapter.
            ///
            /// @param request `request` value used by the operation.
            ///
            /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
            /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
            [[nodiscard]] rhi::RhiExpected<std::unique_ptr<rhi::RhiDevice>> create_device(
                const rhi::DeviceRequest &request) override {
                // A caller that marked a feature *required* has to be told when this adapter cannot
                // provide it, rather than being handed a device that silently lacks it.
                if (!features_.contains_all(request.required_features)) {
                    return std::unexpected(rhi::rhi_error(
                        rhi::RhiErrorCode::Unsupported,
                        "create_device: this WebGPU adapter cannot provide every required feature."));
                }

                // Only features this backend can actually use are requested, and only when the
                // adapter has them: asking for one it lacks fails device creation outright.
                std::vector<WGPUFeatureName> features;
                for (WGPUFeatureName candidate : {WGPUFeatureName_TextureCompressionBC,
                                                  WGPUFeatureName_TimestampQuery,
                                                  WGPUFeatureName_Depth32FloatStencil8,
                                                  WGPUFeatureName_IndirectFirstInstance,
                                                  WGPUFeatureName_Float32Filterable}) {
                    if (wgpuAdapterHasFeature(adapter_, candidate) != 0) {
                        features.push_back(candidate);
                    }
                }

                struct DeviceResult {
                    WGPUDevice device = nullptr;
                    std::string message;
                } result;

                WGPUDeviceDescriptor device_desc{};
                device_desc.label = wgpu_string(request.label);
                device_desc.requiredFeatureCount = features.size();
                device_desc.requiredFeatures = features.data();
                // A lost device is fatal for the frame loop above; logged here so it is not silent.
                device_desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
                device_desc.deviceLostCallbackInfo.callback =
                    [](const WGPUDevice *, WGPUDeviceLostReason, WGPUStringView message, void *, void *) {
                        Foundation::log_error("WebGPU device lost: {}", to_string(message));
                    };
                device_desc.uncapturedErrorCallbackInfo.callback =
                    [](const WGPUDevice *, WGPUErrorType, WGPUStringView message, void *, void *) {
                        Foundation::log_error("WebGPU validation error: {}", to_string(message));
                    };

                WGPURequestDeviceCallbackInfo callback{};
                callback.mode = WGPUCallbackMode_WaitAnyOnly;
                callback.callback = [](WGPURequestDeviceStatus status, WGPUDevice device,
                                       WGPUStringView message, void *user_data, void *) {
                    auto &out = *static_cast<DeviceResult *>(user_data);
                    if (status == WGPURequestDeviceStatus_Success) {
                        out.device = device;
                    } else {
                        out.message = to_string(message);
                    }
                };
                callback.userdata1 = &result;

                if (!wait_on(instance_, wgpuAdapterRequestDevice(adapter_, &device_desc, callback)) ||
                    result.device == nullptr) {
                    return std::unexpected(webgpu_error("request_device", result.message));
                }

                rhi::DeviceLimits device_limits = limits_;
                device_limits.supports_bc_texture_compression =
                    wgpuDeviceHasFeature(result.device, WGPUFeatureName_TextureCompressionBC) != 0;

                return std::make_unique<WebGpuDevice>(instance_, result.device, info_, device_limits,
                                                      features_);
            }

          private:
            /// Records the RHI features this adapter can actually honour.
            ///
            /// WebGPU's own optional-feature list is short and mostly does not line up with the
            /// RHI's, which is modelled on Vulkan's. Only the ones with a genuine WebGPU
            /// counterpart are claimed here; everything else stays unset, which is the honest
            /// answer rather than an omission -- WebGPU has no ray tracing, mesh shaders, bindless
            /// descriptors or timeline semaphores to report in the first place.
            ///
            /// @note This function does not throw exceptions.
            void populate_features() noexcept {
                const auto claim = [this](WGPUFeatureName dawn_feature, rhi::Feature rhi_feature) {
                    if (wgpuAdapterHasFeature(adapter_, dawn_feature) != 0) {
                        features_.set(rhi_feature);
                    }
                };
                claim(WGPUFeatureName_TextureCompressionBC, rhi::Feature::TextureCompressionBC);
                claim(WGPUFeatureName_TextureCompressionETC2, rhi::Feature::TextureCompressionETC2);
                claim(WGPUFeatureName_TextureCompressionASTC, rhi::Feature::TextureCompressionASTC);
                claim(WGPUFeatureName_TimestampQuery, rhi::Feature::TimestampQueries);
                claim(WGPUFeatureName_IndirectFirstInstance, rhi::Feature::DrawIndirectFirstInstance);
                claim(WGPUFeatureName_DualSourceBlending, rhi::Feature::DualSourceBlending);
                claim(WGPUFeatureName_ClipDistances, rhi::Feature::ShaderClipDistance);
                claim(WGPUFeatureName_Subgroups, rhi::Feature::SubgroupOperations);

                // Not gated on an optional WebGPU feature: occlusion queries are part of the core
                // API, so every adapter has them.
                features_.set(rhi::Feature::OcclusionQueries);
            }

            WGPUInstance instance_ = nullptr;
            WGPUAdapter adapter_ = nullptr;
            rhi::AdapterInfo info_{};
            rhi::DeviceLimits limits_{};
            rhi::FeatureSet features_{};
            rhi::FeatureProperties feature_properties_{};
            std::vector<rhi::QueueInfo> queue_infos_{};
        };

        /// A WebGPU instance.
        class WebGpuInstance final : public rhi::RhiInstance {
          public:
            /// Constructs an instance around a Dawn instance handle.
            ///
            /// @param instance The Dawn instance, whose ownership passes to this object.
            ///
            /// @note This function does not throw exceptions.
            explicit WebGpuInstance(WGPUInstance instance) noexcept : instance_(instance) {}

            /// Destroys the instance.
            ///
            /// @note This function does not throw exceptions.
            ~WebGpuInstance() override {
                if (instance_ != nullptr) {
                    wgpuInstanceRelease(instance_);
                }
            }

            [[nodiscard]] rhi::BackendType backend_type() const noexcept override {
                return rhi::BackendType::WebGpu;
            }

            /// Enumerates the adapters this instance can reach.
            ///
            /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
            /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
            [[nodiscard]] rhi::RhiExpected<std::vector<std::unique_ptr<rhi::RhiAdapter>>>
            enumerate_adapters() override {
                // WebGPU has no adapter enumeration: it asks the implementation to *pick* one for a
                // stated preference. Both preferences are requested and de-duplicated, which on a
                // hybrid machine surfaces the discrete and the integrated GPU separately and on a
                // single-GPU machine collapses to one entry. Whatever comes back is already
                // restricted to Vulkan, Metal, or D3D12, because those are the only backends this
                // Dawn is compiled with.
                std::vector<std::unique_ptr<rhi::RhiAdapter>> adapters;
                std::vector<WGPUAdapter> seen;

                for (WGPUPowerPreference preference :
                     {WGPUPowerPreference_HighPerformance, WGPUPowerPreference_LowPower}) {
                    struct AdapterResult {
                        WGPUAdapter adapter = nullptr;
                        std::string message;
                    } result;

                    WGPURequestAdapterOptions options{};
                    options.powerPreference = preference;

                    WGPURequestAdapterCallbackInfo callback{};
                    callback.mode = WGPUCallbackMode_WaitAnyOnly;
                    callback.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter,
                                           WGPUStringView message, void *user_data, void *) {
                        auto &out = *static_cast<AdapterResult *>(user_data);
                        if (status == WGPURequestAdapterStatus_Success) {
                            out.adapter = adapter;
                        } else {
                            out.message = to_string(message);
                        }
                    };
                    callback.userdata1 = &result;

                    if (!wait_on(instance_, wgpuInstanceRequestAdapter(instance_, &options, callback)) ||
                        result.adapter == nullptr) {
                        continue;
                    }

                    // The two preferences commonly resolve to the same physical adapter; comparing
                    // the reported device IDs avoids listing it twice.
                    WGPUAdapterInfo info{};
                    bool duplicate = false;
                    if (wgpuAdapterGetInfo(result.adapter, &info) == WGPUStatus_Success) {
                        for (WGPUAdapter existing : seen) {
                            WGPUAdapterInfo other{};
                            if (wgpuAdapterGetInfo(existing, &other) == WGPUStatus_Success) {
                                duplicate = other.deviceID == info.deviceID &&
                                            other.vendorID == info.vendorID;
                                wgpuAdapterInfoFreeMembers(other);
                            }
                            if (duplicate) {
                                break;
                            }
                        }
                        wgpuAdapterInfoFreeMembers(info);
                    }
                    if (duplicate) {
                        wgpuAdapterRelease(result.adapter);
                        continue;
                    }

                    seen.push_back(result.adapter);
                    adapters.push_back(std::make_unique<WebGpuAdapter>(instance_, result.adapter));
                }

                if (adapters.empty()) {
                    return std::unexpected(webgpu_error(
                        "enumerate_adapters",
                        "no adapter is reachable through Vulkan, Metal, or D3D12"));
                }
                return adapters;
            }

          private:
            WGPUInstance instance_ = nullptr;
        };

        /// Creates the WebGPU instance the backend registry hands out.
        ///
        /// @param desc `desc` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<std::unique_ptr<rhi::RhiInstance>> create_webgpu_instance(
            const rhi::InstanceDesc &desc) {
            // TimedWaitAny is what makes wgpuInstanceWaitAny usable to block on a future, which is
            // how every asynchronous WebGPU entry point is turned back into the synchronous call
            // the RHI declares.
            std::vector<WGPUInstanceFeatureName> required{WGPUInstanceFeatureName_TimedWaitAny};

            WGPUInstanceDescriptor instance_desc{};
            instance_desc.requiredFeatureCount = required.size();
            instance_desc.requiredFeatures = required.data();

            // Dawn's toggles are how validation is controlled; the RHI's enable_validation maps onto
            // *disabling* Dawn's "skip_validation" rather than enabling anything.
            const char *enabled_toggles[] = {"allow_unsafe_apis"};
            const char *disabled_toggles[] = {"skip_validation"};
            WGPUDawnTogglesDescriptor toggles{};
            toggles.chain.sType = WGPUSType_DawnTogglesDescriptor;
            toggles.enabledToggleCount = 1;
            toggles.enabledToggles = enabled_toggles;
            if (desc.enable_validation) {
                toggles.disabledToggleCount = 1;
                toggles.disabledToggles = disabled_toggles;
            }
            instance_desc.nextInChain = &toggles.chain;

            WGPUInstance instance = wgpuCreateInstance(&instance_desc);
            if (instance == nullptr) {
                return std::unexpected(webgpu_error("wgpuCreateInstance"));
            }
            return std::make_unique<WebGpuInstance>(instance);
        }

    } // namespace

    /// Returns this backend's registration entry.
    ///
    /// @return Returns the current backend registration value.
    /// @note This function does not throw exceptions.
    RHI::BackendRegistration webgpu_backend_registration() noexcept {
        return {.backend = RHI::BackendType::WebGpu, .name = "WebGPU", .create_instance = create_webgpu_instance};
    }

} // namespace SFT::Core::WebGpu
