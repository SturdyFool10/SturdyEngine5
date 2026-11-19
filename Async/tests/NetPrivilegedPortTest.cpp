/// Binding a port below the operating system's unprivileged range must fail with an error that says why and how to get around it,
/// and that a program can recognise (`IoErrorReason::PrivilegedPort`). Runs only where the failure can happen: Linux, as an
/// unprivileged user, with the default port threshold. Elsewhere (root, a lowered threshold, other systems) it passes without checking.

#include <Async/Net.hpp>
#include <Async/Scheduler.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <unistd.h>

namespace {
    using namespace SFT::Async;

    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::fprintf(stderr, "FAILED: %s\n", message);
            ++failures;
        }
    }

    bool failure_is_expected() {
#if defined(__linux__)
        if (geteuid() == 0) return false;
        std::ifstream file{"/proc/sys/net/ipv4/ip_unprivileged_port_start"};
        unsigned start = 1024;
        file >> start;
        return start > 80;
#else
        return false;
#endif
    }

    void check_privileged_port_error(const IoError &error, const char *what) {
        if (std::getenv("STURDY_TEST_SHOW_MESSAGE") != nullptr) std::fprintf(stderr, "%s\n\n", error.message.c_str());
        check(error.code == IoErrorCode::PermissionDenied, what);
        check(error.reason == IoErrorReason::PrivilegedPort, "the error carries the PrivilegedPort reason");
        check(error.message.find("CAP_NET_BIND_SERVICE") != std::string::npos, "the message names the capability");
        check(error.message.find("ip_unprivileged_port_start") != std::string::npos, "the message names the system setting");
        check(error.remedy.find("setcap") != std::string::npos && error.remedy.find("8080") != std::string::npos,
              "the remedy lists setcap and a high-port route");
        check(error.message.find(error.remedy) != std::string::npos, "the message includes the remedy for callers that only see the text");
    }
} // namespace

int main() {
    Scheduler::initialize(2);
    if (failure_is_expected()) {
        auto tcp = TcpListener::listen("127.0.0.1", 80).wait();
        check(!tcp.has_value(), "a TCP listener on port 80 must fail for an unprivileged user");
        if (!tcp) check_privileged_port_error(tcp.error(), "TCP listen reports PermissionDenied");

        auto any = TcpListener::listen("", 443).wait();
        check(!any.has_value(), "a dual-stack listener on port 443 must fail for an unprivileged user");
        if (!any) check_privileged_port_error(any.error(), "dual-stack listen reports PermissionDenied");

        auto udp = UdpSocket::bind("127.0.0.1", 53).wait();
        check(!udp.has_value(), "a UDP socket on port 53 must fail for an unprivileged user");
        if (!udp) check_privileged_port_error(udp.error(), "UDP bind reports PermissionDenied");
    }

    // A high port, or "any port", is not a privileged-port failure and must still work.
    auto fine = TcpListener::listen("127.0.0.1", 0).wait();
    check(fine.has_value(), "an ephemeral port is always available");

    // A port that is simply taken is a different failure and must not be described as a privilege problem.
    if (fine) {
        auto clash = TcpListener::listen("127.0.0.1", *fine->local_port()).wait();
        if (!clash) check(clash.error().reason == IoErrorReason::None, "address-in-use is not reported as a privileged-port error");
    }
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
