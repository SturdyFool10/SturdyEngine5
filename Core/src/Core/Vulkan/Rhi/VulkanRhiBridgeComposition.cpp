// Vulkan-side composition-present interop — see VulkanRhiBridgeComposition.hpp for the seam this
// implements and why, and graphicsPlatform's CompositionPresent.hpp for the presenter this imports
// from. Same two-file, internally-guarded split as graphicsPlatform's own
// CompositionPresent.cpp / CompositionPresentUnsupported.cpp, so call sites never need their own
// `#if defined(_WIN32)`.

#pragma region Imports
#if defined(_WIN32)
// VulkanRhiBridgeComposition.hpp pulls in "volk.h" itself (needed on every platform for the plain
// portable declarations it exposes) — that first inclusion is what decides, for this whole
// translation unit, whether vulkan_win32.h's Win32-specific structs/functions get declared, so the
// platform setup has to land before that header's own #include, not after. vulkan_win32.h uses
// HANDLE/DWORD/etc. directly without including windows.h itself, hence windows.h has to come first
// here too. NOMINMAX keeps windows.h's min/max macros from shadowing std::min/std::max used below.
// This define is local to this TU (volk.h's include guard means no other file including volk.h is
// affected), so it can't leak platform-specific declarations into the portable header's other users.
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

        // Maps to the DXGI_FORMAT the composition presenter's shared texture is actually allocated
        // as. sRGB variants intentionally collapse onto their UNORM counterpart rather than failing:
        // DXGI flip-model (and composition) swap chains reject an _SRGB BackBufferFormat outright
        // (DXGI_ERROR_INVALID_CALL) — the standard D3D pattern is a UNORM back buffer with an _SRGB
        // render-target *view* over it for the gamma-correct write, and the bit layout between a
        // format and its _SRGB sibling is identical (same texel size and byte layout; _SRGB only
        // changes how reads/writes are converted, not what's stored) — so importing the UNORM D3D
        // allocation and creating the Vulkan-side VkImage with the caller's true requested format
        // (done below, via `vk_format`, not this function's return value) reproduces the same
        // gamma-correct write behavior the native swapchain path would have given it.
        [[nodiscard]] std::optional<GraphicsPlatform::CompositionFormat> vk_format_to_composition_format(
            VkFormat format) noexcept {
            switch (format) {
                case VK_FORMAT_B8G8R8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_SRGB: return GraphicsPlatform::CompositionFormat::Bgra8Unorm;
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB: return GraphicsPlatform::CompositionFormat::Rgba8Unorm;
                case VK_FORMAT_R16G16B16A16_SFLOAT: return GraphicsPlatform::CompositionFormat::Rgba16Float;
                // DXGI's R10G10B10A2_UNORM is 2-bit-alpha-first, matching Vulkan's packed
                // A2B10G10R10 component order (not A2R10G10B10 — the swizzle, not just the bit
                // widths, has to match for a shared-memory import to read as the same pixels on both
                // sides). No _SRGB sibling exists for 10-bit formats in DXGI, so there's nothing to
                // collapse here.
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

        // Handles crossing the Vulkan/D3D boundary are documented (VK_KHR_external_memory_win32 /
        // VK_KHR_external_semaphore_win32) as staying owned by the application — Vulkan duplicates
        // what it needs internally at import time rather than taking ownership of the handle passed
        // in. The presenter (graphicsPlatform) already owns render_complete_nt_handle,
        // present_complete_nt_handle, and every CompositionSharedImage::nt_handle for its own
        // lifetime and will CloseHandle them on teardown — so importing the presenter's exact handle
        // value here and later closing it ourselves would race the presenter's own close against
        // ours. Duplicating first sidesteps the ambiguity entirely: Vulkan gets a handle nobody but
        // this function ever owns, safe to close the instant the import call returns, and the
        // presenter's original is never touched.
        [[nodiscard]] HANDLE duplicate_handle_for_import(HANDLE source) noexcept {
            HANDLE duplicated = nullptr;
            const HANDLE process = GetCurrentProcess();
            if (!DuplicateHandle(process, source, process, &duplicated, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                return nullptr;
            }
            return duplicated;
        }

        // Imports the two shared fences from an already-created presenter. Split out so
        // resize_composition_swapchain_resources() can skip this entirely and just move the previous
        // call's semaphores over instead — resize() is documented to leave the underlying D3D fences
        // untouched, so re-importing them would only reproduce the same two semaphores at extra cost.
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
                    // No TEMPORARY bit: this semaphore's entire purpose for the swapchain's whole
                    // lifetime is to mirror this one D3D fence, not to borrow its payload once.
                    .flags = 0,
                    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT,
                    .handle = duplicated,
                    .name = nullptr,
                };
                const VkResult result = vkImportSemaphoreWin32HandleKHR(device, &import_info);
                // Per VK_KHR_external_semaphore_win32: ownership of an NT handle passed for import is
                // not transferred to Vulkan, which duplicates what it needs during this call — safe
                // (and, per that same contract, this side's responsibility) to close immediately after.
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

        // Imports every shared image the presenter currently exposes, appending to `resources.images`/
        // `.views` — safe to call with either empty (fresh create) or already-populated (never actually
        // the case in practice, since every caller either starts fresh or has just cleared them via
        // CompositionPresenter::resize) vectors. Shared between create_composition_swapchain_resources()
        // and resize_composition_swapchain_resources() since the per-image import dance itself (create,
        // find a memory type, duplicate-import the handle, allocate, bind, view) doesn't depend on
        // whether the presenter backing it is brand new or being reused after a resize.
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
                // The NT-handle D3D11 texture type — matches how the presenter created its shared
                // handles (IDXGIResource1::CreateSharedHandle, not the legacy GDI-style KMT global
                // share handle, which would need VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT
                // instead and a different, ownerless import contract).
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
                // D3D11_USAGE_DEFAULT with no CPU access (how the presenter created these) surfaces
                // to Vulkan as optimal (driver-private) tiling, the same as every other render target
                // this bridge creates.
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
            // Dedicated allocation is required (not just recommended) by the spec whenever the
            // imported payload is itself a dedicated D3D11 resource, which every shared texture the
            // presenter creates is — vkAllocateMemory returns VK_ERROR_OUT_OF_DEVICE_MEMORY or worse
            // without this.
            const VkMemoryDedicatedAllocateInfo dedicated_info{
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .pNext = &import_memory_info,
                .image = image->vk_handle(),
                .buffer = VK_NULL_HANDLE,
            };
            // allocationSize comes from this bridge's own vkGetImageMemoryRequirements on the image
            // it just created (requirements.size above) — never from
            // CompositionSharedImage::allocation_size_bytes, which that field's own doc comment
            // (CompositionPresent.hpp) documents as a best-effort estimate, not the authoritative
            // size an import is required to state.
            const VkMemoryAllocateInfo allocate_info{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &dedicated_info,
                .allocationSize = requirements.size,
                .memoryTypeIndex = *memory_type_index,
            };
            VkDeviceMemory memory = VK_NULL_HANDLE;
            const VkResult allocate_result = vkAllocateMemory(device, &allocate_info, nullptr, &memory);
            // Same ownership contract as the semaphore import above: Vulkan does not take ownership
            // of this NT handle, so it is this side's responsibility (and safe) to close its
            // duplicate immediately once the call has returned, success or not.
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
                // Same order as destroy_composition_swapchain_resources' steady-state teardown below
                // (image before the memory bound to it), spelled out explicitly here rather than
                // relying on `image`'s destructor running after this function returns.
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

        // deviceLUID is exactly 8 bytes (VK_LUID_SIZE) per spec, the same width as a Windows LUID and
        // as adapter_luid_bits below — a plain memcpy is an exact, lossless reinterpretation.
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

        // Fences first: failing fast here (before allocating any image) keeps the error path from
        // having to unwind partially-imported images.
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
        // The D3D presenter may still be waiting for Vulkan writes to its shared images. Complete every
        // Vulkan submission before resize() releases those images, otherwise its D3D-side teardown can
        // invalidate memory that an in-flight command buffer still references.
        const VkResult idle_result = vkDeviceWaitIdle(device);
        if (idle_result != VK_SUCCESS) {
            return graphics_backend_error(
                idle_result == VK_ERROR_DEVICE_LOST ? GraphicsBackendErrorCode::DeviceLost
                                                    : GraphicsBackendErrorCode::OperationFailed,
                "Composition presenter resize could not wait for the Vulkan device to become idle.");
        }

        CompositionSwapchainResources resources{};
        // Reusing the presenter in place — not creating a new one — is the entire point: it keeps the
        // same D3D device, DXGI factory, and already-attached DirectComposition target/visual alive
        // across the resize, instead of paying full device creation plus a target detach/reattach
        // (visibly disruptive — that reattach is what caused flicker during live window resizing)
        // every single time the window changes size.
        resources.presenter = std::move(previous.presenter);
        // resize() leaves the underlying D3D fences untouched, so the semaphores already imported from
        // them remain valid. Their render-complete value has to travel with the semaphore: a Vulkan
        // timeline semaphore may only be signaled with a strictly greater value than its current one.
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
        // Views before images (a view referencing a destroyed image is invalid to hold even
        // momentarily), images before their bound memory (freeing memory a live VkImage is still
        // bound to is a validation error even if nothing accesses it again), semaphores are
        // independent of both and can go in any order relative to them, and the presenter — which
        // owns the D3D-side originals every one of these was imported from — last, so its own
        // teardown never has to reason about what Vulkan has or hasn't released yet.
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

#else // !defined(_WIN32)

    RendererExpected<CompositionSwapchainResources> create_composition_swapchain_resources(
        VkDevice /*device*/,
        VkPhysicalDevice /*physical_device*/,
        const GraphicsPlatform::NativeSurfaceHandle & /*surface*/,
        VkFormat /*vk_format*/,
        VkImageUsageFlags /*usage*/,
        u32 /*width*/,
        u32 /*height*/,
        u32 /*image_count*/,
        GraphicsPlatform::CompositionAlphaMode /*alpha_mode*/) {
        return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                      "Composition present is implemented only on Windows.");
    }

    void destroy_composition_swapchain_resources(VkDevice /*device*/,
                                                 CompositionSwapchainResources & /*resources*/) noexcept {
    }

#endif // defined(_WIN32)

} // namespace SFT::Core::Vulkan
