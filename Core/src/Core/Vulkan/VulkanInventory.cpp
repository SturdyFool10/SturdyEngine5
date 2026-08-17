#include <Core/Vulkan/VulkanInventory.hpp>

#include <Core/Vulkan/VulkanPhysicalDevice.hpp>

#include <cstring>
#include <format>
#include <utility>

namespace SFT::Core::Vulkan {

    namespace {

        /// Converts the backend-specific value to the corresponding RHI representation.
        ///
        /// @param type Type value to inspect, select, or convert.
        ///
        /// @return Returns the value converted to RHI device type representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::DeviceType to_rhi_device_type(VkPhysicalDeviceType type) noexcept {
            switch (type) {
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    return RHI::DeviceType::IntegratedGpu;
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    return RHI::DeviceType::DiscreteGpu;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                    return RHI::DeviceType::VirtualGpu;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:
                    return RHI::DeviceType::Cpu;
                default:
                    return RHI::DeviceType::Other;
            }
        }

        /// Performs the physical device ID operation for `Vulkan` using the supplied arguments.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::string physical_device_id(const VulkanPhysicalDevice &device) {
            VkPhysicalDeviceIDProperties ids{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ids};
            vkGetPhysicalDeviceProperties2(device.vk_handle(), &properties);
            if (ids.deviceLUIDValid != VK_TRUE) {
                return {};
            }
            u64 bits = 0;
            static_assert(VK_LUID_SIZE == sizeof(bits));
            std::memcpy(&bits, ids.deviceLUID, sizeof(bits));
            return std::format("windows-luid:{:016x}", bits);
        }

        class VulkanInventoryAdapter final : public RHI::RhiAdapter {
          public:
            /// Constructs a `VulkanInventoryAdapter` from the supplied initialization values.
            ///
            /// @param device Device used or affected by the operation.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            explicit VulkanInventoryAdapter(VulkanPhysicalDevice device) : device_(std::move(device)) {
                const VkPhysicalDeviceProperties &properties = device_.properties();
                info_.name = device_.name().cpp_string();
                info_.vendor = device_.vendor_name();
                info_.driver_version = device_.driver_version_string().cpp_string();
                info_.api_version = device_.api_version_string().cpp_string();
                info_.backend = RHI::BackendType::Vulkan;
                info_.device_type = to_rhi_device_type(properties.deviceType);
                info_.vendor_id = properties.vendorID;
                info_.device_id = properties.deviceID;
                info_.is_discrete = info_.device_type == RHI::DeviceType::DiscreteGpu;
                info_.physical_device_id = physical_device_id(device_);
                limits_.max_texture_dimension_2d = properties.limits.maxImageDimension2D;
                limits_.max_texture_array_layers = properties.limits.maxImageArrayLayers;
                limits_.max_bind_groups = properties.limits.maxBoundDescriptorSets;
                limits_.max_push_constants_size = properties.limits.maxPushConstantsSize;
                limits_.max_vertex_buffers = properties.limits.maxVertexInputBindings;
                limits_.max_vertex_attributes = properties.limits.maxVertexInputAttributes;
                limits_.max_color_attachments = properties.limits.maxColorAttachments;
                limits_.timestamp_period_ns = properties.limits.timestampPeriod;
                queues_.push_back({.queue = RHI::QueueClass::Graphics,
                                   .capabilities = RHI::QueueCapability::Graphics | RHI::QueueCapability::Compute |
                                                   RHI::QueueCapability::Transfer,
                                   .lane_count = 1,
                                   .physical_group = 0,
                                   .likely_parallel_with_graphics = false,
                                   .dedicated = true,
                                   .label = "Vulkan graphics queue"});
            }

            /// Returns the current or globally available info value.
            ///
            /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
            /// @note This function does not throw exceptions.
            [[nodiscard]] const RHI::AdapterInfo &info() const noexcept override { return info_; }
            /// Returns the current or globally available supported features value.
            ///
            /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
            /// @note This function does not throw exceptions.
            [[nodiscard]] const RHI::FeatureSet &supported_features() const noexcept override { return features_; }
            /// Returns the current or globally available feature properties value.
            ///
            /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
            /// @note This function does not throw exceptions.
            [[nodiscard]] const RHI::FeatureProperties &feature_properties() const noexcept override { return properties_; }
            /// Returns the current or globally available supported extensions value.
            ///
            /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
            /// @note This function does not throw exceptions.
            [[nodiscard]] span<const RHI::ExtensionId> supported_extensions() const noexcept override { return extensions_; }
            /// Returns the current or globally available queue infos value.
            ///
            /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
            /// @note This function does not throw exceptions.
            [[nodiscard]] span<const RHI::QueueInfo> queue_infos() const noexcept override { return queues_; }
            /// Returns the current or globally available limits value.
            ///
            /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
            /// @note This function does not throw exceptions.
            [[nodiscard]] const RHI::DeviceLimits &limits() const noexcept override { return limits_; }
            /// Creates a device from the supplied parameters.
            ///
            /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
            /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
            /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`.
            [[nodiscard]] RHI::RhiExpected<unique_ptr<RHI::RhiDevice>> create_device(const RHI::DeviceRequest &) override {
                return RHI::rhi_error(RHI::RhiErrorCode::Unsupported,
                                      "The Vulkan inventory adapter is discovery-only; renderer-owned Vulkan device creation remains in Core::VulkanBackend.");
            }

          private:
            VulkanPhysicalDevice device_;
            RHI::AdapterInfo info_{};
            RHI::FeatureSet features_{};
            RHI::FeatureProperties properties_{};
            RHI::DeviceLimits limits_{};
            vector<RHI::ExtensionId> extensions_;
            vector<RHI::QueueInfo> queues_;
        };

        class VulkanInventoryInstance final : public RHI::RhiInstance {
          public:
            /// Constructs a `VulkanInventoryInstance` from the supplied initialization values.
            ///
            /// @param instance Instance used or affected by the operation.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            explicit VulkanInventoryInstance(VkInstance instance) : instance_(instance) {}
            /// Destroys the `VulkanInventoryInstance` and releases resources owned by it.
            ///
            /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
            ~VulkanInventoryInstance() override { vkDestroyInstance(instance_, nullptr); }
            /// Returns the current or globally available backend type value.
            ///
            /// @return Returns the current backend type value.
            /// @note This function does not throw exceptions.
            [[nodiscard]] RHI::BackendType backend_type() const noexcept override { return RHI::BackendType::Vulkan; }
            /// Enumerates adapters using the supplied arguments and current state.
            ///
            /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
            /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
            /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::OperationFailed`.
            [[nodiscard]] RHI::RhiExpected<vector<unique_ptr<RHI::RhiAdapter>>> enumerate_adapters() override {
                auto devices = VulkanPhysicalDevice::enumerate(instance_);
                if (!devices)
                    return RHI::rhi_error(RHI::RhiErrorCode::OperationFailed, devices.error().message);
                vector<unique_ptr<RHI::RhiAdapter>> adapters;
                for (VulkanPhysicalDevice &device : *devices)
                    adapters.push_back(std::make_unique<VulkanInventoryAdapter>(std::move(device)));
                return adapters;
            }

          private:
            VkInstance instance_ = VK_NULL_HANDLE;
        };

        /// Creates a vulkan inventory instance from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::OperationFailed`.
        [[nodiscard]] RHI::RhiExpected<unique_ptr<RHI::RhiInstance>> create_vulkan_inventory_instance(const RHI::InstanceDesc &desc) {
            if (volkInitialize() != VK_SUCCESS)
                return RHI::rhi_error(RHI::RhiErrorCode::OperationFailed, "Vulkan loader initialization failed.");
            VkApplicationInfo application{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = desc.application_name.data(), .applicationVersion = desc.application_version, .pEngineName = desc.engine_name.data(), .engineVersion = desc.engine_version, .apiVersion = VK_API_VERSION_1_1};
            VkInstanceCreateInfo create_info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application};
            VkInstance instance = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateInstance(&create_info, nullptr, &instance); result != VK_SUCCESS)
                return RHI::rhi_error(RHI::RhiErrorCode::OperationFailed, std::format("vkCreateInstance failed ({})", static_cast<i32>(result)));
            volkLoadInstance(instance);
            return unique_ptr<RHI::RhiInstance>(std::make_unique<VulkanInventoryInstance>(instance));
        }
    } // namespace

    /// Returns the current or globally available vulkan inventory backend registration value.
    ///
    /// @return Returns the current vulkan inventory backend registration value.
    /// @note This function does not throw exceptions.
    RHI::BackendRegistration vulkan_inventory_backend_registration() noexcept {
        return {.backend = RHI::BackendType::Vulkan, .name = "Vulkan", .create_instance = create_vulkan_inventory_instance};
    }
} // namespace SFT::Core::Vulkan
