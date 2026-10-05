#include <Renderer/AutoExposure.hpp>

#include <algorithm>
#include <cmath>
#include <span>

#include <Renderer/RendererModule.hpp>
#include <Renderer/TemporalUpscaler.hpp>

namespace SFT::Renderer {

    namespace {

        constexpr u32 kTileSizePx = 32;
        constexpr u32 kHistogramBins = 64;
        constexpr u32 kTilesPerRow = 64;

        // Must match the push-constant blocks in Shaders/auto_exposure_{histogram,resolve}.slang.
        struct HistogramConstants {
            glm::uvec2 extent;
            u32 tiles_x;
            u32 pad0;
            f32 min_log2_luminance;
            f32 inv_log2_range;
            f32 center_weight;
            f32 pad1;
        };
        static_assert(sizeof(HistogramConstants) == 32);

        struct ResolveConstants {
            u32 tile_count;
            u32 reset;
            f32 min_log2_luminance;
            f32 log2_range;
            f32 low_percent;
            f32 high_percent;
            f32 key_value;
            f32 compensation_scale;
            f32 min_exposure;
            f32 max_exposure;
            f32 adapt_up;
            f32 adapt_down;
            f32 delta_seconds;
            f32 pad0;
            f32 pad1;
            f32 pad2;
        };
        static_assert(sizeof(ResolveConstants) == 64);

        template <class Constants>
        [[nodiscard]] std::span<const std::byte> bytes_of(const Constants &constants) {
            return std::as_bytes(std::span<const Constants>{&constants, 1});
        }

        [[nodiscard]] ComputeKernelDescription kernel_description(std::string_view module, std::string_view entry) {
            return ComputeKernelDescription{
                .shader_path = "Shaders/" + std::string{module} + ".slang",
                .module_name = std::string{module},
                .entry_point = std::string{entry},
                .label = std::string{module},
            };
        }

    } // namespace

    Core::RendererExpected<AutoExposureHistory::Lease> AutoExposureHistory::acquire(RHI::RhiDevice &device, u64 key) {
        auto slots = slots_.lock();
        Slot &slot = (*slots)[key];
        if (slot.device != &device) {
            // A different (or first) device: whatever handles the slot held died with the old one.
            slot = Slot{};
            auto texture = device.create_texture(RHI::TextureDesc{
                .dimension = RHI::TextureDimension::Dim2D,
                .format = RHI::Format::R32Float,
                .extent = RHI::Extent3D{.width = 1, .height = 1, .depth_or_layers = 1},
                .mip_levels = 1,
                .samples = RHI::SampleCount::X1,
                .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
                .label = "auto exposure state",
            });
            if (!texture) {
                slots->erase(key);
                return std::unexpected(graphics_error_from_rhi(texture.error(), "create auto exposure state"));
            }
            auto view = device.create_texture_view(RHI::TextureViewDesc{
                .texture = *texture,
                .view_type = RHI::TextureViewType::View2D,
                .base_mip_level = 0,
                .mip_level_count = 1,
                .label = "auto exposure state view",
            });
            if (!view) {
                device.destroy_texture(*texture);
                slots->erase(key);
                return std::unexpected(graphics_error_from_rhi(view.error(), "create auto exposure state view"));
            }
            slot.device = &device;
            slot.texture = *texture;
            slot.view = *view;
        }
        const auto now = std::chrono::steady_clock::now();
        Lease lease{.texture = slot.texture, .view = slot.view, .has_history = slot.initialized};
        if (slot.initialized) {
            lease.delta_seconds = std::clamp(std::chrono::duration<f32>(now - slot.last_use).count(), 0.001f, 0.5f);
        }
        slot.initialized = true;
        slot.last_use = now;
        return lease;
    }

    void AutoExposureHistory::release(RHI::RhiDevice *device) noexcept {
        auto slots = slots_.lock();
        if (device != nullptr) {
            for (auto &[key, slot] : *slots) {
                if (slot.device != device) continue;
                if (slot.view) device->destroy_texture_view(slot.view);
                if (slot.texture) device->destroy_texture(slot.texture);
            }
        }
        slots->clear();
    }

