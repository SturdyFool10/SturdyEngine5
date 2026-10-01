#include <Renderer/HistoryTextures.hpp>

#include <Renderer/RendererModule.hpp>

namespace SFT::Renderer {

    Core::RendererExpected<HistoryTextureCache::Lease> HistoryTextureCache::acquire(RHI::RhiDevice &device, u64 key,
                                                                                    const Desc &desc,
                                                                                    std::optional<u64> write_frame) {
        auto slots = slots_.lock();
        Slot &slot = (*slots)[key];
        const bool stale = slot.device != &device || slot.format != desc.format || slot.extent.width != desc.extent.width ||
                           slot.extent.height != desc.extent.height || !slot.texture;
        if (stale) {
            if (slot.device == &device) {
                if (slot.view) device.destroy_texture_view(slot.view);
                if (slot.texture) device.destroy_texture(slot.texture);
            }
            slot = Slot{};
            auto texture = device.create_texture(RHI::TextureDesc{
                .dimension = RHI::TextureDimension::Dim2D,
                .format = desc.format,
                .extent = RHI::Extent3D{.width = std::max(desc.extent.width, 1u), .height = std::max(desc.extent.height, 1u),
                                        .depth_or_layers = 1},
                .mip_levels = 1,
                .samples = RHI::SampleCount::X1,
                .usage = desc.usage,
                .label = desc.label,
            });
            if (!texture) {
                slots->erase(key);
                return std::unexpected(graphics_error_from_rhi(texture.error(), desc.label));
            }
            auto view = device.create_texture_view(RHI::TextureViewDesc{
                .texture = *texture,
                .view_type = RHI::TextureViewType::View2D,
                .base_mip_level = 0,
                .mip_level_count = 1,
                .label = desc.label,
            });
            if (!view) {
                device.destroy_texture(*texture);
                slots->erase(key);
                return std::unexpected(graphics_error_from_rhi(view.error(), desc.label));
            }
            slot.device = &device;
            slot.texture = *texture;
            slot.view = *view;
            slot.format = desc.format;
            slot.extent = RHI::Extent3D{.width = std::max(desc.extent.width, 1u), .height = std::max(desc.extent.height, 1u),
                                        .depth_or_layers = 1};
        }
        Lease lease{.texture = slot.texture, .view = slot.view, .format = slot.format, .extent = slot.extent,
                    .last_written_frame = slot.last_written_frame};
        if (write_frame) {
            slot.last_written_frame = *write_frame;
        }
        return lease;
    }

    void HistoryTextureCache::release(RHI::RhiDevice *device) noexcept {
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

    RenderGraphTextureHandle import_history_texture(RenderGraph &graph, const HistoryTextureCache::Lease &lease,
                                                    bool has_contents, const char *label) {
        constexpr RHI::PipelineStage kStages = RHI::PipelineStage::ComputeShader | RHI::PipelineStage::FragmentShader;
        constexpr RHI::AccessFlags kAccess = RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite;
        return graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = lease.texture,
            .default_view = lease.view,
            .format = lease.format,
            .extent = lease.extent,
            .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
            .initial_layout = has_contents ? RHI::TextureLayout::General : RHI::TextureLayout::Undefined,
            .initial_stage = has_contents ? kStages : RHI::PipelineStage::None,
            .initial_access = has_contents ? kAccess : RHI::AccessFlags::None,
            .final_layout = RHI::TextureLayout::General,
            .final_stage = kStages,
            .final_access = kAccess,
            .label = label,
        });
    }

} // namespace SFT::Renderer
