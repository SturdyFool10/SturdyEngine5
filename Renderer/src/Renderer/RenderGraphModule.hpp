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

        /// Buffers travel the same way: a feature publishes one under a semantic and its consumers look it up.
        template <typename Semantic>
        void publish_buffer(RenderGraphBufferHandle buffer) {
            const std::string_view key = semantic_key<Semantic>();
            for (BufferEntry &entry : buffer_entries_) {
                if (entry.key == key) {
                    entry.buffer = buffer;
                    return;
                }
            }
            buffer_entries_.push_back(BufferEntry{.key = key, .buffer = buffer});
        }

        template <typename Semantic>
        [[nodiscard]] RenderGraphBufferHandle buffer() const noexcept {
            const std::string_view key = semantic_key<Semantic>();
            for (const BufferEntry &entry : buffer_entries_) {
                if (entry.key == key) {
                    return entry.buffer;
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

        struct BufferEntry {
            std::string_view key;
            RenderGraphBufferHandle buffer{};
        };
        std::vector<TextureEntry> texture_entries_;
        std::vector<BufferEntry> buffer_entries_;
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


        /// Deferred base colour (written by the `gbuffer` feature, read by lighting and the spectral paths).
        struct GBufferAlbedo {
            static constexpr std::string_view name = "sturdy.render.gbuffer-albedo";
        };

        /// Deferred encoded normals.
        struct GBufferNormal {
            static constexpr std::string_view name = "sturdy.render.gbuffer-normal";
        };

        /// Deferred material parameters (roughness/metallic/AO... per the material shaders).
        struct GBufferMaterial {
            static constexpr std::string_view name = "sturdy.render.gbuffer-material";
        };

        /// Deferred emissive radiance.
        struct GBufferEmissive {
            static constexpr std::string_view name = "sturdy.render.gbuffer-emissive";
        };

        /// Deferred per-pixel motion vectors.
        struct GBufferMotion {
            static constexpr std::string_view name = "sturdy.render.gbuffer-motion";
        };

        /// Cascaded directional shadow atlas (written by `shadow_maps`).
        struct DirectionalShadowAtlas {
            static constexpr std::string_view name = "sturdy.render.directional-shadow-atlas";
        };

        /// Spot-light shadow atlas (written by `shadow_maps`).
        struct PunctualShadowAtlas {
            static constexpr std::string_view name = "sturdy.render.punctual-shadow-atlas";
        };

        /// Hierarchical depth pyramid (written by `hiz_build`, read by instance culling next frame).
        struct HiZPyramid {
            static constexpr std::string_view name = "sturdy.render.hiz-pyramid";
        };

        /// Atmosphere transmittance LUT.
        struct TransmittanceLut {
            static constexpr std::string_view name = "sturdy.render.atmosphere-transmittance-lut";
        };

        /// Atmosphere multiple-scattering LUT.
        struct MultiScatteringLut {
            static constexpr std::string_view name = "sturdy.render.atmosphere-multi-scattering-lut";
        };

        /// Atmosphere sky-view LUT.
        struct SkyViewLut {
            static constexpr std::string_view name = "sturdy.render.atmosphere-sky-view-lut";
        };

        /// GPU-culled indirect draw commands (buffer; `instance_culling` -> `gbuffer`).
        struct InstanceIndirectCommands {
            static constexpr std::string_view name = "sturdy.render.instance-indirect-commands";
        };

        /// GPU-culled instance index list (buffer; `instance_culling` -> `gbuffer`).
        struct CompactedInstanceIndices {
            static constexpr std::string_view name = "sturdy.render.compacted-instance-indices";
        };


        /// Screen-space ambient occlusion (written by `ambient_occlusion`, read by `lighting`).
        struct AmbientOcclusion {
            static constexpr std::string_view name = "sturdy.render.ambient-occlusion";
        };

        /// ReSTIR GI surfel irradiance (written by `global_illumination`, read by `lighting`).
        struct SurfelIrradiance {
            static constexpr std::string_view name = "sturdy.render.surfel-irradiance";
        };

        /// Half-resolution, exposure-free copy of last frame's lit scene colour the screen-space GI reads radiance
        /// from (published by `screen_space_gi`, rewritten by `screen_space_gi_history`).
        struct ScreenSpaceGiRadianceHistory {
            static constexpr std::string_view name = "sturdy.render.ssgi-radiance-history";
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
