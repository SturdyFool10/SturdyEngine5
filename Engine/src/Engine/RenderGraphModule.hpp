#pragma once

#include <Foundation/src/Foundation.hpp>

#include <cstddef>
#include <filesystem>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

#include "RenderTarget.hpp"

namespace SFT::Engine {

    class RenderGraph;

    struct RenderGraphTextureHandle {
        u32 index = std::numeric_limits<u32>::max();
        u32 generation = 0;

        /// Converts the `RenderGraphTextureHandle` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit constexpr operator bool() const noexcept {
            return index != std::numeric_limits<u32>::max() && generation != 0;
        }
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(RenderGraphTextureHandle, RenderGraphTextureHandle) noexcept = default;
    };

    struct RenderGraphPassHandle {
        u32 index = std::numeric_limits<u32>::max();
        u32 generation = 0;

        /// Converts the `RenderGraphPassHandle` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit constexpr operator bool() const noexcept {
            return index != std::numeric_limits<u32>::max() && generation != 0;
        }
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(RenderGraphPassHandle, RenderGraphPassHandle) noexcept = default;
    };


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

        /// Renders resolution using the current rendering state.
        ///
        /// @param scale `scale` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr RenderGraphExtent render_resolution(f32 scale = 1.0f) noexcept {
            return RenderGraphExtent{.mode = RenderGraphExtentMode::RenderResolution,
                                     .scale_x = scale, .scale_y = scale};
        }
        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @return Returns the current presentation resolution value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr RenderGraphExtent presentation_resolution() noexcept {
            return RenderGraphExtent{.mode = RenderGraphExtentMode::PresentationResolution};
        }
        /// Performs the relative to operation for `RenderGraphExtent` using the supplied arguments.
        ///
        /// @param texture Texture used or affected by the operation.
        /// @param scale `scale` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr RenderGraphExtent relative_to(
            RenderGraphTextureHandle texture, f32 scale = 1.0f) noexcept {
            return RenderGraphExtent{.mode = RenderGraphExtentMode::RelativeToInput,
                                     .scale_x = scale, .scale_y = scale, .input = texture};
        }
        /// Performs the absolute operation for `RenderGraphExtent` using the supplied arguments.
        ///
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
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
        ComputeEffect,
        Copy,
        ToneMapping,
        DebugOverlay,
        Present,
    };


    struct FullscreenEffectDescription {
        std::filesystem::path shader_path;
        std::string module_name;
        std::string fragment_entry_point = "fragmentMain";
        std::vector<std::byte> push_constants;
        UString label;

        /// Sets the push constants for this `FullscreenEffectDescription`.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename Constants>
            requires std::is_trivially_copyable_v<Constants>
        FullscreenEffectDescription &set_push_constants(const Constants &constants) {
            const std::span<const Constants> values{&constants, 1};
            const std::span<const std::byte> bytes = std::as_bytes(values);
            push_constants.assign(bytes.begin(), bytes.end());
            return *this;
        }
    };

    using RasterEffectDescription = FullscreenEffectDescription;


    struct ComputeEffectDescription {
        std::filesystem::path shader_path;
        std::string module_name;
        std::string compute_entry_point = "computeMain";
        std::vector<std::byte> push_constants;
        UString label;

        /// Sets the push constants for this `ComputeEffectDescription`.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename Constants>
            requires std::is_trivially_copyable_v<Constants>
        ComputeEffectDescription &set_push_constants(const Constants &constants) {
            const std::span<const Constants> values{&constants, 1};
            const std::span<const std::byte> bytes = std::as_bytes(values);
            push_constants.assign(bytes.begin(), bytes.end());
            return *this;
        }
    };


    struct CopyDescription {
        UString label;
    };

    struct RenderGraphPassDescription {
        RenderGraphPassHandle handle{};
        RenderGraphPassKind kind = RenderGraphPassKind::DeferredScene;
        RenderGraphTextureHandle input{};
        RenderGraphTextureHandle output{};
        FullscreenEffectDescription fullscreen_effect{};
        ComputeEffectDescription compute_effect{};
        CopyDescription copy{};

        RenderTargetHandle target{};
        UString label;
    };

    namespace RenderModules {

        struct DeferredScene {
            /// Builds the requested object or derived state.
            ///
            /// @param graph `graph` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct AntiAliasing {
            RenderGraphTextureHandle input{};
            /// Builds the requested object or derived state.
            ///
            /// @param graph `graph` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct Bloom {
            RenderGraphTextureHandle input{};
            /// Builds the requested object or derived state.
            ///
            /// @param graph `graph` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct FullscreenEffect {
            RenderGraphTextureHandle input{};
            FullscreenEffectDescription effect{};
            /// Builds the requested object or derived state.
            ///
            /// @param graph `graph` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct RasterEffect {
            RenderGraphTextureHandle input{};
            RasterEffectDescription effect{};
            /// Builds the requested object or derived state.
            ///
            /// @param graph `graph` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct ComputeEffect {
            RenderGraphTextureHandle input{};
            ComputeEffectDescription effect{};
            /// Builds the requested object or derived state.
            ///
            /// @param graph `graph` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct Copy {
            RenderGraphTextureHandle input{};
            CopyDescription copy{};
            /// Builds the requested object or derived state.
            ///
            /// @param graph `graph` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct ToneMapping {
            RenderGraphTextureHandle input{};
            /// Builds the requested object or derived state.
            ///
            /// @param graph `graph` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct DebugOverlay {
            RenderGraphTextureHandle input{};
            /// Builds the requested object or derived state.
            ///
            /// @param graph `graph` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] RenderGraphTextureHandle build(RenderGraph &graph) const;
        };

        struct Present {
            RenderGraphTextureHandle input{};

            RenderTargetHandle target{};
            /// Builds the requested object or derived state.
            ///
            /// @param graph `graph` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] RenderGraphPassHandle build(RenderGraph &graph) const;
        };

    } // namespace RenderModules

} // namespace SFT::Engine
