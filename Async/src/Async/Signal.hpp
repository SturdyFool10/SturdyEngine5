#pragma once

#include <initializer_list>
#include <expected>
#include <span>

#include <Async/Cancellation.hpp>
#include <Async/IoError.hpp>
#include <Async/Task.hpp>

namespace SFT::Async {

    /// Completes with the number of the first of `signals` delivered to the process (tokio::signal). The wait costs no
    /// thread; the handler is installed when you call this and removed when it completes or is cancelled. Use the
    /// `SIGINT`/`SIGTERM`/`SIGHUP`... constants of <csignal> (Windows delivers SIGINT and SIGTERM only).
    /// Cancelling `cancellation` fails the handle with `IoOperationCancelled`.
    [[nodiscard]] TaskHandle<std::expected<int, IoError>> wait_for_signal(std::initializer_list<int> signals, CancellationToken cancellation = {});
    /// Same, for a list built at run time (the bindings use this).
    [[nodiscard]] TaskHandle<std::expected<int, IoError>> wait_for_signal(std::span<const int> signals, CancellationToken cancellation = {});

    /// Completes when the user interrupts the process (Ctrl-C / SIGINT).
    [[nodiscard]] TaskHandle<std::expected<int, IoError>> ctrl_c(CancellationToken cancellation = {});

    /// Completes on SIGINT or SIGTERM -- the usual "shut down gracefully" pair for servers and tools.
    [[nodiscard]] TaskHandle<std::expected<int, IoError>> shutdown_signal(CancellationToken cancellation = {});

} // namespace SFT::Async
