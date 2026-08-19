#pragma once

#include <Foundation/Foundation.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace SFT::RHI {


    enum class Format : u32;

} // namespace SFT::RHI

namespace SFT::Engine {


    enum class TextureKind : u8;

} // namespace SFT::Engine

namespace SFT::Engine::Detail {


    /// Compresses bc7 into the requested representation.
    ///
    /// @param rgba8 `rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc7(
        std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb);


    /// Compresses bc7 mip chain into the requested representation.
    ///
    /// @param rgba8_mips `rgba8_mips` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param mip_levels `mip_levels` value used by the operation.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc7_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, bool srgb);


    /// Compresses bc1 into the requested representation.
    ///
    /// @param rgba8 `rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc1(
        std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb);
    /// Compresses bc1 mip chain into the requested representation.
    ///
    /// @param rgba8_mips `rgba8_mips` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param mip_levels `mip_levels` value used by the operation.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc1_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, bool srgb);


    /// Compresses bc3 into the requested representation.
    ///
    /// @param rgba8 `rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc3(
        std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb);
    /// Compresses bc3 mip chain into the requested representation.
    ///
    /// @param rgba8_mips `rgba8_mips` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param mip_levels `mip_levels` value used by the operation.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc3_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, bool srgb);


    /// Compresses bc4 into the requested representation.
    ///
    /// @param rgba8 `rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param channel `channel` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc4(
        std::span<const std::byte> rgba8, u32 width, u32 height, u32 channel = 0);
    /// Compresses bc4 mip chain into the requested representation.
    ///
    /// @param rgba8_mips `rgba8_mips` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param mip_levels `mip_levels` value used by the operation.
    /// @param channel `channel` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc4_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, u32 channel = 0);


    /// Compresses bc5 into the requested representation.
    ///
    /// @param rgba8 `rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param channel0 `channel0` value used by the operation.
    /// @param channel1 `channel1` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc5(
        std::span<const std::byte> rgba8, u32 width, u32 height, u32 channel0 = 0, u32 channel1 = 1);
    /// Compresses bc5 mip chain into the requested representation.
    ///
    /// @param rgba8_mips `rgba8_mips` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param mip_levels `mip_levels` value used by the operation.
    /// @param channel0 `channel0` value used by the operation.
    /// @param channel1 `channel1` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc5_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels,
        u32 channel0 = 0, u32 channel1 = 1);


    /// Compresses gdeflate sibling into the requested representation.
    ///
    /// @param bc7_blocks `bc7_blocks` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_gdeflate_sibling(
        std::span<const std::byte> bc7_blocks, u32 width, u32 height, bool srgb);


    /// Selects bc format that best satisfies the supplied requirements.
    ///
    /// @param kind `kind` value used by the operation.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RHI::Format choose_bc_format(TextureKind kind, bool srgb) noexcept;


    /// Packs orm rgba8 using the supplied arguments and current state.
    ///
    /// @param occlusion_rgba8 `occlusion_rgba8` value used by the operation.
    /// @param metallic_roughness_rgba8 `metallic_roughness_rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<std::vector<std::byte>> pack_orm_rgba8(
        std::span<const std::byte> occlusion_rgba8, std::span<const std::byte> metallic_roughness_rgba8,
        u32 width, u32 height);


    /// Packs metallic roughness rg using the supplied arguments and current state.
    ///
    /// @param metallic_roughness_rgba8 `metallic_roughness_rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<std::vector<std::byte>> pack_metallic_roughness_rg(
        std::span<const std::byte> metallic_roughness_rgba8, u32 width, u32 height);

} // namespace SFT::Engine::Detail
