#pragma once

#include <Foundation/Foundation.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <string_view>
#include <type_traits>

namespace SFT::Engine {

    enum class AssetType : u8 {
        Invalid = 0,
        Model,
        Shader,
        Sound,
        Texture,
        File,
    };

    [[nodiscard]] constexpr std::string_view to_string(AssetType type) noexcept {
        switch (type) {
            case AssetType::Model: return "model";
            case AssetType::Shader: return "shader";
            case AssetType::Sound: return "sound";
            case AssetType::Texture: return "texture";
            case AssetType::File: return "file";
            default: return "invalid";
        }
    }

    // Opaque, cheap, generation-checked asset identity. It deliberately contains no renderer/RHI
    // handle: consumers can store it in ECS components or native game objects without taking on GPU
    // lifetime or synchronization responsibilities.
    class Asset {
      public:
        constexpr Asset() noexcept = default;

        [[nodiscard]] constexpr AssetType type() const noexcept { return type_; }
        [[nodiscard]] constexpr u64 id() const noexcept { return id_; }
        [[nodiscard]] constexpr u32 generation() const noexcept { return generation_; }
        [[nodiscard]] constexpr bool is(AssetType type) const noexcept { return type_ == type && is_valid(); }
        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return owner_ != 0 && id_ != 0 && generation_ != 0 && type_ != AssetType::Invalid;
        }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return is_valid(); }
        [[nodiscard]] constexpr usize hash() const noexcept {
            u64 value = owner_ ^ (id_ + 0x9e3779b97f4a7c15ULL + (owner_ << 6u) + (owner_ >> 2u));
            value ^= static_cast<u64>(generation_) << 32u;
            value ^= static_cast<u64>(type_);
            return static_cast<usize>(value);
        }

        friend constexpr bool operator==(Asset, Asset) noexcept = default;

      private:
        friend class AssetManager;

        constexpr Asset(u64 owner, u64 id, u32 generation, AssetType type) noexcept
            : owner_(owner), id_(id), generation_(generation), type_(type) {}

        u64 owner_ = 0;
        u64 id_ = 0;
        u32 generation_ = 0;
        AssetType type_ = AssetType::Invalid;
    };

    static_assert(std::is_trivially_copyable_v<Asset>);
    static_assert(std::is_standard_layout_v<Asset>);

    enum class AssetErrorCode : u8 {
        InvalidAsset,
        WrongType,
        NotFound,
        IoFailure,
        DecodeFailure,
        InvalidDescription,
        BackendFailure,
        InUse,
        Unsupported,
    };

    struct AssetError {
        AssetErrorCode code = AssetErrorCode::InvalidAsset;
        UString message;
        std::filesystem::path source;
    };

    template <typename T>
    using AssetExpected = std::expected<T, AssetError>;

    using AssetResult = AssetExpected<void>;

    struct AssetInfo {
        Asset asset{};
        UString label;
        std::filesystem::path source;
        usize memory_bytes = 0;
        bool loaded = false;
    };

} // namespace SFT::Engine

template <>
struct std::hash<SFT::Engine::Asset> {
    [[nodiscard]] constexpr std::size_t operator()(SFT::Engine::Asset asset) const noexcept {
        return asset.hash();
    }
};
