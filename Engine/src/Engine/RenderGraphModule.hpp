#pragma once

#include <Foundation/src/Foundation.hpp>

#include <cstddef>
#include <filesystem>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace SFT::Engine {

    class RenderGraph;

    struct RenderGraphTextureHandle {
        u32 index = std::numeric_limits<u32>::max();
        u32 generation = 0;

        [[nodiscard]] explicit constexpr operator bool() const noexcept {
            return index != std::numeric_limits<u32>::max() && generation != 0;
        }
        friend constexpr bool operator==(RenderGraphTextureHandle, RenderGraphTextureHandle) noexcept = default;
    };

    struct RenderGraphPassHandle {
        u32 index = std::numeric_limits<u32>::max();
        u32 generation = 0;

        [[nodiscard]] explicit constexpr operator bool() const noexcept {
            return index != std::numeric_limits<u32>::max() && generation != 0;
        }
        friend constexpr bool operator==(RenderGraphPassHandle, RenderGraphPassHandle) noexcept = default;
    };

    // Semantic formats deliberately stop above RHI. Renderer resolves these to concrete backend-
    // compatible formats when lowering the graph for a surface.
    enum class RenderGraphTextureFormat : u8 {
        Inherit,
        SceneLinearHdr,
        DisplayEncoded,
        Depth,
        R8Unorm,
        RG16Float,
        RGBA16Float,
        R32Float,
    };

    enum class RenderGraphExtentMode : u8 {
        RenderResolution,
        PresentationResolution,
        RelativeToInput,
        Absolute,
    };

    struct RenderGraphExtent {
        RenderGraphExtentMode mode = RenderGraphExtentMode::RenderResolution;
        f32 scale_x = 1.0f;
        f32 scale_y = 1.0f;
        u32 width = 0;
        u32 height = 0;
        RenderGraphTextureHandle input{};

        [[nodiscard]] static constexpr RenderGraphExtent render_resolution(f32 scale = 1.0f) noexcept {
            return RenderGraphExtent{.mode = RenderGraphExtentMode::RenderResolution,
                                     .scale_x = scale, .scale_y = scale};
        }
        [[nodiscard]] static constexpr RenderGraphExtent presentation_resolution() noexcept {
            return RenderGraphExtent{.mode = RenderGraphExtentMode::PresentationResolution};
        }
        [[nodiscard]] static constexpr RenderGraphExtent relative_to(
            RenderGraphTextureHandle texture, f32 scale = 1.0f) noexcept {
            return RenderGraphExtent{.mode = RenderGraphExtentMode::RelativeToInput,
                                     .scale_x = scale, .scale_y = scale, .input = texture};
        }
        [[nodiscard]] static constexpr RenderGraphExtent absolute(u32 width, u32 height) noexcept {
            return RenderGraphExtent{.mode = RenderGraphExtentMode::Absolute,
                                     .width = width, .height = height};
        }
    };

    struct RenderGraphTextureDescription {
        RenderGraphTextureFormat format = RenderGraphTextureFormat::SceneLinearHdr;
        RenderGraphExtent extent = RenderGraphExtent::render_resolution();
        u32 mip_levels = 1;
        UString label;
    };

    enum class RenderGraphPassKind : u8 {
        DeferredScene,
        AntiAliasing,
        Bloom,
        FullscreenEffect,
        ToneMapping,
        DebugOverlay,
        Present,
    };

    // Engine-owned, RHI-free fullscreen module description. Source identity and constants are copied
    // into the immutable prepared-frame snapshot; Renderer owns compilation, reflection, descriptors,
    // command recording, synchronization, and retirement.
    struct FullscreenEffectDescription {
        std::filesystem::path shader_path;
        std::string module_name;
        std::string fragment_entry_point = "fragmentMain";
        std::vector<std::byte> push_constants;
        UString label;

        template <typename Constants>
            requires std::is_trivially_copyable_v<Constants>
        FullscreenEffectDescription &set_push_constants(const Constants &constants) {
            const std::span<const Constants> values{&constants, 1};
            const std::span<const std::byte> bytes = std::as_bytes(values);
            push_constants.assign(bytes.begin(), bytes.end());
            return *this;
        }
    };

    struct RenderGraphPassDescription {
        RenderGraphPassHandle handle{};
        RenderGraphPassKind kind = RenderGraphPassKind::DeferredScene;
        RenderGraphTextureHandle input{};
        RenderGraphTextureHandle output{};
        FullscreenEffectDescription fullscreen_effect{};
        UString label;
    };

    namespace RenderModules {

        struct DeferredScene {
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct AntiAliasing {
            RenderGraphTextureHandle input{};
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct Bloom {
            RenderGraphTextureHandle input{};
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct FullscreenEffect {
            RenderGraphTextureHandle input{};
            FullscreenEffectDescription effect{};
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct ToneMapping {
            RenderGraphTextureHandle input{};
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct DebugOverlay {
            RenderGraphTextureHandle input{};
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct Present {
            RenderGraphTextureHandle input{};
            [[nodiscard]] RenderGraphPassHandle build(RenderGraph &graph) const;
        };

    } // namespace RenderModules

} // namespace SFT::Engine
