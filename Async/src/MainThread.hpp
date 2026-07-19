#pragma once

#include <functional>

namespace SFT::Async {

    void run_on_main_thread(std::function<void()> fn) noexcept;
    void pump_main_thread() noexcept;

} // namespace SFT::Async
