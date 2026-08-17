





#pragma region Imports
#if defined(_WIN32)








#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "VulkanRhiBridgeComposition.hpp"
#pragma endregion

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;

namespace SFT::Core::Vulkan {

#if defined(_WIN32)

    namespace {











        [[nodiscard]] std::optional<GraphicsPlatform::CompositionFormat> vk_format_to_composition_format(
            VkFormat format) noexcept {
            switch (format) {
                case VK_FORMAT_B8G8R8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_SRGB: return GraphicsPlatform::CompositionFormat::Bgra8Unorm;
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB: return GraphicsPlatform::CompositionFormat::Rgba8Unorm;
                case VK_FORMAT_R16G16B16A16_SFLOAT: return GraphicsPlatform::CompositionFormat::Rgba16Float;





                case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return GraphicsPlatform::CompositionFormat::Rgb10a2Unorm;
                default: return std::nullopt;
            }
        }

        [[nodiscard]] std::optional<u32> find_device_local_memory_type_index(
            VkPhysicalDevice physical_device, u32 allowed_type_bits) noexcept {
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
            for (u32 index = 0; index < properties.memoryTypeCount; ++index) {
                const bool allowed = (allowed_type_bits & (1u << index)) != 0;
                const bool device_local =
                    (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
                if (allowed && device_local) {
                    return index;
                }
            }
            return std::nullopt;
        }











        [[nodiscard]] HANDLE duplicate_handle_for_import(HANDLE source) noexcept {
            HANDLE duplicated = nullptr;
            const HANDLE process = GetCurrentProcess();
            if (!DuplicateHandle(process, source, process, &duplicated, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                return nullptr;
            }
            return duplicated;
        }





        [[nodiscard]] RendererResult import_composition_fences(VkDevice device, CompositionSwapchainResources &resources) {
            if (vkImportSemaphoreWin32HandleKHR == nullptr) {
                return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                              "Composition present requires VK_KHR_external_semaphore_win32, "
                                              "which this device did not enable.");
            }
            const GraphicsPlatform::CompositionSharedFences shared_fences = resources.presenter->shared_fences();
            const auto import_timeline_semaphore = [&](void *nt_handle) -> RendererExpected<VulkanSemaphore> {
                auto semaphore = VulkanSemaphore::create_timeline(device, 0);
                if (!semaphore) {
                    return std::unexpected(semaphore.error());
                }
                HANDLE duplicated = duplicate_handle_for_import(static_cast<HANDLE>(nt_handle));
                if (duplicated == nullptr) {
                    return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                                  "Composition present: DuplicateHandle failed for a shared fence.");
                }
                const VkImportSemaphoreWin32HandleInfoKHR import_info{
                    .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
                    .pNext = nullptr,
                    .semaphore = semaphore->vk_handle(),


                    .flags = 0,
                    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT,
                    .handle = duplicated,
                    .name = nullptr,
                };
                const VkResult result = vkImportSemaphoreWin32HandleKHR(device, &import_info);



                CloseHandle(duplicated);
                if (result != VK_SUCCESS) {
                    return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                                  "vkImportSemaphoreWin32HandleKHR failed.");
                }
                return std::move(*semaphore);
            };

            auto render_complete = import_timeline_semaphore(shared_fences.render_complete_nt_handle);
            if (!render_complete) {
                return std::unexpected(render_complete.error());
            }
            resources.render_complete_semaphore = std::move(*render_complete);

            auto present_complete = import_timeline_semaphore(shared_fences.present_complete_nt_handle);
            if (!present_complete) {
                return std::unexpected(present_complete.error());
            }
            resources.present_complete_semaphore = std::move(*present_complete);
            return {};
        }








