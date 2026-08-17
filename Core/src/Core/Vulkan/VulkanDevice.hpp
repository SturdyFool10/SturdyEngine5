#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <algorithm>
#include <optional>
#include <ranges>
#include <span>
#include <vector>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>

using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::optional;
using std::span;
using std::vector;

namespace SFT::Core::Vulkan {


    class VulkanDevice {
      public:


        struct DeviceCreateDesc {
            optional<u32> graphics_queue_family{};
            optional<u32> present_queue_family{};
            optional<u32> compute_queue_family{};
            optional<u32> transfer_queue_family{};
            optional<u32> sparse_queue_family{};
            optional<u32> video_decode_queue_family{};
            optional<u32> video_encode_queue_family{};


            u32 present_queue_index = 0;
            u32 graphics_queue_count = 1;
            u32 compute_queue_count = 1;
            u32 transfer_queue_count = 1;
            u32 sparse_queue_count = 1;
            u32 video_decode_queue_count = 1;
            u32 video_encode_queue_count = 1;
            span<const char *> extensions{};


            const void *features_pnext = nullptr;
        };

        /// Constructs a `VulkanDevice` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanDevice() = default;

        /// Destroys the `VulkanDevice` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanDevice();

        /// Disables this construction form for `VulkanDevice`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanDevice(const VulkanDevice &) = delete;
        /// Assigns a new value to this `VulkanDevice`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanDevice &operator=(const VulkanDevice &) = delete;

        /// Constructs a `VulkanDevice` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanDevice(VulkanDevice &&o) noexcept;

        /// Assigns a new value to this `VulkanDevice`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanDevice &operator=(VulkanDevice &&o) noexcept;


        /// Creates a `VulkanDevice` resource or value from the supplied parameters.
        ///
        /// @param physical `physical` value used by the operation.
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanDevice> create(
            VkPhysicalDevice physical,
            const DeviceCreateDesc &desc) noexcept;


        /// Returns the Vulkan handle associated with this `VulkanDevice`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDevice vk_handle() const noexcept;
        /// Returns the physical Vulkan handle associated with this `VulkanDevice`.
        ///
        /// @return Returns the current physical Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkPhysicalDevice physical_vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanDevice`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;

        /// Returns the current or globally available graphics queue value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<VulkanQueue> &graphics_queue() noexcept;
        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<VulkanQueue> &present_queue() noexcept;
        /// Computes queue using the supplied arguments and current state.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<VulkanQueue> &compute_queue() noexcept;
        /// Returns the current or globally available transfer queue value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<VulkanQueue> &transfer_queue() noexcept;
        /// Returns the current or globally available sparse queue value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<VulkanQueue> &sparse_queue() noexcept;
        /// Returns the current or globally available video decode queue value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<VulkanQueue> &video_decode_queue() noexcept;
        /// Returns the current or globally available video encode queue value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<VulkanQueue> &video_encode_queue() noexcept;
        /// Returns the current or globally available graphics queue lanes value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] vector<VulkanQueue> &graphics_queue_lanes() noexcept;
        /// Computes queue lanes using the supplied arguments and current state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] vector<VulkanQueue> &compute_queue_lanes() noexcept;
        /// Returns the current or globally available transfer queue lanes value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] vector<VulkanQueue> &transfer_queue_lanes() noexcept;
        /// Returns the current or globally available sparse queue lanes value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] vector<VulkanQueue> &sparse_queue_lanes() noexcept;
        /// Returns the current or globally available video decode queue lanes value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] vector<VulkanQueue> &video_decode_queue_lanes() noexcept;
        /// Returns the current or globally available video encode queue lanes value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] vector<VulkanQueue> &video_encode_queue_lanes() noexcept;

        /// Waits for idle to complete.
        ///
        /// @note This function does not throw exceptions.
        void wait_idle() noexcept;

        /// Destroys or releases the `VulkanDevice` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;


        /// Allocates memory.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkDeviceMemory> allocate_memory(const VkMemoryAllocateInfo &info) noexcept;

        /// Releases previously allocated storage or resources.
        ///
        /// @param memory `memory` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void free_memory(VkDeviceMemory memory) noexcept;

        /// Maps memory for access.
        ///
        /// @param memory `memory` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param size Requested or available size for the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<void *> map_memory(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags = 0) noexcept;

        /// Unmaps memory.
        ///
        /// @param memory `memory` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void unmap_memory(VkDeviceMemory memory) noexcept;

        /// Flushes mapped memory ranges.
        ///
        /// @param ranges `ranges` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult flush_mapped_memory_ranges(span<const VkMappedMemoryRange> ranges) noexcept;

