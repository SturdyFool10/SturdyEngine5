#pragma once

#include "Core/Renderer.hpp"

#include <concepts>
#include <memory>
#include <utility>

namespace SFT::Core {

    // The renderer abstraction seam. Every graphics API (Vulkan today; Metal, WebGPU later) is a
    // subclass. The glue (Engine) speaks only this interface and the API-agnostic types in
    // Renderer.hpp / RenderSurface.hpp — no Vulkan ever leaks upward. The backend owns ALL
    // graphics state: device, swapchain, frame pacing, the render graph, and its own threading.
    class EngineBackend {
        protected:
            struct ConstructorKey {
                private:
                    friend class EngineBackend;
                    constexpr ConstructorKey() = default;
            };

            explicit constexpr EngineBackend(ConstructorKey) noexcept {}

        public:
            virtual ~EngineBackend() = default;

            EngineBackend(const EngineBackend&) = delete;
            EngineBackend& operator=(const EngineBackend&) = delete;
            EngineBackend(EngineBackend&&) = delete;
            EngineBackend& operator=(EngineBackend&&) = delete;

            // --- Renderer interface (API-agnostic) -----------------------------------------
            // Bring up the device + surface + swapchain for the given window surface/extent.
            virtual RendererResult initialize(const RendererInit& init) = 0;
            // What this backend can actually do (queried after initialize()).
            [[nodiscard]] virtual RendererCapabilities capabilities() const = 0;
            // Render and present one frame. The backend owns acquire/record/submit/present and
            // (eventually) its own worker threads and render-graph passes.
            virtual RendererResult render_frame(const FrameInput& frame) = 0;
            // The framebuffer changed size (or the swapchain went out of date). A zero extent
            // (minimized) is valid and pauses presentation.
            virtual RendererResult on_resize(Extent2D extent) = 0;
            // Block until the device is idle (safe teardown / pre-resize).
            virtual void wait_idle() noexcept = 0;
            // --------------------------------------------------------------------------------

            template <typename Backend, typename... Args>
            requires std::derived_from<Backend, EngineBackend>
            && requires(Args&&... args) {
                new Backend(ConstructorKey{}, std::forward<Args>(args)...);
            }
            [[nodiscard]]
            static std::unique_ptr<Backend> create(Args&&... args)
            {
                return std::unique_ptr<Backend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
            }

            template <typename Backend, typename... Args>
            requires std::derived_from<Backend, EngineBackend>
            && requires(Args&&... args) {
                new Backend(ConstructorKey{}, std::forward<Args>(args)...);
            }
            [[nodiscard]]
            static std::shared_ptr<Backend> create_shared(Args&&... args)
            {
                return std::shared_ptr<Backend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
            }
    };

    // Constructs the Vulkan backend without dragging volk/Vulkan headers into the caller. This
    // is the API-selection switch point: a future build picks Vulkan/Metal/WebGPU here.
    [[nodiscard]] std::unique_ptr<EngineBackend> create_vulkan_backend();

} // namespace SFT::Core
