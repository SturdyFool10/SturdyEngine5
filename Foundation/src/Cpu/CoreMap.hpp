#pragma once

#include <Foundation/src/Cpu/CpuTopology.hpp>
#include <Foundation/src/Cpu/Extensions.hpp>
#include <Foundation/src/Types.hpp>

#include <utility>
#include <vector>

namespace SFT::Foundation::Cpu {


    struct CoreCapabilities {
        std::vector<bool> extensions;
        usize l1d_bytes = 0;
        usize l1i_bytes = 0;
        usize l2_bytes = 0;
        usize l3_bytes = 0;
        u32 x2apic_id = 0;
        CoreType type = CoreType::Unknown;

        /// Performs the has operation for `CoreCapabilities` using the supplied arguments.
        ///
        /// @param extension `extension` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has(Extension extension) const noexcept;
    };


    /// Compares the operands for equality.
    ///
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    ///
    /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool operator==(const CoreCapabilities &a, const CoreCapabilities &b) noexcept;


    class CoreMap {
      public:

        /// Returns the current or globally available instance value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static const CoreMap &instance() noexcept;

        /// Returns the core count for this `CoreMap`.
        ///
        /// @return Returns the current core count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize core_count() const noexcept;
        /// Performs the core operation for `CoreMap` using the supplied arguments.
        ///
        /// @param logical_index Zero-based index of the target element or entry.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const CoreCapabilities &core(usize logical_index) const noexcept;


        /// Returns the current or globally available capabilities of current core value.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const CoreCapabilities *capabilities_of_current_core() const noexcept;


        /// Returns the distinct type count for this `CoreMap`.
        ///
        /// @return Returns the current distinct type count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize distinct_type_count() const noexcept;
        /// Reports whether hybrid holds for this `CoreMap`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_hybrid() const noexcept;

        /// Performs the type index of core operation for `CoreMap` using the supplied arguments.
        ///
        /// @param logical_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize type_index_of_core(usize logical_index) const noexcept;


        /// Performs the core indices of type operation for `CoreMap` using the supplied arguments.
        ///
        /// @param type_index Zero-based index of the target element or entry.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const std::vector<usize> &core_indices_of_type(usize type_index) const noexcept;


        /// Returns the physical core count for this `CoreMap`.
        ///
        /// @return Returns the current physical core count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize physical_core_count() const noexcept;


        /// Performs the physical core of operation for `CoreMap` using the supplied arguments.
        ///
        /// @param logical_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize physical_core_of(usize logical_index) const noexcept;


        /// Performs the logical cores of physical core operation for `CoreMap` using the supplied arguments.
        ///
        /// @param physical_index Zero-based index of the target element or entry.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const std::vector<usize> &logical_cores_of_physical_core(usize physical_index) const noexcept;

      private:
        /// Constructs a `CoreMap` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        CoreMap();

        std::vector<CoreCapabilities> cores_;
        std::vector<usize> type_of_core_;
        std::vector<std::vector<usize>> cores_of_type_;
        std::vector<std::pair<u32, usize>> index_of_x2apic_id_;
        std::vector<usize> physical_core_of_logical_;
        std::vector<std::vector<usize>> logical_cores_of_physical_core_;
    };

} // namespace SFT::Foundation::Cpu