        [[nodiscard]] RendererResult import_composition_images(
            VkDevice device, VkPhysicalDevice physical_device, VkFormat vk_format, VkImageUsageFlags usage,
            u32 width, u32 height, CompositionSwapchainResources &resources) {
            const std::span<const GraphicsPlatform::CompositionSharedImage> shared_images =
                resources.presenter->shared_images();
            resources.images.reserve(resources.images.size() + shared_images.size());
            resources.views.reserve(resources.views.size() + shared_images.size());

            for (const GraphicsPlatform::CompositionSharedImage &shared_image : shared_images) {
            const VkExternalMemoryImageCreateInfo external_info{
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                .pNext = nullptr,




                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
            };
            const VkImageCreateInfo image_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = &external_info,
                .flags = 0,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = vk_format,
                .extent = {width, height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,



                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            auto image = VulkanImage::create(device, image_info);
            if (!image) {
                destroy_composition_swapchain_resources(device, resources);
                return std::unexpected(image.error());
            }

            const VkMemoryRequirements requirements = image->memory_requirements();
            const std::optional<u32> memory_type_index =
                find_device_local_memory_type_index(physical_device, requirements.memoryTypeBits);
            if (!memory_type_index.has_value()) {
                destroy_composition_swapchain_resources(device, resources);
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                              "Composition present: no device-local memory type accepts the "
                                              "imported image's requirements.");
            }

            HANDLE duplicated = duplicate_handle_for_import(static_cast<HANDLE>(shared_image.nt_handle));
            if (duplicated == nullptr) {
                destroy_composition_swapchain_resources(device, resources);
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                              "Composition present: DuplicateHandle failed for a shared image.");
            }
            const VkImportMemoryWin32HandleInfoKHR import_memory_info{
                .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                .pNext = nullptr,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
                .handle = duplicated,
                .name = nullptr,
            };




            const VkMemoryDedicatedAllocateInfo dedicated_info{
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .pNext = &import_memory_info,
                .image = image->vk_handle(),
                .buffer = VK_NULL_HANDLE,
            };





            const VkMemoryAllocateInfo allocate_info{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &dedicated_info,
                .allocationSize = requirements.size,
                .memoryTypeIndex = *memory_type_index,
            };
            VkDeviceMemory memory = VK_NULL_HANDLE;
            const VkResult allocate_result = vkAllocateMemory(device, &allocate_info, nullptr, &memory);



            CloseHandle(duplicated);
            if (allocate_result != VK_SUCCESS) {
                destroy_composition_swapchain_resources(device, resources);
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                              "vkAllocateMemory (imported D3D11 texture) failed.");
            }

            if (auto bind = image->bind_memory(memory); !bind) {
                image->destroy();
                vkFreeMemory(device, memory, nullptr);
                destroy_composition_swapchain_resources(device, resources);
                return std::unexpected(bind.error());
            }

            auto view = image->create_view(VK_IMAGE_ASPECT_COLOR_BIT);
            if (!view) {



                image->destroy();
                vkFreeMemory(device, memory, nullptr);
                destroy_composition_swapchain_resources(device, resources);
                return std::unexpected(view.error());
            }