        /// Performs the invalidate mapped memory ranges operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param ranges `ranges` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult invalidate_mapped_memory_ranges(span<const VkMappedMemoryRange> ranges) noexcept;


        /// Creates a buffer from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkBuffer> create_buffer(const VkBufferCreateInfo &info) noexcept;

        /// Destroys the buffer identified by the supplied parameters.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_buffer(VkBuffer buffer) noexcept;

        /// Performs the buffer memory requirements operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkMemoryRequirements buffer_memory_requirements(VkBuffer buffer) const noexcept;

        /// Performs the buffer memory requirements2 operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkMemoryRequirements2 buffer_memory_requirements2(VkBuffer buffer) const noexcept;

        /// Binds buffer memory for subsequent operations.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param memory `memory` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult bind_buffer_memory(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset = 0) noexcept;


        /// Performs the buffer device address operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDeviceAddress buffer_device_address(VkBuffer buffer) const noexcept;

        /// Performs the buffer opaque capture address operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 buffer_opaque_capture_address(VkBuffer buffer) const noexcept;


        /// Creates a image from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkImage> create_image(const VkImageCreateInfo &info) noexcept;

        /// Destroys the image identified by the supplied parameters.
        ///
        /// @param image `image` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_image(VkImage image) noexcept;

        /// Performs the image memory requirements operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param image `image` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkMemoryRequirements image_memory_requirements(VkImage image) const noexcept;

        /// Performs the image memory requirements2 operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param image `image` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkMemoryRequirements2 image_memory_requirements2(VkImage image) const noexcept;

        /// Binds image memory for subsequent operations.
        ///
        /// @param image `image` value used by the operation.
        /// @param memory `memory` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult bind_image_memory(VkImage image, VkDeviceMemory memory, VkDeviceSize offset = 0) noexcept;

        /// Performs the image subresource layout operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param image `image` value used by the operation.
        /// @param subresource `subresource` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkSubresourceLayout image_subresource_layout(VkImage image,
                                                                   const VkImageSubresource &subresource) const noexcept;


        /// Creates a image view from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkImageView> create_image_view(const VkImageViewCreateInfo &info) noexcept;

        /// Destroys the image view identified by the supplied parameters.
        ///
        /// @param view `view` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_image_view(VkImageView view) noexcept;


        /// Creates a sampler from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkSampler> create_sampler(const VkSamplerCreateInfo &info) noexcept;

        /// Destroys the sampler identified by the supplied parameters.
        ///
        /// @param sampler Sampler used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_sampler(VkSampler sampler) noexcept;


        /// Creates a shader module from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkShaderModule> create_shader_module(const VkShaderModuleCreateInfo &info) noexcept;

        /// Destroys the shader module identified by the supplied parameters.
        ///
        /// @param shader_module Shader used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_shader_module(VkShaderModule shader_module) noexcept;


        /// Creates a pipeline layout from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkPipelineLayout> create_pipeline_layout(const VkPipelineLayoutCreateInfo &info) noexcept;

        /// Destroys the pipeline layout identified by the supplied parameters.
        ///
        /// @param layout `layout` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_pipeline_layout(VkPipelineLayout layout) noexcept;


        /// Creates a pipeline cache from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkPipelineCache> create_pipeline_cache(const VkPipelineCacheCreateInfo &info) noexcept;

        /// Destroys the pipeline cache identified by the supplied parameters.
        ///
        /// @param cache `cache` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_pipeline_cache(VkPipelineCache cache) noexcept;

