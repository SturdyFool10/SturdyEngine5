#pragma once

namespace SFT::UiWorkbench {


    /// Performs the install crash handler operation using the supplied arguments.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void install_crash_handler();

} // namespace SFT::UiWorkbench