    glm::uvec2 auto_exposure_tile_extent(Core::Extent2D extent) noexcept {
        return glm::uvec2{(std::max(extent.x, 1u) + kTileSizePx - 1) / kTileSizePx,
                          (std::max(extent.y, 1u) + kTileSizePx - 1) / kTileSizePx};
    }

    Core::RendererExpected<AutoExposureResult> add_auto_exposure_passes(
        Renderer &renderer, RHI::RhiDevice &device, AutoExposureHistory &history, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, const AutoExposureDescription &description,
        const RenderGraphSettings &settings) {
        if (!description.source) {
            return std::unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed, "Auto exposure requires a scene colour render-graph texture."});
        }
        if (description.output_format == RHI::Format::Undefined) {
            return std::unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed, "Auto exposure needs the output format."});
        }
        const AutoExposureSettings &exposure = settings.frame.auto_exposure;

        const auto histogram_kernel = renderer.prepare_compute_kernel(kernel_description("auto_exposure_histogram", "histogramMain"));
        if (!histogram_kernel) return std::unexpected(histogram_kernel.error());
        const auto resolve_kernel = renderer.prepare_compute_kernel(kernel_description("auto_exposure_resolve", "resolveMain"));
        if (!resolve_kernel) return std::unexpected(resolve_kernel.error());

        CustomPostProcessEffect apply_effect;
        apply_effect.shader_path = "Shaders/auto_exposure_apply.slang";
        apply_effect.module_name = "auto_exposure_apply";
        apply_effect.fragment_entry_point = "fragmentMain";
        apply_effect.extra_input_count = 1;
        apply_effect.label = UString{"auto exposure apply"};
        if (Core::RendererResult ready = renderer.prepare_fullscreen_effect(apply_effect, description.output_format);
            !ready.has_value()) {
            return std::unexpected(ready.error());
        }

        auto lease = history.acquire(device, description.history_key);
        if (!lease) return std::unexpected(lease.error());

        const RenderGraphTextureHandle exposure_state = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = lease->texture,
            .default_view = lease->view,
            .format = RHI::Format::R32Float,
            .extent = RHI::Extent3D{.width = 1, .height = 1, .depth_or_layers = 1},
            .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
            .initial_layout = lease->has_history ? RHI::TextureLayout::General : RHI::TextureLayout::Undefined,
            .initial_stage = lease->has_history ? RHI::PipelineStage::ComputeShader | RHI::PipelineStage::FragmentShader
                                                : RHI::PipelineStage::None,
            .initial_access = lease->has_history ? RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite
                                                 : RHI::AccessFlags::None,
            .final_layout = RHI::TextureLayout::General,
            .final_stage = RHI::PipelineStage::ComputeShader | RHI::PipelineStage::FragmentShader,
            .final_access = RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite,
            .label = "auto exposure state",
        });

        const glm::uvec2 tiles = auto_exposure_tile_extent(description.extent);
        const u32 tile_count = tiles.x * tiles.y;
        const RenderGraphTextureHandle tile_histogram = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::R32Float,
            .extent = RHI::Extent3D{
                .width = kHistogramBins * kTilesPerRow,
                .height = (tile_count + kTilesPerRow - 1) / kTilesPerRow,
                .depth_or_layers = 1,
            },
            .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
            .label = "auto exposure tile histograms",
        });
        const RenderGraphTextureHandle destination = graph.create_texture(RenderGraphTextureDesc{
            .format = description.output_format,
            .extent = description.output_extent,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage |
                     RHI::TextureUsage::TransferSrc | RHI::TextureUsage::TransferDst,
            .label = "auto exposure output",
        });

        const f32 min_log2 = std::min(exposure.min_log2_luminance, exposure.max_log2_luminance - 1.0f);
        const f32 log2_range = std::max(exposure.max_log2_luminance - min_log2, 1.0f);
        const HistogramConstants histogram_constants{
            .extent = glm::uvec2{description.extent.x, description.extent.y},
            .tiles_x = tiles.x,
            .min_log2_luminance = min_log2,
            .inv_log2_range = 1.0f / log2_range,
            .center_weight = std::clamp(exposure.center_weight, 0.0f, 1.0f),
        };
        const f32 low = std::clamp(exposure.low_percent, 0.0f, 0.99f);
        const f32 high = std::clamp(exposure.high_percent, low + 0.01f, 1.0f);
        const ResolveConstants resolve_constants{
            .tile_count = tile_count,
            .reset = lease->has_history ? 0u : 1u,
            .min_log2_luminance = min_log2,
            .log2_range = log2_range,
            .low_percent = low,
            .high_percent = high,
            .key_value = std::max(exposure.key_value, 0.001f),
            .compensation_scale = std::exp2(exposure.compensation_ev),
            .min_exposure = std::max(exposure.min_exposure, 1.0e-4f),
            .max_exposure = std::max(exposure.max_exposure, exposure.min_exposure),
            .adapt_up = exposure.adapt_up_speed,
            .adapt_down = exposure.adapt_down_speed,
            .delta_seconds = lease->delta_seconds,
        };

        graph.add_compute_pass("auto exposure histogram"_ustr)
            .add_sampled_texture(description.source)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = tile_histogram, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *histogram_kernel, source = description.source,
                          tile_histogram, tiles, histogram_constants](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {
                    {"sceneColor", context.texture(source).default_view},
                    {"tileHistogramOut", context.texture(tile_histogram).default_view},
                };
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, bytes_of(histogram_constants),
                                                      glm::uvec3{tiles, 1u}, transient_bind_groups);
            });

        graph.add_compute_pass("auto exposure resolve"_ustr)
            .add_sampled_texture(tile_histogram)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = exposure_state, .read = true, .write = true})
            .set_side_effect(true)
            .set_execute([&renderer, &transient_bind_groups, kernel = *resolve_kernel, tile_histogram, exposure_state,
                          resolve_constants](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {
                    {"tileHistogram", context.texture(tile_histogram).default_view},
                    {"exposureState", context.texture(exposure_state).default_view},
                };
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, bytes_of(resolve_constants),
                                                      glm::uvec3{1u, 1u, 1u}, transient_bind_groups);
            });

        const Core::Extent2D output_extent{description.output_extent.width, description.output_extent.height};
        graph.add_render_pass("auto exposure apply"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = destination,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = description.source})
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = exposure_state})
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = output_extent.x, .height = output_extent.y})
            .set_execute([&renderer, &transient_bind_groups, source = description.source, exposure_state,
                          format = description.output_format, output_extent,
                          effect = std::move(apply_effect)](RenderGraphContext &context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .width = static_cast<f32>(output_extent.x),
                    .height = static_cast<f32>(output_extent.y),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = output_extent.x, .height = output_extent.y});
                const RHI::TextureViewHandle extras[] = {context.texture(exposure_state).default_view};
                return renderer.record_fullscreen_effect(pass, context.texture(source).default_view, format, effect,
                                                         transient_bind_groups, extras);
            });

        return AutoExposureResult{.exposed = destination, .exposure = exposure_state};
    }

    Core::RendererResult build_auto_exposure_feature(FrameBuildContext &frame) {
        if (!frame.settings.frame.auto_exposure.enabled || frame.direct_overlay_presentation) {
            return {};
        }
        const RenderGraphTextureHandle source = frame.resources.texture<RenderGraphSemantics::SceneHdrColor>();
        if (!source) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Auto exposure needs the SceneHdrColor texture, but no earlier feature published it.");
        }
        auto result = add_auto_exposure_passes(
            frame.renderer, frame.device, frame.renderer.auto_exposure_history(), frame.graph, frame.transient_bind_groups,
            AutoExposureDescription{
                .source = source,
                .extent = scene_color_extent(frame),
                .output_extent = RHI::Extent3D{.width = scene_color_extent(frame).x, .height = scene_color_extent(frame).y,
                                               .depth_or_layers = 1},
                .output_format = frame.deferred_formats.scene_color,
                .history_key = static_cast<u64>(frame.surface.window_id),
            },
            frame.settings);
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        frame.resources.publish_texture<RenderGraphSemantics::SceneHdrColor>(result->exposed);
        return {};
    }

} // namespace SFT::Renderer
