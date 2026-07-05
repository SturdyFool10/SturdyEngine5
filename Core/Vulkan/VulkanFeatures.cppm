// GENERATED — the vendor-agnostic Vulkan extension catalog (features) plus composed graphics
// techniques (superfeatures). Features map fine-grained application capabilities onto the
// extensions that back them; each extension entry carries its init stage (instance vs device),
// how badly the feature wants it (MustHave vs NiceToHave), and its core-promotion version.
//
// Scope & curation (Vulkan 1.4.350 registry, VK_HEADER_VERSION 350):
//   * Only VK_KHR_* / VK_EXT_* (vendor-agnostic); single-vendor extensions excluded.
//   * Extensions wholly superseded (registry promotedto=<extension>/deprecatedby/obsoletedby) are
//     dropped in favor of their successor.
//   * Basic surface-creation extensions (VK_KHR_surface and the per-window-system VK_*_surface
//     extensions) are NOT listed: the windowing library reports the exact set to enable via its
//     required-instance-extensions query, so they are enabled from there, not selected here.
//   * Capabilities are fine-grained (e.g. ShaderAtomics, YcbcrMultiplanar, PresentTiming) so an app
//     enables exactly what it uses — surplus extensions can cost performance.
//
// Superfeatures (VulkanTechnique) bundle several features into a graphics technique (e.g.
// RayTracedRendering, GpuDrivenRendering), each member tagged MustHave/NiceToHave for that technique.
//
// Base capabilities (base == true) — engine will not initialize without them: CoreRendering
// (dynamic rendering + synchronization2, core in 1.3), GpuTiming (calibrated timestamps), plus
// Presentation (swapchain; the surface itself comes from the windowing library).
//
// Do not hand-edit the tables below; regenerate from the matching registry vk.xml.
module;
#include "volk.h"

#include <array>
#include <span>
#include <string_view>

export module Sturdy.Core:VulkanFeatures;

import Sturdy.Foundation;

using std::array;
using std::span;
using std::string_view;

export namespace SFT::Core::Vulkan {

    enum class VulkanInitStage : u8 {
        Instance,
        Device,
    };

    enum class VulkanExtensionNecessity : u8 {
        MustHave,
        NiceToHave,
    };

    enum class VulkanAppFeature : u8 {
        CoreRendering, // base
        GpuTiming, // base
        Presentation, // base
        DirectDisplay,
        PresentTiming,
        SwapchainCapabilities,
        HdrOutput,
        RayTracing,
        MeshShading,
        ShadingRateAndDensity,
        VideoDecodeH264,
        VideoDecodeH265,
        VideoDecodeAV1,
        VideoDecodeVP9,
        VideoEncodeH264,
        VideoEncodeH265,
        VideoEncodeAV1,
        VideoEnhancements,
        BindlessAndDescriptors,
        DynamicState,
        Synchronization,
        ExternalMemory,
        BufferDeviceAddress,
        MemoryAllocationControl,
        MultiGpuDeviceGroup,
        MemoryReporting,
        ExternalInterop,
        PipelineManagement,
        DeviceMaintenance,
        DataTransferAndCopy,
        CommandBufferRecording,
        PrivateData,
        ImageLayoutManagement,
        DrawingCommands,
        DepthClipAndClamp,
        RasterizationModes,
        BlendingAndFramebufferFetch,
        VertexInput,
        TransformFeedback,
        QueriesAndStatistics,
        ShaderObject,
        ShaderNumericTypes,
        ShaderAtomics,
        ShaderSubgroupOps,
        ShaderMemoryModel,
        ShaderFloatControls,
        ShaderInvocationControl,
        ComputeShaderFeatures,
        ShaderStageIO,
        ShaderTooling,
        ImageFormats,
        SamplingAndFiltering,
        ImageViewsAndAccess,
        YcbcrMultiplanar,
        RenderPassAndAttachments,
        DebugTooling,
        DeviceIntrospection,
        QueueManagement,
        RobustnessAndSafety,
        Portability,
    };

    // A composed graphics technique (superfeature) — a bundle of VulkanAppFeatures.
    enum class VulkanTechnique : u8 {
        RayTracedRendering,
        GpuDrivenRendering,
        MeshShadingPipeline,
        DeferredShading,
        HdrPresentation,
        BindlessRendering,
        GpuComputeAcceleration,
        MediaPlayback,
        VariableRateShading,
        OrderIndependentTransparency,
        VirtualReality,
        PostProcessing,
        GpuParticleSimulation,
    };

    // One extension a feature depends on, tagged with where and how badly it is wanted.
    struct VulkanExtensionRequirement {
        const char *name;                    // canonical registry name (feeds ppEnabledExtensionNames)
        VulkanInitStage stage;
        VulkanExtensionNecessity necessity;
        u32 promoted_to_core;                // API version this became core (0 = never)
        string_view purpose;

        // Whether this name still has to be requested at the negotiated API version: once promoted to
        // core the name is redundant (and some drivers stop advertising it), so it must not be pushed.
        [[nodiscard]] constexpr bool needs_request(u32 api_version) const noexcept {
            return promoted_to_core == 0 || api_version < promoted_to_core;
        }
    };

    // The extensions backing a single capability; stage/necessity segmentation lives per entry.
    struct FeatureVulkanExtensionList {
        VulkanAppFeature feature;
        string_view display_name;
        bool base;
        span<const VulkanExtensionRequirement> extensions;

        [[nodiscard]] constexpr u32 count(VulkanInitStage stage, VulkanExtensionNecessity necessity) const noexcept {
            u32 n = 0;
            for (const auto &ext : extensions) {
                if (ext.stage == stage && ext.necessity == necessity) { ++n; }
            }
            return n;
        }
        [[nodiscard]] constexpr bool has_must_have() const noexcept {
            for (const auto &ext : extensions) {
                if (ext.necessity == VulkanExtensionNecessity::MustHave) { return true; }
            }
            return false;
        }
    };

    // One feature within a technique, tagged with how badly the technique wants it.
    struct TechniqueFeature {
        VulkanAppFeature feature;
        VulkanExtensionNecessity necessity;
    };

    // A technique (superfeature) and the features it comprises.
    struct VulkanTechniqueGroup {
        VulkanTechnique technique;
        string_view display_name;
        span<const TechniqueFeature> features;
    };

