#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanHelpers.hpp>

using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using std::nullopt;
using std::optional;
using std::string_view;
using std::vector;

namespace SFT::Core::Vulkan {


    class VulkanPhysicalDevice {
      public:
        /// Constructs a `VulkanPhysicalDevice` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanPhysicalDevice() = default;

        /// Constructs a `VulkanPhysicalDevice` from the supplied initialization values.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit VulkanPhysicalDevice(VkPhysicalDevice device);


        /// Enumerates the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param instance Instance used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] static RendererExpected<vector<VulkanPhysicalDevice>> enumerate(VkInstance instance);

        /// Returns the Vulkan handle associated with this `VulkanPhysicalDevice`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkPhysicalDevice vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanPhysicalDevice`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;

        /// Returns the current or globally available properties value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const VkPhysicalDeviceProperties &properties() const noexcept;
        /// Returns the current or globally available features value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const VkPhysicalDeviceFeatures &features() const noexcept;


        /// Queries features2 from the active backend or runtime state.
        ///
        /// @param features `features` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void query_features2(VkPhysicalDeviceFeatures2 &features) const noexcept;
        /// Returns the current or globally available memory properties value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const VkPhysicalDeviceMemoryProperties &memory_properties() const noexcept;
        /// Returns the current or globally available queue families value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const vector<VkQueueFamilyProperties> &queue_families() const noexcept;


        /// Returns the current or globally available name value.
        ///
        /// @return Returns the current name value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ustr name() const;
        /// Returns a human-readable name for the supplied type value.
        ///
        /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *type_name() const noexcept;


        /// Returns the current or globally available vendor ID value.
        ///
        /// @return Returns the current vendor ID value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 vendor_id() const noexcept;
        /// Returns a human-readable name for the supplied vendor value.
        ///
        /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *vendor_name() const noexcept;
        /// Returns the current or globally available device ID value.
        ///
        /// @return Returns the current device ID value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 device_id() const noexcept;


        /// Returns the current or globally available driver version value.
        ///
        /// @return Returns the current driver version value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 driver_version() const noexcept;
        /// Returns the current or globally available driver version string value.
        ///
        /// @return Returns the current driver version string value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString driver_version_string() const;


        /// Returns the current or globally available API version value.
        ///
        /// @return Returns the current API version value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 api_version() const noexcept;
        /// Returns the current or globally available API version string value.
        ///
        /// @return Returns the current API version string value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString api_version_string() const;


        /// Returns the current or globally available timestamp period value.
        ///
        /// @return Returns the current timestamp period value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 timestamp_period() const noexcept;


        /// Performs the timestamp valid bits operation for `VulkanPhysicalDevice` using the supplied arguments.
        ///
        /// @param queue_family_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 timestamp_valid_bits(u32 queue_family_index) const noexcept;


        /// Returns the current or globally available calibrateable time domains value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<vector<VkTimeDomainKHR>> calibrateable_time_domains() const noexcept;


        /// Returns the current or globally available score value.
        ///
        /// @return Returns the current score value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 score() const noexcept;


        /// Finds the requested entry in the available state.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<u32> findGraphicsQueue(VkSurfaceKHR surface) noexcept;


        /// Finds present queue family in the available state.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<u32> find_present_queue_family(VkSurfaceKHR surface) noexcept;


        /// Performs the queue family supports present operation for `VulkanPhysicalDevice` using the supplied arguments.
        ///
        /// @param family_index Zero-based index of the target element or entry.
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool queue_family_supports_present(u32 family_index, VkSurfaceKHR surface) const noexcept;

        /// Finds compute queue family in the available state.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<u32> find_compute_queue_family() noexcept;

        /// Finds transfer queue family in the available state.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<u32> find_transfer_queue_family() noexcept;

        /// Finds sparse binding queue family in the available state.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<u32> find_sparse_binding_queue_family() noexcept;

        /// Finds protected queue family in the available state.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<u32> find_protected_queue_family() noexcept;

        /// Finds video decode queue family in the available state.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<u32> find_video_decode_queue_family() noexcept;

        /// Finds video encode queue family in the available state.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<u32> find_video_encode_queue_family() noexcept;

        /// Finds optical flow queue family in the available state.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<u32> find_optical_flow_queue_family() noexcept;


        /// Enumerates extensions using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] RendererExpected<vector<VkExtensionProperties>> enumerate_extensions() const;


        /// Reports whether extension holds for this `VulkanPhysicalDevice`.
        ///
        /// @param name Name used to identify or label the target.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool supports_extension(string_view name) const;


        /// Performs the surface capabilities operation for `VulkanPhysicalDevice` using the supplied arguments.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkSurfaceCapabilitiesKHR>
        surface_capabilities(VkSurfaceKHR surface) const noexcept;

        /// Performs the surface formats operation for `VulkanPhysicalDevice` using the supplied arguments.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] RendererExpected<vector<VkSurfaceFormatKHR>>
        surface_formats(VkSurfaceKHR surface) const;

        /// Performs the surface present modes operation for `VulkanPhysicalDevice` using the supplied arguments.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] RendererExpected<vector<VkPresentModeKHR>>
        surface_present_modes(VkSurfaceKHR surface) const;

      private:

        /// Finds queue family with in the available state.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<u32> find_queue_family_with(VkQueueFlags flags) const noexcept;

        VkPhysicalDevice device_ = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties properties_ = {};
        VkPhysicalDeviceFeatures features_ = {};
        VkPhysicalDeviceMemoryProperties memory_properties_ = {};
        vector<VkQueueFamilyProperties> queue_families_;
        optional<u32> gfxQueueFamIdx = nullopt;
        optional<u32> presentQueueFamIdx = nullopt;
        optional<u32> computeQueueFamIdx = nullopt;
        optional<u32> transferQueueFamIdx = nullopt;
        optional<u32> sparseBindingQueueFamIdx = nullopt;
        optional<u32> protectedQueueFamIdx = nullopt;
        optional<u32> videoDecodeQueueFamIdx = nullopt;
        optional<u32> videoEncodeQueueFamIdx = nullopt;
        optional<u32> opticalFlowQueueFamIdx = nullopt;
    };

} // namespace SFT::Core::Vulkan
