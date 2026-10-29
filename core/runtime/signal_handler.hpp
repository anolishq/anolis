#pragma once

#include <atomic>

namespace anolis {
namespace runtime {

class SignalHandler {
public:
    static void install();
    /// On POSIX, ignore SIGPIPE, so a write to a provider that has closed its
    /// stdin fails with EPIPE instead of ending the process (#549). Spawned
    /// providers get the default disposition back before exec.
    static void ignore_broken_pipe();
    static bool is_shutdown_requested();

private:
    static void handle_signal(int signal);
    static std::atomic<bool> shutdown_requested_;
};

}  // namespace runtime
}  // namespace anolis