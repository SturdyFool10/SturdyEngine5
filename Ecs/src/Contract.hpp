#pragma once

#include <Foundation/src/Log.hpp>

#include <exception>
#include <utility>

namespace SFT::Ecs::Detail {


    /// Performs the contract violation operation for `Detail` using the supplied arguments.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    /// @param args `args` value used by the operation.
    ///
    /// @note Terminates the process if an invariant required by this unchecked operation is violated.
    /// @note This function does not throw exceptions.
    template <class... Args>
    [[noreturn]] void contract_violation(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        Foundation::log_error(format, std::forward<Args>(args)...);
        Foundation::flush_logs();
        std::terminate();
    }

} // namespace SFT::Ecs::Detail
