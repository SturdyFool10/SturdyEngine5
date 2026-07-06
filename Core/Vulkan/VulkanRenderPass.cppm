module;
#pragma region Imports
#include "volk.h"
#include <span>
#pragma endregion

export module Sturdy.Core:VulkanRenderPass;

import :RendererError;
import Sturdy.Foundation;

using SFT::Core::renderer_error;
using SFT::Core::RendererErrorCode;
using SFT::Core::RendererExpected;
using std::span;

export namespace SFT::Core::Vulkan {

    // ─── VulkanRenderPass ────────────────────────────────────────────────────────
    // Dynamic rendering (vkCmdBeginRendering) is preferred for Vulkan 1.3+ — these
    // types exist for drivers or passes that require explicit render passes.

    class VulkanRenderPass {
      public:
        VulkanRenderPass() = default;
        ~VulkanRenderPass() { destroy(); }

        VulkanRenderPass(const VulkanRenderPass &) = delete;
        VulkanRenderPass &operator=(const VulkanRenderPass &) = delete;

        VulkanRenderPass(VulkanRenderPass &&o) noexcept
            : device_(o.device_), render_pass_(o.render_pass_) {
            o.device_ = VK_NULL_HANDLE;
            o.render_pass_ = VK_NULL_HANDLE;
        }
        VulkanRenderPass &operator=(VulkanRenderPass &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                render_pass_ = o.render_pass_;
                o.device_ = VK_NULL_HANDLE;
                o.render_pass_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanRenderPass> create(
            VkDevice device,
            const VkRenderPassCreateInfo2 &info) noexcept {
            VkRenderPass rp = VK_NULL_HANDLE;
            if (vkCreateRenderPass2(device, &info, nullptr, &rp) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateRenderPass2 failed.");
            VulkanRenderPass out;
            out.device_ = device;
            out.render_pass_ = rp;
            return out;
        }

        [[nodiscard]] VkRenderPass vk_handle() const noexcept { return render_pass_; }
        [[nodiscard]] bool is_valid() const noexcept { return render_pass_ != VK_NULL_HANDLE; }

        void destroy() noexcept {
            if (render_pass_ == VK_NULL_HANDLE)
                return;
            vkDestroyRenderPass(device_, render_pass_, nullptr);
            render_pass_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkRenderPass render_pass_ = VK_NULL_HANDLE;
    };

    // ─── VulkanFramebuffer ───────────────────────────────────────────────────────

    class VulkanFramebuffer {
      public:
        VulkanFramebuffer() = default;
        ~VulkanFramebuffer() { destroy(); }

        VulkanFramebuffer(const VulkanFramebuffer &) = delete;
        VulkanFramebuffer &operator=(const VulkanFramebuffer &) = delete;

        VulkanFramebuffer(VulkanFramebuffer &&o) noexcept
            : device_(o.device_), framebuffer_(o.framebuffer_),
              width_(o.width_), height_(o.height_), layers_(o.layers_) {
            o.device_ = VK_NULL_HANDLE;
            o.framebuffer_ = VK_NULL_HANDLE;
        }
        VulkanFramebuffer &operator=(VulkanFramebuffer &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                framebuffer_ = o.framebuffer_;
                width_ = o.width_;
                height_ = o.height_;
                layers_ = o.layers_;
                o.device_ = VK_NULL_HANDLE;
                o.framebuffer_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanFramebuffer> create(
            VkDevice device,
            const VkFramebufferCreateInfo &info) noexcept {
            VkFramebuffer fb = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(device, &info, nullptr, &fb) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateFramebuffer failed.");
            VulkanFramebuffer out;
            out.device_ = device;
            out.framebuffer_ = fb;
            out.width_ = info.width;
            out.height_ = info.height;
            out.layers_ = info.layers;
            return out;
        }

        [[nodiscard]] VkFramebuffer vk_handle() const noexcept { return framebuffer_; }
        [[nodiscard]] bool is_valid() const noexcept { return framebuffer_ != VK_NULL_HANDLE; }
        [[nodiscard]] u32 width() const noexcept { return width_; }
        [[nodiscard]] u32 height() const noexcept { return height_; }
        [[nodiscard]] u32 layers() const noexcept { return layers_; }

        void destroy() noexcept {
            if (framebuffer_ == VK_NULL_HANDLE)
                return;
            vkDestroyFramebuffer(device_, framebuffer_, nullptr);
            framebuffer_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
        u32 width_ = 0;
        u32 height_ = 0;
        u32 layers_ = 1;
    };

} // namespace SFT::Core::Vulkan
