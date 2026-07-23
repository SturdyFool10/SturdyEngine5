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
#include "TileGrid.hpp"
#include "TextAtlas.hpp"
#include "TextInstance.hpp"

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
        void on_surface_resize_needed(Core::RenderSurfaceHandle surface, Core::Extent2D extent) noexcept;
        [[nodiscard]] Core::RendererResult set_presentation_settings(Core::RenderSurfaceHandle surface,
                                                                     const Core::PresentationSettings &settings);
        [[nodiscard]] Core::RendererResult reconfigure_backend(const Core::RendererCreateInfo &create_info);

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

        [[nodiscard]] const Core::RendererCapabilities &capabilities() const noexcept;
        [[nodiscard]] const RHI::FeatureNegotiationReport *feature_negotiation_report() const noexcept;
        [[nodiscard]] optional<Core::GpuInfo> gpu_info() const;

        // Low-level escape hatches. `graphics_backend()` gives backend-specific extension points via
        // dynamic_cast when needed; `rhi_device()` is the API-agnostic low-level RHI surface.
        [[nodiscard]] Core::EngineBackend *graphics_backend() noexcept;
        [[nodiscard]] const Core::EngineBackend *graphics_backend() const noexcept;
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

        // Wraps an already-created RHI texture (+ view + optional sampler) as a Renderer::TextureHandle
        // without uploading/owning it — the concrete mechanism behind "bind an off-screen render target
        // as a shader input" (see TextRenderTarget). destroy_texture() skips the RHI destroy calls for
        // adopted textures (TextureResource::owns_gpu_resources = false); the caller retains ownership.
        [[nodiscard]] Core::RendererExpected<TextureHandle> adopt_texture(RHI::TextureHandle texture,
                                                                           RHI::TextureViewHandle view,
                                                                           RHI::SamplerHandle sampler,
                                                                           const char *label = nullptr);

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

        // Dev-time shader hot-reload driver: periodically mtime-polls the `Shaders/` tree and reloads
        // every source-backed material template whose `.slang` file changed since the last scan. Returns
        // how many templates were reloaded this tick (0 in the common no-edit / throttled case).
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
        // One in-flight frame's deferred-cleanup bundle (see plans/async-submission-model.md). The async
        // model records + submits a frame and moves on without waiting; the GPU resources that frame still
        // references — its command buffer, the render graph's transient targets, and any bind groups minted
        // while recording — can't be freed until that frame's fence retires. They live here until this ring
        // slot is reused max_frames_in_flight frames later, at which point its fence is guaranteed signaled.
        struct FrameDeferredTargets {
            Core::Extent2D extent{};
            DeferredTargetFormats formats{};
            RHI::TextureHandle gbuffer_albedo{};
            RHI::TextureViewHandle gbuffer_albedo_view{};
            RHI::TextureHandle gbuffer_normal{};
            RHI::TextureViewHandle gbuffer_normal_view{};
            RHI::TextureHandle gbuffer_material{};
            RHI::TextureViewHandle gbuffer_material_view{};
            RHI::TextureHandle scene_color{};
            RHI::TextureViewHandle scene_color_view{};
            RHI::TextureHandle depth{};
            RHI::TextureViewHandle depth_view{};
        };

        static constexpr u32 max_directional_shadow_cascades = 4;
        static constexpr u32 max_lighting_spot_lights = 8;
        static constexpr u32 max_lighting_point_lights = 8;
        static constexpr u32 max_shadowed_point_lights = 4;
        static constexpr u32 max_shadow_views = max_directional_shadow_cascades +
                                                max_lighting_spot_lights +
                                                max_shadowed_point_lights * 6;

        // All GPU shadow/lighting structs contain only 16-byte-aligned vectors and matrices. Their
        // matching definitions live in Shaders/deferred_shadow_lighting.slang; static assertions in
        // RendererShadow.cpp guard the constant-buffer ABI against accidental packing drift.
        struct alignas(16) ShadowViewGpuData {
            glm::mat4 view_projection{1.0f};
            glm::vec4 atlas_scale_bias{}; // scale.xy, bias.xy
            glm::vec4 depth_params{};     // near, far, perspective(0/1), light radius in local UV
            // World-space tile span (orthographic) or span at unit depth (perspective), followed by
            // tile resolution. Used to express receiver bias in texels instead of arbitrary meters.
            glm::vec4 filter_params{};
        };

        struct alignas(16) DirectionalLightGpuData {
            glm::vec4 direction_angular_radius{};
            glm::vec4 radiance_shadow{};
            glm::vec4 cascade_splits{};
            glm::vec4 cascade_params{}; // cascade count, blend fraction, first view, unused
        };

        struct alignas(16) SpotLightGpuData {
            glm::vec4 position_range{};
            glm::vec4 direction_outer_cos{};
            glm::vec4 radiance_inner_cos{};
            glm::vec4 shadow_params{}; // view index (-1 = none), source radius, unused...
        };

        struct alignas(16) PointLightGpuData {
            glm::vec4 position_range{};
            glm::vec4 radiance_source_radius{};
            glm::vec4 shadow_params{}; // first cube-face view (-1 = none), unused...
        };

        struct alignas(16) ShadowLightingGpuData {
            glm::mat4 inverse_view_projection{1.0f};
            glm::mat4 view{1.0f};
            glm::vec4 camera_position_near{};
            glm::vec4 ambient_radiance_exposure{};
            glm::vec4 background_color{};
            glm::vec4 counts{};       // spot lights, point lights, shadow views, shadows enabled
            glm::vec4 shadow_params{}; // atlas texel, normal bias, PCSS enabled, max distance
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

        struct FrameBloomTargets {
            Core::Extent2D source_extent{};
            u32 requested_levels = 0;
            RHI::TextureViewHandle scene_source_view{};
            vector<Core::Extent2D> extents;
            RHI::TextureHandle texture{};
            vector<RHI::TextureViewHandle> views;
            vector<RHI::BindGroupHandle> downsample_bind_groups;
            vector<RHI::BindGroupHandle> upsample_bind_groups;
        };

        // The bloom-composite output (see record_bloom_composite) is the same logical resource every
        // frame bloom is active — same extent, same format — so like FrameDeferredTargets/
        // FrameBloomTargets it's a persistent, resize-on-demand allocation rather than a
        // graph.create_texture() the render graph would otherwise mint (and the RHI backend behind it
        // would allocate a fresh VkImage/VkImageView for) fresh every single frame for no reason: the
        // graph itself is rebuilt every frame, but the GPU resource backing this particular slot
        // doesn't need to be.
        struct FrameCompositeTarget {
            Core::Extent2D extent{};
            RHI::Format format = RHI::Format::Undefined;
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
        };

        // Per-ring-slot GPU per-pass timing (debug-overlay only — see render_frame_rhi's debug_overlay
        // gate). `query_set` holds 2 Timestamp slots per RenderGraph pass (begin+end); `pending` is
        // the label/slot-index list RenderGraph::execute() just filled for the frame this slot is
        // about to submit. `has_pending_results` is set once that submission happens and cleared once
        // read back — which only ever happens the NEXT time this same ring slot is reused, right
        // after waiting on its fence (the earliest point the GPU is guaranteed to have written every
        // timestamp from that prior submission).
        struct FrameGpuTimingTarget {
            RHI::QuerySetHandle query_set{};
            u32 capacity = 0;
            vector<RenderGraph::GpuPassTiming> pending;
            bool has_pending_results = false;
        };

        // Per-ring-slot CPU timing, mirroring FrameGpuTimingTarget above but with no query
        // set/fence delay to wait on — `pass_timings`/`stage_timings` are both ready the instant
        // render_frame_rhi finishes recording this slot's frame. Still surfaced one frame stale
        // like the GPU numbers, purely because the debug-overlay text for frame N is built before
        // frame N's own RenderGraph::execute() call runs (see render_frame_rhi).
        // `pass_timings`: one entry per RenderGraph pass, wall-clock CPU cost of recording it
        // (RenderGraph::CpuPassTiming). `stage_timings`: coarser top-of-render_frame_rhi stages
        // (ScopedRendererStageTimer's accumulate_into) plus whatever the caller staged into
        // FrameSubmission::pre_dispatch_stage_timings_ms before render_frame_rhi ever started.
        struct FrameCpuTimingTarget {
            vector<RenderGraph::CpuPassTiming> pass_timings;
            vector<std::pair<string, f64>> stage_timings;
            bool has_pending_results = false;
        };

        struct FrameInFlight {
            RHI::FenceHandle fence{};
            RHI::CommandBufferHandle command_buffer{};
            vector<RHI::TextureHandle> transient_textures;
            vector<RHI::TextureViewHandle> transient_texture_views;
            vector<RHI::BindGroupHandle> transient_bind_groups;
            // Buffers retired mid-frame (e.g. a text-atlas staging buffer, or an instance buffer
            // outgrown and replaced) that a just-submitted command buffer may still reference —
            // freed here once this ring slot's fence proves the GPU is done with them, same
            // fire-and-forget contract as transient_textures/transient_bind_groups above.
            vector<RHI::BufferHandle> transient_buffers;
            // Reused after this slot's fence retires; unlike transient_buffers/groups these are
            // not recreated or destroyed every frame.
            TextFrameResources text_overlay_resources{};
            vector<RHI::SwapchainHandle> retired_swapchains;
            vector<RHI::TextureHandle> retired_presentation_textures;
            vector<RHI::TextureViewHandle> retired_presentation_texture_views;
            FrameDeferredTargets deferred_targets{};
            FrameShadowTargets shadow_targets{};
            FrameBloomTargets bloom_targets{};
            FrameCompositeTarget composite_target{};
            FrameGpuTimingTarget gpu_timing{};
            FrameCpuTimingTarget cpu_timing{};
            bool submitted = false;
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
            bool primary = false;
            bool rhi_swapchain_dirty = true;
            // Ring of N = desired_frames_in_flight (well, capabilities_.max_frames_in_flight — see
            // render_frame_rhi) deferred-cleanup slots, one per window: each window has its own swapchain
            // and therefore its own frame-in-flight lifetime, so this can never be a Renderer-wide
            // member if two windows are to render concurrently without racing on each other's fences.
            vector<FrameInFlight> frames_in_flight;
        };

        struct RenderItem {
            MeshHandle mesh{};
            MaterialInstanceHandle material{};
            glm::mat4 world_transform{1.0f};
            u64 stable_id = 0;
            u32 sort_key = 0;
        };

        // Tracks what record_render_item last bound within one render pass so a run of draws sharing
        // (material, mesh) — the order render_frame_dispatch sorts submission.draws into — can skip
        // rebinding a pipeline/bind-group/vertex-buffer that's already current. Default-constructed
        // (all-zero handles) at the top of each pass; every field is invalid before the first draw, so
        // the first item in a pass always binds everything regardless.
        struct RenderItemBindingState {
            RHI::RenderPipelineHandle pipeline{};
            MaterialInstanceHandle material{};
            u32 material_frame_slot = ~0u;
            // Every mesh shares one vertex/index arena buffer (see Renderer::vertex_arena_/
            // index_arena_), so the buffer binding itself only needs to happen once per pass, not
            // per-mesh — this just tracks whether that first bind has happened yet.
            bool arena_bound = false;
        };

        // Fully call-local replacement for what used to be six Renderer-wide "current frame" member
        // fields (frame_draws_/frame_camera_/frame_view_projection_/frame_lighting_/deferred_formats_/
        // frame_transient_bind_groups_) — those raced directly when two windows rendered concurrently
        // (one clearing frame_draws_ while another's render graph was still reading it mid-recording).
        // Callers build one of these on the stack and thread it by reference through render_frame_rhi()
        // and everything it calls.
        struct FrameSubmission {
            vector<RenderItem> draws;
            // Light-position debug gizmos — recorded in their own forward pass (record_render_item
            // with a single color target), never fed through the Z-prepass/G-buffer passes.
            vector<RenderItem> gizmo_draws;
            glm::mat4 view_projection{1.0f};
            CameraView camera{};
            SceneLighting lighting{};
            DeferredTargetFormats deferred_formats{};
            RenderGraphSettings render_graph{};
            vector<RHI::BindGroupHandle> transient_bind_groups;
            vector<RHI::BufferHandle> transient_buffers;
            TextAtlasRetiredResources retired_text_atlas_resources;
            UString debug_label;
            // CPU stage timings the caller (render_frame/render_frame_dispatch) measured before
            // render_frame_rhi even started — extraction from SceneRenderable into `draws`, then
            // sorting them by (material, mesh). Folded into the same per-slot debug-overlay report
            // as render_frame_rhi's own internal stage timings and RenderGraph's per-pass CPU
            // timings, so the overlay shows the full CPU picture, not just the RHI-facing half.
            vector<std::pair<string, f64>> pre_dispatch_stage_timings_ms;
        };

        // GPU state for the fullscreen tonemap post-process pass: the compiled shader + modules, its
        // reflection-derived bind-group/pipeline layouts, a sampler for the scene texture, and a per-
        // swapchain-format render-pipeline cache. Built lazily on first use (ensure_tonemap_resources).
        struct TonemapPipelineVariant {
            RHI::Format color_format = RHI::Format::Undefined;
            RHI::RenderPipelineHandle pipeline{};
        };
        // Layout mirrors VkDrawIndexedIndirectCommand field-for-field — see Shaders/
        // gpu_instance_cull.slang's matching struct and record_instanced_batches's doc comment
        // (RendererGpuCulling.cpp). CPU-written each frame with instance_count left at 0; the cull
        // compute shader atomically increments it per surviving instance.
        struct GpuDrawIndexedIndirectCommand {
            u32 index_count = 0;
            u32 instance_count = 0;
            u32 first_index = 0;
            i32 vertex_offset = 0;
            u32 first_instance = 0;
        };

        // One contiguous run of a sorted RenderItem list sharing (mesh, material), large enough to
        // be worth a GPU-culled instanced indirect draw instead of N separate CPU-recorded ones —
        // see Renderer::detect_instanced_batches (RendererGpuCulling.cpp).
        struct InstancedBatch {
            MeshHandle mesh{};
            MaterialInstanceHandle material{};
            // Index into the sorted RenderItem list this batch was detected from, and (since
            // prepare_scene_gpu_data uploads SceneObjectGpuData in that same order) into this
            // frame's object buffer too.
            u32 first_object_index = 0;
            u32 instance_count = 0;
        };

        // Lazily-built resources for the GPU-driven instanced-batch cull compute pass (Shaders/
        // gpu_instance_cull.slang) and the instanced vertex stage it feeds (Shaders/
        // gbuffer_geometry_instanced.slang) — see instanced_pipeline_for's doc comment for why the
        // latter's bind-group layout (instance_data_bind_group_layout, set 1) is hand-built here
        // rather than derived from a material template's own reflection.
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

        // One material template's instanced-draw pipeline, keyed by (color formats, depth format)
        // like MaterialPipelineVariant. `pipeline_layout` combines the template's own reflected set
        // 0 (reused as-is) with InstanceCullResources::instance_data_bind_group_layout at set 1 —
        // built once per template, cached alongside its pipelines here rather than rebuilt per
        // variant.
        struct InstancedPipelineVariant {
            vector<RHI::Format> color_formats;
            RHI::Format depth_format = RHI::Format::Undefined;
            RHI::RenderPipelineHandle pipeline{};
        };
        struct InstancedTemplateResources {
            RHI::PipelineLayoutHandle pipeline_layout{};
            vector<InstancedPipelineVariant> pipeline_variants;
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

        // GPU state for the explicit bloom-composite pass (scene HDR + resolved bloom mip chain -> one
        // scene-linear HDR result). Two sampled textures + one sampler in a single reflected bind
        // group, one render pipeline per color format — same shape as TonemapResources used to have
        // before bloom compositing moved out of the tonemap shader into its own pass.
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
            u32 set = 0;
            u32 image_binding = 0;
            u32 sampler_binding = 0;
        };

        struct SceneFrameGpuResources {
            RHI::BufferHandle view_buffer{};
            RHI::BufferHandle object_buffer{};
            usize object_capacity = 0;
            // GPU-driven instanced-batch draw path (Renderer::record_instanced_batches,
            // RendererGpuCulling.cpp) — one GpuDrawIndexedIndirectCommand per detected batch, and a
            // shared compacted-instance-index buffer every batch writes its own region of. Both
            // resized (never shrunk within a frame) to this frame's batch count / total candidate
            // instance count; see ensure_instance_cull_frame_resources.
            RHI::BufferHandle indirect_commands_buffer{};
            usize indirect_commands_capacity = 0;
            RHI::BufferHandle compacted_indices_buffer{};
            usize compacted_indices_capacity = 0;
        };

        // Lazily-built resources for the debug HUD text overlay (scene label, camera, FPS, ...)
        // rendered each frame in render_frame_rhi(). Same lazy-build-once-and-cache pattern as
        // the other fullscreen resources above.
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
            // Optional: best-effort emoji fallback (Noto Color Emoji), used via
            // Text::shape_with_fallback when present. `has_emoji_font` is false (not just
            // `emoji_font.valid()`) so a load failure degrades to primary-font-only text instead of
            // failing the whole overlay — see find_default_emoji_font_path()'s caller.
            Text::Font emoji_font;
            bool has_emoji_font = false;
            TextAtlas atlas;
            TextPipeline pipeline;
            u64 font_id = 0;
            u64 emoji_font_id = 0;
            // Keyed by glyph_id alone, so only ever populated for the primary font's glyphs — glyph
            // indices are font-local and would collide against the emoji font's, but emoji glyphs
            // never need an extracted outline (Text::rasterize_color_glyph rasterizes straight from
            // the font), so they never populate or look up this cache.
            std::unordered_map<u32, Text::GlyphOutline> outline_cache;
            // Large documents are virtualized to visible lines. Each line is shaped at most once
            // per source change, while the final visible instance list is reused wholesale until
            // the viewport or visible text changes.
            usize first_cached_line = 0;
            vector<CachedLine> line_cache;
            CachedVisibleLayout visible_layout;
            bool ready = false;
        };

        struct ShaderHotReloadPollResult {
            std::shared_ptr<Core::Slang::ShaderWatcher> watcher;
            vector<Core::Slang::ShaderChange> changes;
        };

        // Briefly locks window_surfaces_ to find the record matching `surface`, then returns the (stable,
        // heap-allocated) raw pointer with the lock released. Matches the same "caller-owns-lifetime,
        // lock only protects the container's own structure" contract VulkanRhiResourcePool documents.
        [[nodiscard]] WindowSurfaceRecord *window_surface(Core::RenderSurfaceHandle surface) noexcept;
        [[nodiscard]] const WindowSurfaceRecord *window_surface(Core::RenderSurfaceHandle surface) const noexcept;
        [[nodiscard]] Core::RendererResult ensure_rhi_presentation_resources(WindowSurfaceRecord &record);
        // `known_extent`, when given, skips querying the Window directly — render_frame_rhi's per-frame
        // hot path already has a fresh extent from FrameInput and must never touch the Window itself
        // (see render_frame_rhi's own comment on this). Left unset, this queries the window directly,
        // which is only ever exercised from the cold-start path (ensure_rhi_presentation_resources during
        // create_window_surface/initialize(), single-threaded, before any concurrent rendering begins).
        [[nodiscard]] Core::RendererResult recreate_rhi_swapchain(WindowSurfaceRecord &record, u64 frame_index = 0,
                                                                   optional<Core::Extent2D> known_extent = std::nullopt);
        [[nodiscard]] Core::RendererResult ensure_rhi_depth_resources(WindowSurfaceRecord &record);
        // Looks up `surface`, calls render_frame_rhi(), and on a DeviceLost error runs the recover-then-
        // retry-once sequence, re-resolving the record afterward (recovery may have rebuilt it).
        [[nodiscard]] Core::RendererResult render_frame_dispatch(Core::RenderSurfaceHandle surface,
                                                                  const Core::FrameInput &frame,
                                                                  FrameSubmission &submission);
        [[nodiscard]] Core::RendererResult render_frame_rhi(WindowSurfaceRecord &record,
                                                            const Core::FrameInput &frame,
                                                            FrameSubmission &submission);
        // Destroys one in-flight frame slot's deferred GPU resources (command buffer, transient graph
        // targets, transient bind groups) but NOT its reusable fence. The caller must have already
        // ensured the slot's fence signaled — this only destroys, it never waits.
        void reclaim_frame_slot(FrameInFlight &slot, bool destroy_retired_presentation = false) noexcept;
        [[nodiscard]] Core::RendererResult ensure_frame_deferred_targets(FrameInFlight &slot,
                                                                         Core::Extent2D extent,
                                                                         const DeferredTargetFormats &formats);
        void destroy_frame_deferred_targets(FrameInFlight &slot) noexcept;
        [[nodiscard]] Core::RendererResult ensure_frame_shadow_targets(FrameInFlight &slot, u32 atlas_size);
        void destroy_frame_shadow_targets(FrameInFlight &slot) noexcept;
        [[nodiscard]] Core::RendererResult prepare_shadow_frame(const FrameSubmission &submission,
                                                                 FrameShadowTargets &targets,
                                                                 PreparedShadowFrame &prepared);
        [[nodiscard]] Core::RendererResult ensure_frame_bloom_targets(FrameInFlight &slot,
                                                                      Core::Extent2D extent,
                                                                      u32 requested_levels);
        void destroy_frame_bloom_targets(FrameInFlight &slot) noexcept;
        [[nodiscard]] Core::RendererResult ensure_frame_composite_target(FrameInFlight &slot,
                                                                         Core::Extent2D extent,
                                                                         RHI::Format format);
        void destroy_frame_composite_target(FrameInFlight &slot) noexcept;

        // Grows (never shrinks) `slot.gpu_timing.query_set` to at least `2 * required_pass_count`
        // slots — a RenderGraph's pass count is data-dependent (for example, on bloom levels), so
        // this resizes on demand like every other frame target here rather than
        // assuming a fixed upper bound. Destroys and recreates (losing any not-yet-read-back pending
        // results) only when growing; existing capacity is always reused for a same-or-smaller frame.
        [[nodiscard]] Core::RendererResult ensure_frame_gpu_timing_target(FrameInFlight &slot, u32 required_pass_count);
        void destroy_frame_gpu_timing_target(FrameInFlight &slot) noexcept;
        // Waits for every in-flight frame (of one window's ring) to finish, then reclaims its resources
        // (including retired swapchains/presentation textures — safe here specifically because of the
        // wait_idle, see reclaim_frame_slot's comment). The sanctioned heavy wait for teardown / periodic
        // retired-swapchain flush — NOT the per-frame path. Leaves each slot's fence allocated but reset
        // (unsignaled) so the ring is immediately reusable.
        void drain_frames_in_flight(WindowSurfaceRecord &record) noexcept;
        // Cleans up superseded swapchains/presentation textures that recreate_rhi_swapchain() couldn't
        // safely destroy immediately (its present isn't fenced, so it retires them onto a
        // frame-in-flight slot instead — see reclaim_frame_slot's comment). `opportunistic` (call this
        // on any frame that ISN'T itself recreating the swapchain) flushes as soon as there's any
        // backlog at all — a live swapchain is dead weight the WSI carries on every acquire/present
        // until it's gone, and a non-resizing frame has no live-resize responsiveness to protect, so
        // there's no reason to wait. `!opportunistic` (call this from inside an active resize) only
        // trips a small bounded safety-net threshold, so a fast continuous resize drag — which
        // recreates every frame, by design, see render_frame_rhi — doesn't pay a wait_idle() on every
        // single one of those frames; it still won't grow the backlog without bound for a drag that
        // runs long enough to never hit a non-resizing frame.
        void maybe_flush_retired_swapchains(WindowSurfaceRecord &record, bool opportunistic) noexcept;
        void destroy_rhi_presentation_resources(WindowSurfaceRecord &record) noexcept;
        [[nodiscard]] Core::RendererResult prepare_scene_gpu_data(u64 frame_index, const FrameSubmission &submission);
        void destroy_scene_gpu_resources() noexcept;
        // `depth_only`: skip the material's color pipeline/attachments entirely and draw with its
        // depth-only variant instead (see depth_only_pipeline_for's doc comment) — used by the Z
        // prepass that runs before "deferred gbuffer geometry" to eliminate occluded-fragment shading
        // cost. Material bind groups are still bound either way: the depth-only fragment (when the
        // template has one) needs base_color_texture + alpha_cutoff to alpha-test correctly.
        // `standard_depth_test`: only meaningful when depth_only is false — see
        // material_pipeline_for's doc comment. Defaulted so every existing (Z-prepass-backed) caller
        // is unaffected; a forward-rendered draw with no prepass of its own (e.g. debug gizmos) must
        // pass true or its fragments will fail material_pipeline_for's default Equal-depth-test almost
        // universally.
        // `binding_state`: carries the previous call's bound pipeline/mesh/material within the same
        // render pass so repeated draws that share state (after the caller sorts submission.draws by
        // (material, mesh) — see render_frame_dispatch) skip redundant set_pipeline/set_bind_group/
        // set_vertex_buffer/set_index_buffer calls instead of reissuing them every single draw.
        // CPU frustum cull: true if `item`'s world-space bounding sphere (its mesh's object-space
        // bounds, transformed by world_transform — see MeshResource::bounds_center/bounds_radius)
        // intersects `frustum`. An unknown mesh conservatively returns true so record_render_item's
        // own lookup produces the real error instead of this silently skipping it.
        [[nodiscard]] bool render_item_visible(const RenderItem &item, const Frustum &frustum) noexcept;

        // Templated on encoder type so the same recording logic works against both
        // RHI::RenderPassEncoder (the primary, serial path) and RHI::RenderBundleEncoder (the
        // per-thread secondary path used by record_render_items_culled) — the two share an
        // identical draw/bind/push-constant surface (RHI/Command.hpp) but no common base class.
        // Defined in RendererLifecycle.cpp; every instantiation is used from within that same
        // translation unit, so the definition doesn't need to live in this header.
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
                                                               f32 shadow_slope_bias = 0.0f);

        // Frustum-culls `items` against `frustum` (render_item_visible), then records survivors
        // into `pass`. Below kParallelRecordThreshold survivors, or with no worker threads
        // available, this is just today's single serial loop. Above the threshold, survivors are
        // split into contiguous chunks (preserving the caller's (material, mesh) sort-coherence
        // within each chunk) and recorded concurrently, each chunk into its own
        // RHI::RenderBundleEncoder via Async::Scheduler::spawn — the exact chunking pattern
        // prepare_scene_gpu_data already uses for object-buffer packing (RendererScene.cpp) — then
        // stitched into `pass` with one execute_bundles call. `bundle_label` names the bundles for
        // any GPU-side debug tooling.
        //
        // Materials are pre-warmed (prepare_material_frame called once per distinct material, on
        // this thread, before any worker task starts) specifically so concurrent per-chunk calls to
        // record_render_item never race on the same MaterialInstanceFrame's bind-group rebuild —
        // prepare_material_frame only mutates state when a frame's dirty flags are set, and warming
        // them here first means every worker thread's later call is a pure read.
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
                                                                       bool shadow_map = false,
                                                                       f32 shadow_depth_bias = 0.0f,
                                                                       f32 shadow_slope_bias = 0.0f);

        // ── GPU-driven instanced batch draws (RendererGpuCulling.cpp) ──
        // Scans `sorted_draws` (already sorted by (material, mesh) — see render_frame_dispatch) for
        // contiguous same-(mesh, material) runs at or above the minimum batch size and returns one
        // InstancedBatch per run found. Callers route a batch's instances through
        // record_instanced_batches instead of the individual record_render_items_culled path.
        [[nodiscard]] vector<InstancedBatch> detect_instanced_batches(span<const RenderItem> sorted_draws) const;

        [[nodiscard]] Core::RendererResult ensure_instance_cull_resources();
        // Analogous to prepare_scene_gpu_data: called once per frame, before the render graph is
        // declared. (Re)allocates `resources`' indirect-command/compacted-index buffers if this
        // frame's batches need more room than last frame's, then writes every batch's
        // GpuDrawIndexedIndirectCommand (index_count/first_index/vertex_offset from its mesh,
        // instance_count left at 0 for the cull compute shader to fill in).
        [[nodiscard]] Core::RendererResult prepare_instance_cull_gpu_data(span<const InstancedBatch> batches,
                                                                          SceneFrameGpuResources &resources);
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> instanced_pipeline_for(
            MaterialTemplateResource &material_template, span<const RHI::Format> color_formats, RHI::Format depth_format);

        // Records one compute dispatch per batch (frustum cull + compaction) into `pass`, writing
        // into `resources`' indirect-command/compacted-index buffers — see
        // Shaders/gpu_instance_cull.slang's header comment for the buffer protocol. Caller must
        // insert a compute-write -> indirect-draw-read barrier (RenderGraph's compute-pass builder
        // only tracks texture hazards) before any of `record_instanced_batches`'s draws run.
        [[nodiscard]] Core::RendererResult record_instance_cull(RHI::ComputePassEncoder &pass,
                                                                span<const InstancedBatch> batches,
                                                                const glm::mat4 &view_projection,
                                                                SceneFrameGpuResources &resources,
                                                                vector<RHI::BindGroupHandle> &transient_bind_groups);

        // Records one draw_indexed_indirect per batch into `pass`, consuming the buffers
        // record_instance_cull wrote (after the caller's barrier). Every batch shares the material
        // template's existing per-instance bind group (set 0, from prepare_material_frame — the
        // material system is completely unaware batching exists) plus one instance-data bind group
        // (set 1) bound once per batch with a dynamic offset into the shared compacted-indices
        // buffer.
        [[nodiscard]] Core::RendererResult record_instanced_batches(RHI::RenderPassEncoder &pass,
                                                                    span<const InstancedBatch> batches,
                                                                    span<const RHI::Format> color_formats,
                                                                    RHI::Format depth_format,
                                                                    u64 frame_index,
                                                                    const glm::mat4 &view_projection,
                                                                    SceneFrameGpuResources &resources,
                                                                    vector<RHI::BindGroupHandle> &transient_bind_groups);

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
        // By default (`standard_depth_test = false`) assumes a prior Z prepass already wrote the
        // definitive depth for this frame (depth_compare == Equal, depth_write_enable == false) —
        // true for the "deferred gbuffer geometry" pass, which always runs after "z prepass". Pass
        // `standard_depth_test = true` for a forward-rendered draw with no Z-prepass of its own (e.g.
        // debug gizmos) — a standard Less-compare, depth-writing test against whatever's already in
        // the depth buffer, instead of an Equal test that would reject nearly every fragment.
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> material_pipeline_for(
            MaterialTemplateResource &material_template, span<const RHI::Format> color_formats, RHI::Format depth_format,
            bool standard_depth_test = false);
        // Lazily builds + caches a template's depth-only pipeline: same vertex stage + (if the
        // template's shader declared one) the depth-only fragment entry for alpha-tested cutout, zero
        // color attachments, real depth write (depth_compare == Less) — this is the pipeline the Z
        // prepass itself draws with.
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> depth_only_pipeline_for(
            MaterialTemplateResource &material_template, RHI::Format depth_format,
            bool shadow_map = false, f32 depth_bias = 0.0f, f32 slope_bias = 0.0f);

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

        // ── Deferred lighting + raster shadows ──
        [[nodiscard]] Core::RendererResult ensure_shadow_lighting_resources();
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> shadow_lighting_pipeline_for(
            RHI::Format color_format);
        [[nodiscard]] Core::RendererResult record_shadow_lighting(
            RHI::RenderPassEncoder &pass,
            RHI::TextureViewHandle albedo_view,
            RHI::TextureViewHandle normal_view,
            RHI::TextureViewHandle material_view,
            RHI::TextureViewHandle depth_view,
            RHI::TextureViewHandle shadow_atlas_view,
            RHI::BufferHandle lighting_buffer,
            RHI::Format color_format,
            vector<RHI::BindGroupHandle> &transient_bind_groups);
        void destroy_shadow_lighting_resources() noexcept;
        void destroy_shadow_lighting_resources_locked(ShadowLightingResources &resources) noexcept;

        // ── Fullscreen post-processes ──
        [[nodiscard]] Core::RendererResult ensure_bloom_resources(RHI::Format color_format);
        [[nodiscard]] Core::RendererResult record_bloom_draw(RHI::RenderPassEncoder &pass,
                                                              RHI::TextureViewHandle source_view,
                                                              glm::vec2 source_texel_size,
                                                              f32 threshold, f32 soft_knee, f32 scatter,
                                                              bool prefilter, bool upsample,
                                                              RHI::BindGroupHandle bind_group);
        [[nodiscard]] Core::RendererResult record_bloom_downsample(RHI::RenderPassEncoder &pass,
                                                                    RHI::TextureViewHandle source_view,
                                                                    glm::vec2 source_texel_size,
                                                                    const RenderGraphSettings &settings,
                                                                    bool apply_threshold,
                                                                    RHI::BindGroupHandle bind_group);
        [[nodiscard]] Core::RendererResult record_bloom_upsample(RHI::RenderPassEncoder &pass,
                                                                  RHI::TextureViewHandle source_view,
                                                                  glm::vec2 source_texel_size,
                                                                  const RenderGraphSettings &settings,
                                                                  RHI::BindGroupHandle bind_group);
        void destroy_bloom_resources() noexcept;
        void destroy_bloom_resources_locked(BloomResources &resources) noexcept;

        // Mints a one-off (source texture + bloom sampler) bind group against bloom_'s cached sampled
        // layout. Used for bloom's level-0 downsample, whose source is the (possibly custom-effect-
        // produced, therefore per-frame-transient) BeforeBloom result rather than a stable persistent
        // view — so unlike every other bloom mip's bind group, it cannot be cached in FrameBloomTargets
        // and must be created fresh per frame and retired with that frame (transient_bind_groups).
        [[nodiscard]] Core::RendererExpected<RHI::BindGroupHandle> create_bloom_source_bind_group(
            RHI::TextureViewHandle source_view);

        // Explicit HDR bloom composite: scene HDR + resolved bloom mip chain -> one scene-linear HDR
        // result, so AfterBloomBeforeToneMap custom effects and tonemapping both see a single plain
        // texture instead of bloom being folded into the tonemap shader.
        [[nodiscard]] Core::RendererResult ensure_bloom_composite_resources();
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> bloom_composite_pipeline_for(RHI::Format color_format);
        [[nodiscard]] Core::RendererResult record_bloom_composite(RHI::RenderPassEncoder &pass,
                                                                   RHI::TextureViewHandle scene_view,
                                                                   RHI::TextureViewHandle bloom_view,
                                                                   RHI::Format color_format,
                                                                   f32 bloom_intensity,
                                                                   vector<RHI::BindGroupHandle> &transient_bind_groups);
        void destroy_bloom_composite_resources() noexcept;
        void destroy_bloom_composite_resources_locked(BloomCompositeResources &resources) noexcept;

        [[nodiscard]] Core::RendererResult ensure_custom_post_process(const CustomPostProcessEffect &effect,
                                                                      RHI::Format color_format);
        [[nodiscard]] Core::RendererResult record_custom_post_process(RHI::RenderPassEncoder &pass,
                                                                      RHI::TextureViewHandle source_view,
                                                                      RHI::Format color_format,
                                                                      const CustomPostProcessEffect &effect,
                                                                      vector<RHI::BindGroupHandle> &transient_bind_groups);
        void destroy_custom_post_process_resources() noexcept;

        // Lazily compiles Shaders/fullscreen_tonemap.slang and builds its reflection-derived layouts +
        // sampler (once). Builds/caches the render pipeline for one swapchain color format. Records the
        // fullscreen draw sampling `source_view` into the currently-bound render pass; the bind group it
        // mints is appended to `transient_bind_groups` (the caller's FrameSubmission) and freed after the
        // frame fence retires.
        [[nodiscard]] Core::RendererResult ensure_tonemap_resources();
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> tonemap_pipeline_for(RHI::Format color_format);
        [[nodiscard]] Core::RendererResult record_tonemap(RHI::RenderPassEncoder &pass,
                                                          RHI::TextureViewHandle source_view,
                                                          RHI::Format color_format,
                                                          const RenderGraphSettings &settings,
                                                          vector<RHI::BindGroupHandle> &transient_bind_groups);
        void destroy_tonemap_resources() noexcept;
        // Caller must already hold tonemap_'s guard.
        void destroy_tonemap_resources_locked(TonemapResources &resources) noexcept;

        // Debug HUD text overlay: lazily loads a default UI font + builds an atlas/pipeline, then
        // shapes+draws `lines` (top-to-bottom) starting at `origin_px`. Split across the render
        // graph boundary so glyph rasterization/upload and the instance buffer write — the only
        // GPU-command-recording parts — happen once, into the frame's own shared command encoder,
        // before any render pass begins (see RendererLifecycle.cpp's render_frame_rhi): no separate
        // submit+fence+wait, no mid-frame stall. Atlas staging buffers are appended to
        // `transient_buffers` for frame-fence-gated cleanup; instance buffers and their bind groups
        // instead live in the reusable per-frame `TextFrameResources` slot and only grow or rebuild
        // when their capacity or atlas image view changes.
        [[nodiscard]] Core::RendererResult ensure_text_overlay_resources();
        [[nodiscard]] Core::RendererResult prepare_text_overlay(RHI::CommandEncoder &encoder,
                                                                 span<const UString> lines,
                                                                 glm::vec2 origin_px,
                                                                 glm::vec2 viewport_size_px,
                                                                 TextFrameResources &frame_resources,
                                                                 vector<RHI::BufferHandle> &transient_buffers,
                                                                 TextAtlasRetiredResources &retired_atlas_resources,
                                                                 vector<TextDrawBatch> &out_batches);
        // Issues the instanced draws for a batch set already produced by prepare_text_overlay(),
        // against the currently-bound render pass.
        [[nodiscard]] Core::RendererResult draw_text_overlay(RHI::RenderPassEncoder &pass,
                                                              span<const TextDrawBatch> batches,
                                                              glm::vec2 viewport_size_px);
        void destroy_text_overlay_resources() noexcept;
        void destroy_text_overlay_resources_locked(TextOverlayResources &resources) noexcept;

        // Rebuilds the whole backend + every window surface's presentation resources. Holds the
        // window_surfaces_ Async::Mutex for the structural parts of the rebuild; recovering_from_device_loss_
        // guards against re-entrant recovery calls. A window's render racing a concurrent recovery rebuild
        // of its own record's fields is an accepted, documented scope boundary (recovery is the rare/
        // exceptional path) — same stance as VulkanRhiResourcePool's "destroying a resource still
        // referenced by in-flight work is caller error, not something the lock needs to catch".
        [[nodiscard]] Core::RendererResult recover_from_device_loss();
        [[nodiscard]] Core::RendererResult rebuild_backend_from_create_info(const Core::RendererCreateInfo &create_info,
                                                                            const char *reason);
        [[nodiscard]] Core::RendererResult restore_gpu_resources_after_recovery();
        void invalidate_gpu_resource_handles_no_destroy() noexcept;
        [[nodiscard]] static Core::GraphicsBackendError graphics_error_from_rhi(const RHI::RhiError &error,
                                                                               const char *operation);

        unique_ptr<Core::EngineBackend> graphics_backend_;
        Core::RendererCreateInfo recovery_create_info_{};
        // Async::Mutex<T> physically hides the vector behind lock() — every accessor gets a MutexGuard,
        // so there's no way to touch window_surfaces_ without holding the lock (unlike a plain vector +
        // separately-declared mutex, which relies on every call site remembering to lock it). unique_ptr
        // elements so a WindowSurfaceRecord's address stays stable across push_back/erase — a render call
        // only needs the lock for the brief lookup, then keeps using the (stable) pointer unlocked.
        mutable Async::Mutex<vector<unique_ptr<WindowSurfaceRecord>>> window_surfaces_;
        Core::RendererCapabilities capabilities_{};
        // A single growable GPU buffer that mesh uploads sub-allocate append-only ranges from, instead
        // of each Mesh owning its own dedicated VkBuffer — see try_upload_mesh/grow_geometry_arena.
        // Growth (doubling) re-uploads every already-resident mesh's retained CPU-side vertices/indices
        // at their existing (stable) offsets into the new, bigger buffer; this only happens during
        // asset loading, never mid-frame, so its O(resident data) cost is a non-issue.
        struct GeometryArena {
            RHI::BufferHandle buffer{};
            RHI::BufferUsage usage = RHI::BufferUsage::None;
            u64 capacity_bytes = 0;
            u64 used_bytes = 0;
        };
        [[nodiscard]] Core::RendererResult grow_geometry_arena(GeometryArena &arena, u64 required_bytes,
                                                               const char *label);
        GeometryArena vertex_arena_{.usage = RHI::BufferUsage::Vertex | RHI::BufferUsage::TransferDst};
        GeometryArena index_arena_{.usage = RHI::BufferUsage::Index | RHI::BufferUsage::TransferDst};
        vector<MeshResource> meshes_;
        vector<MaterialResource> materials_;
        vector<TextureResource> textures_;
        vector<MaterialTemplateResource> material_templates_;
        vector<MaterialInstanceResource> material_instances_;
        TextureHandle default_white_texture_{};
        // Legacy accumulator for the public submit_draw() API + the plain render_frame(surface, frame)
        // fallback overload only — never touched by the RenderFrameDesc path (which uses a fully
        // call-local FrameSubmission instead). An empty accumulator now intentionally renders no
        // geometry; content is always supplied by an API consumer.
        vector<RenderItem> frame_draws_;
        vector<SceneFrameGpuResources> scene_frame_resources_;
        // Lazily created by the first async poll over the `Shaders/` tree; primed so the first poll
        // reports only edits made after the engine started. Polling is throttled and runs on Async workers
        // because the watcher recursively stats the shader tree and project roots can live on slow filesystems.
        std::shared_ptr<Core::Slang::ShaderWatcher> shader_watcher_;
        optional<Async::TaskHandle<ShaderHotReloadPollResult>> shader_hot_reload_poll_;
        steady_clock::time_point next_shader_hot_reload_poll_{};
        // Each lazy-build-once-and-cache resource gets its own Async::Mutex, same rationale as
        // window_surfaces_ above — ensure_*()/​*_pipeline_for() hold the guard for their whole
        // check-then-build body, so concurrent first-use from two windows' render calls can't double-build
        // or corrupt the cache. Each is fast once warm, so this only ever serializes the rare cold-start/
        // new-variant path, never per-frame recording or submission.
        Async::Mutex<BloomResources> bloom_;
        Async::Mutex<BloomCompositeResources> bloom_composite_;
        Async::Mutex<ShadowLightingResources> shadow_lighting_;
        Async::Mutex<TonemapResources> tonemap_;
        Async::Mutex<TextOverlayResources> text_overlay_;
        // material_pipeline_for()'s per-template pipeline cache, keyed by MaterialTemplateHandle::value.
        // Not stored inline on MaterialTemplateResource: that struct lives by value inside
        // vector<MaterialTemplateResource> material_templates_, and an Async::Mutex<T> member would
        // make it (and therefore that vector) non-movable. Keeping the cache here, external to the
        // resource, sidesteps that entirely while still using the same Async::Mutex<T> pattern as
        // every other lazy cache above instead of a bare std::mutex.
        Async::Mutex<std::unordered_map<u64, vector<MaterialPipelineVariant>>> material_pipeline_variants_;
        // depth_only_pipeline_for()'s per-template cache, same rationale/shape as
        // material_pipeline_variants_ above (keyed by MaterialTemplateHandle::value).
        Async::Mutex<std::unordered_map<u64, vector<DepthOnlyPipelineVariant>>> depth_only_pipeline_variants_;
        Async::Mutex<vector<CustomPostProcessResources>> custom_post_process_resources_;
        Async::Mutex<InstanceCullResources> instance_cull_;
        // instanced_pipeline_for()'s per-template cache, same rationale/shape as
        // material_pipeline_variants_ above (keyed by MaterialTemplateHandle::value).
        Async::Mutex<std::unordered_map<u64, InstancedTemplateResources>> instanced_pipeline_variants_;
        bool initialized_ = false;
        bool recovering_from_device_loss_ = false;
    };

} // namespace SFT::Renderer
