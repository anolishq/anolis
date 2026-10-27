/**
 * @file provider_process.cpp
 * @brief Platform-specific child-process lifecycle for spawned providers.
 *
 * This layer owns process creation and teardown details only. Protocol
 * handshakes and request/response validation live one layer up in
 * `ProviderHandle`.
 */

#include "provider_process.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "logging/logger.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace anolis {
namespace provider {

// is_running() is called from other threads (HTTP, polling, the main loop)
// while shutdown() runs on the one restarting the provider (#557). So the pid
// is atomic, and the Windows process handle, set once by spawn(), is closed
// only by the destructor.
struct ProviderProcess::Impl {
#ifdef _WIN32
    HANDLE process_handle = nullptr;
#else
    std::atomic<pid_t> pid{-1};
    int stdin_write_fd = -1;
    int stdout_read_fd = -1;
#endif
};

ProviderProcess::ProviderProcess(const std::string &provider_id, const std::string &executable_path,
                                 const std::vector<std::string> &args, int shutdown_timeout_ms)
    : provider_id_(provider_id),
      executable_path_(executable_path),
      args_(args),
      shutdown_timeout_ms_(shutdown_timeout_ms),
      impl_(std::make_unique<Impl>()) {}

ProviderProcess::~ProviderProcess() {
    shutdown();
#ifdef _WIN32
    if (impl_->process_handle) {
        CloseHandle(impl_->process_handle);
        impl_->process_handle = nullptr;
    }
#endif
}

bool ProviderProcess::spawn() {
    LOG_INFO("[" << provider_id_ << "] Spawning: " << executable_path_);

    // Fail before fork/CreateProcess if the configured binary path is missing.
    if (!std::filesystem::exists(executable_path_)) {
        error_ = "Executable not found: " + executable_path_;
        LOG_ERROR("[" << provider_id_ << "] " << error_);
        return false;
    }

#ifdef _WIN32
    return spawn_windows();
#else
    return spawn_linux();
#endif
}

#ifdef _WIN32
bool ProviderProcess::spawn_windows() {
    // Create pipes with inheritable handles
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE stdin_read_child = NULL;
    HANDLE stdin_write_parent = NULL;
    HANDLE stdout_read_parent = NULL;
    HANDLE stdout_write_child = NULL;

    // Create stdin pipe
    if (!CreatePipe(&stdin_read_child, &stdin_write_parent, &sa, 0)) {
        error_ = "Failed to create stdin pipe: " + std::to_string(GetLastError());
        return false;
    }
    // Parent's write handle should not be inherited
    SetHandleInformation(stdin_write_parent, HANDLE_FLAG_INHERIT, 0);

    // Create stdout pipe
    if (!CreatePipe(&stdout_read_parent, &stdout_write_child, &sa, 0)) {
        error_ = "Failed to create stdout pipe: " + std::to_string(GetLastError());
        CloseHandle(stdin_read_child);
        CloseHandle(stdin_write_parent);
        return false;
    }
    // Parent's read handle should not be inherited
    SetHandleInformation(stdout_read_parent, HANDLE_FLAG_INHERIT, 0);

    // Setup STARTUPINFO with redirected handles
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read_child;
    si.hStdOutput = stdout_write_child;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);  // Inherit stderr

    PROCESS_INFORMATION pi = {};

    // Convert path to absolute
    std::string abs_path = std::filesystem::absolute(executable_path_).string();

    // CreateProcess (command line must be mutable)
    std::string cmdline = "\"" + abs_path + "\"";
    for (const auto &arg : args_) {
        cmdline += " \"" + arg + "\"";
    }
    std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back('\0');

    BOOL success = CreateProcessA(NULL,  // lpApplicationName (if NULL, first token of command line is used)
                                  cmdline_buf.data(),  // lpCommandLine (mutable)
                                  NULL,                // lpProcessAttributes
                                  NULL,                // lpThreadAttributes
                                  TRUE,                // bInheritHandles
                                  0,                   // dwCreationFlags
                                  NULL,                // lpEnvironment
                                  NULL,                // lpCurrentDirectory
                                  &si,                 // lpStartupInfo
                                  &pi                  // lpProcessInformation
    );

    // Close child-side handles in parent
    CloseHandle(stdin_read_child);
    CloseHandle(stdout_write_child);

    if (!success) {
        error_ = "CreateProcess failed: " + std::to_string(GetLastError());
        CloseHandle(stdin_write_parent);
        CloseHandle(stdout_read_parent);
        return false;
    }

    // Close thread handle (don't need it)
    CloseHandle(pi.hThread);

    // Store handles
    impl_->process_handle = pi.hProcess;

    // Give to client
    client_.set_handles(stdin_write_parent, stdout_read_parent);

