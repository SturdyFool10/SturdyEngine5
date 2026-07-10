module;

#pragma region Imports
#include <memory>
#include <optional>
#include <span>
#include <vector>
#pragma endregion

export module Sturdy.Renderer:Renderer;

import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.RHI;
import Sturdy.Platform;
import :Mesh;
import :Resources;

using std::optional;
using std::span;
using std::unique_ptr;
using std::vector;

export namespace SFT::Renderer {

    class Renderer {
      public:
        Renderer();
        ~Renderer();

        Renderer(const Renderer &) = delete;
        Renderer &operator=(const Renderer &) = delete;
        Renderer(Renderer &&) = delete;
        Renderer &operator=(Renderer &&) = delete;

        [[nodiscard]] Core::RendererExpected<Core::RenderSurfaceHandle> initialize(
            const Core::RendererCreateInfo &create_info);

        [[nodiscard]] Core::RendererExpected<Core::RenderSurfaceHandle> create_window_surface(
            Platform::Windowing::Window &window,
            u32 desired_frames_in_flight = 2);

        void destroy_window_surface(Core::RenderSurfaceHandle surface) noexcept;
        void on_surface_resize_needed(Core::RenderSurfaceHandle surface) noexcept;

        [[nodiscard]] Core::RendererResult render_frame(Core::RenderSurfaceHandle surface,
                                                        const Core::FrameInput &frame);

        void wait_idle() noexcept;

        [[nodiscard]] const Core::RendererCapabilities &capabilities() const noexcept { return capabilities_; }
        [[nodiscard]] const RHI::FeatureNegotiationReport *feature_negotiation_report() const noexcept;
        [[nodiscard]] optional<Core::GpuInfo> gpu_info() const;

        // Low-level escape hatches. `graphics_backend()` gives backend-specific extension points via
        // dynamic_cast when needed; `rhi_device()` is the API-agnostic low-level RHI surface.
        [[nodiscard]] Core::EngineBackend *graphics_backend() noexcept { return graphics_backend_.get(); }
        [[nodiscard]] const Core::EngineBackend *graphics_backend() const noexcept { return graphics_backend_.get(); }
        [[nodiscard]] RHI::RhiDevice *rhi_device() noexcept;
        [[nodiscard]] const RHI::RhiDevice *rhi_device() const noexcept;

        // High-level geometry API: callers hand geometry to the renderer with function calls, not RHI
        // descriptors. The renderer owns the CPU-side resource record and uploads through RHI when the
        // active backend has implemented the needed resource calls.
        [[nodiscard]] Core::RendererExpected<MeshHandle> create_mesh(span<const GeometryVertex> vertices,
                                                                     span<const u32> indices,
                                                                     const char *label = nullptr);

        // Uploads a CPU-resident Mesh (see :Mesh — Mesh::cube(), Mesh::uv_sphere(), ...) to the GPU
        // and stamps the resulting handle back onto it, so mesh.is_gpu_resident()/mesh.gpu_handle()
        // reflect the upload afterward. Uploading an already-resident mesh is a no-op that just
        // returns its existing handle — callers don't need to guard re-upload themselves.
        [[nodiscard]] Core::RendererExpected<MeshHandle> upload(Mesh &mesh);
        void destroy_mesh(MeshHandle handle) noexcept;
        [[nodiscard]] MeshResource *mesh(MeshHandle handle) noexcept;
        [[nodiscard]] const MeshResource *mesh(MeshHandle handle) const noexcept;

        [[nodiscard]] Core::RendererExpected<MaterialHandle> create_material(const char *label = nullptr);
        void destroy_material(MaterialHandle handle) noexcept;
        [[nodiscard]] MaterialResource *material(MaterialHandle handle) noexcept;
        [[nodiscard]] const MaterialResource *material(MaterialHandle handle) const noexcept;

        void destroy_all_resources() noexcept;

      private:
        struct WindowSurfaceRecord {
            Platform::Windowing::Window *window = nullptr;
            Core::RenderSurfaceHandle surface{};
            RHI::SurfaceHandle rhi_surface{};
            RHI::SwapchainHandle rhi_swapchain{};
            Core::Extent2D swapchain_extent{};
            u32 desired_frames_in_flight = 2;
            bool primary = false;
            bool rhi_swapchain_dirty = true;
        };

        [[nodiscard]] WindowSurfaceRecord *window_surface(Core::RenderSurfaceHandle surface) noexcept;
        [[nodiscard]] const WindowSurfaceRecord *window_surface(Core::RenderSurfaceHandle surface) const noexcept;
        [[nodiscard]] Core::RendererResult ensure_rhi_presentation_resources(WindowSurfaceRecord &record);
        [[nodiscard]] Core::RendererResult recreate_rhi_swapchain(WindowSurfaceRecord &record);
        [[nodiscard]] Core::RendererResult render_frame_rhi(WindowSurfaceRecord &record,
                                                            const Core::FrameInput &frame);
        void destroy_rhi_presentation_resources(WindowSurfaceRecord &record) noexcept;

        [[nodiscard]] Core::RendererResult try_upload_mesh(MeshResource &mesh);
        [[nodiscard]] Core::RendererResult recover_from_device_loss();
        [[nodiscard]] Core::RendererResult restore_gpu_resources_after_recovery();
        void invalidate_gpu_resource_handles_no_destroy() noexcept;
        [[nodiscard]] static Core::GraphicsBackendError graphics_error_from_rhi(const RHI::RhiError &error,
                                                                               const char *operation);

        unique_ptr<Core::EngineBackend> graphics_backend_;
        Core::RendererCreateInfo recovery_create_info_{};
        vector<WindowSurfaceRecord> window_surfaces_;
        Core::RendererCapabilities capabilities_{};
        vector<MeshResource> meshes_;
        vector<MaterialResource> materials_;
        vector<TextureResource> textures_;
        bool initialized_ = false;
        bool recovering_from_device_loss_ = false;
    };

} // namespace SFT::Renderer
