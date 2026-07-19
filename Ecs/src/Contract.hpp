#pragma once

#include <Foundation/src/Log.hpp>

#include <exception>
#include <utility>

namespace SFT::Ecs::Detail {

    // All fatal ECS runtime contracts pass through here so a useful console diagnostic is flushed
    // before std::terminate invokes the application's termination handler.
    template <class... Args>
    [[noreturn]] void contract_violation(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        Foundation::log_error(format, std::forward<Args>(args)...);
        Foundation::flush_logs();
        std::terminate();
    }

} // namespace SFT::Ecs::Detail
