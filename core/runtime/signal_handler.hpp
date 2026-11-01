#pragma once

#include <atomic>

namespace anolis {
namespace runtime {

class SignalHandler {
public:
    static void install();
    /// On POSIX, ignore SIGPIPE, so a write to a provider that has closed its
    /// stdin fails with EPIPE instead of ending the process (#549). The first
    /// call saves the disposition the process started with.
    static void ignore_broken_pipe();

    /// The SIGPIPE disposition the process started with, before
    /// ignore_broken_pipe() replaced it; SIG_DFL if it has not run. A spawned
    /// provider gets it back before exec, so providers start as they did
    /// before: under systemd's IgnoreSIGPIPE=yes, with SIGPIPE ignored.
    using Disposition = void (*)(int);
    static Disposition inherited_broken_pipe_disposition();
    static bool is_shutdown_requested();

private:
    static void handle_signal(int signal);
    static std::atomic<bool> shutdown_requested_;
};

}  // namespace runtime
}  // namespace anolis