            resources.images.push_back(CompositionSwapchainImage{.image = std::move(*image), .memory = memory});
            resources.views.push_back(std::move(*view));
        }

        return {};
        }

    } // namespace

    RendererExpected<CompositionSwapchainResources> create_composition_swapchain_resources(
        VkDevice device,
        VkPhysicalDevice physical_device,
        const GraphicsPlatform::NativeSurfaceHandle &surface,
        VkFormat vk_format,
        VkImageUsageFlags usage,
        u32 width,
        u32 height,
        u32 image_count,
        GraphicsPlatform::CompositionAlphaMode alpha_mode) {
        const std::optional<GraphicsPlatform::CompositionFormat> composition_format =
            vk_format_to_composition_format(vk_format);
        if (!composition_format.has_value()) {
            return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                          "Composition present: this swapchain's Vulkan format has no "
                                          "composition-swapchain equivalent.");
        }

        VkPhysicalDeviceIDProperties id_properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                .pNext = &id_properties};
        vkGetPhysicalDeviceProperties2(physical_device, &properties2);



        u64 adapter_luid_bits = 0;
        std::memcpy(&adapter_luid_bits, id_properties.deviceLUID, sizeof(id_properties.deviceLUID));

        const GraphicsPlatform::CompositionPresenterDesc presenter_desc{
            .surface = surface,
            .width = width,
            .height = height,
            .buffer_count = image_count,
            .shared_image_count = image_count,
            .format = *composition_format,
            .alpha_mode = alpha_mode,
            .adapter_luid = adapter_luid_bits,
            .use_adapter_luid = id_properties.deviceLUIDValid == VK_TRUE,
        };
        GraphicsPlatform::QueryResult<std::unique_ptr<GraphicsPlatform::CompositionPresenter>> presenter_result =
            GraphicsPlatform::create_composition_presenter(presenter_desc);
        if (!presenter_result) {
            return graphics_backend_error(GraphicsBackendErrorCode::Unsupported, presenter_result.message.message);
        }

        CompositionSwapchainResources resources{};
        resources.presenter = std::move(presenter_result.value);



        if (auto imported = import_composition_fences(device, resources); !imported) {
            destroy_composition_swapchain_resources(device, resources);
            return std::unexpected(imported.error());
        }
        if (auto imported = import_composition_images(device, physical_device, vk_format, usage, width, height, resources);
            !imported) {
            destroy_composition_swapchain_resources(device, resources);
            return std::unexpected(imported.error());
        }
        return resources;
    }

    RendererExpected<CompositionSwapchainResources> resize_composition_swapchain_resources(
        VkDevice device,
        VkPhysicalDevice physical_device,
        CompositionSwapchainResources &&previous,
        VkFormat vk_format,
        VkImageUsageFlags usage,
        u32 width,
        u32 height) {
        if (!previous.presenter) {
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                          "resize_composition_swapchain_resources: `previous` has no presenter "
                                          "to reuse.");
        }













        (void)previous.presenter->set_live_scale(width, height);










        if (previous.render_complete_value != 0) {
            if (RendererResult waited = previous.render_complete_semaphore.wait(previous.render_complete_value);
                !waited) {
                return graphics_backend_error(
                    waited.error().code == GraphicsBackendErrorCode::DeviceLost
                        ? GraphicsBackendErrorCode::DeviceLost
                        : GraphicsBackendErrorCode::OperationFailed,
                    "Composition presenter resize could not wait for the Vulkan render-complete fence.");
            }
        }

        CompositionSwapchainResources resources{};





        resources.presenter = std::move(previous.presenter);



        resources.render_complete_semaphore = std::move(previous.render_complete_semaphore);
        resources.present_complete_semaphore = std::move(previous.present_complete_semaphore);
        resources.render_complete_value = previous.render_complete_value;

        if (const GraphicsPlatform::QueryMessage resized = resources.presenter->resize(width, height); !resized) {
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                          std::string("Composition presenter resize failed: ") + resized.message);
        }
        if (auto imported = import_composition_images(device, physical_device, vk_format, usage, width, height, resources);
            !imported) {
            destroy_composition_swapchain_resources(device, resources);
            return std::unexpected(imported.error());
        }
        return resources;
    }

    void destroy_composition_swapchain_resources(VkDevice device, CompositionSwapchainResources &resources) noexcept {






        resources.views.clear();
        for (CompositionSwapchainImage &image : resources.images) {
            image.image.destroy();
            if (image.memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, image.memory, nullptr);
                image.memory = VK_NULL_HANDLE;
            }
        }
        resources.images.clear();
        resources.render_complete_semaphore.destroy();
        resources.present_complete_semaphore.destroy();
        resources.presenter.reset();
    }

#else

    RendererExpected<CompositionSwapchainResources> create_composition_swapchain_resources(
        VkDevice           ,
        VkPhysicalDevice                    ,
        const GraphicsPlatform::NativeSurfaceHandle &            ,
        VkFormat              ,
        VkImageUsageFlags          ,
        u32          ,
        u32           ,
        u32                ,
        GraphicsPlatform::CompositionAlphaMode               ) {
        return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                      "Composition present is implemented only on Windows.");
    }

    void destroy_composition_swapchain_resources(VkDevice           ,
                                                 CompositionSwapchainResources &              ) noexcept {
    }

#endif // defined(_WIN32)

} // namespace SFT::Core::Vulkan
