#include <Core/Vulkan/VulkanDescriptors.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanDescriptorSetLayout::~VulkanDescriptorSetLayout() { destroy(); }

/// Performs the vulkan descriptor set layout operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(VulkanDescriptorSetLayout &&o) noexcept
            : device_(o.device_), layout_(o.layout_) {
            ZoneScopedN("VulkanDescriptorSetLayout::VulkanDescriptorSetLayout");
            o.device_ = VK_NULL_HANDLE;
            o.layout_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanDescriptorSetLayout &VulkanDescriptorSetLayout::operator=(VulkanDescriptorSetLayout &&o) noexcept {
            ZoneScopedN("VulkanDescriptorSetLayout::operator=");
            if (this != &o) {
                destroy();
                device_ = o.device_;
                layout_ = o.layout_;
                o.device_ = VK_NULL_HANDLE;
                o.layout_ = VK_NULL_HANDLE;
            }
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanDescriptorSetLayout> VulkanDescriptorSetLayout::create(
            VkDevice device,
            const VkDescriptorSetLayoutCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDescriptorSetLayout::create");
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateDescriptorSetLayout failed.");
            VulkanDescriptorSetLayout out;
            out.device_ = device;
            out.layout_ = layout;
            return out;
        }

/// Creates a from bindings from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param bindings `bindings` value used by the operation.
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanDescriptorSetLayout> VulkanDescriptorSetLayout::create_from_bindings(
            VkDevice device,
            span<const VkDescriptorSetLayoutBinding> bindings,
            VkDescriptorSetLayoutCreateFlags flags) noexcept {
            ZoneScopedN("VulkanDescriptorSetLayout::create_from_bindings");
            VkDescriptorSetLayoutCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = flags,
                .bindingCount = static_cast<u32>(bindings.size()),
                .pBindings = bindings.data(),
            };
            return create(device, info);
        }

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkDescriptorSetLayout VulkanDescriptorSetLayout::vk_handle() const noexcept { return layout_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanDescriptorSetLayout::is_valid() const noexcept { return layout_ != VK_NULL_HANDLE; }

/// Performs the support operation for `Vulkan` using the supplied arguments.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkDescriptorSetLayoutSupport VulkanDescriptorSetLayout::support(const VkDescriptorSetLayoutCreateInfo &info) const noexcept {
            ZoneScopedN("VulkanDescriptorSetLayout::support");
            VkDescriptorSetLayoutSupport s{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
                .pNext = nullptr};
            vkGetDescriptorSetLayoutSupport(device_, &info, &s);
            return s;
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanDescriptorSetLayout::destroy() noexcept {
            ZoneScopedN("VulkanDescriptorSetLayout::destroy");
            if (layout_ == VK_NULL_HANDLE)
                return;
            vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
            layout_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

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
DescriptorSetLayoutBuilder &DescriptorSetLayoutBuilder::add_binding(u32 binding,
                                                VkDescriptorType type,
                                                VkShaderStageFlags stages,
                                                u32 count,
                                                const VkSampler *immutable_samplers) {
            ZoneScopedN("DescriptorSetLayoutBuilder::add_binding");
            bindings_.push_back(VkDescriptorSetLayoutBinding{
                .binding = binding,
                .descriptorType = type,
                .descriptorCount = count,
                .stageFlags = stages,
                .pImmutableSamplers = immutable_samplers,
            });
            binding_flags_.push_back(0);
            return *this;
        }

/// Sets the last binding flags for this `Vulkan`.
///
/// @param flags Flags controlling optional behavior.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
DescriptorSetLayoutBuilder &DescriptorSetLayoutBuilder::set_last_binding_flags(VkDescriptorBindingFlags flags) noexcept {
            ZoneScopedN("DescriptorSetLayoutBuilder::set_last_binding_flags");
            if (!binding_flags_.empty()) {
                binding_flags_.back() = flags;
            }
            return *this;
        }

/// Sets the flags for this `Vulkan`.
///
/// @param flags Flags controlling optional behavior.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
DescriptorSetLayoutBuilder &DescriptorSetLayoutBuilder::set_flags(VkDescriptorSetLayoutCreateFlags flags) noexcept {
            ZoneScopedN("DescriptorSetLayoutBuilder::set_flags");
            flags_ = flags;
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanDescriptorSetLayout> DescriptorSetLayoutBuilder::create(VkDevice device) const noexcept {
            ZoneScopedN("DescriptorSetLayoutBuilder::create");
            VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                .pNext = nullptr,
                .bindingCount = static_cast<u32>(binding_flags_.size()),
                .pBindingFlags = binding_flags_.empty() ? nullptr : binding_flags_.data(),
            };
            bool has_binding_flags = false;
            for (VkDescriptorBindingFlags flags : binding_flags_) {
                has_binding_flags = has_binding_flags || flags != 0;
            }
            VkDescriptorSetLayoutCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .pNext = has_binding_flags ? &binding_flags_info : nullptr,
                .flags = flags_,
                .bindingCount = static_cast<u32>(bindings_.size()),
                .pBindings = bindings_.empty() ? nullptr : bindings_.data(),
            };
            return VulkanDescriptorSetLayout::create(device, info);
        }

/// Binds the supplied resource or state for subsequent operations.
///
/// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
/// @note This function does not throw exceptions.
[[nodiscard]] span<const VkDescriptorSetLayoutBinding> DescriptorSetLayoutBuilder::bindings() const noexcept { return bindings_; }

/// Binds the supplied resource or state for subsequent operations.
///
/// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
/// @note This function does not throw exceptions.
[[nodiscard]] span<const VkDescriptorBindingFlags> DescriptorSetLayoutBuilder::binding_flags() const noexcept { return binding_flags_; }

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanDescriptorPool::~VulkanDescriptorPool() { destroy(); }

/// Performs the vulkan descriptor pool operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanDescriptorPool::VulkanDescriptorPool(VulkanDescriptorPool &&o) noexcept
            : device_(o.device_), pool_(o.pool_) {
            ZoneScopedN("VulkanDescriptorPool::VulkanDescriptorPool");
            o.device_ = VK_NULL_HANDLE;
            o.pool_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanDescriptorPool &VulkanDescriptorPool::operator=(VulkanDescriptorPool &&o) noexcept {
            ZoneScopedN("VulkanDescriptorPool::operator=");
            if (this != &o) {
                destroy();
                device_ = o.device_;
                pool_ = o.pool_;
                o.device_ = VK_NULL_HANDLE;
                o.pool_ = VK_NULL_HANDLE;
            }
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanDescriptorPool> VulkanDescriptorPool::create(
            VkDevice device,
            const VkDescriptorPoolCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDescriptorPool::create");
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(device, &info, nullptr, &pool) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateDescriptorPool failed.");
            VulkanDescriptorPool out;
            out.device_ = device;
            out.pool_ = pool;
            return out;
        }

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
[[nodiscard]] RendererExpected<VulkanDescriptorPool> VulkanDescriptorPool::create_from_sizes(
            VkDevice device,
            span<const VkDescriptorPoolSize> sizes,
            u32 max_sets,
            VkDescriptorPoolCreateFlags flags) noexcept {
            ZoneScopedN("VulkanDescriptorPool::create_from_sizes");
            VkDescriptorPoolCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = flags,
                .maxSets = max_sets,
                .poolSizeCount = static_cast<u32>(sizes.size()),
                .pPoolSizes = sizes.data(),
            };
            return create(device, info);
        }

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkDescriptorPool VulkanDescriptorPool::vk_handle() const noexcept { return pool_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanDescriptorPool::is_valid() const noexcept { return pool_ != VK_NULL_HANDLE; }

/// Allocates storage or a resource.
///
/// @param layouts `layouts` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
[[nodiscard]] RendererExpected<vector<VkDescriptorSet>> VulkanDescriptorPool::allocate(
            span<const VkDescriptorSetLayout> layouts) const {
            ZoneScopedN("VulkanDescriptorPool::allocate");
            VkDescriptorSetAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .pNext = nullptr,
                .descriptorPool = pool_,
                .descriptorSetCount = static_cast<u32>(layouts.size()),
                .pSetLayouts = layouts.data(),
            };
            vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
            if (vkAllocateDescriptorSets(device_, &info, sets.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory, "vkAllocateDescriptorSets failed.");
            return sets;
        }

/// Allocates one.
///
/// @param layout `layout` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkDescriptorSet> VulkanDescriptorPool::allocate_one(
            VkDescriptorSetLayout layout) const noexcept {
            ZoneScopedN("VulkanDescriptorPool::allocate_one");
            VkDescriptorSetAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .pNext = nullptr,
                .descriptorPool = pool_,
                .descriptorSetCount = 1,
                .pSetLayouts = &layout,
            };
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(device_, &info, &set) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory, "vkAllocateDescriptorSets failed.");
            return set;
        }

/// Allocates one.
///
/// @param layout `layout` value used by the operation.
/// @param variable_descriptor_count Number of elements or operations to process.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkDescriptorSet> VulkanDescriptorPool::allocate_one(
            VkDescriptorSetLayout layout,
            u32 variable_descriptor_count) const noexcept {
            ZoneScopedN("VulkanDescriptorPool::allocate_one");
            VkDescriptorSetVariableDescriptorCountAllocateInfo variable_count_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
                .pNext = nullptr,
                .descriptorSetCount = 1,
                .pDescriptorCounts = &variable_descriptor_count,
            };
            VkDescriptorSetAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .pNext = &variable_count_info,
                .descriptorPool = pool_,
                .descriptorSetCount = 1,
                .pSetLayouts = &layout,
            };
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(device_, &info, &set) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory, "vkAllocateDescriptorSets failed.");
            return set;
        }

