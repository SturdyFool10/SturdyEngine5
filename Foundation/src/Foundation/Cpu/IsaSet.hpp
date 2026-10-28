#pragma once

#include <Foundation/Cpu/Extensions.hpp>
#include <Foundation/Types.hpp>

#include <bitset>
#include <vector>

namespace SFT::Foundation::Cpu {

    class IsaSet {
      public:
        static constexpr usize extension_count = static_cast<usize>(Extension::Count);

        constexpr IsaSet() noexcept = default;
        explicit IsaSet(const std::vector<bool> &extensions) noexcept;

        [[nodiscard]] constexpr bool has(Extension extension) const noexcept {
            const usize index = static_cast<usize>(extension);
            return index < extension_count && bits_.test(index);
        }

        constexpr void add(Extension extension) noexcept {
            const usize index = static_cast<usize>(extension);
            if (index < extension_count) {
                bits_.set(index);
            }
        }

        constexpr void remove(Extension extension) noexcept {
            const usize index = static_cast<usize>(extension);
            if (index < extension_count) {
                bits_.reset(index);
            }
        }

        [[nodiscard]] constexpr bool contains(const IsaSet &required) const noexcept {
            return (bits_ & required.bits_) == required.bits_;
        }

        [[nodiscard]] constexpr bool empty() const noexcept { return bits_.none(); }
        [[nodiscard]] constexpr IsaSet intersection(const IsaSet &other) const noexcept {
            IsaSet result;
            result.bits_ = bits_ & other.bits_;
            return result;
        }

        constexpr IsaSet &operator|=(const IsaSet &other) noexcept {
            bits_ |= other.bits_;
            return *this;
        }

        friend constexpr IsaSet operator|(IsaSet lhs, const IsaSet &rhs) noexcept { return lhs |= rhs; }
        friend constexpr bool operator==(const IsaSet &, const IsaSet &) noexcept = default;

      private:
        std::bitset<extension_count> bits_{};
    };

    enum class IsaTier : u8 {
        Scalar,
        X86Sse2,
        X86Avx,
        X86Avx2,
        X86Avx512,
        ArmNeon,
        ArmSve2,
        RiscvV,
    };

    [[nodiscard]] IsaSet required_extensions(IsaTier tier) noexcept;
    [[nodiscard]] IsaTier best_tier(const IsaSet &available) noexcept;
    /// Returns the cached, OS-usable ISA set for the logical CPU currently running this thread.
    /// Returns an empty set when the current CPU cannot be matched to the process capability map.
    [[nodiscard]] IsaSet current_core_isa_set() noexcept;

} // namespace SFT::Foundation::Cpu