    namespace Detail {

        inline constexpr array core_rendering_exts{
        VulkanExtensionRequirement{"VK_EXT_dynamic_rendering_unused_attachments", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "dynamic rendering unused attachments"},
        VulkanExtensionRequirement{"VK_KHR_dynamic_rendering", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, VK_API_VERSION_1_3, "dynamic rendering"},
        VulkanExtensionRequirement{"VK_KHR_dynamic_rendering_local_read", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "dynamic rendering local read"},
        VulkanExtensionRequirement{"VK_KHR_synchronization2", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, VK_API_VERSION_1_3, "synchronization2"},
        };

        inline constexpr array gpu_timing_exts{
        VulkanExtensionRequirement{"VK_KHR_calibrated_timestamps", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "calibrated timestamps"},
        };

        inline constexpr array presentation_exts{
        VulkanExtensionRequirement{"VK_KHR_swapchain", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "swapchain"},
        };

        inline constexpr array direct_display_exts{
        VulkanExtensionRequirement{"VK_EXT_acquire_drm_display", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "acquire drm display"},
        VulkanExtensionRequirement{"VK_EXT_acquire_xlib_display", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "acquire xlib display"},
        VulkanExtensionRequirement{"VK_EXT_direct_mode_display", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "direct mode display"},
        VulkanExtensionRequirement{"VK_KHR_display", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "display"},
        VulkanExtensionRequirement{"VK_EXT_display_control", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "display control"},
        VulkanExtensionRequirement{"VK_EXT_display_surface_counter", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "display surface counter"},
        VulkanExtensionRequirement{"VK_KHR_display_swapchain", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "display swapchain"},
        VulkanExtensionRequirement{"VK_KHR_get_display_properties2", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "get display properties2"},
        };