    LOG_INFO("[" << provider_id_ << "] Process spawned successfully (PID=" << pi.dwProcessId << ")");
    return true;
}
#else
bool ProviderProcess::spawn_linux() {
    // The runtime keeps the parent write/read ends and hands them to
    // FramedStdioClient; the child gets the opposite ends mapped onto stdin
    // and stdout before exec.
    int stdin_pipe[2];
    int stdout_pipe[2];

    if (pipe(stdin_pipe) < 0) {
        error_ = "Failed to create stdin pipe";
        return false;
    }
    if (pipe(stdout_pipe) < 0) {
        error_ = "Failed to create stdout pipe";
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        error_ = "Fork failed";
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // Child process
        // Redirect stdin
        dup2(stdin_pipe[0], STDIN_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);

        // Redirect stdout
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        // stderr stays connected to parent's stderr

        // Execute provider
        std::string abs_path = std::filesystem::absolute(executable_path_).string();

        // Construct argv
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(abs_path.c_str()));
        for (const auto &arg : args_) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execv(abs_path.c_str(), argv.data());

        // If we get here, exec failed
        LOG_ERROR("exec failed for " << abs_path << ": " << errno);
        _exit(1);
    }

    // Parent process
    impl_->pid.store(pid);
    close(stdin_pipe[0]);   // Close read end of stdin pipe
    close(stdout_pipe[1]);  // Close write end of stdout pipe

    impl_->stdin_write_fd = stdin_pipe[1];
    impl_->stdout_read_fd = stdout_pipe[0];

    client_.set_handles(impl_->stdin_write_fd, impl_->stdout_read_fd);

    LOG_INFO("[" << provider_id_ << "] Process spawned successfully (PID=" << pid << ")");
    return true;
}
#endif

bool ProviderProcess::is_running() const {
#ifdef _WIN32
    if (!impl_->process_handle) return false;
    DWORD exit_code;
    if (!GetExitCodeProcess(impl_->process_handle, &exit_code)) return false;
    return exit_code == STILL_ACTIVE;
#else
    const pid_t pid = impl_->pid.load();
    if (pid <= 0) return false;

    // Use waitpid(WNOHANG) for child liveness. kill(pid, 0) is not sufficient
    // because zombies still "exist" and would be misclassified as running.
    int status = 0;
    while (true) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == 0) {
            return true;  // Child still running
        }
        if (result == pid) {
            return false;  // Child exited (reaped by this call)
        }
        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                return false;  // Already reaped
            }
            return false;  // Conservative on unexpected errors
        }
    }
#endif
}

void ProviderProcess::shutdown() {
    const bool running = is_running();

    if (running) {
        LOG_INFO("[" << provider_id_ << "] Initiating shutdown");

        // Shutdown policy is ordered: first close stdin to signal EOF, then
        // wait for a clean exit, and only then escalate to termination.
        client_.close_stdin();

        bool exited = wait_for_exit(shutdown_timeout_ms_);

        if (exited) {
            LOG_INFO("[" << provider_id_ << "] Clean shutdown");
        } else {
            LOG_WARN("[" << provider_id_ << "] Timeout - forcing termination");
            force_terminate();
            wait_for_exit(500);
        }
    } else {
#ifndef _WIN32
        // Normalize tracked PID state when process already exited before shutdown().
        wait_for_exit(0);
#endif
    }

    // Always release pipe handles, even if the process already exited. This
    // keeps provider teardown idempotent from the caller's point of view.
    client_.close_stdin();
    client_.close_stdout();

#ifndef _WIN32
    impl_->stdin_write_fd = -1;
    impl_->stdout_read_fd = -1;
#endif
}

bool ProviderProcess::wait_for_exit(int timeout_ms) {
#ifdef _WIN32
    if (!impl_->process_handle) return true;
    DWORD result = WaitForSingleObject(impl_->process_handle, timeout_ms);
    return result == WAIT_OBJECT_0;
#else
    const pid_t pid = impl_->pid.load();
    if (pid <= 0) {
        return true;
    }

    auto start = std::chrono::steady_clock::now();
    while (true) {
        int status;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            // Process exited
            impl_->pid = -1;
            return true;
        }
        if (result == -1) {
            // Error occurred
            if (errno == ECHILD) {
                // Process already reaped (shouldn't happen with our fixed is_running, but handle defensively)
                impl_->pid = -1;
                return true;
            }
            if (errno == EINTR) {
                // Interrupted by signal, retry
                continue;
            }
            // Other error
            return false;
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= timeout_ms) {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
}

void ProviderProcess::force_terminate() {
#ifdef _WIN32
    if (impl_->process_handle) {
        TerminateProcess(impl_->process_handle, 1);
    }
#else
    if (const pid_t pid = impl_->pid.load(); pid > 0) {
        kill(pid, SIGKILL);
    }
#endif
}

}  // namespace provider
}  // namespace anolis
