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

        // Queries the real HDR capability of whichever display `surface`'s window currently sits
        // on — see RHI::RhiDevice::query_hdr_capabilities's own doc comment. Each window has its own
        // WindowSurfaceRecord/RHI::SurfaceHandle, so this is correctly per-window: a multi-window app
        // with windows on different monitors gets a different, independently accurate answer for each.
        [[nodiscard]] RHI::RhiExpected<RHI::SurfaceHdrCapabilityQuery> query_hdr_capabilities(
            Core::RenderSurfaceHandle surface) const;

        // See RHI::HdrContentLightLevelUpdate's own doc comment (RHI/HdrDisplay.hpp) for exactly
        // what this is (a manual, caller-supplied per-scene metadata refresh) and is not (real
        // HDR10+/ST 2094-40 — this engine doesn't analyze scene luminance itself). No-ops usefully
        // (returns Unsupported) on a surface not currently presenting Hdr10St2084.
        [[nodiscard]] RHI::RhiResult update_hdr_content_light_level(
            Core::RenderSurfaceHandle surface, const RHI::HdrContentLightLevelUpdate &update);

        // Requested-vs-effective presentation state for this surface's live swapchain — lets a caller
        // (e.g. Application's adaptive frame pacing) tell a vsync-paced window (Fifo/FifoRelaxed/
        // FifoLatestReady) from an uncapped one (Mailbox/Immediate) without reaching into RHI directly.
        // Default-constructed (TearFreeOrdered/Fifo) when this surface has no swapchain yet (first
        // frame not rendered) — the conservative "assume vsync-paced" answer.
        [[nodiscard]] RHI::PresentationResolution presentation_resolution(Core::RenderSurfaceHandle surface) const noexcept;

        // Last completed frame's CPU/GPU pass timing breakdown for `surface` (see
        // FrameTimingSnapshot's own doc comment, Scene.hpp) — populated whenever that surface's
        // render graph runs with RenderGraphSettings::debug_overlay enabled, regardless of whether
        // draw_overlay_text is also set. Default-constructed (has_data = false) when `surface` is
        // unregistered or no debug_overlay frame has completed yet.
        [[nodiscard]] FrameTimingSnapshot last_frame_timings(Core::RenderSurfaceHandle surface) const noexcept;

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
        // descriptors. The renderer owns both the GPU allocation and an authoritative CPU replay copy,
        // allowing all renderer-managed geometry to survive a complete backend/device reconstruction.
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
        // `data` must contain `mip_levels` levels ordered largest-to-smallest, each tightly packed using
        // the format's texel/block rules and with each level start padded to its copy-offset alignment
        // (see texture_mip_chain_bytes in RendererTextures.cpp), or be empty to allocate an uninitialized
        // texture. Mirrors the mesh upload path (staged copy through
        // the RHI). The default remains one level for existing procedural/data-texture callers.
        // `concurrent_queue_classes`: forwarded to RHI::TextureDesc::concurrent_queue_classes (see its
        // own doc comment) -- empty (the default) keeps today's VK_SHARING_MODE_EXCLUSIVE behavior for
        // every existing caller. A streaming caller that will submit its pixel upload from the
        // Transfer queue while this texture is bound/sampled via Graphics passes
        // {QueueClass::Graphics, QueueClass::Transfer} here.
        [[nodiscard]] Core::RendererExpected<TextureHandle> create_texture(u32 width, u32 height,
                                                                           RHI::Format format,
                                                                           span<const std::byte> data,
                                                                           const char *label = nullptr,
                                                                           span<const RHI::QueueClass> concurrent_queue_classes = {},
                                                                           u32 mip_levels = 1);
        void destroy_texture(TextureHandle handle) noexcept;
        [[nodiscard]] TextureResource *texture(TextureHandle handle) noexcept;
        [[nodiscard]] const TextureResource *texture(TextureHandle handle) const noexcept;

        // ── Asynchronous texture streaming support (Engine::TextureStreamer) ──
        // `create_texture(..., data = {}, ...)` above already mints an image/view/sampler without
        // uploading anything (see its own doc comment: "empty to allocate an uninitialized texture"),
        // which is what a streaming caller uses to get an immediately bindable handle before the real
        // pixel data has arrived. The two methods below are the rest of that story:
        //
        // clear_placeholder_texture: fills a just-created, still-image-only texture with a solid color
        // (Undefined→TransferDst→ClearColorImage→ShaderReadOnly, one-shot, waits — the whole thing is
        // small enough that synchronous is fine and keeps a streamed texture from ever exposing
        // uninitialized memory to a shader in the (short) window before its real pixels land).
        [[nodiscard]] Core::RendererResult clear_placeholder_texture(TextureHandle handle, RHI::ClearColor color);
        // submit_texture_upload: records + submits (WITHOUT waiting) the same barrier/copy/barrier
        // sequence upload_texture_rgba uses internally, against an already-created texture and a
        // caller-owned, caller-written staging buffer/offset. The caller (Engine::TextureStreamer)
        // polls/waits on the returned submission's fence itself and owns cleanup of both the fence and
        // command buffer once confirmed signaled; the staging buffer's lifetime is the caller's own
        // responsibility (a ring-owned chunk, not a fresh allocation).
        [[nodiscard]] Core::RendererExpected<TextureUploadSubmission> submit_texture_upload(
            TextureResource &resource, u32 width, u32 height, RHI::Format format,
            RHI::BufferHandle staging, u64 staging_offset = 0, RHI::QueueLane queue = {});

        [[nodiscard]] Core::RendererExpected<OffscreenRenderTargetHandle> create_offscreen_render_target(
            const OffscreenRenderTargetDescription &description);
        void destroy_offscreen_render_target(OffscreenRenderTargetHandle handle) noexcept;
        [[nodiscard]] optional<OffscreenRenderTargetDescription> offscreen_render_target_description(
            OffscreenRenderTargetHandle handle) const;
        [[nodiscard]] TextureHandle offscreen_render_target_texture(OffscreenRenderTargetHandle handle) const noexcept;

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
            RHI::SampleCount samples = RHI::SampleCount::X1;
            RHI::TextureHandle gbuffer_albedo{};
            RHI::TextureViewHandle gbuffer_albedo_view{};
            RHI::TextureHandle gbuffer_normal{};
            RHI::TextureViewHandle gbuffer_normal_view{};
            RHI::TextureHandle gbuffer_material{};
            RHI::TextureViewHandle gbuffer_material_view{};
            RHI::TextureHandle gbuffer_emissive{};
            RHI::TextureViewHandle gbuffer_emissive_view{};
            // Screen-space motion vector — written by the deferred gbuffer geometry pass's
            // fragmentMain (see DeferredTargetFormats::motion's doc comment), same as the other
            // gbuffer_* targets above.
            RHI::TextureHandle motion{};
            RHI::TextureViewHandle motion_view{};
            RHI::TextureHandle scene_color{};
            RHI::TextureViewHandle scene_color_view{};
            RHI::TextureHandle depth{};
            RHI::TextureViewHandle depth_view{};
            // The only multisampled deferred target. SRAA reconstructs subpixel shading from this
            // high-frequency visibility buffer while all expensive material/lighting buffers stay 1x.
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
            glm::vec4 gtao_params{};    // radius, falloff start, thin-feature thickness, intensity
            glm::vec4 viewport_params{}; // inverse extent xy, projection Y scale, quality+1 (0=off)
            glm::vec4 spectral_params{}; // x = SpectralRenderMode numeric value, remaining reserved
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

        // GPU constant-buffer mirror of Shaders/sturdy_atmosphere.slang's AtmosphereGpuData —
        // field-for-field identical, pinned by static_assert in RendererAtmosphere.cpp (same
        // convention ShadowLightingGpuData already follows). Packed into vec4s (never a bare vec3)
        // so this struct's layout matches the Slang side byte-for-byte with no implicit padding.
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

        // Per-FrameInFlight-slot GPU state for the atmosphere constant buffer — deliberately just a
        // buffer, unlike FrameShadowTargets: the transmittance/multi-scattering/sky-view LUT textures
        // themselves are ordinary graph.create_texture() transients (baked and consumed entirely
        // within one frame, see Renderer::record_atmosphere_lut_bakes), not persistent per-slot
        // resources, since they have no history/cross-frame dependency to preserve.
        struct FrameAtmosphereTargets {
            RHI::BufferHandle constants_buffer{};
        };

        // Fractionally-scaled bloom levels cannot use hardware mips (which are fixed at powers of two),
        // so each level owns one independently-sized image. The total RG11B10 footprint remains bounded
        // by a geometric series and each per-frame slot reuses these allocations until extent/settings change.
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

        // A single, Renderer-owned (not per-FrameInFlight-ring-slot) Hi-Z pyramid — deliberately not
        // shaped like FrameBloomTargets/FrameDeferredTargets above, which are per-slot: this needs to
        // hold *last completed frame's* data specifically (a real "history buffer", one frame stale),
        // not whichever ring slot happens to be reused this frame (which could be N frames stale with
        // N desired_frames_in_flight). Rebuilt every frame (Renderer::record_hiz_build,
        // RendererHiZ.cpp) from that frame's own just-finished resolved depth, for the *next* frame's
        // Renderer::record_instance_cull occlusion test to read — see gpu_instance_cull.slang's
        // occlusion test and this frame's "gpu instance cull" pass ordering (RendererLifecycle.cpp)
        // for why it can't be same-frame data.
        struct HiZPyramidTargets {
            // The pyramid texture's OWN mip-0 (base level) extent — half the real resolved depth
            // extent, not equal to it, since the reduction shader halves its source even for mip 0
            // (see Renderer::ensure_hiz_pyramid's doc comment, RendererHiZ.cpp). This is what
            // gpu_instance_cull.slang's `hiZExtent` push constant carries and indexes texels with.
            Core::Extent2D extent{};
            u32 mip_levels = 0;
            RHI::TextureHandle texture{};
            // One single-mip view per level (reduceMain's destination attachment for that level, and
            // — for every level but the last — reduceMain's `source` input for the next level up).
            vector<RHI::TextureViewHandle> mip_views;
            // Every level in one view, for gpu_instance_cull.slang's `hiZPyramid.Load(int3(xy, mip))`.
            RHI::TextureViewHandle full_view{};
            // False until this frame's build pass has actually run once (freshly created, or just
            // resized/format-changed) — gates the occlusion test off (frustum-only that frame) rather
            // than reading stale-content-that-was-never-written. See InstanceCullConstants::
            // cameraPositionHiZValid.w in gpu_instance_cull.slang.
            bool has_valid_data = false;
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

        struct FrameSpectralPhotonTargets {
            RHI::BufferHandle photons{};
            RHI::BufferHandle valid_count{};
            RHI::BufferHandle hash_heads{};
            u32 photon_capacity = 0;
            u32 hash_capacity = 0;
            // Whether this slot's buffers currently hold a real emitted photon map. Lets
            // prepare_spectral_photon_mapping/record_spectral_integrator skip re-emitting the caustic
            // photon map (a full 262144-photon, up-to-8-bounce ray-traced pass) on frames where the
            // spectral accumulation buffer isn't resetting anyway — a static/converged camera has no
            // need to pay that cost every single frame. Reset to false whenever the buffers themselves
            // are (re)allocated, see destroy_frame_spectral_photon_targets.
            bool populated = false;
            // FNV-1a signature of the geometry/sun/photon-settings state this slot's photon map was
            // last emitted from — deliberately excludes the camera's view-projection matrix (the
            // caustic map is view-independent), unlike SpectralAccumulationTarget::state_signature
            // which includes it. See render_frame_rhi's spectral_photon_signature computation.
            u64 state_signature = 0;
        };

        // Presentation resources cannot be reclaimed after the graphics submission fence alone:
        // vkQueuePresentKHR may still reference the old swapchain. On backends with a presentation
        // completion fence, the fence list tracks precisely that remaining ownership; unsupported
        // backends use the conservative wait_idle() path in maybe_flush_retired_swapchains().
        struct RetiredPresentationResources {
            RHI::SwapchainHandle swapchain{};
            RHI::TextureHandle depth_texture{};
            RHI::TextureViewHandle depth_view{};
            vector<RHI::FenceHandle> completion_fences;
        };

        struct FrameInFlight {
            RHI::FenceHandle fence{};
            // One entry per command buffer execute_parallel() finished this frame (the primary encoder
            // plus one per render-graph pass level, or a single entry when the graph was small enough
            // to stay on execute_parallel's serial fallback) — see render_frame_rhi.
            vector<RHI::CommandBufferHandle> command_buffers;
            vector<RHI::TextureHandle> transient_textures;
            vector<RHI::TextureViewHandle> transient_texture_views;
            vector<RHI::BindGroupHandle> transient_bind_groups;
            // See FrameSubmission::transient_render_bundles' own doc comment for why these can't be
            // destroyed synchronously right after execute_bundles() — moved here (from
            // FrameSubmission) at the end of render_frame_rhi, destroyed by reclaim_frame_slot once
            // this slot's fence signals, same as every other transient_* field here.
            vector<RHI::RenderBundleHandle> transient_render_bundles;
            // Buffers retired mid-frame (e.g. a text-atlas staging buffer, or an instance buffer
            // outgrown and replaced) that a just-submitted command buffer may still reference —
            // freed here once this ring slot's fence proves the GPU is done with them, same
            // fire-and-forget contract as transient_textures/transient_bind_groups above.
            vector<RHI::BufferHandle> transient_buffers;
            vector<RHI::AccelerationStructureHandle> transient_acceleration_structures;
            // Reused after this slot's fence retires; unlike transient_buffers/groups these are
            // not recreated or destroyed every frame.
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
            // Fixed 4-slot (2 begin/end pairs) query set for GPU work recorded onto the frame's raw
            // encoder before the RenderGraph exists (TLAS build, photon hash-buffer clear) — it can't
            // ride RenderGraph::execute_parallel's own per-pass query allocation since that isn't sized
            // until the graph is fully declared and compiled, much later in the same frame. Deliberately
            // NOT part of FrameGpuTimingTarget: that target's query_set is destroyed+recreated whenever
            // required_pass_count grows past its capacity (see ensure_frame_gpu_timing_target), which
            // would tear down this handle mid-frame while already-recorded timestamp writes referencing
            // it are still pending submission. Fixed size, created once, never resized.
            RHI::QuerySetHandle pregraph_gpu_timing_query_set{};
            vector<RenderGraph::GpuPassTiming> pregraph_gpu_timing_pending;
            RHI::AccelerationStructureHandle spectral_tlas{};
            RHI::BufferHandle spectral_scene_instances{};
            RHI::BufferHandle spectral_materials{};
            // Frame-local descriptor heap source used by every spectral camera/photon material query.
            // Renderer texture handles remain authoritative; bind groups resolve their live views/samplers.
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
            // Last per-window SDR reference white used by the direct UI display transform, in nits.
            // Zero means no platform value has been observed/logged yet.
            f32 ui_reference_white_nits = 0.0f;
            bool primary = false;
            bool rhi_swapchain_dirty = true;
            // Completion fences for presents already issued from the current swapchain. Only retained
            // while unsignaled; on recreation they move with the old swapchain into
            // retired_presentation_resources so a maintenance-capable backend can reclaim precisely
            // that generation without a device-wide wait.
            vector<RHI::FenceHandle> active_presentation_completion_fences;
            optional<RHI::FenceHandle> pending_present_completion_fence;
            vector<RetiredPresentationResources> retired_presentation_resources;
            // RHI::RhiDevice::present() (ultimately vkQueuePresentKHR) is issued through Renderer's
            // shared PresentationCoordinator (presentation_coordinator_for()), not this window's own
            // render thread. On Windows the driver commonly blocks the calling thread inside that
            // call until the frame's GPU work actually finishes -- present-mode independent, not
            // just vsync/FIFO pacing (see render_frame_rhi's present-issue site) -- so handing it off
            // lets the render thread move straight on to recording the next frame rather than idling
            // through that wait, while also giving every window sharing one native queue a single,
            // ordered point of issuance instead of N independently-threaded ones racing
            // VulkanQueue::submission_lock_ with no ordering guarantee between them.
            //
            // The previous frame's outstanding present, if any. At most one is ever in flight:
            // render_frame_rhi always drains this (see its swapchain-recreate section) before reading
            // rhi_swapchain_dirty or letting the swapchain be recreated/destroyed -- destroying a
            // swapchain still referenced by an unexecuted vkQueuePresentKHR call is
            // VUID-vkDestroySwapchainKHR-swapchain-01282. destroy_rhi_presentation_resources() also
            // drains this before tearing the window down.
            optional<Async::TaskHandle<RHI::RhiExpected<RHI::PresentOutcome>>> pending_present;
            // Queue-mutex wait time recorded by the last drained pending_present's device->present()
            // call, surfaced into the *next* frame's stage timings the same one-frame-stale way
            // GPU/CPU pass timings already are (FrameGpuTimingTarget's doc comment) -- there is no
            // earlier point at which the async present's own timings are known. Written only by the
            // presentation coordinator thread, read only after pending_present->wait() returns
            // (TaskState's done-flag release/acquire orders the two), so this never needs its own lock.
            f64 last_present_lock_wait_ms = 0.0;
            SpectralAccumulationTarget spectral_accumulation{};
            // Ring of N = desired_frames_in_flight (well, capabilities_.max_frames_in_flight — see
            // render_frame_rhi) deferred-cleanup slots, one per window: each window has its own swapchain
            // and therefore its own frame-in-flight lifetime, so this can never be a Renderer-wide
            // member if two windows are to render concurrently without racing on each other's fences.
            vector<FrameInFlight> frames_in_flight;
            // Same per-window rationale as frames_in_flight immediately above: keyed by
            // frame_index % frame_count, and each window keeps its own independent frame_index, so a
            // Renderer-wide vector here would alias two windows onto the same slot's view_buffer/
            // object_buffer — real cross-window content corruption (not just a race) once more than one
            // window renders real scene content, since a shared slot's buffers could be mid-use by one
            // window's still-recording command buffer while another window's frame overwrites them.
            // Real bug found and fixed alongside giving every window its own render thread — see
            // memory project_multi_window_render_threading.
            vector<SceneFrameGpuResources> scene_frame_resources;
            // Reused frame-to-frame instead of a fresh stack-local RenderGraph per render_frame_rhi()
            // call. reset() retains the outer pass/resource container capacities; pass-builder-local
            // labels, attachment vectors, and callbacks are still reconstructed. One per window for
            // the same reason frames_in_flight is per-window, not Renderer-wide: only one frame is ever
            // being declared for a given window at a time (render_frame_rhi is synchronous per call), so
            // there's no cross-window or cross-frame contention to worry about.
            RenderGraph graph;
            // Semantic resources published and consumed by reusable graph modules. Retained with the
            // graph so reset() preserves capacity and steady-state frame lowering does not allocate.
            RenderGraphBlackboard graph_resources;
            // Same per-window rationale as frames_in_flight/scene_frame_resources above, plus a sharper
            // failure mode: HiZPyramidTargets is deliberately a single persistent "last completed
            // frame's" history buffer, not a ring (see its own doc comment) — a Renderer-wide instance
            // meant two windows would resize/rebuild the *same* pyramid out from under each other every
            // time their extents differ (near-guaranteed for a primary window + smaller torn-off
            // panels), and since its texture/view handles are captured unlocked and read later in the
            // same frame's render graph (RendererLifecycle.cpp), a resize mid-frame could destroy a
            // texture the other window's graph still references — a real GPU handle lifetime hazard,
            // not just wasted rebuild work. Real bug found during the same session's lock-contention
            // audit — see memory project_multi_window_render_threading.
            HiZPyramidTargets hiz_pyramid;
            // Last completed frame's CPU/GPU timing readback (see FrameTimingSnapshot's own doc
            // comment, Scene.hpp). The render thread publishes while the application thread may call
            // last_frame_timings(), so the snapshot itself must be synchronized independently of the
            // window_surfaces_ container lock (which only protects record lookup/lifetime structure).
            // Heap allocation keeps WindowSurfaceRecord movable for its aggregate make_unique path.
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
            // This draw's position in FrameSubmission::draws at the moment render_frame_dispatch
            // stamps it (right after the (material, mesh) sort, before any pass-specific
            // filtering/copying) — matches SceneObjectGpuData's index in prepare_scene_gpu_data's
            // object_buffer 1:1, regardless of which filtered view of submission.draws (gbuffer_draws,
            // an instanced batch's range, ...) this RenderItem is later read through. Only meaningful
            // for draws recorded with record_render_item's with_object_history=true.
            u32 object_index = 0;
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
            // Set 1 for with_object_history draws (Shaders/gbuffer_geometry_history.slang's
            // sceneObjects/sceneView) — the same bind group for every draw in a pass/bundle
            // regardless of material, so this only needs to be bound once, exactly like arena_bound.
            RHI::BindGroupHandle bound_object_history_group{};
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
            u64 frame_index = 0;
            CameraView camera{};
            SceneLighting lighting{};
            DeferredTargetFormats deferred_formats{};
            RenderGraphSettings render_graph{};
            OffscreenRenderTargetHandle offscreen_target{};
            vector<RHI::BindGroupHandle> transient_bind_groups;
            vector<RHI::BufferHandle> transient_buffers;
            // Render bundles (secondary command buffers) finished this frame via
            // record_render_items_culled's/record_shadow_view_chunk's parallel paths and already
            // consumed by a pass.execute_bundles() call. Real bug found+fixed this session: destroying
            // a bundle's underlying command pool/buffer (VulkanRhiDeviceBridge::destroy_render_bundle)
            // is NOT safe immediately after execute_bundles() returns — execute_bundles only *records*
            // a vkCmdExecuteCommands referencing it into the primary command buffer, which the GPU
            // hasn't even been asked to run yet (let alone finished) at that point. Must live at least
            // as long as this frame's own command buffers do — so retiring bundles go here instead,
            // moved into FrameInFlight::transient_render_bundles at the end of render_frame_rhi and
            // destroyed only once that ring slot's fence proves the GPU is done with them, same
            // fire-and-forget contract as transient_bind_groups/transient_buffers above.
            vector<RHI::RenderBundleHandle> transient_render_bundles;
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

        // What record_instance_cull needs from last frame's Hi-Z pyramid (WindowSurfaceRecord::
        // hiz_pyramid) to run this frame's occlusion test — a plain data snapshot rather than a
        // reference to the record field itself, so callers further down the dispatch loop don't need
        // to keep threading a WindowSurfaceRecord& through just for this.
        struct HiZCullInput {
            RHI::TextureViewHandle pyramid_view{};
            u32 extent_width = 0;
            u32 extent_height = 0;
            u32 mip_count = 0;
            bool valid = false;
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
            RHI::SampleCount samples = RHI::SampleCount::X1;
            RHI::RenderPipelineHandle pipeline{};
        };
        struct InstancedTemplateResources {
            RHI::PipelineLayoutHandle pipeline_layout{};
            vector<InstancedPipelineVariant> pipeline_variants;
        };

        // Per-object motion vectors for the *non*-instanced per-item draw path (RendererObjectHistory.cpp).
        // Mirrors InstanceCullResources' shape minus the compute-cull half: a separately-compiled vertex
        // stage (Shaders/gbuffer_geometry_history.slang's vertexMainWithHistory) that reads model/
        // previous_model from the same SceneObjectGpuData/SceneViewGpuData buffers prepare_scene_gpu_data
        // already fills for the instanced path, indexed by a tiny per-draw push constant instead of the
        // ordinary SceneDrawConstants{view_projection, model} push constant (128 bytes, Vulkan's
        // guaranteed push-constant minimum — no room for a previous_model mat4 too). Compiled as its own
        // module so its extra sceneObjects/sceneView bind group never touches the z-prepass/shadow/gizmo
        // pipelines, which keep using the ordinary vertexMain + SceneDrawConstants unchanged.
        struct ObjectHistoryResources {
            Core::Slang::Shader vertex_shader;
            RHI::ShaderModuleHandle vertex_module{};
            RHI::BindGroupLayoutHandle bind_group_layout{};
            bool ready = false;
        };

        // One material template's with-object-history pipeline, keyed by (color formats, depth format,
        // depth policy, samples) like MaterialPipelineVariant. `pipeline_layout` combines the template's own reflected
        // set 0 (reused as-is) with ObjectHistoryResources::bind_group_layout at set 1.
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

        // NVIDIA SRAA reconstruction resources. The three reflected sampled-image bindings are:
        // 1x shaded HDR color, 1x shaded depth, and multisampled geometry depth.
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
            // Shared linear/clamp-to-edge sampler for the atmosphere LUTs (transmittance/multi-
            // scattering/sky-view) this pass now also samples — created alongside gbuffer_sampler/
            // shadow_sampler in ensure_shadow_lighting_resources.
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

        // GPU state for the Hi-Z pyramid's mip-reduction pass (Shaders/hiz_build.slang): one shader
        // (vertexMain + reduceMain), one bind-group layout (a single Texture2D<float> `source` — no
        // sampler, reduceMain only ever does point `.Load`s), one pipeline reused for every mip level
        // and for the first level's real-depth-texture source — see HiZPyramidTargets's doc comment
        // for why the same reduction logic serves both. Built lazily on first use, same shape as
        // BloomResources above.
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

        // One compiled compute pipeline for one of the three atmosphere LUT bake shaders
        // (Shaders/sky_transmittance_lut.slang / sky_multi_scattering_lut.slang / sky_view_lut.slang)
        // — same shape as Renderer::ensure_instance_cull_resources' single-shader build, repeated
        // three times by AtmosphereLutResources below.
        struct AtmosphereLutBakePipeline {
            Core::Slang::Shader shader;
            RHI::ShaderModuleHandle module{};
            RHI::BindGroupLayoutHandle bind_group_layout{};
            RHI::PipelineLayoutHandle pipeline_layout{};
            RHI::ComputePipelineHandle pipeline{};
        };

        // Built lazily on first use, same ready-flag idiom as HiZBuildResources/ShadowLightingResources
        // above. The three LUTs are rebaked every frame (see record_atmosphere_lut_bakes) rather than
        // cached, so this only ever holds pipelines/layouts, never the LUT textures themselves.
        struct AtmosphereLutResources {
            AtmosphereLutBakePipeline transmittance;
            AtmosphereLutBakePipeline multi_scattering;
            AtmosphereLutBakePipeline sky_view;
            RHI::SamplerHandle lut_sampler{};
            bool ready = false;
        };

        // GPU state for the explicit bloom-composite pass (scene HDR + reconstructed bloom pyramid -> one
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
            // Real procedural sky for the miss/background term (environmentRgb in
            // spectral_integrators.slang) — the same baked LUTs + AtmosphereGpuData buffer
            // Shaders/deferred_shadow_lighting.slang resolves the rasterized sky from, threaded
            // through so a path-traced miss ray reads the identical atmosphere instead of a fake
            // hardcoded gradient. See Renderer::record_atmosphere_lut_bakes (RendererLifecycle.cpp).
            RHI::TextureViewHandle transmittance_lut{};
            RHI::TextureViewHandle sky_view_lut{};
            RHI::BufferHandle atmosphere_constants{};
        };

        // prepare_spectral_scene_acceleration_structure()'s per-material memoization: the ~13
        // read_material_parameter lookups + per-slot texture resolution it currently redoes for every
        // draw, every frame, only actually change when the material's content_revision changes.
        // Deliberately duplicates SpectralMaterialGpu's scalar fields rather than depending on that
        // (translation-unit-local, in RendererSpectralPathTracing.cpp) type — texture indices are NOT
        // cached here since those are frame-local bindless-heap positions assigned by
        // append_material_texture, not a property of the material itself.
        struct SpectralMaterialParameterCacheEntry {
            u64 content_revision = 0;
            glm::vec4 base_color{0.8f, 0.8f, 0.8f, 1.0f};
            glm::vec4 emissive_and_strength{0.0f, 0.0f, 0.0f, 1.0f};
            // x roughness factor, y metallic factor, z occlusion strength, w dielectric F0.
            glm::vec4 surface{0.5f, 0.0f, 1.0f, 0.04f};
            // x transmission, y Cauchy A, z Cauchy B (um^2), w absorption coefficient.
            glm::vec4 transmission{0.0f, 1.4878f, 0.0042f, 0.0f};
            // x alpha cutoff, y normal-map scale, zw reserved.
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
            // Linear/clamp-to-edge sampler for the atmosphere LUTs environmentRgb samples — same
            // desc as ShadowLightingResources::atmosphere_sampler, kept as its own instance since
            // this resource struct is destroyed/rebuilt independently of shadow lighting's.
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
        [[nodiscard]] Core::RendererExpected<OffscreenRenderTargetGpuResources>
        create_offscreen_render_target_gpu_resources(const OffscreenRenderTargetDescription &description);
        [[nodiscard]] optional<ResolvedOffscreenRenderTarget> resolve_offscreen_render_target(
            OffscreenRenderTargetHandle handle) const noexcept;
        void mark_offscreen_render_target_initialized(OffscreenRenderTargetHandle handle) noexcept;
        void invalidate_offscreen_render_targets_after_device_loss() noexcept;
        [[nodiscard]] Core::RendererResult restore_offscreen_render_targets_after_recovery();
        void destroy_all_offscreen_render_targets() noexcept;

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
        // Waits for record.pending_present (a no-op if there isn't one), applies its result to
        // record.rhi_swapchain_dirty / propagates a hard error, and appends its queue-lock-wait timing
        // into `stage_timings_ms`. Must be called before rhi_swapchain_dirty is read for this frame's
        // recreate decision, and before any code destroys/recreates this record's swapchain -- see
        // WindowSurfaceRecord::pending_present's doc comment.
        [[nodiscard]] Core::RendererResult drain_pending_present(
            WindowSurfaceRecord &record, vector<std::pair<string, f64>> *stage_timings_ms);
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
                                                                         const DeferredTargetFormats &formats,
                                                                         RHI::SampleCount samples);
        void destroy_frame_deferred_targets(FrameInFlight &slot) noexcept;
        [[nodiscard]] Core::RendererResult ensure_frame_shadow_targets(FrameInFlight &slot, u32 atlas_size);
        void destroy_frame_shadow_targets(FrameInFlight &slot) noexcept;

        // ── Atmosphere / sky (Shaders/sturdy_atmosphere.slang + sky_*_lut.slang) ──
        [[nodiscard]] Core::RendererResult ensure_frame_atmosphere_targets(FrameInFlight &slot);
        void destroy_frame_atmosphere_targets(FrameInFlight &slot) noexcept;
        // Fills AtmosphereGpuData (hardcoded Earth-default physics constants for now — see
        // RendererAtmosphere.cpp's own doc comment; user-facing settings are a follow-up) and
        // uploads it into `constants_buffer`.
        [[nodiscard]] Core::RendererResult prepare_atmosphere_frame(const FrameSubmission &submission,
                                                                    RHI::BufferHandle constants_buffer);
        [[nodiscard]] Core::RendererResult ensure_atmosphere_lut_resources();
        // Declares the three LUT-bake compute passes into `graph` and returns their transient texture
        // handles. Rebaked every frame — see AtmosphereLutResources' own doc comment for why nothing
        // here is cached across frames the way HiZ/bloom resources are.
        [[nodiscard]] Core::RendererResult record_atmosphere_lut_bakes(
            RenderGraph &graph, RHI::BufferHandle atmosphere_buffer,
            RenderGraphTextureHandle &out_transmittance_lut, RenderGraphTextureHandle &out_multi_scattering_lut,
            RenderGraphTextureHandle &out_sky_view_lut, vector<RHI::BindGroupHandle> &transient_bind_groups);
        void destroy_atmosphere_lut_resources() noexcept;
        [[nodiscard]] Core::RendererResult prepare_shadow_frame(const FrameSubmission &submission,
                                                                 FrameShadowTargets &targets,
                                                                 PreparedShadowFrame &prepared,
                                                                 Core::Extent2D render_extent);
        [[nodiscard]] Core::RendererResult ensure_frame_bloom_targets(FrameInFlight &slot,
                                                                      Core::Extent2D extent,
                                                                      u32 requested_levels,
                                                                      f32 downsample_ratio);
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
        // Creates `slot.pregraph_gpu_timing_query_set` once (fixed 4 slots); a no-op on every later
        // call. See the field's own doc comment on why this is separate from ensure_frame_gpu_timing_target.
        [[nodiscard]] Core::RendererResult ensure_frame_pregraph_gpu_timing_target(FrameInFlight &slot);
        void destroy_frame_pregraph_gpu_timing_target(FrameInFlight &slot) noexcept;
        // Waits for every in-flight frame (of one window's ring) to finish, then reclaims its resources
        // (including retired swapchains/presentation textures — safe here specifically because of the
        // wait_idle, see reclaim_frame_slot's comment). The sanctioned heavy wait for teardown / periodic
        // retired-swapchain flush — NOT the per-frame path. Leaves each slot's fence allocated but reset
        // (unsignaled) so the ring is immediately reusable.
        void drain_frames_in_flight(WindowSurfaceRecord &record) noexcept;
        // Cleans up superseded swapchains/presentation textures that recreate_rhi_swapchain() couldn't
        // safely destroy immediately. Backends exposing RHI::Feature::SwapchainMaintenance poll the
        // presentation-completion fences attached to each retired generation, so live resize never
        // needs to idle unrelated work. The portable fallback retains the bounded wait_idle() policy.
        void maybe_flush_retired_swapchains(WindowSurfaceRecord &record, bool opportunistic) noexcept;
        void reclaim_completed_presentation_fences(WindowSurfaceRecord &record) noexcept;
        void reclaim_completed_retired_presentations(WindowSurfaceRecord &record) noexcept;
        void destroy_retired_presentations(WindowSurfaceRecord &record) noexcept;
        void destroy_rhi_presentation_resources(WindowSurfaceRecord &record) noexcept;
        [[nodiscard]] Core::RendererResult prepare_scene_gpu_data(
            WindowSurfaceRecord &record, u64 frame_index, const FrameSubmission &submission);
        void destroy_scene_gpu_resources(vector<SceneFrameGpuResources> &resources) noexcept;
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
                                                               f32 shadow_slope_bias = 0.0f,
                                                               RHI::SampleCount samples = RHI::SampleCount::X1,
                                                               bool with_object_history = false,
                                                               RHI::BindGroupHandle object_history_group = {});

        // Frustum-culls `items` against `frustum` (render_item_visible), then records survivors
        // into `pass`. If `use_bundles` is false, this is just a single serial loop. If true,
        // survivors are split into contiguous chunks (preserving the caller's (material, mesh)
        // sort-coherence within each chunk) and recorded concurrently, each chunk into its own
        // RHI::RenderBundleEncoder via Async::Scheduler::spawn — the exact chunking pattern
        // prepare_scene_gpu_data already uses for object-buffer packing (RendererScene.cpp) — then
        // stitched into `pass` with one execute_bundles call. `bundle_label` names the bundles for
        // any GPU-side debug tooling.
        //
        // `use_bundles` is a caller-supplied decision, not recomputed here from the post-cull
        // survivor count: Vulkan requires vkCmdBeginRendering to declare up front (via
        // RHI::RenderPassDesc::allow_bundles) whether a render-pass instance will use
        // execute_bundles, so the caller must decide (typically from a pre-cull visible count
        // against kParallelRecordThreshold, same threshold this function used to apply
        // internally) *before* declaring the pass, then pass that exact same decision here so the
        // branch taken can never drift from the flag the pass was actually opened with — same
        // fix, same reasoning, as the raster-shadow-atlas pass's shadow_atlas_uses_bundles (see
        // RendererLifecycle.cpp's "raster shadow atlas" pass declaration).
        //
        // Materials are pre-warmed (prepare_material_frame called once per distinct material, on
        // this thread, before any worker task starts) specifically so concurrent per-chunk calls to
        // record_render_item never race on the same MaterialInstanceFrame's bind-group rebuild —
        // prepare_material_frame only mutates state when a frame's dirty flags are set, and warming
        // them here first means every worker thread's later call is a pure read.
        //
        // `retired_bundles` (when the parallel path is taken) receives the RenderBundleHandles that
        // were just consumed by pass.execute_bundles() — these must NOT be destroyed synchronously
        // by the caller (execute_bundles only records a reference for the GPU to run later; the
        // handle must outlive this frame's submission, same as transient_bind_groups/transient_buffers).
        // Callers should append these into FrameSubmission::transient_render_bundles.
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

        // Records `views` — each with its own viewport/scissor/frustum/view-projection — into
        // `encoder`, depth-only, sharing a single RenderItemBindingState across the whole span. Used
        // for shadow-atlas rendering (RendererLifecycle.cpp's "raster shadow atlas" pass): unlike
        // record_render_items_culled (one shared frustum per call, pre-culled once), each view here
        // culls `draws` against its own frustum internally, since a shadow atlas pass covers many
        // independent views (cascades, spot cones, point cube faces) in one RenderGraph pass. Sharing
        // one binding_state across the chunk — instead of resetting it per view — is deliberate: since
        // `draws` is already globally sorted by (material, mesh), consecutive views recorded by the
        // same encoder often keep drawing the same pipeline/bind-group/vertex-buffer, and skip
        // redundant rebinding exactly like within a single ordinary pass. Templated on Encoder for the
        // same reason record_render_item is: called with RHI::RenderPassEncoder directly for the
        // small-view-count serial path, and with RHI::RenderBundleEncoder (one per worker-assigned
        // chunk of views, via Async::Scheduler::spawn) for the parallel path — each bundle sets its
        // own viewport/scissor internally rather than relying on inheriting it from the primary pass,
        // which is what makes recording it concurrently with other views' bundles safe.
        template <typename Encoder>
        [[nodiscard]] Core::RendererResult record_shadow_view_chunk(Encoder &encoder,
                                                                     span<const ShadowRenderView> views,
                                                                     span<const RenderItem> draws,
                                                                     RHI::Format depth_format,
                                                                     u64 frame_index,
                                                                     f32 shadow_depth_bias,
                                                                     f32 shadow_slope_bias);

        // ── GPU-driven instanced batch draws (RendererGpuCulling.cpp) ──
        // Scans `sorted_draws` (already sorted by (material, mesh) — see render_frame_dispatch) for
        // contiguous same-(mesh, material) runs at or above the minimum batch size and returns one
        // InstancedBatch per run found. Callers route a batch's instances through
        // record_instanced_batches instead of the individual record_render_items_culled path.
        [[nodiscard]] vector<InstancedBatch> detect_instanced_batches(span<const RenderItem> sorted_draws) const;

        [[nodiscard]] Core::RendererResult ensure_instance_cull_resources();
        void destroy_instance_cull_resources() noexcept;
        // Analogous to prepare_scene_gpu_data: called once per frame, before the render graph is
        // declared. (Re)allocates `resources`' indirect-command/compacted-index buffers if this
        // frame's batches need more room than last frame's, then writes every batch's
        // GpuDrawIndexedIndirectCommand (index_count/first_index/vertex_offset from its mesh,
        // instance_count left at 0 for the cull compute shader to fill in).
        [[nodiscard]] Core::RendererResult prepare_instance_cull_gpu_data(span<const InstancedBatch> batches,
                                                                          SceneFrameGpuResources &resources);
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> instanced_pipeline_for(
            MaterialTemplateResource &material_template, span<const RHI::Format> color_formats,
            RHI::Format depth_format, RHI::SampleCount samples = RHI::SampleCount::X1);

        // ── Per-object motion vectors for non-instanced draws (RendererObjectHistory.cpp) ──
        [[nodiscard]] Core::RendererResult ensure_object_history_resources();
        void destroy_object_history_resources() noexcept;
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> history_pipeline_for(
            MaterialTemplateResource &material_template, span<const RHI::Format> color_formats,
            RHI::Format depth_format, bool standard_depth_test = false,
            RHI::SampleCount samples = RHI::SampleCount::X1);
        // One bind group (set 1: sceneObjects StructuredBuffer + sceneView ConstantBuffer) built once
        // per frame from `resources`' already-populated view_buffer/object_buffer (prepare_scene_gpu_data
        // fills both every frame regardless of whether any draw uses object history) and pushed onto
        // `transient_bind_groups` for frame-lifetime cleanup — see FrameSubmission::transient_bind_groups.
        [[nodiscard]] Core::RendererExpected<RHI::BindGroupHandle> ensure_object_history_bind_group(
            SceneFrameGpuResources &resources, vector<RHI::BindGroupHandle> &transient_bind_groups);

        // Records one compute dispatch per batch (frustum + Hi-Z occlusion cull, plus compaction)
        // into `pass`, writing into `resources`' indirect-command/compacted-index buffers — see
        // Shaders/gpu_instance_cull.slang's header comment for the buffer protocol. The declaring
        // render-graph compute and G-buffer passes publish their buffer writes/reads, so graph-managed
        // dependencies and barriers order these writes before record_instanced_batches consumes them.
        // `camera_world_position` and `hiz` feed the occlusion test — see HiZCullInput's own doc
        // comment; `hiz.valid == false` (first frame / just resized) skips it, frustum-only that
        // frame, same as before this test existed.
        [[nodiscard]] Core::RendererResult record_instance_cull(RHI::ComputePassEncoder &pass,
                                                                span<const InstancedBatch> batches,
                                                                const glm::mat4 &view_projection,
                                                                const glm::vec3 &camera_world_position,
                                                                const HiZCullInput &hiz,
                                                                SceneFrameGpuResources &resources,
                                                                vector<RHI::BindGroupHandle> &transient_bind_groups);

        // ── Hi-Z (hierarchical depth) occlusion culling (RendererHiZ.cpp) ──
        // Lazily compiles/builds the shared mip-reduction pipeline (Shaders/hiz_build.slang) used by
        // every level of every frame's pyramid.
        [[nodiscard]] Core::RendererResult ensure_hiz_build_resources();
        void destroy_hiz_build_resources() noexcept;
        void destroy_hiz_pyramid(HiZPyramidTargets &pyramid) noexcept;
        // (Re)allocates `pyramid` (destroying and recreating on resize/format change, which also
        // resets `has_valid_data`) so its base level is half of `depth_extent` (the same resolved,
        // single-sample depth extent record_hiz_build's next call will reduce into it — see
        // HiZPyramidTargets::extent's own doc comment for why half, not equal) with a full mip chain
        // down to 1x1.
        [[nodiscard]] Core::RendererResult ensure_hiz_pyramid(HiZPyramidTargets &pyramid, Core::Extent2D depth_extent);
        // Records `pyramid.mip_levels` fullscreen reduceMain draws into the render graph: level 0
        // reduces `depth_view` (the real resolved depth texture, `depth_extent`-sized) into pyramid
        // mip 0; level K>0 reduces pyramid mip K-1 into mip K. Called once per frame, right after the
        // "deferred gbuffer geometry" pass (once depth is final) — see RendererLifecycle.cpp's call
        // site for why there and not earlier. Sets `pyramid.has_valid_data = true` once recorded, for
        // *next* frame's record_instance_cull to consume.
        [[nodiscard]] Core::RendererResult record_hiz_build(RenderGraph &graph, RenderGraphTextureHandle depth_texture,
                                                             RHI::TextureViewHandle depth_view, Core::Extent2D depth_extent,
                                                             RenderGraphTextureHandle pyramid_texture, HiZPyramidTargets &pyramid,
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
                                                                    const glm::mat4 &previous_view_projection,
                                                                    SceneFrameGpuResources &resources,
                                                                    vector<RHI::BindGroupHandle> &transient_bind_groups,
                                                                    RHI::SampleCount samples = RHI::SampleCount::X1);

        [[nodiscard]] Core::RendererResult try_upload_mesh(MeshResource &mesh);

        // ── Material/texture internals ──
        // Uploads tightly-packed pixel `data` into `resource`'s already-created RHI texture via a
        // staged buffer copy + layout transitions (one-shot command buffer, waits — the pre-frame-graph
        // upload path, same shape as the mesh staging copy).
        [[nodiscard]] Core::RendererResult create_owned_texture_gpu(TextureResource &resource,
                                                                     span<const RHI::QueueClass> concurrent_queue_classes = {});
        // submit_texture_upload is declared public above (Engine::TextureStreamer needs it); its
        // implementation and this synchronous wrapper live together in RendererTextures.cpp.
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
            bool standard_depth_test = false, RHI::SampleCount samples = RHI::SampleCount::X1);
        // Lazily builds + caches a template's depth-only pipeline: same vertex stage + (if the
        // template's shader declared one) the depth-only fragment entry for alpha-tested cutout, zero
        // color attachments, real depth write (depth_compare == Less) — this is the pipeline the Z
        // prepass itself draws with.
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> depth_only_pipeline_for(
            MaterialTemplateResource &material_template, RHI::Format depth_format,
            bool shadow_map = false, f32 depth_bias = 0.0f, f32 slope_bias = 0.0f,
            RHI::SampleCount samples = RHI::SampleCount::X1);

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
        void destroy_shadow_lighting_resources() noexcept;
        void destroy_shadow_lighting_resources_locked(ShadowLightingResources &resources) noexcept;

        // ── Fullscreen render-graph modules ──
        // These helpers lower reusable semantic modules into the RHI-aware graph. They consume and
        // publish typed resources through RenderGraphBlackboard rather than threading lifecycle-local
        // source variables through the entire frame declaration function.
        [[nodiscard]] Core::RendererResult build_deferred_msaa_module(
            RenderGraphModuleBuildContext &context,
            FrameSubmission &submission,
            RHI::SampleCount samples);
        [[nodiscard]] Core::RendererResult build_post_process_aa_module(
            RenderGraphModuleBuildContext &context,
            FrameSubmission &submission);
        [[nodiscard]] Core::RendererResult build_custom_graph_stage(
            RenderGraphModuleBuildContext &context,
            FrameSubmission &submission,
            PostProcessStage stage,
            span<RenderGraphTextureHandle> logical_textures);
        [[nodiscard]] Core::RendererResult build_bloom_module(
            RenderGraphModuleBuildContext &context,
            FrameSubmission &submission,
            FrameInFlight &frame_slot,
            bool enabled,
            RHI::Format bloom_format);
        [[nodiscard]] Core::RendererResult build_tonemap_module(
            RenderGraphModuleBuildContext &context,
            FrameSubmission &submission,
            RHI::Format presentation_format,
            bool hdr_output,
            Core::HdrColorSpaceMode hdr_color_space);

        // ── Fullscreen post-processes ──
        // NVIDIA SRAA: reconstructs 1x deferred shading from multisampled depth visibility before
        // bloom/tonemapping. See Shaders/deferred_msaa_reconstruction.slang.
        [[nodiscard]] Core::RendererResult ensure_deferred_msaa_resources();
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> deferred_msaa_pipeline_for(
            RHI::Format color_format);
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
        void destroy_deferred_msaa_resources() noexcept;
        void destroy_deferred_msaa_resources_locked(DeferredMsaaResources &resources) noexcept;

        [[nodiscard]] Core::RendererResult ensure_bloom_resources(RHI::Format color_format);
        [[nodiscard]] Core::RendererResult record_bloom_draw(RHI::RenderPassEncoder &pass,
                                                              RHI::TextureViewHandle source_view,
                                                              glm::vec2 source_texel_size,
                                                              f32 threshold, f32 soft_knee, f32 scatter,
                                                              glm::vec2 filter_scale,
                                                              bool prefilter, bool upsample,
                                                              RHI::BindGroupHandle bind_group);
        [[nodiscard]] Core::RendererResult record_bloom_downsample(RHI::RenderPassEncoder &pass,
                                                                    RHI::TextureViewHandle source_view,
                                                                    glm::vec2 source_texel_size,
                                                                    const RenderGraphSettings &settings,
                                                                    glm::vec2 filter_scale,
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
        // view — so unlike every other bloom level's bind group, it cannot be cached in FrameBloomTargets
        // and must be created fresh per frame and retired with that frame (transient_bind_groups).
        [[nodiscard]] Core::RendererExpected<RHI::BindGroupHandle> create_bloom_source_bind_group(
            RHI::TextureViewHandle source_view);

        // Explicit HDR bloom composite: scene HDR + reconstructed bloom pyramid -> one scene-linear HDR
        // result, so AfterBloomBeforeToneMap custom effects and tonemapping both see a single plain
        // texture instead of bloom being folded into the tonemap shader.
        [[nodiscard]] Core::RendererResult ensure_bloom_composite_resources();
        [[nodiscard]] Core::RendererExpected<RHI::RenderPipelineHandle> bloom_composite_pipeline_for(RHI::Format color_format);
        [[nodiscard]] Core::RendererResult record_bloom_composite(RHI::RenderPassEncoder &pass,
                                                                   RHI::TextureViewHandle scene_view,
                                                                   RHI::TextureViewHandle bloom_view,
                                                                   RHI::Format color_format,
                                                                   f32 bloom_intensity,
                                                                   bool threshold_enabled,
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

        [[nodiscard]] Core::RendererResult ensure_post_process_aa_resources(
            const RenderGraphSettings &settings,
            RHI::Format color_format);
        [[nodiscard]] Core::RendererResult record_post_process_aa(
            RHI::RenderPassEncoder &pass,
            RHI::TextureViewHandle source_view,
            RHI::Format color_format,
            const RenderGraphSettings &settings,
            vector<RHI::BindGroupHandle> &transient_bind_groups);

        [[nodiscard]] Core::RendererResult ensure_spectral_path_tracing_resources(SpectralRenderMode mode);
        [[nodiscard]] Core::RendererResult ensure_spectral_mesh_acceleration_structures(
            span<const RenderItem> draws);
        [[nodiscard]] Core::RendererResult prepare_spectral_scene_acceleration_structure(
            RHI::CommandEncoder &encoder, FrameInFlight &slot, const FrameSubmission &submission);
        [[nodiscard]] Core::RendererResult ensure_spectral_accumulation_target(
            WindowSurfaceRecord &record, Core::Extent2D extent);
        void destroy_spectral_accumulation_target(WindowSurfaceRecord &record) noexcept;
        [[nodiscard]] Core::RendererResult ensure_frame_spectral_photon_targets(
            FrameInFlight &slot, u32 photon_capacity);
        void destroy_frame_spectral_photon_targets(FrameInFlight &slot) noexcept;
        [[nodiscard]] Core::RendererResult prepare_spectral_photon_mapping(
            RHI::CommandEncoder &encoder, FrameInFlight &slot, const FrameSubmission &submission,
            bool emission_needed, u64 photon_signature);
        [[nodiscard]] Core::RendererResult record_spectral_photon_pass(
            RHI::ComputePassEncoder &pass, FrameInFlight &slot, const FrameSubmission &submission,
            usize pipeline_index, const char *label);
        [[nodiscard]] Core::RendererResult record_spectral_photon_emission(
            RHI::ComputePassEncoder &pass, FrameInFlight &slot, const FrameSubmission &submission);
        [[nodiscard]] Core::RendererResult record_spectral_photon_hash(
            RHI::ComputePassEncoder &pass, FrameInFlight &slot, const FrameSubmission &submission);
        [[nodiscard]] Core::RendererResult record_spectral_integrator(
            RHI::ComputePassEncoder &pass, FrameInFlight &slot, const FrameSubmission &submission,
            Core::Extent2D extent, const SpectralIntegratorViews &views, bool accumulation_reset);
        [[nodiscard]] Core::RendererResult record_spectral_depth_commit(
            RHI::RenderPassEncoder &pass, FrameInFlight &slot, RHI::TextureViewHandle primary_depth_view,
            Core::Extent2D extent);
        void destroy_spectral_path_tracing_resources() noexcept;
        void destroy_spectral_path_tracing_resources_locked(SpectralPathTracingResources &resources) noexcept;

        [[nodiscard]] Core::RendererResult ensure_custom_compute_effect(const CustomComputeEffect &effect);
        [[nodiscard]] Core::RendererResult record_custom_compute_effect(
            RHI::ComputePassEncoder &pass,
            RHI::TextureViewHandle source_view,
            RHI::TextureViewHandle output_view,
            Core::Extent2D extent,
            const CustomComputeEffect &effect,
            vector<RHI::BindGroupHandle> &transient_bind_groups);
        void destroy_custom_compute_effect_resources() noexcept;

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
                                                          vector<RHI::BindGroupHandle> &transient_bind_groups,
                                                          bool preserve_alpha = false);
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
        mutable Async::Mutex<vector<OffscreenRenderTargetRecord>> offscreen_render_targets_;
        // Centralize vkQueuePresentKHR issuance across every window that shares a native queue --
        // see PresentationCoordinator's own doc comment. Constructed unconditionally (not lazily) so
        // two windows' render threads can never race a lazy-create check; the compute one costs
        // nothing when nothing ever presents from compute (PresentationSettings::
        // allow_present_from_compute defaults to false -- see Core/Renderer.hpp). Selected per
        // swapchain via presentation_coordinator_for(), keyed by
        // RHI::PresentationResolution::present_queue_is_compute (the backend's own resolved answer,
        // never re-derived here).
        PresentationCoordinator graphics_presentation_coordinator_{"PresentationCoordinator-Graphics"};
        PresentationCoordinator compute_presentation_coordinator_{"PresentationCoordinator-Compute"};
        [[nodiscard]] PresentationCoordinator &presentation_coordinator_for(bool present_via_compute) noexcept {
            return present_via_compute ? compute_presentation_coordinator_ : graphics_presentation_coordinator_;
        }
        Core::RendererCapabilities capabilities_{};
        // A single growable GPU buffer that mesh uploads sub-allocate append-only ranges from, instead
        // of each Mesh owning its own dedicated VkBuffer — see try_upload_mesh/grow_geometry_arena.
        // Growth (doubling) preserves the old used range with one GPU-to-GPU copy at stable offsets;
        // it does not replay every retained CPU payload. This only happens during asset loading,
        // never mid-frame, so its O(resident data) cost is a non-issue.
        struct GeometryArena {
            RHI::BufferHandle buffer{};
            RHI::BufferUsage usage = RHI::BufferUsage::None;
            u64 capacity_bytes = 0;
            u64 used_bytes = 0;
        };
        [[nodiscard]] Core::RendererResult grow_geometry_arena(GeometryArena &arena, u64 required_bytes,
                                                               const char *label);
        // TransferSrc (on top of TransferDst) is required so grow_geometry_arena can copy the old
        // buffer's contents into a newly-grown one GPU-side instead of replaying every resident
        // mesh's retained CPU-side vertices/indices.
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
        // Legacy accumulator for the public submit_draw() API + the plain render_frame(surface, frame)
        // fallback overload only — never touched by the RenderFrameDesc path (which uses a fully
        // call-local FrameSubmission instead). An empty accumulator now intentionally renders no
        // geometry; content is always supplied by an API consumer.
        vector<RenderItem> frame_draws_;
        // Lazily created by the first async poll over the `Shaders/` tree; primed so the first poll
        // reports only edits made after the engine started. Polling is throttled and runs on Async workers
        // because the watcher recursively stats the shader tree and project roots can live on slow filesystems.
        std::shared_ptr<Core::Slang::ShaderWatcher> shader_watcher_;
        optional<Async::TaskHandle<ShaderHotReloadPollResult>> shader_hot_reload_poll_;
        steady_clock::time_point next_shader_hot_reload_poll_{};
        // Guards poll_shader_hot_reload()'s whole check-then-poll-then-reload body — same "hold the
        // lock for the whole function" discipline as material_frame_prepare_lock_/
        // transient_bind_groups_lock_ above. Both render_frame() overloads call this unconditionally
        // at the top of every frame, so with one render thread per window it would otherwise run
        // concurrently for every open window: two threads racing shader_hot_reload_poll_'s optional/
        // shared_ptr reset-and-reassign, or one thread's reload_material_template() mutating
        // material_templates_/material_instances_ while another window's frame is mid-poll here.
        Async::Mutex<u8> shader_hot_reload_lock_;
        // Each lazy-build-once-and-cache resource gets its own Async::Mutex, same rationale as
        // window_surfaces_ above — ensure_*()/​*_pipeline_for() hold the guard for their whole
        // check-then-build body, so concurrent first-use from two windows' render calls can't double-build
        // or corrupt the cache. Each is fast once warm, so this only ever serializes the rare cold-start/
        // new-variant path, never per-frame recording or submission.
        Async::Mutex<BloomResources> bloom_;
        Async::Mutex<BloomCompositeResources> bloom_composite_;
        Async::Mutex<ShadowLightingResources> shadow_lighting_;
        Async::Mutex<DeferredMsaaResources> deferred_msaa_;
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
        Async::Mutex<vector<CustomComputeEffectResources>> custom_compute_effect_resources_;
        Async::Mutex<SpectralPathTracingResources> spectral_path_tracing_;
        // Keyed by MaterialInstanceHandle::value — see SpectralMaterialParameterCacheEntry's own doc
        // comment. Handle values are never recycled (create_material_instance only ever appends,
        // destroy_material_instance zeroes the slot in place), so a stale entry can only ever be
        // shadowed by a content_revision mismatch, never silently misattributed to a different
        // material that reused the same handle value.
        Async::Mutex<std::unordered_map<u64, SpectralMaterialParameterCacheEntry>> spectral_material_parameter_cache_;
        Async::Mutex<InstanceCullResources> instance_cull_;
        // instanced_pipeline_for()'s per-template cache, same rationale/shape as
        // material_pipeline_variants_ above (keyed by MaterialTemplateHandle::value).
        Async::Mutex<std::unordered_map<u64, InstancedTemplateResources>> instanced_pipeline_variants_;
        Async::Mutex<ObjectHistoryResources> object_history_;
        // history_pipeline_for()'s per-template cache, same rationale/shape as
        // instanced_pipeline_variants_ above (keyed by MaterialTemplateHandle::value).
        Async::Mutex<std::unordered_map<u64, ObjectHistoryTemplateResources>> object_history_pipeline_variants_;
        // Guards prepare_material_frame()'s whole check-then-rebuild body (RendererMaterial.cpp),
        // same "hold the lock for the whole function" discipline as material_pipeline_variants_ above,
        // just without a cache map to key it by — MaterialInstanceFrame's dirty flags/bind_groups
        // vector live inline on the (caller-owned) MaterialInstanceResource, not in a Renderer-owned
        // cache, so there's nothing here to store except the lock itself. Real bug this fixes: unlike
        // material_pipeline_for(), prepare_material_frame() had no lock at all until this was added —
        // fine for the pre-existing render-bundle parallel-recording path (RendererLifecycle.cpp
        // explicitly pre-warms every distinct material single-threaded before going parallel, so its
        // workers only ever see frame.bind_groups_dirty already false), but RenderGraph::execute_parallel
        // (Stage 4 of the render-parallelization roadmap) can run two entirely different passes
        // concurrently with no such pre-warm — if they share a material neither has touched yet this
        // frame, both could race into the same MaterialInstanceFrame's dirty-rebuild block at once
        // (concurrent bind_groups.clear()/push_back, concurrent double destroy_bind_group on the same
        // handles) — real heap corruption, reproduced and root-caused this session (see memory
        // project_render_threading for the crash signatures this explains).
        Async::Mutex<u8> material_frame_prepare_lock_;
        // Guards every push_back into a frame's shared `transient_bind_groups` vector (owned by the
        // caller — FrameSubmission/render_frame_rhi's local, threaded by reference through
        // record_hiz_build/record_shadow_lighting/record_instanced_batches/bloom/tonemap/custom-post-
        // process/object-history and a couple of inline call sites in RendererLifecycle.cpp). Real bug
        // this fixes, found and root-caused this session (see memory project_render_threading): under
        // RenderGraph::execute_parallel (Stage 4), two of those pass-recording callbacks can now
        // legitimately run on different worker threads at once (e.g. Hi-Z pyramid building has no
        // dependency on deferred shadow lighting's inputs, so they land in the same execution level)
        // — every one of them was calling plain vector::push_back on the *same* vector with zero
        // synchronization, a textbook concurrent-push_back data race and the actual cause of the
        // intermittent heap corruption (varying crash signature: glibc malloc, a validation-layer
        // internal allocator, RADV's own allocator, even a VMA assert during unrelated teardown —
        // classic "corruption surfaces wherever the next heap operation happens to look") that
        // survived even after fixing the also-real-but-insufficient prepare_material_frame race.
        Async::Mutex<u8> transient_bind_groups_lock_;
        // CPU-side history for real per-object motion vectors (SceneObjectGpuData::previous_model) —
        // keyed by RenderItem::stable_id, a persistent per-object identity (for real ECS content,
        // `(entity.generation << 32) | entity.index` — see EcsRendering.cpp), not object_index (which
        // is only this frame's packing position). Window render threads can dispatch concurrently,
        // so access is guarded while each submission snapshots its previous transform and after scene
        // packing commits its current transforms. Packing itself reads the stamped RenderItem field,
        // never this shared map.
        Async::Mutex<std::unordered_map<u64, glm::mat4>> previous_world_transforms_;
        Async::Mutex<HiZBuildResources> hiz_build_;
        Async::Mutex<AtmosphereLutResources> atmosphere_lut_;
        bool initialized_ = false;
        bool recovering_from_device_loss_ = false;
    };

} // namespace SFT::Renderer
