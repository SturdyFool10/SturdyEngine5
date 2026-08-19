#pragma once

#include <Foundation/Foundation.hpp>

#include <string_view>
#include <vector>

#include <Core/Core.hpp>

#include <Renderer/RenderGraph.hpp>

namespace SFT::Renderer {


    class RenderGraphBlackboard {
      public:
        /// Constructs a `RenderGraphBlackboard` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderGraphBlackboard();

        /// Resets the object to its baseline state.
        ///
        /// @note This function does not throw exceptions.
        void reset() noexcept;

        /// Performs the publish texture operation for `RenderGraphBlackboard` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

        /// Returns the current or globally available texture value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
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

        /// Reports whether texture holds for this `RenderGraphBlackboard`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        template <typename Semantic>
        [[nodiscard]] bool contains_texture() const noexcept {
            return static_cast<bool>(texture<Semantic>());
        }

        /// Returns the texture count for this `RenderGraphBlackboard`.
        ///
        /// @return Returns the current texture count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize texture_count() const noexcept;

      private:
        struct TextureEntry {
            std::string_view key;
            RenderGraphTextureHandle texture{};
        };

        /// Returns the current or globally available semantic key value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        template <typename Semantic>
        [[nodiscard]] static constexpr std::string_view semantic_key() noexcept {
            return std::string_view{Semantic::name};
        }

        std::vector<TextureEntry> texture_entries_;
    };

    namespace RenderGraphSemantics {


        struct SceneHdrColor {
            static constexpr std::string_view name = "sturdy.render.scene-hdr-color";
        };


        struct ResolvedSceneDepth {
            static constexpr std::string_view name = "sturdy.render.resolved-scene-depth";
        };


        struct RasterVisibilityDepth {
            static constexpr std::string_view name = "sturdy.render.raster-visibility-depth";
        };


        struct PresentationTarget {
            static constexpr std::string_view name = "sturdy.render.presentation-target";
        };


        struct ReusableSceneHdrScratch {
            static constexpr std::string_view name = "sturdy.render.reusable-scene-hdr-scratch";
        };

    } // namespace RenderGraphSemantics

    struct RenderGraphModuleBuildContext {
        RenderGraph &graph;
        RenderGraphBlackboard &resources;
        Core::Extent2D render_extent{};
        Core::Extent2D presentation_extent{};

        /// Renders texture extent using the current rendering state.
        ///
        /// @return Returns the current render texture extent value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::Extent3D render_texture_extent() const noexcept;
    };

} // namespace SFT::Renderer
