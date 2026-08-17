#include <Engine/RenderGraph.hpp>

namespace SFT::Engine::RenderModules {

    /// Builds the requested object or derived state.
    ///
    /// @param graph `graph` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Builds the requested object or derived state.
    ///
    /// @param graph `graph` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Builds the requested object or derived state.
    ///
    /// @param graph `graph` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Builds the requested object or derived state.
    ///
    /// @param graph `graph` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    RenderGraphTextureHandle FullscreenEffect::build(RenderGraph &graph) const {
        return graph.add_fullscreen_effect(input, effect);
    }

    /// Builds the requested object or derived state.
    ///
    /// @param graph `graph` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    RenderGraphTextureHandle RasterEffect::build(RenderGraph &graph) const {
        return graph.add_fullscreen_effect(input, effect);
    }

    /// Builds the requested object or derived state.
    ///
    /// @param graph `graph` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    RenderGraphTextureHandle ComputeEffect::build(RenderGraph &graph) const {
        return graph.add_compute_effect(input, effect);
    }

    /// Builds the requested object or derived state.
    ///
    /// @param graph `graph` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    RenderGraphTextureHandle Copy::build(RenderGraph &graph) const {
        return graph.add_copy(input, copy);
    }

    /// Builds the requested object or derived state.
    ///
    /// @param graph `graph` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Builds the requested object or derived state.
    ///
    /// @param graph `graph` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Builds the requested object or derived state.
    ///
    /// @param graph `graph` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    RenderGraphPassHandle Present::build(RenderGraph &graph) const {
        return graph.add_present_pass(input, target);
    }

} // namespace SFT::Engine::RenderModules
