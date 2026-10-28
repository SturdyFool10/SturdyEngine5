#pragma once

#include <Foundation/Cpu/IsaSet.hpp>

#include <memory>

namespace SFT::Foundation::Cpu {

    namespace Detail {
        struct IsaScopeState;
        struct IsaSuspensionState;
    }

    /// Restricts the current thread to the existing-affinity subset of CPUs that satisfy `required`.
    /// The restriction is a CPU set, not a single-core pin, so the scheduler may migrate the thread
    /// among all compatible CPUs until the scope exits and restores the prior affinity.
    class IsaScope {
      public:
        explicit IsaScope(const IsaSet &required);
        ~IsaScope() noexcept;

        IsaScope(const IsaScope &) = delete;
        IsaScope &operator=(const IsaScope &) = delete;
        IsaScope(IsaScope &&) = delete;
        IsaScope &operator=(IsaScope &&) = delete;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] static bool can_enter(const IsaSet &required) noexcept;
        [[nodiscard]] static bool has_compatible_core(const IsaSet &required) noexcept;
        /// Changes the active requirement set and allowed CPU domain. On failure, the prior set and
        /// affinity are left unchanged. Requires an active scope on the calling thread.
        [[nodiscard]] static bool try_set_requirements(const IsaSet &required) noexcept;

      private:
        std::unique_ptr<Detail::IsaScopeState> state_;
        bool valid_ = false;
    };

    class IsaScopeSuspension {
      public:
        IsaScopeSuspension();
        ~IsaScopeSuspension() noexcept;

        IsaScopeSuspension(const IsaScopeSuspension &) = delete;
        IsaScopeSuspension &operator=(const IsaScopeSuspension &) = delete;
        IsaScopeSuspension(IsaScopeSuspension &&) = delete;
        IsaScopeSuspension &operator=(IsaScopeSuspension &&) = delete;

      private:
        std::unique_ptr<Detail::IsaSuspensionState> state_;
    };

    /// Returns the ISA set currently declared by the innermost active scope on this thread.
    [[nodiscard]] const IsaSet &current_isa_requirements() noexcept;

} // namespace SFT::Foundation::Cpu
