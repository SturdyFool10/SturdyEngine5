#pragma once

#include <Core/EngineBackend.hpp>

#pragma region Imports
#include <memory>
#include <optional>
#include <unordered_map>
#pragma endregion

namespace SFT::Core::D3D12 {

    /// Windows-only renderer backend. It owns the API-neutral RHI instance/device pair and translates
    /// Core window surfaces into the D3D12 RHI's Win32 surface records.
    class D3D12Backend final : public EngineBackend {
      public:
        ~D3D12Backend() override;

        RendererExpected<RenderSurfaceHandle> initialize(const RendererCreateInfo &init) override;
        RendererExpected<RenderSurfaceHandle> create_window_surface(
            Platform::Windowing::Window &window,
            u32 desired_frames_in_flight = 2) override;
        void destroy_window_surface(RenderSurfaceHandle surface) noexcept override;
        void on_surface_resize_needed(RenderSurfaceHandle surface, Extent2D extent) noexcept override;
        [[nodiscard]] RendererCapabilities capabilities() const noexcept override;
        [[nodiscard]] RHI::RenderThreadingCapabilities render_threading_capabilities() const noexcept override;
        [[nodiscard]] RHI::RhiDevice *rhi_device() noexcept override;
        [[nodiscard]] const RHI::RhiDevice *rhi_device() const noexcept override;
        [[nodiscard]] RendererExpected<RHI::SurfaceHandle> rhi_surface_for(RenderSurfaceHandle surface) override;
        [[nodiscard]] optional<GpuInfo> gpu_info() const override;
        void wait_idle() noexcept override;

      private:
        friend class ::SFT::Core::EngineBackend;
        explicit D3D12Backend(ConstructorKey key);

        [[nodiscard]] RendererExpected<RenderSurfaceHandle> create_surface(
            Platform::Windowing::Window &window);
        void destroy_resources() noexcept;
        void populate_capabilities(const RendererCreateInfo &init) noexcept;

        RendererCapabilities capabilities_{};
        optional<RHI::AdapterInfo> adapter_info_{};
        std::unordered_map<Platform::Windowing::WindowId, RHI::SurfaceHandle> surfaces_;
        std::unique_ptr<RHI::RhiInstance> instance_;
        std::unique_ptr<RHI::RhiDevice> device_;
        bool initialized_ = false;
    };

} // namespace SFT::Core::D3D12
