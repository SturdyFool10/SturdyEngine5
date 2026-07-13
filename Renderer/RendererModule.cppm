module;

#pragma region Imports
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include <glm/mat4x4.hpp>
#pragma endregion

export module Sturdy.Renderer:Renderer;

import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.RHI;
import Sturdy.Platform;
import :Mesh;
import :Material;
import :Scene;
import :ReflectionBinding;
import :Resources;
import :RenderGraph;

using std::optional;
using std::span;
using std::string_view;
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

        // High-level scene/view entry point. Game/editor code submits a camera and renderable list; the
        // renderer validates handles and lowers it into the existing per-frame render-list path.
        [[nodiscard]] Core::RendererResult render_frame(const RenderFrameDesc &desc);

        // Queues one mesh/material pair for the next render_frame() call. This is the renderer-level draw
        // submission seam: higher layers stay out of RHI details, while the renderer can sort/batch these
        // into efficient backend work later.
        [[nodiscard]] Core::RendererResult submit_draw(MeshHandle mesh, MaterialInstanceHandle material);

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

        // Third escape-hatch tier, above `rhi_device()`: a typed RHI device-extension interface (see
        // RHI/Extensions.cppm), e.g. `Core::Vulkan::VulkanNativeAccessExtension` for raw Vulkan handles.
        // Returns nullptr if no device is up yet, the backend doesn't offer `Extension`, or the app
        // didn't request it (RendererFeatureRequest::enable_native_access_extension and friends) at
        // initialize()-time. Template body must stay inline here — every call site instantiates it.
        template <typename Extension>
        [[nodiscard]] Extension *native_extension() noexcept {
            RHI::RhiDevice *device = rhi_device();
            if (device == nullptr) {
                return nullptr;
            }
            return dynamic_cast<Extension *>(device->extension_interface(Extension::id()));
        }

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

        // Textures: upload tightly-packed pixel data into a GPU texture (+ a default view and sampler).
        // `data` must be `width * height * bytes_per_texel(format)` bytes, or empty to allocate an
        // uninitialized texture. Mirrors the mesh upload path (staged copy through the RHI).
        [[nodiscard]] Core::RendererExpected<TextureHandle> create_texture(u32 width, u32 height,
                                                                           RHI::Format format,
                                                                           span<const std::byte> data,
                                                                           const char *label = nullptr);
        void destroy_texture(TextureHandle handle) noexcept;
        [[nodiscard]] TextureResource *texture(TextureHandle handle) noexcept;
        [[nodiscard]] const TextureResource *texture(TextureHandle handle) const noexcept;

        // ── Material system (see :Material, plans/material-system.md) ──
        // Builds a reflection-derived template from a compiled shader: RHI bind-group/pipeline layouts,
        // the uniform block's byte layout, and named texture slots all come from the shader's reflection.
        [[nodiscard]] Core::RendererExpected<MaterialTemplateHandle> create_material_template(
            const Core::Slang::Shader &shader, const char *label = nullptr);

        // Like create_material_template, but keeps the `.slang` source and a per-permutation compile cache
        // (see :Material's variant_cache), so the template can compile shader *variants* (SKINNED, ...) on
        // demand and hot-reload from disk. If `source` is file-backed, the template becomes eligible for
        // poll_shader_hot_reload(). See plans/shader-variants-and-hot-reload.md.
        [[nodiscard]] Core::RendererExpected<MaterialTemplateHandle> create_material_template_from_source(
            const Core::Slang::ShaderSource &source,
            const Core::Slang::ShaderCompileOptions &options = {},
            const char *label = nullptr);

        // Recompiles a source-backed template from disk and swaps its GPU objects in place. If the new
        // shader's binding/parameter layout is unchanged, only the shader modules + pipelines are rebuilt
        // and existing instances keep their UBOs; if the layout changed, the whole template + its
        // instances' GPU state are rebuilt. A no-op for templates not created from source. Calls
        // wait_idle() first (the one sanctioned reload-time stall — see plans/async-submission-model.md).
        [[nodiscard]] Core::RendererResult reload_material_template(MaterialTemplateHandle handle);

        // Dev-time shader hot-reload driver: mtime-polls the `Shaders/` tree and reloads every source-
        // backed material template whose `.slang` file changed since the last call. Returns how many
        // templates were reloaded this tick (0 in the common no-edit case). Cheap to call once per frame.
        usize poll_shader_hot_reload();

        void destroy_material_template(MaterialTemplateHandle handle) noexcept;
        [[nodiscard]] MaterialTemplateResource *material_template(MaterialTemplateHandle handle) noexcept;
        [[nodiscard]] const MaterialTemplateResource *material_template(MaterialTemplateHandle handle) const noexcept;

        // Mints a drawable instance of a template with its parameters seeded to the shader's defaults and
        // every texture slot bound to the default white texture.
        [[nodiscard]] Core::RendererExpected<MaterialInstanceHandle> create_material_instance(
            MaterialTemplateHandle material_template, const char *label = nullptr);
        void destroy_material_instance(MaterialInstanceHandle handle) noexcept;
        [[nodiscard]] MaterialInstanceResource *material_instance(MaterialInstanceHandle handle) noexcept;
        [[nodiscard]] const MaterialInstanceResource *material_instance(MaterialInstanceHandle handle) const noexcept;

        // Writes raw bytes into a named parameter's slot in the instance's uniform block (bounds- and
        // size-checked against the reflected parameter). Marks the instance's UBO dirty for re-upload.
        [[nodiscard]] Core::RendererResult set_material_parameter(MaterialInstanceHandle handle,
                                                                  string_view name, span<const std::byte> value);
        // Typed convenience wrappers over set_material_parameter.
        [[nodiscard]] Core::RendererResult set_material_float(MaterialInstanceHandle handle, string_view name, f32 value);
        [[nodiscard]] Core::RendererResult set_material_vec4(MaterialInstanceHandle handle, string_view name,
                                                             f32 x, f32 y, f32 z, f32 w);
        // Binds a texture into a named slot; marks every frame's bind group for rebuild.
        [[nodiscard]] Core::RendererResult set_material_texture(MaterialInstanceHandle handle,
                                                                string_view slot, TextureHandle texture);

        void destroy_all_resources() noexcept;

      private:
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
            bool primary = false;
            bool rhi_swapchain_dirty = true;
        };

        struct RenderItem {
            MeshHandle mesh{};
            MaterialInstanceHandle material{};
            glm::mat4 world_transform{1.0f};
            u64 stable_id = 0;
            u32 sort_key = 0;
        };

        struct DebugSceneResources {
            MeshHandle mesh{};
            MaterialTemplateHandle material_template{};
            MaterialInstanceHandle material_instance{};
        };

        // GPU state for the fullscreen tonemap post-process pass: the compiled shader + modules, its
        // reflection-derived bind-group/pipeline layouts, a sampler for the scene texture, and a per-
        // swapchain-format render-pipeline cache. Built lazily on first use (ensure_tonemap_resources).
        struct TonemapPipelineVariant {
            RHI::Format color_format = RHI::Format::Undefined;
            RHI::RenderPipelineHandle pipeline{};
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

        // One in-flight frame's deferred-cleanup bundle (see plans/async-submission-model.md). The async
        // model records + submits a frame and moves on without waiting; the GPU resources that frame still
        // references — its command buffer, the render graph's transient targets, and any bind groups minted
        // while recording — can't be freed until that frame's fence retires. They live here until this ring
        // slot is reused max_frames_in_flight frames later, at which point its fence is guaranteed signaled.
        struct FrameInFlight {
            RHI::FenceHandle fence{};
            RHI::CommandBufferHandle command_buffer{};
            vector<RHI::TextureHandle> transient_textures;
            vector<RHI::TextureViewHandle> transient_texture_views;
            vector<RHI::BindGroupHandle> transient_bind_groups;
            bool submitted = false;
        };

        // The HDR intermediate the scene renders into before the tonemap pass resolves it to the
        // swapchain. RGBA16Float so lighting output can exceed [0,1] (the whole point of a tonemap step).
        static constexpr RHI::Format scene_color_format = RHI::Format::RGBA16Float;

        [[nodiscard]] WindowSurfaceRecord *window_surface(Core::RenderSurfaceHandle surface) noexcept;
        [[nodiscard]] const WindowSurfaceRecord *window_surface(Core::RenderSurfaceHandle surface) const noexcept;
        [[nodiscard]] Core::RendererResult ensure_rhi_presentation_resources(WindowSurfaceRecord &record);
        [[nodiscard]] Core::RendererResult recreate_rhi_swapchain(WindowSurfaceRecord &record);
        [[nodiscard]] Core::RendererResult ensure_rhi_depth_resources(WindowSurfaceRecord &record);
        [[nodiscard]] Core::RendererResult render_frame_rhi(WindowSurfaceRecord &record,
                                                            const Core::FrameInput &frame);
        // Destroys one in-flight frame slot's deferred GPU resources (command buffer, transient graph
        // targets, transient bind groups) but NOT its reusable fence. The caller must have already
        // ensured the slot's fence signaled — this only destroys, it never waits.
        void reclaim_frame_slot(FrameInFlight &slot) noexcept;
        // Waits for every submitted in-flight frame to finish, then reclaims its resources. The
        // sanctioned heavy wait for teardown / swapchain rebuild — NOT the per-frame path. Leaves each
        // slot's fence allocated but reset (unsignaled) so the ring is immediately reusable.
        void drain_frames_in_flight() noexcept;
        void destroy_rhi_presentation_resources(WindowSurfaceRecord &record) noexcept;
        [[nodiscard]] Core::RendererResult ensure_debug_scene_resources();
        void destroy_debug_scene_resources() noexcept;
        [[nodiscard]] Core::RendererResult record_render_item(RHI::RenderPassEncoder &pass,
                                                              const RenderItem &item,
                                                              RHI::Format color_format,
                                                              RHI::Format depth_format,
                                                              u64 frame_index);

        [[nodiscard]] Core::RendererResult try_upload_mesh(MeshResource &mesh);

        // ── Material/texture internals ──
        // Uploads tightly-packed pixel `data` into `resource`'s already-created RHI texture via a
        // staged buffer copy + layout transitions (one-shot command buffer, waits — the pre-frame-graph
        // upload path, same shape as the mesh staging copy).
        [[nodiscard]] Core::RendererResult upload_texture_rgba(TextureResource &resource, u32 width, u32 height,
                                                               RHI::Format format, span<const std::byte> data);
        // Lazily creates (once) a 1×1 opaque-white texture used to fill unbound material texture slots so
        // a material always has something valid to sample.
        [[nodiscard]] Core::RendererExpected<TextureHandle> ensure_default_white_texture();
        // Lazily builds + caches the render pipeline for one attachment configuration on a template.
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> material_pipeline_for(
            MaterialTemplateResource &material_template, RHI::Format color_format, RHI::Format depth_format);
        // Ensures instance frame slot `frame_slot`'s UBO reflects the CPU value block and its per-set
        // bind groups exist/are rebuilt, then returns the bind groups to bind (index == set order).
        [[nodiscard]] Core::RendererExpected<span<const RHI::BindGroupHandle>> prepare_material_frame(
            MaterialInstanceResource &instance, u32 frame_slot);
        // Fills a template's reflection-derived GPU objects (shader modules, bind-group/pipeline layouts,
        // uniform-block + parameter map, texture slots) from `shader`. Shared by create_material_template,
        // create_material_template_from_source, and reload_material_template. On failure it tears down any
        // objects it already created on `resource` and returns the error.
        [[nodiscard]] Core::RendererResult build_material_template_gpu(MaterialTemplateResource &resource,
                                                                       const Core::Slang::Shader &shader);
        // Seeds an instance's CPU value block from `tmpl`'s parameter defaults, binds its texture slots to
        // the default white texture, and creates its N per-frame UBOs. Shared by create_material_instance
        // and the layout-changed path of reload_material_template.
        [[nodiscard]] Core::RendererResult initialize_material_instance_state(MaterialInstanceResource &instance,
                                                                              MaterialTemplateResource &tmpl);
        void destroy_material_template_gpu(MaterialTemplateResource &resource) noexcept;
        void destroy_material_instance_gpu(MaterialInstanceResource &resource) noexcept;

        // ── Fullscreen tonemap post-process (see :RenderGraph, plans/deferred-pipeline.md) ──
        // Lazily compiles Shaders/fullscreen_tonemap.slang and builds its reflection-derived layouts +
        // sampler (once). Builds/caches the render pipeline for one swapchain color format. Records the
        // fullscreen draw sampling `source_view` into the currently-bound render pass; the bind group it
        // mints is stashed in frame_transient_bind_groups_ and freed after the frame fence retires.
        [[nodiscard]] Core::RendererResult ensure_tonemap_resources();
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> tonemap_pipeline_for(RHI::Format color_format);
        [[nodiscard]] Core::RendererResult record_tonemap(RHI::RenderPassEncoder &pass,
                                                          RHI::TextureViewHandle source_view,
                                                          RHI::Format color_format);
        void destroy_tonemap_resources() noexcept;

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
        vector<MaterialTemplateResource> material_templates_;
        vector<MaterialInstanceResource> material_instances_;
        TextureHandle default_white_texture_{};
        vector<RenderItem> frame_draws_;
        glm::mat4 frame_view_projection_{1.0f};
        // Lazily created on the first poll_shader_hot_reload() over the `Shaders/` tree; primed so the
        // first poll reports only edits made after the engine started.
        optional<Core::Slang::ShaderWatcher> shader_watcher_;
        TonemapResources tonemap_{};
        // Bind groups minted while recording the current frame (e.g. the tonemap pass's scene sampler),
        // destroyed once the frame fence retires — safe because the per-frame path waits on that fence.
        vector<RHI::BindGroupHandle> frame_transient_bind_groups_;
        // Ring of N = max_frames_in_flight deferred-cleanup slots; indexed by FrameInput::frame_index so
        // it stays in lockstep with the material system's per-frame UBO slotting (frame_index % N). See
        // plans/async-submission-model.md.
        vector<FrameInFlight> frames_in_flight_;
        DebugSceneResources debug_scene_{};
        bool initialized_ = false;
        bool recovering_from_device_loss_ = false;
        bool debug_fallback_enabled_ = true;
    };

} // namespace SFT::Renderer
