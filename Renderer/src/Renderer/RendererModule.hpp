#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <Async/Async.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Platform/Platform.hpp>
#include <Text/Text.hpp>
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
        void on_surface_resize_needed(Core::RenderSurfaceHandle surface) noexcept;
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
            RHI::TextureHandle scene_lighting{};
            RHI::TextureViewHandle scene_lighting_view{};
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
            vector<RHI::SwapchainHandle> retired_swapchains;
            vector<RHI::TextureHandle> retired_presentation_textures;
            vector<RHI::TextureViewHandle> retired_presentation_texture_views;
            FrameDeferredTargets deferred_targets{};
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

        // Fully call-local replacement for what used to be six Renderer-wide "current frame" member
        // fields (frame_draws_/frame_camera_/frame_view_projection_/frame_lighting_/deferred_formats_/
        // frame_transient_bind_groups_) — those raced directly when two windows rendered concurrently
        // (one clearing frame_draws_ while another's render graph was still reading it mid-recording).
        // Callers build one of these on the stack and thread it by reference through render_frame_rhi()
        // and everything it calls.
        struct FrameSubmission {
            vector<RenderItem> draws;
            glm::mat4 view_projection{1.0f};
            CameraView camera{};
            SceneLighting lighting{};
            DeferredTargetFormats deferred_formats{};
            vector<RHI::BindGroupHandle> transient_bind_groups;
            vector<RHI::BufferHandle> transient_buffers;
            string debug_label;
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

        struct DeferredLightingPipelineVariant {
            RHI::Format color_format = RHI::Format::Undefined;
            RHI::RenderPipelineHandle pipeline{};
        };
        struct DeferredLightingResources {
            Core::Slang::Shader shader;
            RHI::ShaderModuleHandle vertex_module{};
            RHI::ShaderModuleHandle fragment_module{};
            std::string vertex_entry_point;
            std::string fragment_entry_point;
            std::vector<RHI::BindGroupLayoutHandle> bind_group_layouts;
            std::vector<u32> bind_group_layout_sets;
            RHI::PipelineLayoutHandle pipeline_layout{};
            RHI::SamplerHandle sampler{};
            std::vector<DeferredLightingPipelineVariant> pipeline_variants;
            bool ready = false;
        };

        struct SceneFrameGpuResources {
            RHI::BufferHandle view_buffer{};
            RHI::BufferHandle object_buffer{};
            usize object_capacity = 0;
        };

        // Lazily-built resources for the debug HUD text overlay (scene label, camera, FPS, ...)
        // rendered each frame in render_frame_rhi(). Same lazy-build-once-and-cache pattern as
        // tonemap_/deferred_lighting_ above.
        struct TextOverlayResources {
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
        // Waits for every in-flight frame (of one window's ring) to finish, then reclaims its resources
        // (including retired swapchains/presentation textures — safe here specifically because of the
        // wait_idle, see reclaim_frame_slot's comment). The sanctioned heavy wait for teardown / periodic
        // retired-swapchain flush — NOT the per-frame path. Leaves each slot's fence allocated but reset
        // (unsignaled) so the ring is immediately reusable.
        void drain_frames_in_flight(WindowSurfaceRecord &record) noexcept;
        // Bounds how many superseded swapchains/presentation textures a resize can leave un-destroyed:
        // recreate_rhi_swapchain() can't safely destroy the swapchain it just superseded (its present
        // isn't fenced), so it retires it onto a frame-in-flight slot instead — fine as an occasional
        // thing, but during a fast continuous resize drag (recreating every frame, by design — see
        // render_frame_rhi) that queue would otherwise grow without bound for the drag's whole duration.
        // Called after every recreate; once the retired count crosses a small threshold it pays one
        // drain_frames_in_flight() to clear the backlog, then goes back to accumulating.
        void maybe_flush_retired_swapchains(WindowSurfaceRecord &record) noexcept;
        void destroy_rhi_presentation_resources(WindowSurfaceRecord &record) noexcept;
        [[nodiscard]] Core::RendererResult prepare_scene_gpu_data(u64 frame_index, const FrameSubmission &submission);
        void destroy_scene_gpu_resources() noexcept;
        // Lazy-build-once-and-cache, guarded by lazy_resource_mutex_ internally so concurrent first-use
        // from two windows' render calls can't double-build or corrupt the cache.
        [[nodiscard]] Core::RendererResult ensure_debug_scene_resources();
        void destroy_debug_scene_resources() noexcept;
        // Caller must already hold debug_scene_'s guard — used by ensure_debug_scene_resources() itself
        // (which holds it for its whole check-then-build body) to avoid double-locking a non-recursive
        // Async::Mutex.
        void destroy_debug_scene_resources_locked(DebugSceneResources &scene) noexcept;
        [[nodiscard]] Core::RendererResult record_render_item(RHI::RenderPassEncoder &pass,
                                                              const RenderItem &item,
                                                              span<const RHI::Format> color_formats,
                                                              RHI::Format depth_format,
                                                              u64 frame_index,
                                                              const glm::mat4 &view_projection);

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
            MaterialTemplateResource &material_template, span<const RHI::Format> color_formats, RHI::Format depth_format);
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

        // ── Deferred lighting and fullscreen tonemap post-processes ──
        // Both ensure_*/*_pipeline_for() pairs are lazy-build-once-and-cache, guarded by their own
        // Async::Mutex (deferred_lighting_/tonemap_) for the same reason as debug_scene_.
        [[nodiscard]] Core::RendererResult ensure_deferred_lighting_resources();
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> deferred_lighting_pipeline_for(RHI::Format color_format);
        [[nodiscard]] Core::RendererResult record_deferred_lighting(RHI::RenderPassEncoder &pass,
                                                                    RHI::TextureViewHandle albedo_view,
                                                                    RHI::TextureViewHandle normal_view,
                                                                    RHI::TextureViewHandle material_view,
                                                                    RHI::Format color_format,
                                                                    vector<RHI::BindGroupHandle> &transient_bind_groups);
        void destroy_deferred_lighting_resources() noexcept;
        // Caller must already hold deferred_lighting_'s guard — see destroy_debug_scene_resources_locked.
        void destroy_deferred_lighting_resources_locked(DeferredLightingResources &resources) noexcept;

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
                                                          vector<RHI::BindGroupHandle> &transient_bind_groups);
        void destroy_tonemap_resources() noexcept;
        // Caller must already hold tonemap_'s guard — see destroy_debug_scene_resources_locked.
        void destroy_tonemap_resources_locked(TonemapResources &resources) noexcept;

        // Debug HUD text overlay: lazily loads a default UI font + builds an atlas/pipeline, then
        // shapes+draws `lines` (top-to-bottom) starting at `origin_px`. Split across the render
        // graph boundary so glyph rasterization/upload and the instance buffer write — the only
        // GPU-command-recording parts — happen once, into the frame's own shared command encoder,
        // before any render pass begins (see RendererLifecycle.cpp's render_frame_rhi): no separate
        // submit+fence+wait, no mid-frame stall. Any buffer this call retires (a grown-out instance
        // buffer, an atlas staging buffer) is appended to `transient_buffers` for the caller's
        // frame-fence-gated cleanup, same contract as transient_bind_groups.
        [[nodiscard]] Core::RendererResult ensure_text_overlay_resources();
        [[nodiscard]] Core::RendererResult prepare_text_overlay(RHI::CommandEncoder &encoder,
                                                                 span<const string> lines,
                                                                 glm::vec2 origin_px,
                                                                 vector<RHI::BufferHandle> &transient_buffers,
                                                                 vector<TextDrawBatch> &out_batches);
        // Issues the instanced draws for a batch set already produced by prepare_text_overlay(),
        // against the currently-bound render pass.
        [[nodiscard]] Core::RendererResult draw_text_overlay(RHI::RenderPassEncoder &pass,
                                                              span<const TextDrawBatch> batches,
                                                              glm::vec2 viewport_size_px,
                                                              vector<RHI::BindGroupHandle> &transient_bind_groups);
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
        vector<MeshResource> meshes_;
        vector<MaterialResource> materials_;
        vector<TextureResource> textures_;
        vector<MaterialTemplateResource> material_templates_;
        vector<MaterialInstanceResource> material_instances_;
        TextureHandle default_white_texture_{};
        // Legacy accumulator for the public submit_draw() API + the plain render_frame(surface, frame)
        // fallback overload only — never touched by the RenderFrameDesc path (which uses a fully
        // call-local FrameSubmission instead) and, per today's Engine::render(), never exercised
        // concurrently with another window's render once the demo scene is up.
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
        Async::Mutex<DeferredLightingResources> deferred_lighting_;
        Async::Mutex<TonemapResources> tonemap_;
        Async::Mutex<TextOverlayResources> text_overlay_;
        // material_pipeline_for()'s per-template pipeline_variants cache can't use the same Async::Mutex<T>
        // pattern as above: MaterialTemplateResource lives by value inside vector<MaterialTemplateResource>
        // material_templates_, and Async::Mutex<T> deletes move/copy, which would make the whole resource
        // (and therefore the vector) non-movable. A plain mutex covering just this one cache is the
        // exception here, not the rule.
        std::mutex material_pipeline_mutex_;
        Async::Mutex<DebugSceneResources> debug_scene_;
        bool initialized_ = false;
        bool recovering_from_device_loss_ = false;
    };

} // namespace SFT::Renderer
