#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <chrono>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <Async/src/Async.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Platform/Platform.hpp>
#include <Text/Text.hpp>
#include "Culling.hpp"
#include "Mesh.hpp"
#include "Material.hpp"
#include "Scene.hpp"
#include "ReflectionBinding.hpp"
#include "Resources.hpp"
#include "RenderGraph.hpp"
#include "RenderGraphModule.hpp"
#include "TileGrid.hpp"
#include "TextAtlas.hpp"
#include "TextInstance.hpp"
#include "PresentationCoordinator.hpp"

using std::chrono::steady_clock;
using std::optional;
using std::span;
using std::string;
using std::string_view;
using std::unique_ptr;
using std::vector;

namespace SFT::Renderer {

    class Renderer {
      public:
        /// Constructs a `Renderer` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        Renderer();
        /// Destroys the `Renderer` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~Renderer();

        /// Disables this construction form for `Renderer`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Renderer(const Renderer &) = delete;
        /// Assigns a new value to this `Renderer`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Renderer &operator=(const Renderer &) = delete;
        /// Disables this construction form for `Renderer`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Renderer(Renderer &&) = delete;
        /// Assigns a new value to this `Renderer`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Renderer &operator=(Renderer &&) = delete;

        /// Initializes the `Renderer` for use.
        ///
        /// @param create_info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<Core::RenderSurfaceHandle> initialize(
            const Core::RendererCreateInfo &create_info);

