#pragma once

#include <Foundation/src/Foundation.hpp>

#include <string_view>
#include <vector>

#include <Core/Core.hpp>

#include "RenderGraph.hpp"

namespace SFT::Renderer {

    /// Retained, type-safe semantic resource map used while lowering reusable Renderer modules into the
    /// low-level RHI-aware RenderGraph. Semantic tags carry a stable string name, so a module depends on
    /// "current scene HDR" or "presentation target" rather than a lifecycle-local resource variable.
    ///
    /// The entries vector intentionally survives reset(): WindowSurfaceRecord owns one blackboard beside
    /// its retained RenderGraph, keeping steady-state frame declaration allocation-free after warm-up.
    class RenderGraphBlackboard {
      public:
        RenderGraphBlackboard();

        void reset() noexcept;

        template <typename Semantic>
        void publish_texture(RenderGraphTextureHandle texture) {
            const std::string_view key = semantic_key<Semantic>();
            for (TextureEntry &entry : texture_entries_) {
                if (entry.key == key) {
                    entry.texture = texture;
                    return;
                }
            }
            texture_entries_.push_back(TextureEntry{.key = key, .texture = texture});
        }

        template <typename Semantic>
        [[nodiscard]] RenderGraphTextureHandle texture() const noexcept {
            const std::string_view key = semantic_key<Semantic>();
            for (const TextureEntry &entry : texture_entries_) {
                if (entry.key == key) {
                    return entry.texture;
                }
            }
            return {};
        }

        template <typename Semantic>
        [[nodiscard]] bool contains_texture() const noexcept {
            return static_cast<bool>(texture<Semantic>());
        }

        [[nodiscard]] usize texture_count() const noexcept;

      private:
        struct TextureEntry {
            std::string_view key;
            RenderGraphTextureHandle texture{};
        };

        template <typename Semantic>
        [[nodiscard]] static constexpr std::string_view semantic_key() noexcept {
            return std::string_view{Semantic::name};
        }

        std::vector<TextureEntry> texture_entries_;
    };

    namespace RenderGraphSemantics {

        /// The latest scene-linear HDR result. Modules replace this publication as they transform the
        /// frame, making an effect chain compositional without lifecycle-local source variables.
        struct SceneHdrColor {
            static constexpr std::string_view name = "sturdy.render.scene-hdr-color";
        };

        /// Single-sample scene depth consumed by lighting and post effects.
        struct ResolvedSceneDepth {
            static constexpr std::string_view name = "sturdy.render.resolved-scene-depth";
        };

        /// Visibility depth used by raster geometry. It aliases ResolvedSceneDepth at 1x and names the
        /// multisampled depth image when SRAA is enabled.
        struct RasterVisibilityDepth {
            static constexpr std::string_view name = "sturdy.render.raster-visibility-depth";
        };

        /// Imported swapchain image (or a future off-screen presentation target).
        struct PresentationTarget {
            static constexpr std::string_view name = "sturdy.render.presentation-target";
        };

        /// Optional full-resolution HDR allocation whose earlier contents are dead. AA modules reuse it
        /// as a distinct destination whenever it is not already the current SceneHdrColor.
        struct ReusableSceneHdrScratch {
            static constexpr std::string_view name = "sturdy.render.reusable-scene-hdr-scratch";
        };

    } // namespace RenderGraphSemantics

    struct RenderGraphModuleBuildContext {
        RenderGraph &graph;
        RenderGraphBlackboard &resources;
        Core::Extent2D render_extent{};
        Core::Extent2D presentation_extent{};

        [[nodiscard]] RHI::Extent3D render_texture_extent() const noexcept;
    };

} // namespace SFT::Renderer
