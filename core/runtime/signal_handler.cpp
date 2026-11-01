#include "signal_handler.hpp"

#include <csignal>
#include <mutex>

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

namespace {

SignalHandler::Disposition inherited_sigpipe = SIG_DFL;
std::once_flag sigpipe_saved;

}  // namespace

void SignalHandler::ignore_broken_pipe() {
#ifndef _WIN32
    // Saved once only: a second initialize() would otherwise save SIG_IGN.
    std::call_once(sigpipe_saved, [] {
        const Disposition previous = std::signal(SIGPIPE, SIG_IGN);
        if (previous != SIG_ERR) {
            inherited_sigpipe = previous;
        }
    });
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

SignalHandler::Disposition SignalHandler::inherited_broken_pipe_disposition() { return inherited_sigpipe; }

bool SignalHandler::is_shutdown_requested() { return shutdown_requested_.load(); }

void SignalHandler::handle_signal(int) {
    // Async-signal-safe: only atomic operations allowed
    shutdown_requested_.store(true);
}

}  // namespace runtime
}  // namespace anolis
