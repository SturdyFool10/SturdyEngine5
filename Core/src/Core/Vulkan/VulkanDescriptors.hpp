#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <span>
#include <vector>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::span;
using std::vector;

namespace SFT::Core::Vulkan {


    class VulkanDescriptorSetLayout {
      public:
        /// Constructs a `VulkanDescriptorSetLayout` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanDescriptorSetLayout() = default;
        /// Destroys the `VulkanDescriptorSetLayout` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanDescriptorSetLayout();

        /// Disables this construction form for `VulkanDescriptorSetLayout`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanDescriptorSetLayout(const VulkanDescriptorSetLayout &) = delete;
        /// Assigns a new value to this `VulkanDescriptorSetLayout`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanDescriptorSetLayout &operator=(const VulkanDescriptorSetLayout &) = delete;

        /// Constructs a `VulkanDescriptorSetLayout` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanDescriptorSetLayout(VulkanDescriptorSetLayout &&o) noexcept;
        /// Assigns a new value to this `VulkanDescriptorSetLayout`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanDescriptorSetLayout &operator=(VulkanDescriptorSetLayout &&o) noexcept;

        /// Creates a `VulkanDescriptorSetLayout` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanDescriptorSetLayout> create(
            VkDevice device,
            const VkDescriptorSetLayoutCreateInfo &info) noexcept;


