#include <Engine/RenderGraph.hpp>

namespace SFT::Engine::RenderModules {

    RenderGraphTextureHandle DeferredScene::build(RenderGraph &graph) const {
        return graph.add_builtin_pass(
            RenderGraphPassKind::DeferredScene,
            {},
            RenderGraphTextureDescription{
                .format = RenderGraphTextureFormat::SceneLinearHdr,
                .extent = RenderGraphExtent::render_resolution(),
                .label = UString{"deferred scene HDR"_ustr},
            },
            UString{"deferred scene"_ustr});
    }

    RenderGraphTextureHandle AntiAliasing::build(RenderGraph &graph) const {
        return graph.add_builtin_pass(
            RenderGraphPassKind::AntiAliasing,
            input,
            RenderGraphTextureDescription{
                .format = RenderGraphTextureFormat::Inherit,
                .extent = RenderGraphExtent::relative_to(input),
                .label = UString{"anti-aliased scene HDR"_ustr},
            },
            UString{"anti-aliasing"_ustr});
    }

    RenderGraphTextureHandle Bloom::build(RenderGraph &graph) const {
        return graph.add_builtin_pass(
            RenderGraphPassKind::Bloom,
            input,
            RenderGraphTextureDescription{
                .format = RenderGraphTextureFormat::Inherit,
                .extent = RenderGraphExtent::relative_to(input),
                .label = UString{"bloom-composited scene HDR"_ustr},
            },
            UString{"bloom"_ustr});
    }

    RenderGraphTextureHandle FullscreenEffect::build(RenderGraph &graph) const {
        return graph.add_fullscreen_effect(input, effect);
    }

    RenderGraphTextureHandle ToneMapping::build(RenderGraph &graph) const {
        return graph.add_builtin_pass(
            RenderGraphPassKind::ToneMapping,
            input,
            RenderGraphTextureDescription{
                .format = RenderGraphTextureFormat::DisplayEncoded,
                .extent = RenderGraphExtent::presentation_resolution(),
                .label = UString{"display-encoded scene"_ustr},
            },
            UString{"tone mapping"_ustr});
    }

    RenderGraphTextureHandle DebugOverlay::build(RenderGraph &graph) const {
        return graph.add_builtin_pass(
            RenderGraphPassKind::DebugOverlay,
            input,
            RenderGraphTextureDescription{
                .format = RenderGraphTextureFormat::Inherit,
                .extent = RenderGraphExtent::relative_to(input),
                .label = UString{"scene with debug overlay"_ustr},
            },
            UString{"debug overlay"_ustr});
    }

    RenderGraphPassHandle Present::build(RenderGraph &graph) const {
        return graph.add_present_pass(input);
    }

} // namespace SFT::Engine::RenderModules