        /// Creates a window surface from the supplied parameters.
        ///
        /// @param window Window used or affected by the operation.
        /// @param desired_frames_in_flight `desired_frames_in_flight` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<Core::RenderSurfaceHandle> create_window_surface(
            Platform::Windowing::Window &window,
            u32 desired_frames_in_flight = 2);

        /// Destroys the window surface identified by the supplied parameters.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_window_surface(Core::RenderSurfaceHandle surface) noexcept;
        /// Handles the surface resize needed event.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param extent `extent` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void on_surface_resize_needed(Core::RenderSurfaceHandle surface, Core::Extent2D extent) noexcept;
        /// Sets the presentation settings for this `Renderer`.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param settings Configuration values controlling the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult set_presentation_settings(Core::RenderSurfaceHandle surface,
                                                                     const Core::PresentationSettings &settings);


        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Core::PresentationSettings presentation_settings(Core::RenderSurfaceHandle surface) const noexcept;
        /// Performs the reconfigure backend operation for `Renderer` using the supplied arguments.
        ///
        /// @param create_info Description of the resource or operation to perform.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult reconfigure_backend(const Core::RendererCreateInfo &create_info);


        /// Queries HDR capabilities from the active backend or runtime state.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] RHI::RhiExpected<RHI::SurfaceHdrCapabilityQuery> query_hdr_capabilities(
            Core::RenderSurfaceHandle surface) const;


        /// Updates HDR content light level from the supplied values.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param update `update` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] RHI::RhiResult update_hdr_content_light_level(
            Core::RenderSurfaceHandle surface, const RHI::HdrContentLightLevelUpdate &update);


        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::PresentationResolution presentation_resolution(Core::RenderSurfaceHandle surface) const noexcept;


        /// Performs the last frame timings operation for `Renderer` using the supplied arguments.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] FrameTimingSnapshot last_frame_timings(Core::RenderSurfaceHandle surface) const noexcept;

        /// Renders frame using the current rendering state.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param frame `frame` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult render_frame(Core::RenderSurfaceHandle surface,
                                                        const Core::FrameInput &frame);


        /// Renders frame using the current rendering state.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult render_frame(const RenderFrameDesc &desc);


        /// Submits draw.
        ///
        /// @param mesh `mesh` value used by the operation.
        /// @param material `material` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult submit_draw(MeshHandle mesh, MaterialInstanceHandle material);

        /// Waits for idle to complete.
        ///
        /// @note This function does not throw exceptions.
        void wait_idle() noexcept;

        /// Returns the current or globally available capabilities value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Core::RendererCapabilities &capabilities() const noexcept;
        /// Returns the current or globally available feature negotiation report value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const RHI::FeatureNegotiationReport *feature_negotiation_report() const noexcept;
        /// Returns the current GPU info.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<Core::GpuInfo> gpu_info() const;


        /// Returns the current or globally available graphics backend value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Core::EngineBackend *graphics_backend() noexcept;
        /// Returns the current or globally available graphics backend value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Core::EngineBackend *graphics_backend() const noexcept;
        /// Returns the current or globally available RHI device value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::RhiDevice *rhi_device() noexcept;
        /// Returns the current or globally available RHI device value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const RHI::RhiDevice *rhi_device() const noexcept;


        /// Returns the current or globally available native extension value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        template <typename Extension>
        [[nodiscard]] Extension *native_extension() noexcept {
            RHI::RhiDevice *device = rhi_device();
            if (device == nullptr) {
                return nullptr;
            }
            return dynamic_cast<Extension *>(device->extension_interface(Extension::id()));
        }


        /// Creates a mesh from the supplied parameters.
        ///
        /// @param vertices `vertices` value used by the operation.
        /// @param indices `indices` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<MeshHandle> create_mesh(span<const GeometryVertex> vertices,
                                                                     span<const u32> indices,
                                                                     const char *label = nullptr);


        /// Uploads the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param mesh `mesh` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<MeshHandle> upload(Mesh &mesh);
        /// Destroys the mesh identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_mesh(MeshHandle handle) noexcept;
        /// Performs the mesh operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] MeshResource *mesh(MeshHandle handle) noexcept;
        /// Performs the mesh operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const MeshResource *mesh(MeshHandle handle) const noexcept;

        /// Creates a material from the supplied parameters.
        ///
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<MaterialHandle> create_material(const char *label = nullptr);
        /// Destroys the material identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_material(MaterialHandle handle) noexcept;
        /// Performs the material operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] MaterialResource *material(MaterialHandle handle) noexcept;
        /// Performs the material operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const MaterialResource *material(MaterialHandle handle) const noexcept;


        /// Creates a texture from the supplied parameters.
        ///
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param data Data consumed or referenced by the operation.
        /// @param label `label` value used by the operation.
        /// @param concurrent_queue_classes Queue used or affected by the operation.
        /// @param mip_levels `mip_levels` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<TextureHandle> create_texture(u32 width, u32 height,
                                                                           RHI::Format format,
                                                                           span<const std::byte> data,
                                                                           const char *label = nullptr,
                                                                           span<const RHI::QueueClass> concurrent_queue_classes = {},
                                                                           u32 mip_levels = 1);
        /// Destroys the texture identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_texture(TextureHandle handle) noexcept;
        /// Performs the texture operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] TextureResource *texture(TextureHandle handle) noexcept;
        /// Performs the texture operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const TextureResource *texture(TextureHandle handle) const noexcept;


        /// Clears placeholder texture.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param color `color` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult clear_placeholder_texture(TextureHandle handle, RHI::ClearColor color);


        /// Submits texture upload.
        ///
        /// @param resource `resource` value used by the operation.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param staging `staging` value used by the operation.
        /// @param staging_offset Offset from the beginning of the relevant range or buffer.
        /// @param queue Queue used or affected by the operation.
        /// @param d3d12_padded_rows `d3d12_padded_rows` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<TextureUploadSubmission> submit_texture_upload(
            TextureResource &resource, u32 width, u32 height, RHI::Format format,
            RHI::BufferHandle staging, u64 staging_offset = 0, RHI::QueueLane queue = {},
            bool d3d12_padded_rows = false);

        /// Creates a offscreen render target from the supplied parameters.
        ///
        /// @param description Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<OffscreenRenderTargetHandle> create_offscreen_render_target(
            const OffscreenRenderTargetDescription &description);
        /// Destroys the offscreen render target identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_offscreen_render_target(OffscreenRenderTargetHandle handle) noexcept;
        /// Performs the offscreen render target description operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<OffscreenRenderTargetDescription> offscreen_render_target_description(
            OffscreenRenderTargetHandle handle) const;
        /// Performs the offscreen render target texture operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] TextureHandle offscreen_render_target_texture(OffscreenRenderTargetHandle handle) const noexcept;


        /// Performs the adopt texture operation for `Renderer` using the supplied arguments.
        ///
        /// @param texture Texture used or affected by the operation.
        /// @param view `view` value used by the operation.
        /// @param sampler Sampler used or affected by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<TextureHandle> adopt_texture(RHI::TextureHandle texture,
                                                                           RHI::TextureViewHandle view,
                                                                           RHI::SamplerHandle sampler,
                                                                           const char *label = nullptr);


        /// Creates a material template from the supplied parameters.
        ///
        /// @param shader Shader used or affected by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<MaterialTemplateHandle> create_material_template(
            const Core::Slang::Shader &shader, const char *label = nullptr);


        /// Creates a material template from source from the supplied parameters.
        ///
        /// @param source Source value or resource.
        /// @param options Configuration values controlling the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<MaterialTemplateHandle> create_material_template_from_source(
            const Core::Slang::ShaderSource &source,
            const Core::Slang::ShaderCompileOptions &options = {},
            const char *label = nullptr);


        /// Performs the reload material template operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult reload_material_template(MaterialTemplateHandle handle);


        /// Polls shader hot reload for available work or state changes.
        ///
        /// @return Returns the current poll shader hot reload value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        usize poll_shader_hot_reload();

        /// Destroys the material template identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_material_template(MaterialTemplateHandle handle) noexcept;
        /// Performs the material template operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] MaterialTemplateResource *material_template(MaterialTemplateHandle handle) noexcept;
        /// Performs the material template operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const MaterialTemplateResource *material_template(MaterialTemplateHandle handle) const noexcept;


        /// Creates a material instance from the supplied parameters.
        ///
        /// @param material_template `material_template` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<MaterialInstanceHandle> create_material_instance(
            MaterialTemplateHandle material_template, const char *label = nullptr);
        /// Destroys the material instance identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_material_instance(MaterialInstanceHandle handle) noexcept;
        /// Performs the material instance operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] MaterialInstanceResource *material_instance(MaterialInstanceHandle handle) noexcept;
        /// Performs the material instance operation for `Renderer` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const MaterialInstanceResource *material_instance(MaterialInstanceHandle handle) const noexcept;


        /// Sets the material parameter for this `Renderer`.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param name Name used to identify or label the target.
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult set_material_parameter(MaterialInstanceHandle handle,
                                                                  string_view name, span<const std::byte> value);

        /// Sets the material float for this `Renderer`.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param name Name used to identify or label the target.
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult set_material_float(MaterialInstanceHandle handle, string_view name, f32 value);
        /// Sets the material vec4 for this `Renderer`.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param name Name used to identify or label the target.
        /// @param x `x` value used by the operation.
        /// @param y `y` value used by the operation.
        /// @param z `z` value used by the operation.
        /// @param w `w` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult set_material_vec4(MaterialInstanceHandle handle, string_view name,
                                                             f32 x, f32 y, f32 z, f32 w);

        /// Sets the material texture for this `Renderer`.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param texture Texture used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult set_material_texture(MaterialInstanceHandle handle,
                                                                string_view slot, TextureHandle texture);

        /// Destroys the all resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_all_resources() noexcept;

      private:


        struct FrameDeferredTargets {
            Core::Extent2D extent{};
            DeferredTargetFormats formats{};
            RHI::SampleCount samples = RHI::SampleCount::X1;
            RHI::TextureHandle gbuffer_albedo{};
            RHI::TextureViewHandle gbuffer_albedo_view{};
            RHI::TextureHandle gbuffer_normal{};
            RHI::TextureViewHandle gbuffer_normal_view{};
            RHI::TextureHandle gbuffer_material{};
            RHI::TextureViewHandle gbuffer_material_view{};
            RHI::TextureHandle gbuffer_emissive{};
            RHI::TextureViewHandle gbuffer_emissive_view{};


            RHI::TextureHandle motion{};
            RHI::TextureViewHandle motion_view{};
            RHI::TextureHandle scene_color{};
            RHI::TextureViewHandle scene_color_view{};
            RHI::TextureHandle depth{};
            RHI::TextureViewHandle depth_view{};


            RHI::TextureHandle msaa_depth{};
            RHI::TextureViewHandle msaa_depth_view{};
        };

        static constexpr u32 max_directional_shadow_cascades = 4;
        static constexpr u32 max_lighting_spot_lights = 8;
        static constexpr u32 max_lighting_point_lights = 8;
        static constexpr u32 max_shadowed_point_lights = 4;
        static constexpr u32 max_shadow_views = max_directional_shadow_cascades +
                                                max_lighting_spot_lights +
                                                max_shadowed_point_lights * 6;


        struct alignas(16) ShadowViewGpuData {
            glm::mat4 view_projection{1.0f};
            glm::vec4 atlas_scale_bias{};
            glm::vec4 depth_params{};


            glm::vec4 filter_params{};
        };

        struct alignas(16) DirectionalLightGpuData {
            glm::vec4 direction_angular_radius{};
            glm::vec4 radiance_shadow{};
            glm::vec4 cascade_splits{};
            glm::vec4 cascade_params{};
        };

        struct alignas(16) SpotLightGpuData {
            glm::vec4 position_range{};
            glm::vec4 direction_outer_cos{};
            glm::vec4 radiance_inner_cos{};
            glm::vec4 shadow_params{};
        };

        struct alignas(16) PointLightGpuData {
            glm::vec4 position_range{};
            glm::vec4 radiance_source_radius{};
            glm::vec4 shadow_params{};
        };

        struct alignas(16) ShadowLightingGpuData {
            glm::mat4 inverse_view_projection{1.0f};
            glm::mat4 view_projection{1.0f};
            glm::mat4 view{1.0f};
            glm::vec4 camera_position_near{};
            glm::vec4 ambient_radiance_exposure{};
            glm::vec4 background_color{};
            glm::vec4 counts{};
            glm::vec4 shadow_params{};

            glm::vec4 contact_shadow_params{};
            glm::vec4 gtao_params{};
            glm::vec4 viewport_params{};
            glm::vec4 spectral_params{};
            DirectionalLightGpuData sun{};
            std::array<SpotLightGpuData, max_lighting_spot_lights> spot_lights{};
            std::array<PointLightGpuData, max_lighting_point_lights> point_lights{};
            std::array<ShadowViewGpuData, max_shadow_views> shadow_views{};
        };

        struct ShadowRenderView {
            glm::mat4 view_projection{1.0f};
            Frustum frustum{};
            RHI::Rect2D viewport{};
        };

        struct PreparedShadowFrame {
            ShadowLightingGpuData gpu{};
            vector<ShadowRenderView> render_views;
            bool atlas_used = false;
        };

        struct FrameShadowTargets {
            u32 atlas_size = 0;
            RHI::Format format = RHI::Format::D32Float;
            RHI::TextureHandle atlas{};
            RHI::TextureViewHandle atlas_view{};
            RHI::BufferHandle lighting_buffer{};
        };


        struct alignas(16) AtmosphereGpuData {
            glm::vec4 rayleigh_scattering_exp_scale{};
            glm::vec4 mie_scattering_exp_scale{};
            glm::vec4 mie_extinction_phase_g{};
            glm::vec4 ozone_absorption_center_altitude{};
            glm::vec4 ozone_width_planet_atmosphere_radius{};
            glm::vec4 ground_albedo{};
            glm::vec4 planet_center_world{};
            glm::vec4 camera_position_planet_space{};
            glm::vec4 sun_direction_angular_radius{};
            glm::vec4 sun_illuminance{};
        };


        struct FrameAtmosphereTargets {
            RHI::BufferHandle constants_buffer{};
        };


        struct FrameBloomTargets {
            Core::Extent2D source_extent{};
            u32 requested_levels = 0;
            f32 downsample_ratio = 1.61803398875f;
            vector<Core::Extent2D> extents;
            vector<RHI::TextureHandle> textures;
            vector<RHI::TextureViewHandle> views;
            vector<RHI::BindGroupHandle> downsample_bind_groups;
            vector<RHI::BindGroupHandle> upsample_bind_groups;
        };


        struct HiZPyramidTargets {


            Core::Extent2D extent{};
            u32 mip_levels = 0;
            RHI::TextureHandle texture{};


            vector<RHI::TextureViewHandle> mip_views;

            RHI::TextureViewHandle full_view{};


            bool has_valid_data = false;
        };


        struct FrameCompositeTarget {
            Core::Extent2D extent{};
            RHI::Format format = RHI::Format::Undefined;
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
        };


        struct FrameGpuTimingTarget {
            RHI::QuerySetHandle query_set{};
            u32 capacity = 0;
            vector<RenderGraph::GpuPassTiming> pending;
            bool has_pending_results = false;
        };


        struct FrameCpuTimingTarget {
            vector<RenderGraph::CpuPassTiming> pass_timings;
            vector<std::pair<string, f64>> stage_timings;
            bool has_pending_results = false;
        };

        struct FrameSpectralPhotonTargets {
            RHI::BufferHandle photons{};
            RHI::BufferHandle valid_count{};
            RHI::BufferHandle hash_heads{};
            u32 photon_capacity = 0;
            u32 hash_capacity = 0;


            bool populated = false;


            u64 state_signature = 0;
        };


        struct RetiredPresentationResources {
            RHI::SwapchainHandle swapchain{};
            RHI::TextureHandle depth_texture{};
            RHI::TextureViewHandle depth_view{};
            vector<RHI::FenceHandle> completion_fences;
        };

        struct FrameInFlight {
            RHI::FenceHandle fence{};


            vector<RHI::CommandBufferHandle> command_buffers;
            vector<RHI::TextureHandle> transient_textures;
            vector<RHI::TextureViewHandle> transient_texture_views;
            vector<RHI::BindGroupHandle> transient_bind_groups;


            vector<RHI::RenderBundleHandle> transient_render_bundles;


            vector<RHI::BufferHandle> transient_buffers;
            vector<RHI::AccelerationStructureHandle> transient_acceleration_structures;


            TextFrameResources text_overlay_resources{};
            vector<RHI::SwapchainHandle> retired_swapchains;
            vector<RHI::TextureHandle> retired_presentation_textures;
            vector<RHI::TextureViewHandle> retired_presentation_texture_views;
            FrameDeferredTargets deferred_targets{};
            FrameShadowTargets shadow_targets{};
            FrameAtmosphereTargets atmosphere_targets{};
            FrameBloomTargets bloom_targets{};
            FrameCompositeTarget composite_target{};
            FrameGpuTimingTarget gpu_timing{};
            FrameCpuTimingTarget cpu_timing{};


            RHI::QuerySetHandle pregraph_gpu_timing_query_set{};
            vector<RenderGraph::GpuPassTiming> pregraph_gpu_timing_pending;
            RHI::AccelerationStructureHandle spectral_tlas{};
            RHI::BufferHandle spectral_scene_instances{};
            RHI::BufferHandle spectral_materials{};


            vector<TextureHandle> spectral_material_textures;
            RHI::BufferHandle spectral_frame_constants{};
            RHI::BufferHandle spectral_photon_constants{};
            glm::vec4 spectral_scene_bounds{0.0f, 0.0f, 0.0f, 1.0f};
            FrameSpectralPhotonTargets spectral_photon_targets{};
            bool submitted = false;
        };

        struct OffscreenRenderTargetGpuResources {
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            RHI::SamplerHandle sampler{};
        };

        struct OffscreenRenderTargetRecord {
            OffscreenRenderTargetDescription description{};
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            RHI::SamplerHandle sampler{};
            TextureHandle sampled_texture{};
            bool initialized = false;
            bool alive = false;
        };

        struct ResolvedOffscreenRenderTarget {
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            Core::Extent2D extent{};
            bool initialized = false;
        };

        struct SpectralAccumulationTarget {
            Core::Extent2D extent{};
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            u64 state_signature = 0;
            u64 last_frame_index = 0;
            bool initialized = false;
        };

        struct SceneFrameGpuResources {
            RHI::BufferHandle view_buffer{};
            RHI::BufferHandle object_buffer{};
            usize object_capacity = 0;


            RHI::BufferHandle indirect_commands_buffer{};
            usize indirect_commands_capacity = 0;
            RHI::BufferHandle compacted_indices_buffer{};
            usize compacted_indices_capacity = 0;
        };

        struct WindowSurfaceRecord {
            Platform::Windowing::Window *window = nullptr;
            Core::RenderSurfaceHandle surface{};
            RHI::SurfaceHandle rhi_surface{};
            RHI::SwapchainHandle rhi_swapchain{};
            RHI::TextureHandle depth_texture{};
            RHI::TextureViewHandle depth_view{};
            RHI::Format depth_format = RHI::Format::D32Float;
            Core::Extent2D swapchain_extent{};
            u32 desired_frames_in_flight = 2;
            Core::PresentationSettings presentation{};


            f32 ui_reference_white_nits = 0.0f;
            bool primary = false;
            bool rhi_swapchain_dirty = true;


            vector<RHI::FenceHandle> active_presentation_completion_fences;
            optional<RHI::FenceHandle> pending_present_completion_fence;
            vector<RetiredPresentationResources> retired_presentation_resources;


            optional<Async::TaskHandle<RHI::RhiExpected<RHI::PresentOutcome>>> pending_present;


            f64 last_present_lock_wait_ms = 0.0;
            SpectralAccumulationTarget spectral_accumulation{};


            vector<FrameInFlight> frames_in_flight;


            vector<SceneFrameGpuResources> scene_frame_resources;


            RenderGraph graph;


            RenderGraphBlackboard graph_resources;


            HiZPyramidTargets hiz_pyramid;


            unique_ptr<Async::Mutex<FrameTimingSnapshot>> last_frame_timings =
                std::make_unique<Async::Mutex<FrameTimingSnapshot>>();
        };

        struct RenderItem {
            MeshHandle mesh{};
            MaterialInstanceHandle material{};
            glm::mat4 world_transform{1.0f};
            glm::mat4 previous_world_transform{1.0f};
            u64 stable_id = 0;
            u32 sort_key = 0;


            u32 object_index = 0;
        };


        struct RenderItemBindingState {
            RHI::RenderPipelineHandle pipeline{};
            MaterialInstanceHandle material{};
            u32 material_frame_slot = ~0u;


            bool arena_bound = false;


            RHI::BindGroupHandle bound_object_history_group{};
        };


        struct FrameSubmission {
            vector<RenderItem> draws;


            vector<RenderItem> gizmo_draws;
            glm::mat4 view_projection{1.0f};
            u64 frame_index = 0;
            CameraView camera{};
            SceneLighting lighting{};
            DeferredTargetFormats deferred_formats{};
            RenderGraphSettings render_graph{};
            OffscreenRenderTargetHandle offscreen_target{};
            vector<RHI::BindGroupHandle> transient_bind_groups;
            vector<RHI::BufferHandle> transient_buffers;


            vector<RHI::RenderBundleHandle> transient_render_bundles;
            TextAtlasRetiredResources retired_text_atlas_resources;
            UString debug_label;


            vector<std::pair<string, f64>> pre_dispatch_stage_timings_ms;
        };


        struct TonemapPipelineVariant {
            RHI::Format color_format = RHI::Format::Undefined;
            RHI::RenderPipelineHandle pipeline{};
        };


        struct GpuDrawIndexedIndirectCommand {
            u32 index_count = 0;
            u32 instance_count = 0;
            u32 first_index = 0;
            i32 vertex_offset = 0;
            u32 first_instance = 0;
        };


        struct InstancedBatch {
            MeshHandle mesh{};
            MaterialInstanceHandle material{};


            u32 first_object_index = 0;
            u32 instance_count = 0;
        };


        struct HiZCullInput {
            RHI::TextureViewHandle pyramid_view{};
            u32 extent_width = 0;
            u32 extent_height = 0;
            u32 mip_count = 0;
            bool valid = false;
        };


        struct InstanceCullResources {
            Core::Slang::Shader cull_shader;
            RHI::ShaderModuleHandle cull_module{};
            RHI::BindGroupLayoutHandle cull_bind_group_layout{};
            RHI::PipelineLayoutHandle cull_pipeline_layout{};
            RHI::ComputePipelineHandle cull_pipeline{};

            Core::Slang::Shader instanced_vertex_shader;
            RHI::ShaderModuleHandle instanced_vertex_module{};
            RHI::BindGroupLayoutHandle instance_data_bind_group_layout{};

            bool ready = false;
        };


        struct InstancedPipelineVariant {
            vector<RHI::Format> color_formats;
            RHI::Format depth_format = RHI::Format::Undefined;
            RHI::SampleCount samples = RHI::SampleCount::X1;
            RHI::RenderPipelineHandle pipeline{};
        };
        struct InstancedTemplateResources {
            RHI::PipelineLayoutHandle pipeline_layout{};
            vector<InstancedPipelineVariant> pipeline_variants;
        };


        struct ObjectHistoryResources {
            Core::Slang::Shader vertex_shader;
            RHI::ShaderModuleHandle vertex_module{};
            RHI::BindGroupLayoutHandle bind_group_layout{};
            bool ready = false;
        };


        struct ObjectHistoryPipelineVariant {
            vector<RHI::Format> color_formats;
            RHI::Format depth_format = RHI::Format::Undefined;
            bool standard_depth_test = false;
            RHI::SampleCount samples = RHI::SampleCount::X1;
            RHI::RenderPipelineHandle pipeline{};
        };
        struct ObjectHistoryTemplateResources {
            RHI::PipelineLayoutHandle pipeline_layout{};
            vector<ObjectHistoryPipelineVariant> pipeline_variants;
        };

        struct DeferredMsaaPipelineVariant {
            RHI::Format color_format = RHI::Format::Undefined;
            RHI::RenderPipelineHandle pipeline{};
        };


        struct DeferredMsaaResources {
            Core::Slang::Shader shader;
            RHI::ShaderModuleHandle vertex_module{};
            RHI::ShaderModuleHandle fragment_module{};
            string vertex_entry_point;
            string fragment_entry_point;
            vector<RHI::BindGroupLayoutHandle> bind_group_layouts;
            vector<u32> bind_group_layout_sets;
            RHI::BindGroupLayoutHandle sampled_layout{};
            u32 sampled_set = 0;
            u32 color_binding = 0;
            u32 depth_binding = 0;
            u32 geometry_depth_binding = 0;
            RHI::PipelineLayoutHandle pipeline_layout{};
            vector<DeferredMsaaPipelineVariant> pipeline_variants;
            bool ready = false;
        };

        struct TonemapResources {
            Core::Slang::Shader shader;
            RHI::ShaderModuleHandle vertex_module{};
            RHI::ShaderModuleHandle fragment_module{};
            std::string vertex_entry_point;
            std::string fragment_entry_point;
            std::vector<RHI::BindGroupLayoutHandle> bind_group_layouts;
            std::vector<u32> bind_group_layout_sets;
            RHI::PipelineLayoutHandle pipeline_layout{};
            RHI::SamplerHandle sampler{};
            std::vector<TonemapPipelineVariant> pipeline_variants;
            bool ready = false;
        };

        struct ShadowLightingPipelineVariant {
            RHI::Format color_format = RHI::Format::Undefined;
            RHI::RenderPipelineHandle pipeline{};
        };

        struct ShadowLightingResources {
            Core::Slang::Shader shader;
            RHI::ShaderModuleHandle vertex_module{};
            RHI::ShaderModuleHandle fragment_module{};
            string vertex_entry_point;
            string fragment_entry_point;
            vector<RHI::BindGroupLayoutHandle> bind_group_layouts;
            vector<u32> bind_group_layout_sets;
            RHI::PipelineLayoutHandle pipeline_layout{};
            RHI::SamplerHandle gbuffer_sampler{};
            RHI::SamplerHandle shadow_sampler{};


            RHI::SamplerHandle atmosphere_sampler{};
            vector<ShadowLightingPipelineVariant> pipeline_variants;
            bool ready = false;
        };

        struct BloomResources {
            Core::Slang::Shader shader;
            RHI::ShaderModuleHandle vertex_module{};
            RHI::ShaderModuleHandle prefilter_module{};
            RHI::ShaderModuleHandle downsample_module{};
            RHI::ShaderModuleHandle upsample_module{};
            std::string vertex_entry_point;
            std::string prefilter_entry_point;
            std::string downsample_entry_point;
            std::string upsample_entry_point;
            std::vector<RHI::BindGroupLayoutHandle> bind_group_layouts;
            std::vector<u32> bind_group_layout_sets;
            RHI::PipelineLayoutHandle pipeline_layout{};
            RHI::SamplerHandle sampler{};
            RHI::RenderPipelineHandle prefilter_pipeline{};
            RHI::RenderPipelineHandle downsample_pipeline{};
            RHI::RenderPipelineHandle upsample_pipeline{};
            RHI::BindGroupLayoutHandle sampled_layout{};
            u32 sampled_set = 0;
            u32 image_binding = 0;
            u32 sampler_binding = 0;
            RHI::Format color_format = RHI::Format::Undefined;
            bool ready = false;
        };


        struct HiZBuildResources {
            Core::Slang::Shader shader;
            RHI::ShaderModuleHandle vertex_module{};
            RHI::ShaderModuleHandle reduce_module{};
            RHI::BindGroupLayoutHandle bind_group_layout{};
            RHI::PipelineLayoutHandle pipeline_layout{};
            RHI::RenderPipelineHandle pipeline{};
            u32 source_binding = 0;
            RHI::Format color_format = RHI::Format::R32Float;
            bool ready = false;
        };


        struct AtmosphereLutBakePipeline {
            Core::Slang::Shader shader;
            RHI::ShaderModuleHandle module{};
            RHI::BindGroupLayoutHandle bind_group_layout{};
            RHI::PipelineLayoutHandle pipeline_layout{};
            RHI::ComputePipelineHandle pipeline{};
        };


        struct AtmosphereLutResources {
            AtmosphereLutBakePipeline transmittance;
            AtmosphereLutBakePipeline multi_scattering;
            AtmosphereLutBakePipeline sky_view;
            RHI::SamplerHandle lut_sampler{};
            bool ready = false;
        };


        struct BloomCompositePipelineVariant {
            RHI::Format color_format = RHI::Format::Undefined;
            RHI::RenderPipelineHandle pipeline{};
        };
        struct BloomCompositeResources {
            Core::Slang::Shader shader;
            RHI::ShaderModuleHandle vertex_module{};
            RHI::ShaderModuleHandle fragment_module{};
            std::string vertex_entry_point;
            std::string fragment_entry_point;
            std::vector<RHI::BindGroupLayoutHandle> bind_group_layouts;
            std::vector<u32> bind_group_layout_sets;
            RHI::PipelineLayoutHandle pipeline_layout{};
            RHI::SamplerHandle sampler{};
            std::vector<BloomCompositePipelineVariant> pipeline_variants;
            u32 scene_binding = 0;
            u32 bloom_binding = 0;
            u32 sampler_binding = 0;
            bool ready = false;
        };

        struct CustomPostProcessResources {
            std::string shader_path;
            std::string module_name;
            std::string fragment_entry_point;
            RHI::Format color_format = RHI::Format::Undefined;
            Core::Slang::Shader shader;
            RHI::ShaderModuleHandle vertex_module{};
            RHI::ShaderModuleHandle fragment_module{};
            RHI::BindGroupLayoutHandle bind_group_layout{};
            RHI::PipelineLayoutHandle pipeline_layout{};
            RHI::SamplerHandle sampler{};
            RHI::RenderPipelineHandle pipeline{};
            u32 image_binding = 0;
            u32 sampler_binding = 0;
            u32 push_constant_size = 0;
        };

        struct SpectralIntegratorViews {
            RHI::TextureViewHandle raster_albedo{};
            RHI::TextureViewHandle raster_normal{};
            RHI::TextureViewHandle raster_material{};
            RHI::TextureViewHandle raster_emissive{};
            RHI::TextureViewHandle raster_motion{};
            RHI::TextureViewHandle raster_depth{};
            RHI::TextureViewHandle effect_output{};
            RHI::TextureViewHandle scene_color_output{};
            RHI::TextureViewHandle gbuffer_motion_output{};
            RHI::TextureViewHandle primary_depth_output{};
            RHI::TextureViewHandle accumulation_output{};


            RHI::TextureViewHandle transmittance_lut{};
            RHI::TextureViewHandle sky_view_lut{};
            RHI::BufferHandle atmosphere_constants{};
        };


        struct SpectralMaterialParameterCacheEntry {
            u64 content_revision = 0;
            glm::vec4 base_color{0.8f, 0.8f, 0.8f, 1.0f};
            glm::vec4 emissive_and_strength{0.0f, 0.0f, 0.0f, 1.0f};

            glm::vec4 surface{0.5f, 0.0f, 1.0f, 0.04f};

            glm::vec4 transmission{0.0f, 1.4878f, 0.0042f, 0.0f};

            glm::vec4 alpha_and_normal{0.0f, 1.0f, 0.0f, 0.0f};
            TextureHandle base_color_texture{};
            TextureHandle metallic_roughness_texture{};
            TextureHandle normal_texture{};
            TextureHandle occlusion_texture{};
            TextureHandle emissive_texture{};
        };

        struct SpectralPathTracingResources {
            Core::Slang::Shader shader;
            array<RHI::ShaderModuleHandle, 5> modules{};
            array<RHI::ComputePipelineHandle, 5> pipelines{};
            vector<RHI::BindGroupLayoutHandle> bind_group_layouts;
            vector<u32> bind_group_layout_sets;
            std::unordered_map<string, ReflectedResource> resource_bindings;
            RHI::PipelineLayoutHandle pipeline_layout{};


            RHI::SamplerHandle atmosphere_sampler{};
            Core::Slang::Shader photon_shader;
            array<RHI::ShaderModuleHandle, 2> photon_modules{};
            array<RHI::ComputePipelineHandle, 2> photon_pipelines{};
            RHI::BindGroupLayoutHandle photon_bind_group_layout{};
            RHI::PipelineLayoutHandle photon_pipeline_layout{};
            std::unordered_map<string, ReflectedResource> photon_resource_bindings;
            Core::Slang::Shader depth_commit_shader;
            RHI::ShaderModuleHandle depth_commit_vertex_module{};
            RHI::ShaderModuleHandle depth_commit_fragment_module{};
            RHI::BindGroupLayoutHandle depth_commit_bind_group_layout{};
            RHI::PipelineLayoutHandle depth_commit_pipeline_layout{};
            RHI::RenderPipelineHandle depth_commit_pipeline{};
            u32 depth_commit_texture_binding = 0;
            u32 material_texture_capacity = 0;
            bool ready = false;
        };

        struct CustomComputeEffectResources {
            std::string shader_path;
            std::string module_name;
            std::string compute_entry_point;
            Core::Slang::Shader shader;
            RHI::ShaderModuleHandle module{};
            RHI::BindGroupLayoutHandle bind_group_layout{};
            RHI::PipelineLayoutHandle pipeline_layout{};
            RHI::SamplerHandle sampler{};
            RHI::ComputePipelineHandle pipeline{};
            u32 source_binding = 0;
            u32 sampler_binding = 0;
            u32 output_binding = 0;
            u32 push_constant_size = 0;
        };


        struct TextOverlayResources {
            struct CachedLine {
                UString source;
                optional<Text::ShapedLine> shaped;
                bool initialized = false;
            };

            struct CachedVisibleLayout {
                usize first_line = 0;
                glm::vec2 origin_px{0.0f};
                f32 viewport_height_px = 0.0f;
                vector<UString> source_lines;
                vector<GlyphSlot> slots;
                vector<GlyphInstance> instances;
                bool valid = false;
            };

            Text::Font font;


            Text::Font emoji_font;
            bool has_emoji_font = false;
            TextAtlas atlas;
            TextPipeline pipeline;
            u64 font_id = 0;
            u64 emoji_font_id = 0;


            std::unordered_map<u32, Text::GlyphOutline> outline_cache;


            usize first_cached_line = 0;
            vector<CachedLine> line_cache;
            CachedVisibleLayout visible_layout;
            bool ready = false;
        };

        struct ShaderHotReloadPollResult {
            std::shared_ptr<Core::Slang::ShaderWatcher> watcher;
            vector<Core::Slang::ShaderChange> changes;
        };


        /// Creates a offscreen render target GPU resources from the supplied parameters.
        ///
        /// @param description Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<OffscreenRenderTargetGpuResources>
        create_offscreen_render_target_gpu_resources(const OffscreenRenderTargetDescription &description);
        /// Resolves offscreen render target into the concrete value used by the engine.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<ResolvedOffscreenRenderTarget> resolve_offscreen_render_target(
            OffscreenRenderTargetHandle handle) const noexcept;
        /// Marks offscreen render target initialized using the supplied arguments and current state.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void mark_offscreen_render_target_initialized(OffscreenRenderTargetHandle handle) noexcept;
        /// Performs the invalidate offscreen render targets after device loss operation for `Renderer` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void invalidate_offscreen_render_targets_after_device_loss() noexcept;
        /// Returns the current or globally available restore offscreen render targets after recovery value.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult restore_offscreen_render_targets_after_recovery();
        /// Destroys the all offscreen render targets identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_all_offscreen_render_targets() noexcept;

        /// Performs the window surface operation for `Renderer` using the supplied arguments.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowSurfaceRecord *window_surface(Core::RenderSurfaceHandle surface) noexcept;
        /// Performs the window surface operation for `Renderer` using the supplied arguments.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const WindowSurfaceRecord *window_surface(Core::RenderSurfaceHandle surface) const noexcept;
        /// Finds or creates the RHI presentation resources required by the operation.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_rhi_presentation_resources(WindowSurfaceRecord &record);


        /// Recreates RHI swapchain using the supplied arguments and current state.
        ///
        /// @param record `record` value used by the operation.
        /// @param frame_index Zero-based index of the target element or entry.
        /// @param known_extent `known_extent` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] Core::RendererResult recreate_rhi_swapchain(WindowSurfaceRecord &record, u64 frame_index = 0,
                                                                   optional<Core::Extent2D> known_extent = std::nullopt);


        /// Drains pending present using the supplied arguments and current state.
        ///
        /// @param record `record` value used by the operation.
        /// @param stage_timings_ms `stage_timings_ms` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult drain_pending_present(
            WindowSurfaceRecord &record, vector<std::pair<string, f64>> *stage_timings_ms);
        /// Finds or creates the RHI depth resources required by the operation.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_rhi_depth_resources(WindowSurfaceRecord &record);


        /// Renders frame dispatch using the current rendering state.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param frame `frame` value used by the operation.
        /// @param submission `submission` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult render_frame_dispatch(Core::RenderSurfaceHandle surface,
                                                                  const Core::FrameInput &frame,
                                                                  FrameSubmission &submission);
        /// Renders frame RHI using the current rendering state.
        ///
        /// @param record `record` value used by the operation.
        /// @param frame `frame` value used by the operation.
        /// @param submission `submission` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult render_frame_rhi(WindowSurfaceRecord &record,
                                                            const Core::FrameInput &frame,
                                                            FrameSubmission &submission);


        /// Reclaims frame slot using the supplied arguments and current state.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param destroy_retired_presentation `destroy_retired_presentation` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void reclaim_frame_slot(FrameInFlight &slot, bool destroy_retired_presentation = false) noexcept;
        /// Finds or creates the frame deferred targets required by the operation.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param extent `extent` value used by the operation.
        /// @param formats Format used for the resource, render target, or conversion.
        /// @param samples `samples` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_frame_deferred_targets(FrameInFlight &slot,
                                                                         Core::Extent2D extent,
                                                                         const DeferredTargetFormats &formats,
                                                                         RHI::SampleCount samples);
        /// Destroys the frame deferred targets identified by the supplied parameters.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_frame_deferred_targets(FrameInFlight &slot) noexcept;
        /// Finds or creates the frame shadow targets required by the operation.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param atlas_size Requested or available size for the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_frame_shadow_targets(FrameInFlight &slot, u32 atlas_size);
        /// Destroys the frame shadow targets identified by the supplied parameters.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_frame_shadow_targets(FrameInFlight &slot) noexcept;


        /// Finds or creates the frame atmosphere targets required by the operation.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_frame_atmosphere_targets(FrameInFlight &slot);
        /// Destroys the frame atmosphere targets identified by the supplied parameters.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_frame_atmosphere_targets(FrameInFlight &slot) noexcept;


        /// Prepares atmosphere frame for a later operation.
        ///
        /// @param submission `submission` value used by the operation.
        /// @param constants_buffer Buffer used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult prepare_atmosphere_frame(const FrameSubmission &submission,
                                                                    RHI::BufferHandle constants_buffer);
        /// Finds or creates the atmosphere lut resources required by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_atmosphere_lut_resources();


        /// Records atmosphere lut bakes using the supplied arguments and current state.
        ///
        /// @param graph `graph` value used by the operation.
        /// @param atmosphere_buffer Buffer used or affected by the operation.
        /// @param out_transmittance_lut `out_transmittance_lut` value used by the operation.
        /// @param out_multi_scattering_lut `out_multi_scattering_lut` value used by the operation.
        /// @param out_sky_view_lut `out_sky_view_lut` value used by the operation.
        /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_atmosphere_lut_bakes(
            RenderGraph &graph, RHI::BufferHandle atmosphere_buffer,
            RenderGraphTextureHandle &out_transmittance_lut, RenderGraphTextureHandle &out_multi_scattering_lut,
            RenderGraphTextureHandle &out_sky_view_lut, vector<RHI::BindGroupHandle> &transient_bind_groups);
        /// Destroys the atmosphere lut resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_atmosphere_lut_resources() noexcept;
        /// Prepares shadow frame for a later operation.
        ///
        /// @param submission `submission` value used by the operation.
        /// @param targets `targets` value used by the operation.
        /// @param prepared `prepared` value used by the operation.
        /// @param render_extent `render_extent` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult prepare_shadow_frame(const FrameSubmission &submission,
                                                                 FrameShadowTargets &targets,
                                                                 PreparedShadowFrame &prepared,
                                                                 Core::Extent2D render_extent);
        /// Finds or creates the frame bloom targets required by the operation.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param extent `extent` value used by the operation.
        /// @param requested_levels `requested_levels` value used by the operation.
        /// @param downsample_ratio `downsample_ratio` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_frame_bloom_targets(FrameInFlight &slot,
                                                                      Core::Extent2D extent,
                                                                      u32 requested_levels,
                                                                      f32 downsample_ratio);
        /// Destroys the frame bloom targets identified by the supplied parameters.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_frame_bloom_targets(FrameInFlight &slot) noexcept;
        /// Finds or creates the frame composite target required by the operation.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param extent `extent` value used by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_frame_composite_target(FrameInFlight &slot,
                                                                         Core::Extent2D extent,
                                                                         RHI::Format format);
        /// Destroys the frame composite target identified by the supplied parameters.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_frame_composite_target(FrameInFlight &slot) noexcept;


        /// Finds or creates the frame GPU timing target required by the operation.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param required_pass_count Number of elements or operations to process.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_frame_gpu_timing_target(FrameInFlight &slot, u32 required_pass_count);
        /// Destroys the frame GPU timing target identified by the supplied parameters.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_frame_gpu_timing_target(FrameInFlight &slot) noexcept;


        /// Finds or creates the frame pregraph GPU timing target required by the operation.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_frame_pregraph_gpu_timing_target(FrameInFlight &slot);
        /// Destroys the frame pregraph GPU timing target identified by the supplied parameters.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_frame_pregraph_gpu_timing_target(FrameInFlight &slot) noexcept;


        /// Drains frames in flight using the supplied arguments and current state.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void drain_frames_in_flight(WindowSurfaceRecord &record) noexcept;


        /// Performs the maybe flush retired swapchains operation for `Renderer` using the supplied arguments.
        ///
        /// @param record `record` value used by the operation.
        /// @param opportunistic `opportunistic` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void maybe_flush_retired_swapchains(WindowSurfaceRecord &record, bool opportunistic) noexcept;
        /// Reclaims completed presentation fences using the supplied arguments and current state.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void reclaim_completed_presentation_fences(WindowSurfaceRecord &record) noexcept;
        /// Reclaims completed retired presentations using the supplied arguments and current state.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void reclaim_completed_retired_presentations(WindowSurfaceRecord &record) noexcept;
        /// Destroys the retired presentations identified by the supplied parameters.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_retired_presentations(WindowSurfaceRecord &record) noexcept;
        /// Destroys the RHI presentation resources identified by the supplied parameters.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_rhi_presentation_resources(WindowSurfaceRecord &record) noexcept;
        /// Prepares scene GPU data for a later operation.
        ///
        /// @param record `record` value used by the operation.
        /// @param frame_index Zero-based index of the target element or entry.
        /// @param submission `submission` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult prepare_scene_gpu_data(
            WindowSurfaceRecord &record, u64 frame_index, const FrameSubmission &submission);
        /// Destroys the scene GPU resources identified by the supplied parameters.
        ///
        /// @param resources `resources` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_scene_gpu_resources(vector<SceneFrameGpuResources> &resources) noexcept;


        /// Renders item visible using the current rendering state.
        ///
        /// @param item `item` value used by the operation.
        /// @param frustum `frustum` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool render_item_visible(const RenderItem &item, const Frustum &frustum) noexcept;


        /// Records render item using the supplied arguments and current state.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        template <typename Encoder>
        [[nodiscard]] Core::RendererResult record_render_item(Encoder &pass,
                                                              const RenderItem &item,
                                                              span<const RHI::Format> color_formats,
                                                              RHI::Format depth_format,
                                                              u64 frame_index,
                                                              const glm::mat4 &view_projection,
                                                               bool depth_only,
                                                               RenderItemBindingState &binding_state,
                                                               bool standard_depth_test = false,
                                                               bool shadow_map = false,
                                                               f32 shadow_depth_bias = 0.0f,
                                                               f32 shadow_slope_bias = 0.0f,
                                                               RHI::SampleCount samples = RHI::SampleCount::X1,
                                                               bool with_object_history = false,
                                                               RHI::BindGroupHandle object_history_group = {});


        /// Records render items culled using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param items `items` value used by the operation.
        /// @param frustum `frustum` value used by the operation.
        /// @param color_formats Format used for the resource, render target, or conversion.
        /// @param depth_format Format used for the resource, render target, or conversion.
        /// @param frame_index Zero-based index of the target element or entry.
        /// @param view_projection `view_projection` value used by the operation.
        /// @param depth_only `depth_only` value used by the operation.
        /// @param standard_depth_test `standard_depth_test` value used by the operation.
        /// @param bundle_label `bundle_label` value used by the operation.
        /// @param use_bundles `use_bundles` value used by the operation.
        /// @param retired_bundles `retired_bundles` value used by the operation.
        /// @param shadow_map `shadow_map` value used by the operation.
        /// @param shadow_depth_bias `shadow_depth_bias` value used by the operation.
        /// @param shadow_slope_bias `shadow_slope_bias` value used by the operation.
        /// @param samples `samples` value used by the operation.
        /// @param with_object_history `with_object_history` value used by the operation.
        /// @param object_history_group `object_history_group` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_render_items_culled(RHI::RenderPassEncoder &pass,
                                                                       span<const RenderItem> items,
                                                                       const Frustum &frustum,
                                                                       span<const RHI::Format> color_formats,
                                                                       RHI::Format depth_format,
                                                                       u64 frame_index,
                                                                       const glm::mat4 &view_projection,
                                                                       bool depth_only,
                                                                       bool standard_depth_test,
                                                                       const char *bundle_label,
                                                                       bool use_bundles,
                                                                       vector<RHI::RenderBundleHandle> &retired_bundles,
                                                                       bool shadow_map = false,
                                                                       f32 shadow_depth_bias = 0.0f,
                                                                       f32 shadow_slope_bias = 0.0f,
                                                                       RHI::SampleCount samples = RHI::SampleCount::X1,
                                                                       bool with_object_history = false,
                                                                       RHI::BindGroupHandle object_history_group = {});


        /// Records shadow view chunk using the supplied arguments and current state.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        template <typename Encoder>
        [[nodiscard]] Core::RendererResult record_shadow_view_chunk(Encoder &encoder,
                                                                     span<const ShadowRenderView> views,
                                                                     span<const RenderItem> draws,
                                                                     RHI::Format depth_format,
                                                                     u64 frame_index,
                                                                     f32 shadow_depth_bias,
                                                                     f32 shadow_slope_bias);


        /// Performs the detect instanced batches operation for `Renderer` using the supplied arguments.
        ///
        /// @param sorted_draws `sorted_draws` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<InstancedBatch> detect_instanced_batches(span<const RenderItem> sorted_draws) const;

        /// Finds or creates the instance cull resources required by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_instance_cull_resources();
        /// Destroys the instance cull resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_instance_cull_resources() noexcept;


        /// Prepares instance cull GPU data for a later operation.
        ///
        /// @param batches `batches` value used by the operation.
        /// @param resources `resources` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult prepare_instance_cull_gpu_data(span<const InstancedBatch> batches,
                                                                          SceneFrameGpuResources &resources);
        /// Resolves the instanced pipeline associated with the supplied key, handle, or resource.
        ///
        /// @param material_template `material_template` value used by the operation.
        /// @param color_formats Format used for the resource, render target, or conversion.
        /// @param depth_format Format used for the resource, render target, or conversion.
        /// @param samples `samples` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> instanced_pipeline_for(
            MaterialTemplateResource &material_template, span<const RHI::Format> color_formats,
            RHI::Format depth_format, RHI::SampleCount samples = RHI::SampleCount::X1);


        /// Finds or creates the object history resources required by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_object_history_resources();
        /// Destroys the object history resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_object_history_resources() noexcept;
        /// Resolves the history pipeline associated with the supplied key, handle, or resource.
        ///
        /// @param material_template `material_template` value used by the operation.
        /// @param color_formats Format used for the resource, render target, or conversion.
        /// @param depth_format Format used for the resource, render target, or conversion.
        /// @param standard_depth_test `standard_depth_test` value used by the operation.
        /// @param samples `samples` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> history_pipeline_for(
            MaterialTemplateResource &material_template, span<const RHI::Format> color_formats,
            RHI::Format depth_format, bool standard_depth_test = false,
            RHI::SampleCount samples = RHI::SampleCount::X1);


        /// Finds or creates the object history bind group required by the operation.
        ///
        /// @param resources `resources` value used by the operation.
        /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<RHI::BindGroupHandle> ensure_object_history_bind_group(
            SceneFrameGpuResources &resources, vector<RHI::BindGroupHandle> &transient_bind_groups);


        /// Records instance cull using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param batches `batches` value used by the operation.
        /// @param view_projection `view_projection` value used by the operation.
        /// @param camera_world_position World used or affected by the operation.
        /// @param hiz `hiz` value used by the operation.
        /// @param resources `resources` value used by the operation.
        /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_instance_cull(RHI::ComputePassEncoder &pass,
                                                                span<const InstancedBatch> batches,
                                                                const glm::mat4 &view_projection,
                                                                const glm::vec3 &camera_world_position,
                                                                const HiZCullInput &hiz,
                                                                SceneFrameGpuResources &resources,
                                                                vector<RHI::BindGroupHandle> &transient_bind_groups);


        /// Finds or creates the hiz build resources required by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_hiz_build_resources();
        /// Destroys the hiz build resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_hiz_build_resources() noexcept;
        /// Destroys the hiz pyramid identified by the supplied parameters.
        ///
        /// @param pyramid `pyramid` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_hiz_pyramid(HiZPyramidTargets &pyramid) noexcept;


        /// Finds or creates the hiz pyramid required by the operation.
        ///
        /// @param pyramid `pyramid` value used by the operation.
        /// @param depth_extent `depth_extent` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_hiz_pyramid(HiZPyramidTargets &pyramid, Core::Extent2D depth_extent);


        /// Records hiz build using the supplied arguments and current state.
        ///
        /// @param graph `graph` value used by the operation.
        /// @param depth_texture Texture used or affected by the operation.
        /// @param depth_view `depth_view` value used by the operation.
        /// @param depth_extent `depth_extent` value used by the operation.
        /// @param pyramid_texture Texture used or affected by the operation.
        /// @param pyramid `pyramid` value used by the operation.
        /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_hiz_build(RenderGraph &graph, RenderGraphTextureHandle depth_texture,
                                                             RHI::TextureViewHandle depth_view, Core::Extent2D depth_extent,
                                                             RenderGraphTextureHandle pyramid_texture, HiZPyramidTargets &pyramid,
                                                             vector<RHI::BindGroupHandle> &transient_bind_groups);


        /// Records instanced batches using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param batches `batches` value used by the operation.
        /// @param color_formats Format used for the resource, render target, or conversion.
        /// @param depth_format Format used for the resource, render target, or conversion.
        /// @param frame_index Zero-based index of the target element or entry.
        /// @param view_projection `view_projection` value used by the operation.
        /// @param previous_view_projection `previous_view_projection` value used by the operation.
        /// @param resources `resources` value used by the operation.
        /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
        /// @param samples `samples` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_instanced_batches(RHI::RenderPassEncoder &pass,
                                                                    span<const InstancedBatch> batches,
                                                                    span<const RHI::Format> color_formats,
                                                                    RHI::Format depth_format,
                                                                    u64 frame_index,
                                                                    const glm::mat4 &view_projection,
                                                                    const glm::mat4 &previous_view_projection,
                                                                    SceneFrameGpuResources &resources,
                                                                    vector<RHI::BindGroupHandle> &transient_bind_groups,
                                                                    RHI::SampleCount samples = RHI::SampleCount::X1);

        /// Attempts to upload mesh without requiring normal failure to be exceptional.
        ///
        /// @param mesh `mesh` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult try_upload_mesh(MeshResource &mesh);


        /// Creates a owned texture GPU from the supplied parameters.
        ///
        /// @param resource `resource` value used by the operation.
        /// @param concurrent_queue_classes Queue used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult create_owned_texture_gpu(TextureResource &resource,
                                                                     span<const RHI::QueueClass> concurrent_queue_classes = {});


        /// Uploads texture rgba using the supplied arguments and current state.
        ///
        /// @param resource `resource` value used by the operation.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult upload_texture_rgba(TextureResource &resource, u32 width, u32 height,
                                                               RHI::Format format, span<const std::byte> data);


        /// Finds or creates the default white texture required by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<TextureHandle> ensure_default_white_texture();


        /// Resolves the material pipeline associated with the supplied key, handle, or resource.
        ///
        /// @param material_template `material_template` value used by the operation.
        /// @param color_formats Format used for the resource, render target, or conversion.
        /// @param depth_format Format used for the resource, render target, or conversion.
        /// @param standard_depth_test `standard_depth_test` value used by the operation.
        /// @param samples `samples` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> material_pipeline_for(
            MaterialTemplateResource &material_template, span<const RHI::Format> color_formats, RHI::Format depth_format,
            bool standard_depth_test = false, RHI::SampleCount samples = RHI::SampleCount::X1);


        /// Resolves the depth only pipeline associated with the supplied key, handle, or resource.
        ///
        /// @param material_template `material_template` value used by the operation.
        /// @param depth_format Format used for the resource, render target, or conversion.
        /// @param shadow_map `shadow_map` value used by the operation.
        /// @param depth_bias `depth_bias` value used by the operation.
        /// @param slope_bias `slope_bias` value used by the operation.
        /// @param samples `samples` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> depth_only_pipeline_for(
            MaterialTemplateResource &material_template, RHI::Format depth_format,
            bool shadow_map = false, f32 depth_bias = 0.0f, f32 slope_bias = 0.0f,
            RHI::SampleCount samples = RHI::SampleCount::X1);


        /// Prepares material frame for a later operation.
        ///
        /// @param instance Instance used or affected by the operation.
        /// @param frame_slot Binding or storage slot addressed by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<span<const RHI::BindGroupHandle>> prepare_material_frame(
            MaterialInstanceResource &instance, u32 frame_slot);


        /// Builds material template GPU.
        ///
        /// @param resource `resource` value used by the operation.
        /// @param shader Shader used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult build_material_template_gpu(MaterialTemplateResource &resource,
                                                                       const Core::Slang::Shader &shader);


        /// Initializes material instance state for use.
        ///
        /// @param instance Instance used or affected by the operation.
        /// @param tmpl `tmpl` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult initialize_material_instance_state(MaterialInstanceResource &instance,
                                                                              MaterialTemplateResource &tmpl);
        /// Destroys the material template GPU identified by the supplied parameters.
        ///
        /// @param resource `resource` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_material_template_gpu(MaterialTemplateResource &resource) noexcept;
        /// Destroys the material instance GPU identified by the supplied parameters.
        ///
        /// @param resource `resource` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_material_instance_gpu(MaterialInstanceResource &resource) noexcept;


        /// Finds or creates the shadow lighting resources required by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_shadow_lighting_resources();
        /// Resolves the shadow lighting pipeline associated with the supplied key, handle, or resource.
        ///
        /// @param color_format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> shadow_lighting_pipeline_for(
            RHI::Format color_format);
        /// Records shadow lighting using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param albedo_view `albedo_view` value used by the operation.
        /// @param normal_view `normal_view` value used by the operation.
        /// @param material_view `material_view` value used by the operation.
        /// @param emissive_view `emissive_view` value used by the operation.
        /// @param depth_view `depth_view` value used by the operation.
        /// @param spectral_effect_view `spectral_effect_view` value used by the operation.
        /// @param shadow_atlas_view `shadow_atlas_view` value used by the operation.
        /// @param lighting_buffer Buffer used or affected by the operation.
        /// @param transmittance_lut_view `transmittance_lut_view` value used by the operation.
        /// @param multi_scattering_lut_view `multi_scattering_lut_view` value used by the operation.
        /// @param sky_view_lut_view `sky_view_lut_view` value used by the operation.
        /// @param atmosphere_buffer Buffer used or affected by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_shadow_lighting(
            RHI::RenderPassEncoder &pass,
            RHI::TextureViewHandle albedo_view,
            RHI::TextureViewHandle normal_view,
            RHI::TextureViewHandle material_view,
            RHI::TextureViewHandle emissive_view,
            RHI::TextureViewHandle depth_view,
            RHI::TextureViewHandle spectral_effect_view,
            RHI::TextureViewHandle shadow_atlas_view,
            RHI::BufferHandle lighting_buffer,
            RHI::TextureViewHandle transmittance_lut_view,
            RHI::TextureViewHandle multi_scattering_lut_view,
            RHI::TextureViewHandle sky_view_lut_view,
            RHI::BufferHandle atmosphere_buffer,
            RHI::Format color_format,
            vector<RHI::BindGroupHandle> &transient_bind_groups);
        /// Destroys the shadow lighting resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_shadow_lighting_resources() noexcept;
        /// Destroys the shadow lighting resources locked identified by the supplied parameters.
        ///
        /// @param resources `resources` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_shadow_lighting_resources_locked(ShadowLightingResources &resources) noexcept;


        /// Builds deferred MSAA module.
        ///
        /// @param context Context that supplies state required by the operation.
        /// @param submission `submission` value used by the operation.
        /// @param samples `samples` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult build_deferred_msaa_module(
            RenderGraphModuleBuildContext &context,
            FrameSubmission &submission,
            RHI::SampleCount samples);
        /// Builds post process aa module.
        ///
        /// @param context Context that supplies state required by the operation.
        /// @param submission `submission` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult build_post_process_aa_module(
            RenderGraphModuleBuildContext &context,
            FrameSubmission &submission);
        /// Builds custom graph stage.
        ///
        /// @param context Context that supplies state required by the operation.
        /// @param submission `submission` value used by the operation.
        /// @param stage `stage` value used by the operation.
        /// @param logical_textures Texture used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult build_custom_graph_stage(
            RenderGraphModuleBuildContext &context,
            FrameSubmission &submission,
            PostProcessStage stage,
            span<RenderGraphTextureHandle> logical_textures);
        /// Builds bloom module.
        ///
        /// @param context Context that supplies state required by the operation.
        /// @param submission `submission` value used by the operation.
        /// @param frame_slot Binding or storage slot addressed by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        /// @param bloom_format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult build_bloom_module(
            RenderGraphModuleBuildContext &context,
            FrameSubmission &submission,
            FrameInFlight &frame_slot,
            bool enabled,
            RHI::Format bloom_format);
        /// Builds tonemap module.
        ///
        /// @param context Context that supplies state required by the operation.
        /// @param submission `submission` value used by the operation.
        /// @param presentation_format Format used for the resource, render target, or conversion.
        /// @param hdr_output `hdr_output` value used by the operation.
        /// @param hdr_color_space `hdr_color_space` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult build_tonemap_module(
            RenderGraphModuleBuildContext &context,
            FrameSubmission &submission,
            RHI::Format presentation_format,
            bool hdr_output,
            Core::HdrColorSpaceMode hdr_color_space);


        /// Finds or creates the deferred MSAA resources required by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_deferred_msaa_resources();
        /// Resolves the deferred MSAA pipeline associated with the supplied key, handle, or resource.
        ///
        /// @param color_format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> deferred_msaa_pipeline_for(
            RHI::Format color_format);
        /// Records deferred MSAA reconstruction using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param color_view `color_view` value used by the operation.
        /// @param depth_view `depth_view` value used by the operation.
        /// @param geometry_depth_view `geometry_depth_view` value used by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param extent `extent` value used by the operation.
        /// @param samples `samples` value used by the operation.
        /// @param near_plane `near_plane` value used by the operation.
        /// @param far_plane `far_plane` value used by the operation.
        /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_deferred_msaa_reconstruction(
            RHI::RenderPassEncoder &pass,
            RHI::TextureViewHandle color_view,
            RHI::TextureViewHandle depth_view,
            RHI::TextureViewHandle geometry_depth_view,
            RHI::Format color_format,
            Core::Extent2D extent,
            RHI::SampleCount samples,
            f32 near_plane,
            f32 far_plane,
            vector<RHI::BindGroupHandle> &transient_bind_groups);
        /// Destroys the deferred MSAA resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_deferred_msaa_resources() noexcept;
        /// Destroys the deferred MSAA resources locked identified by the supplied parameters.
        ///
        /// @param resources `resources` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_deferred_msaa_resources_locked(DeferredMsaaResources &resources) noexcept;

        /// Finds or creates the bloom resources required by the operation.
        ///
        /// @param color_format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_bloom_resources(RHI::Format color_format);
        /// Records bloom draw using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param source_view `source_view` value used by the operation.
        /// @param source_texel_size Requested or available size for the operation.
        /// @param threshold `threshold` value used by the operation.
        /// @param soft_knee `soft_knee` value used by the operation.
        /// @param scatter `scatter` value used by the operation.
        /// @param filter_scale `filter_scale` value used by the operation.
        /// @param prefilter `prefilter` value used by the operation.
        /// @param upsample `upsample` value used by the operation.
        /// @param bind_group `bind_group` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_bloom_draw(RHI::RenderPassEncoder &pass,
                                                              RHI::TextureViewHandle source_view,
                                                              glm::vec2 source_texel_size,
                                                              f32 threshold, f32 soft_knee, f32 scatter,
                                                              glm::vec2 filter_scale,
                                                              bool prefilter, bool upsample,
                                                              RHI::BindGroupHandle bind_group);
        /// Records bloom downsample using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param source_view `source_view` value used by the operation.
        /// @param source_texel_size Requested or available size for the operation.
        /// @param settings Configuration values controlling the operation.
        /// @param filter_scale `filter_scale` value used by the operation.
        /// @param apply_threshold `apply_threshold` value used by the operation.
        /// @param bind_group `bind_group` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_bloom_downsample(RHI::RenderPassEncoder &pass,
                                                                    RHI::TextureViewHandle source_view,
                                                                    glm::vec2 source_texel_size,
                                                                    const RenderGraphSettings &settings,
                                                                    glm::vec2 filter_scale,
                                                                    bool apply_threshold,
                                                                    RHI::BindGroupHandle bind_group);
        /// Records bloom upsample using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param source_view `source_view` value used by the operation.
        /// @param source_texel_size Requested or available size for the operation.
        /// @param settings Configuration values controlling the operation.
        /// @param bind_group `bind_group` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_bloom_upsample(RHI::RenderPassEncoder &pass,
                                                                  RHI::TextureViewHandle source_view,
                                                                  glm::vec2 source_texel_size,
                                                                  const RenderGraphSettings &settings,
                                                                  RHI::BindGroupHandle bind_group);
        /// Destroys the bloom resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_bloom_resources() noexcept;
        /// Destroys the bloom resources locked identified by the supplied parameters.
        ///
        /// @param resources `resources` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_bloom_resources_locked(BloomResources &resources) noexcept;


        /// Creates a bloom source bind group from the supplied parameters.
        ///
        /// @param source_view `source_view` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<RHI::BindGroupHandle> create_bloom_source_bind_group(
            RHI::TextureViewHandle source_view);


        /// Finds or creates the bloom composite resources required by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_bloom_composite_resources();
        /// Resolves the bloom composite pipeline associated with the supplied key, handle, or resource.
        ///
        /// @param color_format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> bloom_composite_pipeline_for(RHI::Format color_format);
        /// Records bloom composite using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param scene_view `scene_view` value used by the operation.
        /// @param bloom_view `bloom_view` value used by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param bloom_intensity `bloom_intensity` value used by the operation.
        /// @param threshold_enabled `threshold_enabled` value used by the operation.
        /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_bloom_composite(RHI::RenderPassEncoder &pass,
                                                                   RHI::TextureViewHandle scene_view,
                                                                   RHI::TextureViewHandle bloom_view,
                                                                   RHI::Format color_format,
                                                                   f32 bloom_intensity,
                                                                   bool threshold_enabled,
                                                                   vector<RHI::BindGroupHandle> &transient_bind_groups);
        /// Destroys the bloom composite resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_bloom_composite_resources() noexcept;
        /// Destroys the bloom composite resources locked identified by the supplied parameters.
        ///
        /// @param resources `resources` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_bloom_composite_resources_locked(BloomCompositeResources &resources) noexcept;

        /// Finds or creates the custom post process required by the operation.
        ///
        /// @param effect `effect` value used by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_custom_post_process(const CustomPostProcessEffect &effect,
                                                                      RHI::Format color_format);
        /// Records custom post process using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param source_view `source_view` value used by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param effect `effect` value used by the operation.
        /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_custom_post_process(RHI::RenderPassEncoder &pass,
                                                                      RHI::TextureViewHandle source_view,
                                                                      RHI::Format color_format,
                                                                      const CustomPostProcessEffect &effect,
                                                                      vector<RHI::BindGroupHandle> &transient_bind_groups);
        /// Destroys the custom post process resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_custom_post_process_resources() noexcept;

        /// Finds or creates the post process aa resources required by the operation.
        ///
        /// @param settings Configuration values controlling the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_post_process_aa_resources(
            const RenderGraphSettings &settings,
            RHI::Format color_format);
        /// Records post process aa using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param source_view `source_view` value used by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param settings Configuration values controlling the operation.
        /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_post_process_aa(
            RHI::RenderPassEncoder &pass,
            RHI::TextureViewHandle source_view,
            RHI::Format color_format,
            const RenderGraphSettings &settings,
            vector<RHI::BindGroupHandle> &transient_bind_groups);

        /// Finds or creates the spectral path tracing resources required by the operation.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_spectral_path_tracing_resources(SpectralRenderMode mode);
        /// Finds or creates the spectral mesh acceleration structures required by the operation.
        ///
        /// @param draws Draw descriptions processed in submission order.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_spectral_mesh_acceleration_structures(
            span<const RenderItem> draws);
        /// Prepares spectral scene acceleration structure for a later operation.
        ///
        /// @param encoder `encoder` value used by the operation.
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param submission `submission` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult prepare_spectral_scene_acceleration_structure(
            RHI::CommandEncoder &encoder, FrameInFlight &slot, const FrameSubmission &submission);
        /// Finds or creates the spectral accumulation target required by the operation.
        ///
        /// @param record `record` value used by the operation.
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_spectral_accumulation_target(
            WindowSurfaceRecord &record, Core::Extent2D extent);
        /// Destroys the spectral accumulation target identified by the supplied parameters.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_spectral_accumulation_target(WindowSurfaceRecord &record) noexcept;
        /// Finds or creates the frame spectral photon targets required by the operation.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param photon_capacity `photon_capacity` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_frame_spectral_photon_targets(
            FrameInFlight &slot, u32 photon_capacity);
        /// Destroys the frame spectral photon targets identified by the supplied parameters.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_frame_spectral_photon_targets(FrameInFlight &slot) noexcept;
        /// Prepares spectral photon mapping for a later operation.
        ///
        /// @param encoder `encoder` value used by the operation.
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param submission `submission` value used by the operation.
        /// @param emission_needed `emission_needed` value used by the operation.
        /// @param photon_signature `photon_signature` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult prepare_spectral_photon_mapping(
            RHI::CommandEncoder &encoder, FrameInFlight &slot, const FrameSubmission &submission,
            bool emission_needed, u64 photon_signature);
        /// Records spectral photon pass using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param submission `submission` value used by the operation.
        /// @param pipeline_index Zero-based index of the target element or entry.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_spectral_photon_pass(
            RHI::ComputePassEncoder &pass, FrameInFlight &slot, const FrameSubmission &submission,
            usize pipeline_index, const char *label);
        /// Records spectral photon emission using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param submission `submission` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_spectral_photon_emission(
            RHI::ComputePassEncoder &pass, FrameInFlight &slot, const FrameSubmission &submission);
        /// Records spectral photon hash using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param submission `submission` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_spectral_photon_hash(
            RHI::ComputePassEncoder &pass, FrameInFlight &slot, const FrameSubmission &submission);
        /// Records spectral integrator using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param submission `submission` value used by the operation.
        /// @param extent `extent` value used by the operation.
        /// @param views `views` value used by the operation.
        /// @param accumulation_reset `accumulation_reset` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_spectral_integrator(
            RHI::ComputePassEncoder &pass, FrameInFlight &slot, const FrameSubmission &submission,
            Core::Extent2D extent, const SpectralIntegratorViews &views, bool accumulation_reset);
        /// Records spectral depth commit using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param primary_depth_view `primary_depth_view` value used by the operation.
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_spectral_depth_commit(
            RHI::RenderPassEncoder &pass, FrameInFlight &slot, RHI::TextureViewHandle primary_depth_view,
            Core::Extent2D extent);
        /// Destroys the spectral path tracing resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_spectral_path_tracing_resources() noexcept;
        /// Destroys the spectral path tracing resources locked identified by the supplied parameters.
        ///
        /// @param resources `resources` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_spectral_path_tracing_resources_locked(SpectralPathTracingResources &resources) noexcept;

        /// Finds or creates the custom compute effect required by the operation.
        ///
        /// @param effect `effect` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_custom_compute_effect(const CustomComputeEffect &effect);
        /// Records custom compute effect using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param source_view `source_view` value used by the operation.
        /// @param output_view `output_view` value used by the operation.
        /// @param extent `extent` value used by the operation.
        /// @param effect `effect` value used by the operation.
        /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_custom_compute_effect(
            RHI::ComputePassEncoder &pass,
            RHI::TextureViewHandle source_view,
            RHI::TextureViewHandle output_view,
            Core::Extent2D extent,
            const CustomComputeEffect &effect,
            vector<RHI::BindGroupHandle> &transient_bind_groups);
        /// Destroys the custom compute effect resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_custom_compute_effect_resources() noexcept;


        /// Finds or creates the tonemap resources required by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_tonemap_resources();
        /// Resolves the tonemap pipeline associated with the supplied key, handle, or resource.
        ///
        /// @param color_format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> tonemap_pipeline_for(RHI::Format color_format);
        /// Records tonemap using the supplied arguments and current state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param source_view `source_view` value used by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param settings Configuration values controlling the operation.
        /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
        /// @param preserve_alpha `preserve_alpha` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult record_tonemap(RHI::RenderPassEncoder &pass,
                                                          RHI::TextureViewHandle source_view,
                                                          RHI::Format color_format,
                                                          const RenderGraphSettings &settings,
                                                          vector<RHI::BindGroupHandle> &transient_bind_groups,
                                                          bool preserve_alpha = false);
        /// Destroys the tonemap resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_tonemap_resources() noexcept;

        /// Destroys the tonemap resources locked identified by the supplied parameters.
        ///
        /// @param resources `resources` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_tonemap_resources_locked(TonemapResources &resources) noexcept;


        /// Finds or creates the text overlay resources required by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_text_overlay_resources();
        /// Prepares text overlay for a later operation.
        ///
        /// @param encoder `encoder` value used by the operation.
        /// @param lines `lines` value used by the operation.
        /// @param origin_px `origin_px` value used by the operation.
        /// @param viewport_size_px `viewport_size_px` value used by the operation.
        /// @param frame_resources `frame_resources` value used by the operation.
        /// @param transient_buffers Buffer used or affected by the operation.
        /// @param retired_atlas_resources `retired_atlas_resources` value used by the operation.
        /// @param out_batches `out_batches` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult prepare_text_overlay(RHI::CommandEncoder &encoder,
                                                                 span<const UString> lines,
                                                                 glm::vec2 origin_px,
                                                                 glm::vec2 viewport_size_px,
                                                                 TextFrameResources &frame_resources,
                                                                 vector<RHI::BufferHandle> &transient_buffers,
                                                                 TextAtlasRetiredResources &retired_atlas_resources,
                                                                 vector<TextDrawBatch> &out_batches);


        /// Draws text overlay using the current rendering state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param batches `batches` value used by the operation.
        /// @param viewport_size_px `viewport_size_px` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult draw_text_overlay(RHI::RenderPassEncoder &pass,
                                                              span<const TextDrawBatch> batches,
                                                              glm::vec2 viewport_size_px);
        /// Destroys the text overlay resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_text_overlay_resources() noexcept;
        /// Destroys the text overlay resources locked identified by the supplied parameters.
        ///
        /// @param resources `resources` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_text_overlay_resources_locked(TextOverlayResources &resources) noexcept;


        /// Returns the current or globally available recover from device loss value.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult recover_from_device_loss();
        /// Performs the rebuild backend from create info operation for `Renderer` using the supplied arguments.
        ///
        /// @param create_info Description of the resource or operation to perform.
        /// @param reason `reason` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult rebuild_backend_from_create_info(const Core::RendererCreateInfo &create_info,
                                                                            const char *reason);
        /// Returns the current or globally available restore GPU resources after recovery value.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult restore_gpu_resources_after_recovery();
        /// Performs the invalidate GPU resource handles no destroy operation for `Renderer` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void invalidate_gpu_resource_handles_no_destroy() noexcept;
        /// Performs the graphics error from RHI operation for `Renderer` using the supplied arguments.
        ///
        /// @param error Error value describing the failure.
        /// @param operation `operation` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Core::GraphicsBackendError graphics_error_from_rhi(const RHI::RhiError &error,
                                                                               const char *operation);

        unique_ptr<Core::EngineBackend> graphics_backend_;
        Core::RendererCreateInfo recovery_create_info_{};


        mutable Async::Mutex<vector<unique_ptr<WindowSurfaceRecord>>> window_surfaces_;
        mutable Async::Mutex<vector<OffscreenRenderTargetRecord>> offscreen_render_targets_;


        PresentationCoordinator graphics_presentation_coordinator_{"PresentationCoordinator-Graphics"};
        PresentationCoordinator compute_presentation_coordinator_{"PresentationCoordinator-Compute"};
        /// Resolves the presentation coordinator associated with the supplied key, handle, or resource.
        ///
        /// @param present_via_compute `present_via_compute` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] PresentationCoordinator &presentation_coordinator_for(bool present_via_compute) noexcept;
        Core::RendererCapabilities capabilities_{};


        struct GeometryArena {
            RHI::BufferHandle buffer{};
            RHI::BufferUsage usage = RHI::BufferUsage::None;
            u64 capacity_bytes = 0;
            u64 used_bytes = 0;
        };
        /// Grows geometry arena using the supplied arguments and current state.
        ///
        /// @param arena `arena` value used by the operation.
        /// @param required_bytes `required_bytes` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult grow_geometry_arena(GeometryArena &arena, u64 required_bytes,
                                                               const char *label);


        GeometryArena vertex_arena_{.usage = RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage |
                                             RHI::BufferUsage::TransferSrc | RHI::BufferUsage::TransferDst};
        GeometryArena index_arena_{.usage = RHI::BufferUsage::Index | RHI::BufferUsage::Storage |
                                            RHI::BufferUsage::TransferSrc | RHI::BufferUsage::TransferDst};
        vector<MeshResource> meshes_;
        vector<MaterialResource> materials_;
        vector<TextureResource> textures_;
        vector<MaterialTemplateResource> material_templates_;
        vector<MaterialInstanceResource> material_instances_;
        TextureHandle default_white_texture_{};


        vector<RenderItem> frame_draws_;


        std::shared_ptr<Core::Slang::ShaderWatcher> shader_watcher_;
        optional<Async::TaskHandle<ShaderHotReloadPollResult>> shader_hot_reload_poll_;
        steady_clock::time_point next_shader_hot_reload_poll_{};


        Async::Mutex<u8> shader_hot_reload_lock_;


        Async::Mutex<BloomResources> bloom_;
        Async::Mutex<BloomCompositeResources> bloom_composite_;
        Async::Mutex<ShadowLightingResources> shadow_lighting_;
        Async::Mutex<DeferredMsaaResources> deferred_msaa_;
        Async::Mutex<TonemapResources> tonemap_;
        Async::Mutex<TextOverlayResources> text_overlay_;


        Async::Mutex<std::unordered_map<u64, vector<MaterialPipelineVariant>>> material_pipeline_variants_;


        Async::Mutex<std::unordered_map<u64, vector<DepthOnlyPipelineVariant>>> depth_only_pipeline_variants_;
        Async::Mutex<vector<CustomPostProcessResources>> custom_post_process_resources_;
        Async::Mutex<vector<CustomComputeEffectResources>> custom_compute_effect_resources_;
        Async::Mutex<SpectralPathTracingResources> spectral_path_tracing_;


        Async::Mutex<std::unordered_map<u64, SpectralMaterialParameterCacheEntry>> spectral_material_parameter_cache_;
        Async::Mutex<InstanceCullResources> instance_cull_;


        Async::Mutex<std::unordered_map<u64, InstancedTemplateResources>> instanced_pipeline_variants_;
        Async::Mutex<ObjectHistoryResources> object_history_;


        Async::Mutex<std::unordered_map<u64, ObjectHistoryTemplateResources>> object_history_pipeline_variants_;


        Async::Mutex<u8> material_frame_prepare_lock_;


        Async::Mutex<u8> transient_bind_groups_lock_;


        Async::Mutex<std::unordered_map<u64, glm::mat4>> previous_world_transforms_;
        Async::Mutex<HiZBuildResources> hiz_build_;
        Async::Mutex<AtmosphereLutResources> atmosphere_lut_;


        Async::Mutex<u8> backend_operation_mutex_{0};
        bool initialized_ = false;
        bool recovering_from_device_loss_ = false;
    };

} // namespace SFT::Renderer