/// Releases previously allocated storage or resources.
///
/// @param sets `sets` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDescriptorPool::free(span<const VkDescriptorSet> sets) noexcept {
            ZoneScopedN("VulkanDescriptorPool::free");
            if (vkFreeDescriptorSets(device_, pool_, static_cast<u32>(sets.size()), sets.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkFreeDescriptorSets failed.");
            return {};
        }

/// Resets the object to its baseline state.
///
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDescriptorPool::reset(VkDescriptorPoolResetFlags flags) noexcept {
            ZoneScopedN("VulkanDescriptorPool::reset");
            if (vkResetDescriptorPool(device_, pool_, flags) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetDescriptorPool failed.");
            return {};
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanDescriptorPool::destroy() noexcept {
            ZoneScopedN("VulkanDescriptorPool::destroy");
            if (pool_ == VK_NULL_HANDLE)
                return;
            vkDestroyDescriptorPool(device_, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

/// Performs the vulkan descriptor set operation for `Vulkan` using the supplied arguments.
///
/// @param set `set` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanDescriptorSet::VulkanDescriptorSet(VkDescriptorSet set) noexcept : set_(set) {}

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkDescriptorSet VulkanDescriptorSet::vk_handle() const noexcept { return set_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanDescriptorSet::is_valid() const noexcept { return set_ != VK_NULL_HANDLE; }

/// Converts the `Vulkan` to `bool`.
///
/// @return Returns the boolean result of the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VulkanDescriptorSet::operator bool() const noexcept { return is_valid(); }

/// Sets the descriptor set for this `Vulkan`.
///
/// @param set `set` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
DescriptorSetWriter &DescriptorSetWriter::set_descriptor_set(VkDescriptorSet set) noexcept {
            ZoneScopedN("DescriptorSetWriter::set_descriptor_set");
            set_ = set;
            return *this;
        }

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
DescriptorSetWriter &DescriptorSetWriter::write_buffer(u32 binding,
                                          VkDescriptorType type,
                                          VkBuffer buffer,
                                          VkDeviceSize offset,
                                          VkDeviceSize range,
                                          u32 array_element) {
            ZoneScopedN("DescriptorSetWriter::write_buffer");
            buffer_writes_.push_back(BufferWrite{
                .binding = binding,
                .array_element = array_element,
                .type = type,
                .info = VkDescriptorBufferInfo{
                    .buffer = buffer,
                    .offset = offset,
                    .range = range,
                },
            });
            return *this;
        }

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
DescriptorSetWriter &DescriptorSetWriter::write_uniform_buffer(u32 binding,
                                                  VkBuffer buffer,
                                                  VkDeviceSize offset,
                                                  VkDeviceSize range,
                                                  u32 array_element) {
            ZoneScopedN("DescriptorSetWriter::write_uniform_buffer");
            return write_buffer(binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer, offset, range, array_element);
        }

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
DescriptorSetWriter &DescriptorSetWriter::write_storage_buffer(u32 binding,
                                                  VkBuffer buffer,
                                                  VkDeviceSize offset,
                                                  VkDeviceSize range,
                                                  u32 array_element) {
            ZoneScopedN("DescriptorSetWriter::write_storage_buffer");
            return write_buffer(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer, offset, range, array_element);
        }

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
DescriptorSetWriter &DescriptorSetWriter::write_image(u32 binding,
                                         VkDescriptorType type,
                                         VkImageView view,
                                         VkImageLayout layout,
                                         VkSampler sampler,
                                         u32 array_element) {
            ZoneScopedN("DescriptorSetWriter::write_image");
            image_writes_.push_back(ImageWrite{
                .binding = binding,
                .array_element = array_element,
                .type = type,
                .info = VkDescriptorImageInfo{
                    .sampler = sampler,
                    .imageView = view,
                    .imageLayout = layout,
                },
            });
            return *this;
        }

/// Writes sampled image to the associated destination.
///
/// @param binding `binding` value used by the operation.
/// @param view `view` value used by the operation.
/// @param layout `layout` value used by the operation.
/// @param array_element `array_element` value used by the operation.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
DescriptorSetWriter &DescriptorSetWriter::write_sampled_image(u32 binding,
                                                 VkImageView view,
                                                 VkImageLayout layout,
                                                 u32 array_element) {
            ZoneScopedN("DescriptorSetWriter::write_sampled_image");
            return write_image(binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view, layout, VK_NULL_HANDLE, array_element);
        }

/// Writes storage image to the associated destination.
///
/// @param binding `binding` value used by the operation.
/// @param view `view` value used by the operation.
/// @param layout `layout` value used by the operation.
/// @param array_element `array_element` value used by the operation.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
DescriptorSetWriter &DescriptorSetWriter::write_storage_image(u32 binding,
                                                 VkImageView view,
                                                 VkImageLayout layout,
                                                 u32 array_element) {
            ZoneScopedN("DescriptorSetWriter::write_storage_image");
            return write_image(binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, view, layout, VK_NULL_HANDLE, array_element);
        }

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
DescriptorSetWriter &DescriptorSetWriter::write_combined_image_sampler(u32 binding,
                                                          VkImageView view,
                                                          VkSampler sampler,
                                                          VkImageLayout layout,
                                                          u32 array_element) {
            ZoneScopedN("DescriptorSetWriter::write_combined_image_sampler");
            return write_image(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, view, layout, sampler, array_element);
        }

/// Writes sampler to the associated destination.
///
/// @param binding `binding` value used by the operation.
/// @param sampler Sampler used or affected by the operation.
/// @param array_element `array_element` value used by the operation.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
DescriptorSetWriter &DescriptorSetWriter::write_sampler(u32 binding, VkSampler sampler, u32 array_element) {
            ZoneScopedN("DescriptorSetWriter::write_sampler");
            return write_image(binding, VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, sampler, array_element);
        }

/// Writes texel buffer to the associated destination.
///
/// @param binding `binding` value used by the operation.
/// @param type Type value to inspect, select, or convert.
/// @param view `view` value used by the operation.
/// @param array_element `array_element` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
DescriptorSetWriter &DescriptorSetWriter::write_texel_buffer(u32 binding,
                                                VkDescriptorType type,
                                                VkBufferView view,
                                                u32 array_element) {
            ZoneScopedN("DescriptorSetWriter::write_texel_buffer");
            texel_writes_.push_back(TexelWrite{
                .binding = binding,
                .array_element = array_element,
                .type = type,
                .view = view,
            });
            return *this;
        }

/// Writes acceleration structure to the associated destination.
///
/// @param binding `binding` value used by the operation.
/// @param acceleration_structure `acceleration_structure` value used by the operation.
/// @param array_element `array_element` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
DescriptorSetWriter &DescriptorSetWriter::write_acceleration_structure(u32 binding,
                                                          VkAccelerationStructureKHR acceleration_structure,
                                                          u32 array_element) {
            ZoneScopedN("DescriptorSetWriter::write_acceleration_structure");
            accel_writes_.push_back(AccelWrite{
                .binding = binding,
                .array_element = array_element,
                .acceleration_structure = acceleration_structure,
            });
            return *this;
        }

/// Updates the `Vulkan` state from the supplied values.
///
/// @param device Device used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void DescriptorSetWriter::update(VkDevice device) const {
            ZoneScopedN("DescriptorSetWriter::update");
            vector<VkWriteDescriptorSet> writes;
            writes.reserve(buffer_writes_.size() + image_writes_.size() + texel_writes_.size() +
                           accel_writes_.size());


            vector<VkWriteDescriptorSetAccelerationStructureKHR> accel_infos;
            accel_infos.reserve(accel_writes_.size());

            for (const BufferWrite &write : buffer_writes_) {
                writes.push_back(VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = nullptr,
                    .dstSet = set_,
                    .dstBinding = write.binding,
                    .dstArrayElement = write.array_element,
                    .descriptorCount = 1,
                    .descriptorType = write.type,
                    .pImageInfo = nullptr,
                    .pBufferInfo = &write.info,
                    .pTexelBufferView = nullptr,
                });
            }
            for (const ImageWrite &write : image_writes_) {
                writes.push_back(VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = nullptr,
                    .dstSet = set_,
                    .dstBinding = write.binding,
                    .dstArrayElement = write.array_element,
                    .descriptorCount = 1,
                    .descriptorType = write.type,
                    .pImageInfo = &write.info,
                    .pBufferInfo = nullptr,
                    .pTexelBufferView = nullptr,
                });
            }

            for (const TexelWrite &write : texel_writes_) {
                writes.push_back(VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = nullptr,
                    .dstSet = set_,
                    .dstBinding = write.binding,
                    .dstArrayElement = write.array_element,
                    .descriptorCount = 1,
                    .descriptorType = write.type,
                    .pImageInfo = nullptr,
                    .pBufferInfo = nullptr,
                    .pTexelBufferView = &write.view,
                });
            }
            for (const AccelWrite &write : accel_writes_) {
                accel_infos.push_back(VkWriteDescriptorSetAccelerationStructureKHR{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                    .pNext = nullptr,
                    .accelerationStructureCount = 1,
                    .pAccelerationStructures = &write.acceleration_structure,
                });
                writes.push_back(VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = &accel_infos.back(),
                    .dstSet = set_,
                    .dstBinding = write.binding,
                    .dstArrayElement = write.array_element,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                    .pImageInfo = nullptr,
                    .pBufferInfo = nullptr,
                    .pTexelBufferView = nullptr,
                });
            }

            vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()), writes.empty() ? nullptr : writes.data(), 0, nullptr);
        }

/// Clears the stored state or contents.
///
/// @return Returns the current clear value.
/// @note This function does not throw exceptions.
void DescriptorSetWriter::clear() noexcept {
            ZoneScopedN("DescriptorSetWriter::clear");
            buffer_writes_.clear();
            image_writes_.clear();
            texel_writes_.clear();
            accel_writes_.clear();
        }

} // namespace SFT::Core::Vulkan