        /// Creates a from bindings from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param bindings `bindings` value used by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanDescriptorSetLayout> create_from_bindings(
            VkDevice device,
            span<const VkDescriptorSetLayoutBinding> bindings,
            VkDescriptorSetLayoutCreateFlags flags = 0) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanDescriptorSetLayout`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDescriptorSetLayout vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanDescriptorSetLayout`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;


        /// Performs the support operation for `VulkanDescriptorSetLayout` using the supplied arguments.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDescriptorSetLayoutSupport support(const VkDescriptorSetLayoutCreateInfo &info) const noexcept;

        /// Destroys or releases the `VulkanDescriptorSetLayout` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    };

    class DescriptorSetLayoutBuilder {
      public:
        /// Adds binding using the supplied arguments and current state.
        ///
        /// @param binding `binding` value used by the operation.
        /// @param type Type value to inspect, select, or convert.
        /// @param stages `stages` value used by the operation.
        /// @param count Number of elements or operations to process.
        /// @param immutable_samplers Sampler used or affected by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DescriptorSetLayoutBuilder &add_binding(u32 binding,
                                                VkDescriptorType type,
                                                VkShaderStageFlags stages,
                                                u32 count = 1,
                                                const VkSampler *immutable_samplers = nullptr);

        /// Sets the last binding flags for this `DescriptorSetLayoutBuilder`.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        DescriptorSetLayoutBuilder &set_last_binding_flags(VkDescriptorBindingFlags flags) noexcept;

        /// Sets the flags for this `DescriptorSetLayoutBuilder`.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        DescriptorSetLayoutBuilder &set_flags(VkDescriptorSetLayoutCreateFlags flags) noexcept;

        /// Creates a `DescriptorSetLayoutBuilder` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VulkanDescriptorSetLayout> create(VkDevice device) const noexcept;

        /// Binds the supplied resource or state for subsequent operations.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] span<const VkDescriptorSetLayoutBinding> bindings() const noexcept;
        /// Binds the supplied resource or state for subsequent operations.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] span<const VkDescriptorBindingFlags> binding_flags() const noexcept;

      private:
        vector<VkDescriptorSetLayoutBinding> bindings_;
        vector<VkDescriptorBindingFlags> binding_flags_;
        VkDescriptorSetLayoutCreateFlags flags_ = 0;
    };


    class VulkanDescriptorPool {
      public:
        /// Constructs a `VulkanDescriptorPool` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanDescriptorPool() = default;
        /// Destroys the `VulkanDescriptorPool` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanDescriptorPool();

        /// Disables this construction form for `VulkanDescriptorPool`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanDescriptorPool(const VulkanDescriptorPool &) = delete;
        /// Assigns a new value to this `VulkanDescriptorPool`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanDescriptorPool &operator=(const VulkanDescriptorPool &) = delete;

        /// Constructs a `VulkanDescriptorPool` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanDescriptorPool(VulkanDescriptorPool &&o) noexcept;
        /// Assigns a new value to this `VulkanDescriptorPool`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanDescriptorPool &operator=(VulkanDescriptorPool &&o) noexcept;

        /// Creates a `VulkanDescriptorPool` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanDescriptorPool> create(
            VkDevice device,
            const VkDescriptorPoolCreateInfo &info) noexcept;


        /// Creates a from sizes from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param sizes `sizes` value used by the operation.
        /// @param max_sets `max_sets` value used by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanDescriptorPool> create_from_sizes(
            VkDevice device,
            span<const VkDescriptorPoolSize> sizes,
            u32 max_sets,
            VkDescriptorPoolCreateFlags flags = 0) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanDescriptorPool`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDescriptorPool vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanDescriptorPool`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;

        /// Allocates storage or a resource.
        ///
        /// @param layouts `layouts` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
        [[nodiscard]] RendererExpected<vector<VkDescriptorSet>> allocate(
            span<const VkDescriptorSetLayout> layouts) const;

        /// Allocates one.
        ///
        /// @param layout `layout` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkDescriptorSet> allocate_one(
            VkDescriptorSetLayout layout) const noexcept;

        /// Allocates one.
        ///
        /// @param layout `layout` value used by the operation.
        /// @param variable_descriptor_count Number of elements or operations to process.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VkDescriptorSet> allocate_one(
            VkDescriptorSetLayout layout,
            u32 variable_descriptor_count) const noexcept;

        /// Releases previously allocated storage or resources.
        ///
        /// @param sets `sets` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult free(span<const VkDescriptorSet> sets) noexcept;


        /// Resets the object to its baseline state.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult reset(VkDescriptorPoolResetFlags flags = 0) noexcept;

        /// Destroys or releases the `VulkanDescriptorPool` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkDescriptorPool pool_ = VK_NULL_HANDLE;
    };

    class VulkanDescriptorSet {
      public:
        /// Constructs a `VulkanDescriptorSet` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanDescriptorSet() = default;
        /// Constructs a `VulkanDescriptorSet` from the supplied initialization values.
        ///
        /// @param set `set` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit VulkanDescriptorSet(VkDescriptorSet set) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanDescriptorSet`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDescriptorSet vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanDescriptorSet`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Converts the `VulkanDescriptorSet` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept;

      private:
        VkDescriptorSet set_ = VK_NULL_HANDLE;
    };

    class DescriptorSetWriter {
      public:
        /// Sets the descriptor set for this `DescriptorSetWriter`.
        ///
        /// @param set `set` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        DescriptorSetWriter &set_descriptor_set(VkDescriptorSet set) noexcept;

        /// Writes buffer to the associated destination.
        ///
        /// @param binding `binding` value used by the operation.
        /// @param type Type value to inspect, select, or convert.
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param range Range of values to process.
        /// @param array_element `array_element` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DescriptorSetWriter &write_buffer(u32 binding,
                                          VkDescriptorType type,
                                          VkBuffer buffer,
                                          VkDeviceSize offset,
                                          VkDeviceSize range,
                                          u32 array_element = 0);

        /// Writes uniform buffer to the associated destination.
        ///
        /// @param binding `binding` value used by the operation.
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param range Range of values to process.
        /// @param array_element `array_element` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DescriptorSetWriter &write_uniform_buffer(u32 binding,
                                                  VkBuffer buffer,
                                                  VkDeviceSize offset,
                                                  VkDeviceSize range,
                                                  u32 array_element = 0);

        /// Writes storage buffer to the associated destination.
        ///
        /// @param binding `binding` value used by the operation.
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param range Range of values to process.
        /// @param array_element `array_element` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DescriptorSetWriter &write_storage_buffer(u32 binding,
                                                  VkBuffer buffer,
                                                  VkDeviceSize offset,
                                                  VkDeviceSize range,
                                                  u32 array_element = 0);

        /// Writes image to the associated destination.
        ///
        /// @param binding `binding` value used by the operation.
        /// @param type Type value to inspect, select, or convert.
        /// @param view `view` value used by the operation.
        /// @param layout `layout` value used by the operation.
        /// @param sampler Sampler used or affected by the operation.
        /// @param array_element `array_element` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DescriptorSetWriter &write_image(u32 binding,
                                         VkDescriptorType type,
                                         VkImageView view,
                                         VkImageLayout layout,
                                         VkSampler sampler = VK_NULL_HANDLE,
                                         u32 array_element = 0);

        /// Writes sampled image to the associated destination.
        ///
        /// @param binding `binding` value used by the operation.
        /// @param view `view` value used by the operation.
        /// @param layout `layout` value used by the operation.
        /// @param array_element `array_element` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DescriptorSetWriter &write_sampled_image(u32 binding,
                                                 VkImageView view,
                                                 VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                 u32 array_element = 0);

        /// Writes storage image to the associated destination.
        ///
        /// @param binding `binding` value used by the operation.
        /// @param view `view` value used by the operation.
        /// @param layout `layout` value used by the operation.
        /// @param array_element `array_element` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DescriptorSetWriter &write_storage_image(u32 binding,
                                                 VkImageView view,
                                                 VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL,
                                                 u32 array_element = 0);

        /// Writes combined image sampler to the associated destination.
        ///
        /// @param binding `binding` value used by the operation.
        /// @param view `view` value used by the operation.
        /// @param sampler Sampler used or affected by the operation.
        /// @param layout `layout` value used by the operation.
        /// @param array_element `array_element` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DescriptorSetWriter &write_combined_image_sampler(u32 binding,
                                                          VkImageView view,
                                                          VkSampler sampler,
                                                          VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                          u32 array_element = 0);

        /// Writes sampler to the associated destination.
        ///
        /// @param binding `binding` value used by the operation.
        /// @param sampler Sampler used or affected by the operation.
        /// @param array_element `array_element` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DescriptorSetWriter &write_sampler(u32 binding, VkSampler sampler, u32 array_element = 0);


        /// Writes texel buffer to the associated destination.
        ///
        /// @param binding `binding` value used by the operation.
        /// @param type Type value to inspect, select, or convert.
        /// @param view `view` value used by the operation.
        /// @param array_element `array_element` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DescriptorSetWriter &write_texel_buffer(u32 binding,
                                                VkDescriptorType type,
                                                VkBufferView view,
                                                u32 array_element = 0);


        /// Writes acceleration structure to the associated destination.
        ///
        /// @param binding `binding` value used by the operation.
        /// @param acceleration_structure `acceleration_structure` value used by the operation.
        /// @param array_element `array_element` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DescriptorSetWriter &write_acceleration_structure(u32 binding,
                                                          VkAccelerationStructureKHR acceleration_structure,
                                                          u32 array_element = 0);

        /// Updates the `DescriptorSetWriter` state from the supplied values.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void update(VkDevice device) const;

        /// Clears the stored state or contents.
        ///
        /// @note This function does not throw exceptions.
        void clear() noexcept;

      private:
        struct BufferWrite {
            u32 binding = 0;
            u32 array_element = 0;
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            VkDescriptorBufferInfo info{};
        };
        struct ImageWrite {
            u32 binding = 0;
            u32 array_element = 0;
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            VkDescriptorImageInfo info{};
        };
        struct TexelWrite {
            u32 binding = 0;
            u32 array_element = 0;
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            VkBufferView view = VK_NULL_HANDLE;
        };
        struct AccelWrite {
            u32 binding = 0;
            u32 array_element = 0;
            VkAccelerationStructureKHR acceleration_structure = VK_NULL_HANDLE;
        };

        VkDescriptorSet set_ = VK_NULL_HANDLE;
        vector<BufferWrite> buffer_writes_;
        vector<ImageWrite> image_writes_;
        vector<TexelWrite> texel_writes_;
        vector<AccelWrite> accel_writes_;
    };

} // namespace SFT::Core::Vulkan