        inline constexpr array present_timing_exts{
        VulkanExtensionRequirement{"VK_KHR_incremental_present", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "incremental present"},
        VulkanExtensionRequirement{"VK_KHR_present_id", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "present id"},
        VulkanExtensionRequirement{"VK_KHR_present_id2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "present id2"},
        VulkanExtensionRequirement{"VK_KHR_present_mode_fifo_latest_ready", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "present mode fifo latest ready"},
        VulkanExtensionRequirement{"VK_EXT_present_timing", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "present timing"},
        VulkanExtensionRequirement{"VK_KHR_present_wait", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "present wait"},
        VulkanExtensionRequirement{"VK_KHR_present_wait2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "present wait2"},
        };

        inline constexpr array swapchain_capabilities_exts{
        VulkanExtensionRequirement{"VK_EXT_full_screen_exclusive", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "full screen exclusive"},
        VulkanExtensionRequirement{"VK_KHR_get_surface_capabilities2", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "get surface capabilities2"},
        VulkanExtensionRequirement{"VK_EXT_image_compression_control_swapchain", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "image compression control swapchain"},
        VulkanExtensionRequirement{"VK_KHR_shared_presentable_image", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shared presentable image"},
        VulkanExtensionRequirement{"VK_KHR_surface_maintenance1", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "surface maintenance1"},
        VulkanExtensionRequirement{"VK_KHR_surface_protected_capabilities", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "surface protected capabilities"},
        VulkanExtensionRequirement{"VK_KHR_swapchain_maintenance1", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "swapchain maintenance1"},
        VulkanExtensionRequirement{"VK_KHR_swapchain_mutable_format", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "swapchain mutable format"},
        };

        inline constexpr array hdr_output_exts{
        VulkanExtensionRequirement{"VK_EXT_hdr_metadata", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "hdr metadata"},
        VulkanExtensionRequirement{"VK_EXT_swapchain_colorspace", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "swapchain colorspace"},
        VulkanExtensionRequirement{"VK_EXT_texture_compression_astc_hdr", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "texture compression astc hdr"},
        };

        inline constexpr array ray_tracing_exts{
        VulkanExtensionRequirement{"VK_EXT_graphics_pipeline_library", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "graphics pipeline library"},
        VulkanExtensionRequirement{"VK_EXT_opacity_micromap", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "opacity micromap"},
        VulkanExtensionRequirement{"VK_EXT_pipeline_library_group_handles", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "pipeline library group handles"},
        VulkanExtensionRequirement{"VK_EXT_ray_tracing_invocation_reorder", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "ray tracing invocation reorder"},
        VulkanExtensionRequirement{"VK_KHR_acceleration_structure", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "acceleration structure"},
        VulkanExtensionRequirement{"VK_KHR_deferred_host_operations", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "deferred host operations"},
        VulkanExtensionRequirement{"VK_KHR_pipeline_library", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "pipeline library"},
        VulkanExtensionRequirement{"VK_KHR_ray_query", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "ray query"},
        VulkanExtensionRequirement{"VK_KHR_ray_tracing_maintenance1", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "ray tracing maintenance1"},
        VulkanExtensionRequirement{"VK_KHR_ray_tracing_pipeline", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "ray tracing pipeline"},
        VulkanExtensionRequirement{"VK_KHR_ray_tracing_position_fetch", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "ray tracing position fetch"},
        };

        inline constexpr array mesh_shading_exts{
        VulkanExtensionRequirement{"VK_EXT_mesh_shader", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "mesh shader"},
        };

        inline constexpr array shading_rate_and_density_exts{
        VulkanExtensionRequirement{"VK_EXT_fragment_density_map", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "fragment density map"},
        VulkanExtensionRequirement{"VK_EXT_fragment_density_map2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "fragment density map2"},
        VulkanExtensionRequirement{"VK_EXT_fragment_density_map_offset", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "fragment density map offset"},
        VulkanExtensionRequirement{"VK_EXT_fragment_shader_interlock", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "fragment shader interlock"},
        VulkanExtensionRequirement{"VK_KHR_fragment_shader_barycentric", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "fragment shader barycentric"},
        VulkanExtensionRequirement{"VK_KHR_fragment_shading_rate", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "fragment shading rate"},
        };

        inline constexpr array video_decode_h264_exts{
        VulkanExtensionRequirement{"VK_KHR_video_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video queue"},
        VulkanExtensionRequirement{"VK_KHR_video_decode_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video decode queue"},
        VulkanExtensionRequirement{"VK_KHR_video_decode_h264", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video decode h264"},
        };

        inline constexpr array video_decode_h265_exts{
        VulkanExtensionRequirement{"VK_KHR_video_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video queue"},
        VulkanExtensionRequirement{"VK_KHR_video_decode_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video decode queue"},
        VulkanExtensionRequirement{"VK_KHR_video_decode_h265", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video decode h265"},
        };

        inline constexpr array video_decode_av1_exts{
        VulkanExtensionRequirement{"VK_KHR_video_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video queue"},
        VulkanExtensionRequirement{"VK_KHR_video_decode_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video decode queue"},
        VulkanExtensionRequirement{"VK_KHR_video_decode_av1", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video decode av1"},
        };

        inline constexpr array video_decode_vp9_exts{
        VulkanExtensionRequirement{"VK_KHR_video_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video queue"},
        VulkanExtensionRequirement{"VK_KHR_video_decode_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video decode queue"},
        VulkanExtensionRequirement{"VK_KHR_video_decode_vp9", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video decode vp9"},
        };

        inline constexpr array video_encode_h264_exts{
        VulkanExtensionRequirement{"VK_KHR_video_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video queue"},
        VulkanExtensionRequirement{"VK_KHR_video_encode_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video encode queue"},
        VulkanExtensionRequirement{"VK_KHR_video_encode_h264", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video encode h264"},
        };

        inline constexpr array video_encode_h265_exts{
        VulkanExtensionRequirement{"VK_KHR_video_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video queue"},
        VulkanExtensionRequirement{"VK_KHR_video_encode_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video encode queue"},
        VulkanExtensionRequirement{"VK_KHR_video_encode_h265", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video encode h265"},
        };

        inline constexpr array video_encode_av1_exts{
        VulkanExtensionRequirement{"VK_KHR_video_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video queue"},
        VulkanExtensionRequirement{"VK_KHR_video_encode_queue", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video encode queue"},
        VulkanExtensionRequirement{"VK_KHR_video_encode_av1", VulkanInitStage::Device, VulkanExtensionNecessity::MustHave, 0, "video encode av1"},
        };

        inline constexpr array video_enhancements_exts{
        VulkanExtensionRequirement{"VK_KHR_video_maintenance1", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "video maintenance1"},
        VulkanExtensionRequirement{"VK_KHR_video_maintenance2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "video maintenance2"},
        VulkanExtensionRequirement{"VK_KHR_video_encode_intra_refresh", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "video encode intra refresh"},
        VulkanExtensionRequirement{"VK_KHR_video_encode_quantization_map", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "video encode quantization map"},
        };

        inline constexpr array bindless_and_descriptors_exts{
        VulkanExtensionRequirement{"VK_EXT_descriptor_heap", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "descriptor heap"},
        VulkanExtensionRequirement{"VK_EXT_descriptor_indexing", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "descriptor indexing"},
        VulkanExtensionRequirement{"VK_EXT_inline_uniform_block", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "inline uniform block"},
        VulkanExtensionRequirement{"VK_EXT_mutable_descriptor_type", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "mutable descriptor type"},
        VulkanExtensionRequirement{"VK_KHR_descriptor_update_template", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "descriptor update template"},
        VulkanExtensionRequirement{"VK_KHR_push_descriptor", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "push descriptor"},
        };

        inline constexpr array dynamic_state_exts{
        VulkanExtensionRequirement{"VK_EXT_attachment_feedback_loop_dynamic_state", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "attachment feedback loop dynamic state"},
        VulkanExtensionRequirement{"VK_EXT_color_write_enable", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "color write enable"},
        VulkanExtensionRequirement{"VK_EXT_depth_clip_control", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "depth clip control"},
        VulkanExtensionRequirement{"VK_EXT_extended_dynamic_state", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "extended dynamic state"},
        VulkanExtensionRequirement{"VK_EXT_extended_dynamic_state2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "extended dynamic state2"},
        VulkanExtensionRequirement{"VK_EXT_extended_dynamic_state3", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "extended dynamic state3"},
        VulkanExtensionRequirement{"VK_EXT_vertex_input_dynamic_state", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "vertex input dynamic state"},
        };

        inline constexpr array synchronization_exts{
        VulkanExtensionRequirement{"VK_KHR_external_fence", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "external fence"},
        VulkanExtensionRequirement{"VK_KHR_external_fence_capabilities", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "external fence capabilities"},
        VulkanExtensionRequirement{"VK_KHR_external_fence_fd", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "external fence fd"},
        VulkanExtensionRequirement{"VK_KHR_external_fence_win32", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "external fence win32"},
        VulkanExtensionRequirement{"VK_KHR_external_semaphore", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "external semaphore"},
        VulkanExtensionRequirement{"VK_KHR_external_semaphore_capabilities", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "external semaphore capabilities"},
        VulkanExtensionRequirement{"VK_KHR_external_semaphore_fd", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "external semaphore fd"},
        VulkanExtensionRequirement{"VK_KHR_external_semaphore_win32", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "external semaphore win32"},
        VulkanExtensionRequirement{"VK_KHR_timeline_semaphore", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "timeline semaphore"},
        };

        inline constexpr array external_memory_exts{
        VulkanExtensionRequirement{"VK_KHR_external_memory", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "external memory"},
        VulkanExtensionRequirement{"VK_KHR_external_memory_capabilities", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "external memory capabilities"},
        VulkanExtensionRequirement{"VK_KHR_external_memory_fd", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "external memory fd"},
        VulkanExtensionRequirement{"VK_KHR_external_memory_win32", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "external memory win32"},
        VulkanExtensionRequirement{"VK_EXT_external_memory_dma_buf", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "external memory dma buf"},
        VulkanExtensionRequirement{"VK_EXT_external_memory_host", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "external memory host"},
        VulkanExtensionRequirement{"VK_EXT_external_memory_metal", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "external memory metal"},
        VulkanExtensionRequirement{"VK_EXT_external_memory_acquire_unmodified", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "external memory acquire unmodified"},
        };

        inline constexpr array buffer_device_address_exts{
        VulkanExtensionRequirement{"VK_KHR_buffer_device_address", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "buffer device address"},
        VulkanExtensionRequirement{"VK_KHR_device_address_commands", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "device address commands"},
        };

        inline constexpr array memory_allocation_control_exts{
        VulkanExtensionRequirement{"VK_KHR_dedicated_allocation", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "dedicated allocation"},
        VulkanExtensionRequirement{"VK_EXT_memory_budget", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "memory budget"},
        VulkanExtensionRequirement{"VK_EXT_memory_priority", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "memory priority"},
        VulkanExtensionRequirement{"VK_EXT_pageable_device_local_memory", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "pageable device local memory"},
        VulkanExtensionRequirement{"VK_KHR_map_memory2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "map memory2"},
        VulkanExtensionRequirement{"VK_EXT_map_memory_placed", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "map memory placed"},
        VulkanExtensionRequirement{"VK_KHR_bind_memory2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "bind memory2"},
        VulkanExtensionRequirement{"VK_KHR_get_memory_requirements2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "get memory requirements2"},
        VulkanExtensionRequirement{"VK_EXT_zero_initialize_device_memory", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "zero initialize device memory"},
        VulkanExtensionRequirement{"VK_EXT_memory_decompression", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "memory decompression"},
        };

        inline constexpr array multi_gpu_device_group_exts{
        VulkanExtensionRequirement{"VK_KHR_device_group", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "device group"},
        VulkanExtensionRequirement{"VK_KHR_device_group_creation", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "device group creation"},
        };

        inline constexpr array memory_reporting_exts{
        VulkanExtensionRequirement{"VK_EXT_device_memory_report", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "device memory report"},
        VulkanExtensionRequirement{"VK_EXT_device_address_binding_report", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "device address binding report"},
        };

        inline constexpr array external_interop_exts{
        VulkanExtensionRequirement{"VK_EXT_metal_objects", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "metal objects"},
        VulkanExtensionRequirement{"VK_KHR_win32_keyed_mutex", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "win32 keyed mutex"},
        };

        inline constexpr array pipeline_management_exts{
        VulkanExtensionRequirement{"VK_EXT_device_generated_commands", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "device generated commands"},
        VulkanExtensionRequirement{"VK_EXT_pipeline_creation_cache_control", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "pipeline creation cache control"},
        VulkanExtensionRequirement{"VK_EXT_pipeline_creation_feedback", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "pipeline creation feedback"},
        VulkanExtensionRequirement{"VK_EXT_pipeline_properties", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "pipeline properties"},
        VulkanExtensionRequirement{"VK_EXT_pipeline_protected_access", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "pipeline protected access"},
        VulkanExtensionRequirement{"VK_EXT_pipeline_robustness", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "pipeline robustness"},
        VulkanExtensionRequirement{"VK_KHR_pipeline_binary", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "pipeline binary"},
        VulkanExtensionRequirement{"VK_KHR_pipeline_executable_properties", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "pipeline executable properties"},
        };

        inline constexpr array device_maintenance_exts{
        VulkanExtensionRequirement{"VK_KHR_maintenance1", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "maintenance1"},
        VulkanExtensionRequirement{"VK_KHR_maintenance2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "maintenance2"},
        VulkanExtensionRequirement{"VK_KHR_maintenance3", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "maintenance3"},
        VulkanExtensionRequirement{"VK_KHR_maintenance4", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "maintenance4"},
        VulkanExtensionRequirement{"VK_KHR_maintenance5", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "maintenance5"},
        VulkanExtensionRequirement{"VK_KHR_maintenance6", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "maintenance6"},
        VulkanExtensionRequirement{"VK_KHR_maintenance7", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "maintenance7"},
        VulkanExtensionRequirement{"VK_KHR_maintenance8", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "maintenance8"},
        VulkanExtensionRequirement{"VK_KHR_maintenance9", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "maintenance9"},
        VulkanExtensionRequirement{"VK_KHR_maintenance10", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "maintenance10"},
        VulkanExtensionRequirement{"VK_KHR_maintenance11", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "maintenance11"},
        };

        inline constexpr array data_transfer_and_copy_exts{
        VulkanExtensionRequirement{"VK_KHR_copy_commands2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "copy commands2"},
        VulkanExtensionRequirement{"VK_KHR_copy_memory_indirect", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "copy memory indirect"},
        VulkanExtensionRequirement{"VK_EXT_host_image_copy", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "host image copy"},
        };

        inline constexpr array command_buffer_recording_exts{
        VulkanExtensionRequirement{"VK_EXT_nested_command_buffer", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "nested command buffer"},
        };

        inline constexpr array private_data_exts{
        VulkanExtensionRequirement{"VK_EXT_private_data", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "private data"},
        };

        inline constexpr array image_layout_management_exts{
        VulkanExtensionRequirement{"VK_KHR_unified_image_layouts", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "unified image layouts"},
        };

        inline constexpr array drawing_commands_exts{
        VulkanExtensionRequirement{"VK_EXT_conditional_rendering", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "conditional rendering"},
        VulkanExtensionRequirement{"VK_EXT_multi_draw", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "multi draw"},
        VulkanExtensionRequirement{"VK_EXT_primitive_restart_index", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "primitive restart index"},
        VulkanExtensionRequirement{"VK_EXT_primitive_topology_list_restart", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "primitive topology list restart"},
        VulkanExtensionRequirement{"VK_KHR_draw_indirect_count", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "draw indirect count"},
        VulkanExtensionRequirement{"VK_KHR_index_type_uint8", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "index type uint8"},
        };

        inline constexpr array depth_clip_and_clamp_exts{
        VulkanExtensionRequirement{"VK_EXT_depth_bias_control", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "depth bias control"},
        VulkanExtensionRequirement{"VK_EXT_depth_clamp_control", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "depth clamp control"},
        VulkanExtensionRequirement{"VK_EXT_depth_clip_enable", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "depth clip enable"},
        VulkanExtensionRequirement{"VK_EXT_depth_range_unrestricted", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "depth range unrestricted"},
        VulkanExtensionRequirement{"VK_EXT_post_depth_coverage", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "post depth coverage"},
        VulkanExtensionRequirement{"VK_KHR_depth_clamp_zero_one", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "depth clamp zero one"},
        };

        inline constexpr array rasterization_modes_exts{
        VulkanExtensionRequirement{"VK_EXT_conservative_rasterization", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "conservative rasterization"},
        VulkanExtensionRequirement{"VK_EXT_discard_rectangles", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "discard rectangles"},
        VulkanExtensionRequirement{"VK_EXT_provoking_vertex", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "provoking vertex"},
        VulkanExtensionRequirement{"VK_EXT_sample_locations", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "sample locations"},
        VulkanExtensionRequirement{"VK_KHR_line_rasterization", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "line rasterization"},
        };

        inline constexpr array blending_and_framebuffer_fetch_exts{
        VulkanExtensionRequirement{"VK_EXT_blend_operation_advanced", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "blend operation advanced"},
        VulkanExtensionRequirement{"VK_EXT_shader_tile_image", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader tile image"},
        VulkanExtensionRequirement{"VK_EXT_attachment_feedback_loop_layout", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "attachment feedback loop layout"},
        VulkanExtensionRequirement{"VK_EXT_rasterization_order_attachment_access", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "rasterization order attachment access"},
        };

        inline constexpr array vertex_input_exts{
        VulkanExtensionRequirement{"VK_KHR_vertex_attribute_divisor", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "vertex attribute divisor"},
        VulkanExtensionRequirement{"VK_EXT_legacy_vertex_attributes", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "legacy vertex attributes"},
        VulkanExtensionRequirement{"VK_EXT_legacy_dithering", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "legacy dithering"},
        };

        inline constexpr array transform_feedback_exts{
        VulkanExtensionRequirement{"VK_EXT_transform_feedback", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "transform feedback"},
        };

        inline constexpr array queries_and_statistics_exts{
        VulkanExtensionRequirement{"VK_EXT_host_query_reset", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "host query reset"},
        VulkanExtensionRequirement{"VK_EXT_primitives_generated_query", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "primitives generated query"},
        VulkanExtensionRequirement{"VK_KHR_performance_query", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "performance query"},
        };

        inline constexpr array shader_object_exts{
        VulkanExtensionRequirement{"VK_EXT_shader_object", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader object"},
        };

        inline constexpr array shader_numeric_types_exts{
        VulkanExtensionRequirement{"VK_KHR_16bit_storage", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "16bit storage"},
        VulkanExtensionRequirement{"VK_KHR_8bit_storage", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "8bit storage"},
        VulkanExtensionRequirement{"VK_KHR_shader_float16_int8", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "shader float16 int8"},
        VulkanExtensionRequirement{"VK_KHR_shader_bfloat16", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader bfloat16"},
        VulkanExtensionRequirement{"VK_EXT_shader_float8", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader float8"},
        VulkanExtensionRequirement{"VK_EXT_shader_long_vector", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader long vector"},
        VulkanExtensionRequirement{"VK_EXT_shader_replicated_composites", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader replicated composites"},
        };

        inline constexpr array shader_atomics_exts{
        VulkanExtensionRequirement{"VK_KHR_shader_atomic_int64", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "shader atomic int64"},
        VulkanExtensionRequirement{"VK_EXT_shader_atomic_float", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader atomic float"},
        VulkanExtensionRequirement{"VK_EXT_shader_atomic_float2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader atomic float2"},
        VulkanExtensionRequirement{"VK_EXT_shader_image_atomic_int64", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader image atomic int64"},
        };

        inline constexpr array shader_subgroup_ops_exts{
        VulkanExtensionRequirement{"VK_EXT_subgroup_size_control", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "subgroup size control"},
        VulkanExtensionRequirement{"VK_EXT_shader_subgroup_partitioned", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader subgroup partitioned"},
        VulkanExtensionRequirement{"VK_KHR_shader_subgroup_extended_types", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "shader subgroup extended types"},
        VulkanExtensionRequirement{"VK_KHR_shader_subgroup_rotate", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "shader subgroup rotate"},
        VulkanExtensionRequirement{"VK_KHR_shader_subgroup_uniform_control_flow", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader subgroup uniform control flow"},
        VulkanExtensionRequirement{"VK_KHR_shader_quad_control", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader quad control"},
        VulkanExtensionRequirement{"VK_KHR_shader_maximal_reconvergence", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader maximal reconvergence"},
        };

        inline constexpr array shader_memory_model_exts{
        VulkanExtensionRequirement{"VK_KHR_vulkan_memory_model", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "vulkan memory model"},
        VulkanExtensionRequirement{"VK_EXT_scalar_block_layout", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "scalar block layout"},
        VulkanExtensionRequirement{"VK_KHR_relaxed_block_layout", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "relaxed block layout"},
        VulkanExtensionRequirement{"VK_KHR_uniform_buffer_standard_layout", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "uniform buffer standard layout"},
        VulkanExtensionRequirement{"VK_KHR_storage_buffer_storage_class", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "storage buffer storage class"},
        VulkanExtensionRequirement{"VK_KHR_variable_pointers", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "variable pointers"},
        VulkanExtensionRequirement{"VK_KHR_workgroup_memory_explicit_layout", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "workgroup memory explicit layout"},
        VulkanExtensionRequirement{"VK_KHR_zero_initialize_workgroup_memory", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "zero initialize workgroup memory"},
        VulkanExtensionRequirement{"VK_KHR_shader_untyped_pointers", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader untyped pointers"},
        };

        inline constexpr array shader_float_controls_exts{
        VulkanExtensionRequirement{"VK_KHR_shader_float_controls", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "shader float controls"},
        VulkanExtensionRequirement{"VK_KHR_shader_float_controls2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "shader float controls2"},
        VulkanExtensionRequirement{"VK_KHR_shader_fma", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader fma"},
        };

        inline constexpr array shader_invocation_control_exts{
        VulkanExtensionRequirement{"VK_EXT_shader_demote_to_helper_invocation", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "shader demote to helper invocation"},
        VulkanExtensionRequirement{"VK_KHR_shader_terminate_invocation", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "shader terminate invocation"},
        VulkanExtensionRequirement{"VK_KHR_shader_abort", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader abort"},
        VulkanExtensionRequirement{"VK_KHR_shader_expect_assume", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "shader expect assume"},
        };

        inline constexpr array compute_shader_features_exts{
        VulkanExtensionRequirement{"VK_KHR_compute_shader_derivatives", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "compute shader derivatives"},
        VulkanExtensionRequirement{"VK_KHR_cooperative_matrix", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "cooperative matrix"},
        VulkanExtensionRequirement{"VK_KHR_shader_integer_dot_product", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "shader integer dot product"},
        };

        inline constexpr array shader_stage_io_exts{
        VulkanExtensionRequirement{"VK_EXT_shader_stencil_export", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader stencil export"},
        VulkanExtensionRequirement{"VK_EXT_shader_viewport_index_layer", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "shader viewport index layer"},
        VulkanExtensionRequirement{"VK_KHR_shader_draw_parameters", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "shader draw parameters"},
        VulkanExtensionRequirement{"VK_KHR_shader_constant_data", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader constant data"},
        VulkanExtensionRequirement{"VK_EXT_shader_uniform_buffer_unsized_array", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader uniform buffer unsized array"},
        VulkanExtensionRequirement{"VK_EXT_shader_64bit_indexing", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader 64bit indexing"},
        };

        inline constexpr array shader_tooling_exts{
        VulkanExtensionRequirement{"VK_KHR_shader_non_semantic_info", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "shader non semantic info"},
        VulkanExtensionRequirement{"VK_EXT_shader_module_identifier", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader module identifier"},
        VulkanExtensionRequirement{"VK_KHR_shader_relaxed_extended_instruction", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader relaxed extended instruction"},
        VulkanExtensionRequirement{"VK_KHR_spirv_1_4", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "spirv 1 4"},
        VulkanExtensionRequirement{"VK_KHR_shader_clock", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "shader clock"},
        };

        inline constexpr array image_formats_exts{
        VulkanExtensionRequirement{"VK_EXT_4444_formats", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "4444 formats"},
        VulkanExtensionRequirement{"VK_EXT_astc_decode_mode", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "astc decode mode"},
        VulkanExtensionRequirement{"VK_EXT_image_compression_control", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "image compression control"},
        VulkanExtensionRequirement{"VK_EXT_image_drm_format_modifier", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "image drm format modifier"},
        VulkanExtensionRequirement{"VK_EXT_rgba10x6_formats", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "rgba10x6 formats"},
        VulkanExtensionRequirement{"VK_EXT_texture_compression_astc_3d", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "texture compression astc 3d"},
        VulkanExtensionRequirement{"VK_KHR_format_feature_flags2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "format feature flags2"},
        VulkanExtensionRequirement{"VK_KHR_image_format_list", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "image format list"},
        };

        inline constexpr array sampling_and_filtering_exts{
        VulkanExtensionRequirement{"VK_EXT_border_color_swizzle", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "border color swizzle"},
        VulkanExtensionRequirement{"VK_EXT_custom_border_color", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "custom border color"},
        VulkanExtensionRequirement{"VK_EXT_filter_cubic", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "filter cubic"},
        VulkanExtensionRequirement{"VK_EXT_non_seamless_cube_map", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "non seamless cube map"},
        VulkanExtensionRequirement{"VK_EXT_sampler_filter_minmax", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "sampler filter minmax"},
        VulkanExtensionRequirement{"VK_KHR_sampler_mirror_clamp_to_edge", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "sampler mirror clamp to edge"},
        };

        inline constexpr array image_views_and_access_exts{
        VulkanExtensionRequirement{"VK_EXT_image_2d_view_of_3d", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "image 2d view of 3d"},
        VulkanExtensionRequirement{"VK_EXT_image_sliced_view_of_3d", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "image sliced view of 3d"},
        VulkanExtensionRequirement{"VK_EXT_image_view_min_lod", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "image view min lod"},
        VulkanExtensionRequirement{"VK_EXT_image_robustness", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "image robustness"},
        VulkanExtensionRequirement{"VK_EXT_texel_buffer_alignment", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "texel buffer alignment"},
        };

        inline constexpr array ycbcr_multiplanar_exts{
        VulkanExtensionRequirement{"VK_KHR_sampler_ycbcr_conversion", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "sampler ycbcr conversion"},
        VulkanExtensionRequirement{"VK_EXT_ycbcr_2plane_444_formats", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "ycbcr 2plane 444 formats"},
        VulkanExtensionRequirement{"VK_EXT_ycbcr_image_arrays", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "ycbcr image arrays"},
        };

        inline constexpr array render_pass_and_attachments_exts{
        VulkanExtensionRequirement{"VK_KHR_create_renderpass2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "create renderpass2"},
        VulkanExtensionRequirement{"VK_KHR_depth_stencil_resolve", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "depth stencil resolve"},
        VulkanExtensionRequirement{"VK_KHR_imageless_framebuffer", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "imageless framebuffer"},
        VulkanExtensionRequirement{"VK_KHR_load_store_op_none", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "load store op none"},
        VulkanExtensionRequirement{"VK_KHR_multiview", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "multiview"},
        VulkanExtensionRequirement{"VK_KHR_separate_depth_stencil_layouts", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "separate depth stencil layouts"},
        VulkanExtensionRequirement{"VK_EXT_separate_stencil_usage", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "separate stencil usage"},
        VulkanExtensionRequirement{"VK_EXT_subpass_merge_feedback", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "subpass merge feedback"},
        VulkanExtensionRequirement{"VK_EXT_multisampled_render_to_single_sampled", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "multisampled render to single sampled"},
        VulkanExtensionRequirement{"VK_EXT_custom_resolve", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "custom resolve"},
        };

        inline constexpr array debug_tooling_exts{
        VulkanExtensionRequirement{"VK_EXT_debug_utils", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "debug utils"},
        VulkanExtensionRequirement{"VK_EXT_frame_boundary", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "frame boundary"},
        VulkanExtensionRequirement{"VK_EXT_layer_settings", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "layer settings"},
        VulkanExtensionRequirement{"VK_EXT_tooling_info", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_3, "tooling info"},
        VulkanExtensionRequirement{"VK_EXT_validation_cache", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "validation cache"},
        VulkanExtensionRequirement{"VK_KHR_device_fault", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "device fault"},
        };

        inline constexpr array device_introspection_exts{
        VulkanExtensionRequirement{"VK_EXT_pci_bus_info", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "pci bus info"},
        VulkanExtensionRequirement{"VK_EXT_physical_device_drm", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "physical device drm"},
        VulkanExtensionRequirement{"VK_KHR_driver_properties", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_2, "driver properties"},
        VulkanExtensionRequirement{"VK_KHR_get_physical_device_properties2", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_1, "get physical device properties2"},
        };

        inline constexpr array queue_management_exts{
        VulkanExtensionRequirement{"VK_EXT_queue_family_foreign", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "queue family foreign"},
        VulkanExtensionRequirement{"VK_KHR_global_priority", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, VK_API_VERSION_1_4, "global priority"},
        VulkanExtensionRequirement{"VK_KHR_internally_synchronized_queues", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "internally synchronized queues"},
        };

        inline constexpr array robustness_and_safety_exts{
        VulkanExtensionRequirement{"VK_KHR_robustness2", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "robustness2"},
        };

        inline constexpr array portability_exts{
        VulkanExtensionRequirement{"VK_KHR_portability_enumeration", VulkanInitStage::Instance, VulkanExtensionNecessity::NiceToHave, 0, "portability enumeration"},
        VulkanExtensionRequirement{"VK_KHR_portability_subset", VulkanInitStage::Device, VulkanExtensionNecessity::NiceToHave, 0, "portability subset"},
        };

        inline constexpr array feature_catalog{
            FeatureVulkanExtensionList{VulkanAppFeature::CoreRendering, "Core Rendering", true, core_rendering_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::GpuTiming, "GPU Timing", true, gpu_timing_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::Presentation, "Presentation", true, presentation_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::DirectDisplay, "Direct Display (KMS)", false, direct_display_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::PresentTiming, "Present Timing & Sync", false, present_timing_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::SwapchainCapabilities, "Swapchain Capabilities", false, swapchain_capabilities_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::HdrOutput, "HDR Output", false, hdr_output_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::RayTracing, "Ray Tracing", false, ray_tracing_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::MeshShading, "Mesh Shading", false, mesh_shading_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ShadingRateAndDensity, "Shading Rate & Density", false, shading_rate_and_density_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::VideoDecodeH264, "H.264 Video Decode", false, video_decode_h264_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::VideoDecodeH265, "H.265 Video Decode", false, video_decode_h265_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::VideoDecodeAV1, "AV1 Video Decode", false, video_decode_av1_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::VideoDecodeVP9, "VP9 Video Decode", false, video_decode_vp9_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::VideoEncodeH264, "H.264 Video Encode", false, video_encode_h264_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::VideoEncodeH265, "H.265 Video Encode", false, video_encode_h265_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::VideoEncodeAV1, "AV1 Video Encode", false, video_encode_av1_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::VideoEnhancements, "Video Enhancements", false, video_enhancements_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::BindlessAndDescriptors, "Bindless & Descriptors", false, bindless_and_descriptors_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::DynamicState, "Dynamic Pipeline State", false, dynamic_state_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::Synchronization, "Synchronization & Semaphores", false, synchronization_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ExternalMemory, "External Memory Sharing", false, external_memory_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::BufferDeviceAddress, "Buffer Device Address", false, buffer_device_address_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::MemoryAllocationControl, "Memory Allocation Control", false, memory_allocation_control_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::MultiGpuDeviceGroup, "Multi-GPU Device Groups", false, multi_gpu_device_group_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::MemoryReporting, "Memory Reporting", false, memory_reporting_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ExternalInterop, "External Interop", false, external_interop_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::PipelineManagement, "Pipeline Management", false, pipeline_management_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::DeviceMaintenance, "Device Maintenance Bundles", false, device_maintenance_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::DataTransferAndCopy, "Data Transfer & Copy", false, data_transfer_and_copy_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::CommandBufferRecording, "Command Buffer Recording", false, command_buffer_recording_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::PrivateData, "Private Data", false, private_data_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ImageLayoutManagement, "Image Layout Management", false, image_layout_management_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::DrawingCommands, "Drawing Commands", false, drawing_commands_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::DepthClipAndClamp, "Depth Clip & Clamp", false, depth_clip_and_clamp_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::RasterizationModes, "Rasterization Modes", false, rasterization_modes_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::BlendingAndFramebufferFetch, "Blending & Framebuffer Fetch", false, blending_and_framebuffer_fetch_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::VertexInput, "Vertex Input", false, vertex_input_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::TransformFeedback, "Transform Feedback", false, transform_feedback_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::QueriesAndStatistics, "Queries & Statistics", false, queries_and_statistics_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ShaderObject, "Shader Objects", false, shader_object_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ShaderNumericTypes, "Shader Numeric Types", false, shader_numeric_types_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ShaderAtomics, "Shader Atomics", false, shader_atomics_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ShaderSubgroupOps, "Shader Subgroup Operations", false, shader_subgroup_ops_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ShaderMemoryModel, "Shader Memory Model & Layout", false, shader_memory_model_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ShaderFloatControls, "Shader Float Controls", false, shader_float_controls_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ShaderInvocationControl, "Shader Invocation Control", false, shader_invocation_control_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ComputeShaderFeatures, "Compute Shader Features", false, compute_shader_features_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ShaderStageIO, "Shader Stage I/O", false, shader_stage_io_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ShaderTooling, "Shader Tooling", false, shader_tooling_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ImageFormats, "Image Formats", false, image_formats_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::SamplingAndFiltering, "Sampling & Filtering", false, sampling_and_filtering_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::ImageViewsAndAccess, "Image Views & Access", false, image_views_and_access_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::YcbcrMultiplanar, "YCbCr & Multiplanar", false, ycbcr_multiplanar_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::RenderPassAndAttachments, "Render Pass & Attachments", false, render_pass_and_attachments_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::DebugTooling, "Debug & Tooling", false, debug_tooling_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::DeviceIntrospection, "Device Introspection", false, device_introspection_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::QueueManagement, "Queue Management", false, queue_management_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::RobustnessAndSafety, "Robustness & Safety", false, robustness_and_safety_exts},
            FeatureVulkanExtensionList{VulkanAppFeature::Portability, "Portability", false, portability_exts},
        };

        inline constexpr array ray_traced_rendering_features{
            TechniqueFeature{VulkanAppFeature::RayTracing, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::BufferDeviceAddress, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::BindlessAndDescriptors, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::ShaderAtomics, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array gpu_driven_rendering_features{
            TechniqueFeature{VulkanAppFeature::DrawingCommands, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::BindlessAndDescriptors, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::BufferDeviceAddress, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::PipelineManagement, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array mesh_shading_pipeline_features{
            TechniqueFeature{VulkanAppFeature::MeshShading, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::ShaderSubgroupOps, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::DrawingCommands, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array deferred_shading_features{
            TechniqueFeature{VulkanAppFeature::RenderPassAndAttachments, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::BlendingAndFramebufferFetch, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::ImageFormats, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array hdr_presentation_features{
            TechniqueFeature{VulkanAppFeature::HdrOutput, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::ImageFormats, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::SamplingAndFiltering, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::ShaderNumericTypes, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array bindless_rendering_features{
            TechniqueFeature{VulkanAppFeature::BindlessAndDescriptors, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::BufferDeviceAddress, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::ShaderMemoryModel, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array gpu_compute_acceleration_features{
            TechniqueFeature{VulkanAppFeature::ComputeShaderFeatures, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::ShaderNumericTypes, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::ShaderAtomics, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::ShaderSubgroupOps, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array media_playback_features{
            TechniqueFeature{VulkanAppFeature::YcbcrMultiplanar, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::VideoDecodeH264, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::VideoDecodeH265, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::VideoDecodeAV1, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::VideoDecodeVP9, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::VideoEnhancements, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array variable_rate_shading_features{
            TechniqueFeature{VulkanAppFeature::ShadingRateAndDensity, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::RenderPassAndAttachments, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array order_independent_transparency_features{
            TechniqueFeature{VulkanAppFeature::ShaderAtomics, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::BlendingAndFramebufferFetch, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::ShadingRateAndDensity, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array virtual_reality_features{
            TechniqueFeature{VulkanAppFeature::RenderPassAndAttachments, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::ShadingRateAndDensity, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::PresentTiming, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::SwapchainCapabilities, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array post_processing_features{
            TechniqueFeature{VulkanAppFeature::ComputeShaderFeatures, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::SamplingAndFiltering, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::ImageViewsAndAccess, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::ShaderNumericTypes, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array gpu_particle_simulation_features{
            TechniqueFeature{VulkanAppFeature::ComputeShaderFeatures, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::ShaderAtomics, VulkanExtensionNecessity::MustHave},
            TechniqueFeature{VulkanAppFeature::DrawingCommands, VulkanExtensionNecessity::NiceToHave},
            TechniqueFeature{VulkanAppFeature::BufferDeviceAddress, VulkanExtensionNecessity::NiceToHave},
        };

        inline constexpr array technique_catalog{
            VulkanTechniqueGroup{VulkanTechnique::RayTracedRendering, "Ray-Traced Rendering", ray_traced_rendering_features},
            VulkanTechniqueGroup{VulkanTechnique::GpuDrivenRendering, "GPU-Driven Rendering", gpu_driven_rendering_features},
            VulkanTechniqueGroup{VulkanTechnique::MeshShadingPipeline, "Mesh Shading Pipeline", mesh_shading_pipeline_features},
            VulkanTechniqueGroup{VulkanTechnique::DeferredShading, "Deferred / Visibility-Buffer Shading", deferred_shading_features},
            VulkanTechniqueGroup{VulkanTechnique::HdrPresentation, "HDR Presentation", hdr_presentation_features},
            VulkanTechniqueGroup{VulkanTechnique::BindlessRendering, "Bindless Rendering", bindless_rendering_features},
            VulkanTechniqueGroup{VulkanTechnique::GpuComputeAcceleration, "GPU Compute Acceleration", gpu_compute_acceleration_features},
            VulkanTechniqueGroup{VulkanTechnique::MediaPlayback, "Media Playback", media_playback_features},
            VulkanTechniqueGroup{VulkanTechnique::VariableRateShading, "Variable Rate Shading", variable_rate_shading_features},
            VulkanTechniqueGroup{VulkanTechnique::OrderIndependentTransparency, "Order-Independent Transparency", order_independent_transparency_features},
            VulkanTechniqueGroup{VulkanTechnique::VirtualReality, "Virtual Reality / Stereo", virtual_reality_features},
            VulkanTechniqueGroup{VulkanTechnique::PostProcessing, "Post-Processing", post_processing_features},
            VulkanTechniqueGroup{VulkanTechnique::GpuParticleSimulation, "GPU Particle Simulation", gpu_particle_simulation_features},
        };

    } // namespace Detail

    [[nodiscard]] constexpr span<const FeatureVulkanExtensionList> feature_catalog() noexcept {
        return Detail::feature_catalog;
    }

    [[nodiscard]] constexpr const FeatureVulkanExtensionList &extension_list_for(VulkanAppFeature feature) noexcept {
        for (const auto &entry : Detail::feature_catalog) {
            if (entry.feature == feature) { return entry; }
        }
        return Detail::feature_catalog.front();
    }

    // Techniques the engine knows how to compose from features.
    [[nodiscard]] constexpr span<const VulkanTechniqueGroup> technique_catalog() noexcept {
        return Detail::technique_catalog;
    }

    // The feature bundle for a technique. Never null: every VulkanTechnique has a catalog entry.
    [[nodiscard]] constexpr const VulkanTechniqueGroup &technique_group_for(VulkanTechnique technique) noexcept {
        for (const auto &entry : Detail::technique_catalog) {
            if (entry.technique == technique) { return entry; }
        }
        return Detail::technique_catalog.front();
    }

    [[nodiscard]] constexpr string_view to_string(VulkanInitStage stage) noexcept {
        switch (stage) {
            case VulkanInitStage::Instance: return "instance";
            case VulkanInitStage::Device: return "device";
        }
        return "unknown";
    }
    [[nodiscard]] constexpr string_view to_string(VulkanExtensionNecessity necessity) noexcept {
        switch (necessity) {
            case VulkanExtensionNecessity::MustHave: return "must-have";
            case VulkanExtensionNecessity::NiceToHave: return "nice-to-have";
        }
        return "unknown";
    }

} // namespace SFT::Core::Vulkan