        /// Performs the merge pipeline caches operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param dst Destination value or resource.
        /// @param srcs `srcs` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult merge_pipeline_caches(VkPipelineCache dst,
                                                           span<const VkPipelineCache> srcs) noexcept;

        /// Returns the pipeline cache data associated with this `VulkanDevice`.
        ///
        /// @param cache `cache` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] RendererExpected<vector<u8>> pipeline_cache_data(VkPipelineCache cache) const;


        /// Creates a graphics pipeline from the supplied parameters.
        ///
        /// @param cache `cache` value used by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkPipeline> create_graphics_pipeline(
            VkPipelineCache cache,
            const VkGraphicsPipelineCreateInfo &info) noexcept;

        /// Creates a graphics pipelines from the supplied parameters.
        ///
        /// @param cache `cache` value used by the operation.
        /// @param infos Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] RendererExpected<vector<VkPipeline>> create_graphics_pipelines(
            VkPipelineCache cache,
            span<const VkGraphicsPipelineCreateInfo> infos);

        /// Creates a compute pipeline from the supplied parameters.
        ///
        /// @param cache `cache` value used by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkPipeline> create_compute_pipeline(
            VkPipelineCache cache,
            const VkComputePipelineCreateInfo &info) noexcept;

        /// Creates a compute pipelines from the supplied parameters.
        ///
        /// @param cache `cache` value used by the operation.
        /// @param infos Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] RendererExpected<vector<VkPipeline>> create_compute_pipelines(
            VkPipelineCache cache,
            span<const VkComputePipelineCreateInfo> infos);

        /// Destroys the pipeline identified by the supplied parameters.
        ///
        /// @param pipeline Pipeline used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_pipeline(VkPipeline pipeline) noexcept;


        /// Creates a descriptor set layout from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkDescriptorSetLayout> create_descriptor_set_layout(
            const VkDescriptorSetLayoutCreateInfo &info) noexcept;

        /// Destroys the descriptor set layout identified by the supplied parameters.
        ///
        /// @param layout `layout` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_descriptor_set_layout(VkDescriptorSetLayout layout) noexcept;


        /// Performs the descriptor set layout support operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDescriptorSetLayoutSupport descriptor_set_layout_support(
            const VkDescriptorSetLayoutCreateInfo &info) const noexcept;


        /// Creates a descriptor pool from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkDescriptorPool> create_descriptor_pool(
            const VkDescriptorPoolCreateInfo &info) noexcept;

        /// Destroys the descriptor pool identified by the supplied parameters.
        ///
        /// @param pool `pool` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_descriptor_pool(VkDescriptorPool pool) noexcept;

        /// Resets descriptor pool to its baseline state.
        ///
        /// @param pool `pool` value used by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult reset_descriptor_pool(VkDescriptorPool pool,
                                                           VkDescriptorPoolResetFlags flags = 0) noexcept;


        /// Allocates descriptor sets.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
        [[nodiscard]] RendererExpected<vector<VkDescriptorSet>> allocate_descriptor_sets(
            const VkDescriptorSetAllocateInfo &info);

        /// Releases previously allocated storage or resources.
        ///
        /// @param pool `pool` value used by the operation.
        /// @param sets `sets` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult free_descriptor_sets(VkDescriptorPool pool,
                                                          span<const VkDescriptorSet> sets) noexcept;

        /// Updates descriptor sets from the supplied values.
        ///
        /// @param writes `writes` value used by the operation.
        /// @param copies `copies` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void update_descriptor_sets(span<const VkWriteDescriptorSet> writes,
                                    span<const VkCopyDescriptorSet> copies) noexcept;


        /// Creates a command pool from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkCommandPool> create_command_pool(
            const VkCommandPoolCreateInfo &info) noexcept;

        /// Destroys the command pool identified by the supplied parameters.
        ///
        /// @param pool `pool` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_command_pool(VkCommandPool pool) noexcept;

        /// Resets command pool to its baseline state.
        ///
        /// @param pool `pool` value used by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult reset_command_pool(VkCommandPool pool,
                                                        VkCommandPoolResetFlags flags = 0) noexcept;


        /// Performs the trim command pool operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param pool `pool` value used by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @note This function does not throw exceptions.
        void trim_command_pool(VkCommandPool pool, VkCommandPoolTrimFlags flags = 0) noexcept;


        /// Allocates command buffers.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
        [[nodiscard]] RendererExpected<vector<VkCommandBuffer>> allocate_command_buffers(
            const VkCommandBufferAllocateInfo &info);

        /// Releases previously allocated storage or resources.
        ///
        /// @param pool `pool` value used by the operation.
        /// @param buffers Buffer used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void free_command_buffers(VkCommandPool pool, span<const VkCommandBuffer> buffers) noexcept;

        /// Resets command buffer to its baseline state.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult reset_command_buffer(VkCommandBuffer buffer,
                                                          VkCommandBufferResetFlags flags = 0) noexcept;


        /// Creates a fence from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkFence> create_fence(const VkFenceCreateInfo &info) noexcept;

        /// Destroys the fence identified by the supplied parameters.
        ///
        /// @param fence Fence used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_fence(VkFence fence) noexcept;

        /// Resets fences to its baseline state.
        ///
        /// @param fences Fence used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult reset_fences(span<const VkFence> fences) noexcept;


        /// Reports whether fence signaled holds for this `VulkanDevice`.
        ///
        /// @param fence Fence used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<bool> is_fence_signaled(VkFence fence) const noexcept;


        /// Creates a semaphore from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkSemaphore> create_semaphore(const VkSemaphoreCreateInfo &info) noexcept;

        /// Destroys the semaphore identified by the supplied parameters.
        ///
        /// @param semaphore Semaphore used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_semaphore(VkSemaphore semaphore) noexcept;


        /// Performs the semaphore counter value operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param semaphore Semaphore used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<u64> semaphore_counter_value(VkSemaphore semaphore) const noexcept;

        /// Signals semaphore.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult signal_semaphore(const VkSemaphoreSignalInfo &info) noexcept;


        /// Waits for semaphores to complete.
        ///
        /// @param info Description of the resource or operation to perform.
        /// @param timeout_ns Maximum amount of time to wait before giving up.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult wait_semaphores(const VkSemaphoreWaitInfo &info, u64 timeout_ns) noexcept;


        /// Creates a event from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkEvent> create_event(const VkEventCreateInfo &info) noexcept;

        /// Destroys the event identified by the supplied parameters.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_event(VkEvent event) noexcept;


        /// Performs the event status operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<bool> event_status(VkEvent event) const noexcept;

        /// Sets the event for this `VulkanDevice`.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult set_event(VkEvent event) noexcept;

        /// Resets event to its baseline state.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult reset_event(VkEvent event) noexcept;


        /// Creates a query pool from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkQueryPool> create_query_pool(const VkQueryPoolCreateInfo &info) noexcept;

        /// Destroys the query pool identified by the supplied parameters.
        ///
        /// @param pool `pool` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_query_pool(VkQueryPool pool) noexcept;


        /// Returns the query pool results associated with this `VulkanDevice`.
        ///
        /// @param pool `pool` value used by the operation.
        /// @param first_query `first_query` value used by the operation.
        /// @param query_count Number of elements or operations to process.
        /// @param data Data consumed or referenced by the operation.
        /// @param stride `stride` value used by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult get_query_pool_results(VkQueryPool pool, u32 first_query, u32 query_count, span<u8> data, VkDeviceSize stride, VkQueryResultFlags flags) noexcept;

        /// Resets query pool to its baseline state.
        ///
        /// @param pool `pool` value used by the operation.
        /// @param first_query `first_query` value used by the operation.
        /// @param query_count Number of elements or operations to process.
        ///
        /// @note This function does not throw exceptions.
        void reset_query_pool(VkQueryPool pool, u32 first_query, u32 query_count) noexcept;


        /// Creates a swapchain from the supplied parameters.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkSwapchainKHR> create_swapchain(
            const VkSwapchainCreateInfoKHR &info) noexcept;

        /// Destroys the swapchain identified by the supplied parameters.
        ///
        /// @param swapchain Swapchain used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_swapchain(VkSwapchainKHR swapchain) noexcept;

        /// Performs the swapchain images operation for `VulkanDevice` using the supplied arguments.
        ///
        /// @param swapchain Swapchain used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] RendererExpected<vector<VkImage>> swapchain_images(VkSwapchainKHR swapchain) const;


        /// Acquires next image.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<u32> acquire_next_image(const VkAcquireNextImageInfoKHR &info) noexcept;


        /// Returns a human-readable name for the supplied set debug value.
        ///
        /// @param type Type value to inspect, select, or convert.
        /// @param object_handle Handle identifying the target object or resource.
        /// @param name Name used to identify or label the target.
        ///
        /// @note This function does not throw exceptions.
        void set_debug_name(VkObjectType type, u64 object_handle, const char *name) noexcept;

        /// Sets the debug tag for this `VulkanDevice`.
        ///
        /// @param type Type value to inspect, select, or convert.
        /// @param object_handle Handle identifying the target object or resource.
        /// @param tag_name Name used to identify or label the target.
        /// @param tag_data Data consumed or referenced by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_debug_tag(VkObjectType type, u64 object_handle, u64 tag_name, span<const u8> tag_data) noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;

        optional<VulkanQueue> graphics_queue_{};
        optional<VulkanQueue> present_queue_{};
        optional<VulkanQueue> compute_queue_{};
        optional<VulkanQueue> transfer_queue_{};
        optional<VulkanQueue> sparse_queue_{};
        optional<VulkanQueue> video_decode_queue_{};
        optional<VulkanQueue> video_encode_queue_{};
        vector<VulkanQueue> graphics_queue_lanes_;
        vector<VulkanQueue> compute_queue_lanes_;
        vector<VulkanQueue> transfer_queue_lanes_;
        vector<VulkanQueue> sparse_queue_lanes_;
        vector<VulkanQueue> video_decode_queue_lanes_;
        vector<VulkanQueue> video_encode_queue_lanes_;
    };

} // namespace SFT::Core::Vulkan
