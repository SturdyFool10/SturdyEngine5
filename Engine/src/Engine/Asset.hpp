#pragma once

#include <Foundation/src/Foundation.hpp>

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

    /// Converts the value to string representation.
    ///
    /// @param type Type value to inspect, select, or convert.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
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


    class Asset {
      public:
        /// Constructs a `Asset` in its default state.
        ///
        /// @note This function does not throw exceptions.
        constexpr Asset() noexcept = default;

        /// Returns the runtime or backend type represented by `Asset`.
        ///
        /// @return Returns the current type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr AssetType type() const noexcept { return type_; }
        /// Returns the current or globally available ID value.
        ///
        /// @return Returns the current ID value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr u64 id() const noexcept { return id_; }
        /// Returns the current or globally available generation value.
        ///
        /// @return Returns the current generation value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr u32 generation() const noexcept { return generation_; }
        /// Performs the is operation for `Asset` using the supplied arguments.
        ///
        /// @param type Type value to inspect, select, or convert.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        /// Performs the is operation for `Asset` using the supplied arguments.
        ///
        /// @param type Type value to inspect, select, or convert.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr bool is(AssetType type) const noexcept { return type_ == type && is_valid(); }
        /// Reports whether valid holds for this `Asset`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return owner_ != 0 && id_ != 0 && generation_ != 0 && type_ != AssetType::Invalid;
        }
        /// Converts the `Asset` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return is_valid(); }
        /// Hashes the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @return Returns the current hash value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr usize hash() const noexcept {
            u64 value = owner_ ^ (id_ + 0x9e3779b97f4a7c15ULL + (owner_ << 6u) + (owner_ >> 2u));
            value ^= static_cast<u64>(generation_) << 32u;
            value ^= static_cast<u64>(type_);
            return static_cast<usize>(value);
        }

        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(Asset, Asset) noexcept = default;

      private:
        friend class AssetManager;

        /// Constructs a `Asset` from the supplied initialization values.
        ///
        /// @param owner Owner/context identifier used for validation or diagnostics.
        /// @param id Identifier of the target object or resource.
        /// @param generation `generation` value used by the operation.
        /// @param type Type value to inspect, select, or convert.
        ///
        /// @note This function does not throw exceptions.
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
    /// Invokes the callable behavior provided by `std`.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr std::size_t operator()(SFT::Engine::Asset asset) const noexcept {
        return asset.hash();
    }
};
