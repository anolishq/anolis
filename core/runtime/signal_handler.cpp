#include "signal_handler.hpp"

#include <csignal>

namespace anolis {
namespace runtime {

std::atomic<bool> SignalHandler::shutdown_requested_{false};

void SignalHandler::install() {
    shutdown_requested_.store(false);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
#ifdef _WIN32
    // Windows: also handle SIGBREAK (Ctrl+Break)
    std::signal(SIGBREAK, handle_signal);
#endif
}

bool SignalHandler::is_shutdown_requested() { return shutdown_requested_.load(); }

void SignalHandler::handle_signal(int) {
    // Async-signal-safe: only atomic operations allowed
    shutdown_requested_.store(true);
}

}  // namespace runtime
}  // namespace anolis
