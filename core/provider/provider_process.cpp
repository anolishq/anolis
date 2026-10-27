/**
 * @file provider_process.cpp
 * @brief Platform-specific child-process lifecycle for spawned providers.
 *
 * This layer owns process creation and teardown details only. Protocol
 * handshakes and request/response validation live one layer up in
 * `ProviderHandle`.
 */

#include "provider_process.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
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

struct ProviderProcess::Impl {
#ifdef _WIN32
    HANDLE process_handle = nullptr;
    HANDLE job = nullptr;  // holds the child and anything it starts (#563)
#else
    pid_t pid = -1;
    pid_t pgid = -1;  // the child's own process group (#563)
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

ProviderProcess::~ProviderProcess() { shutdown(); }

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

    // A job holds the child and whatever it starts, so a forced termination
    // ends all of it (#563). The child starts suspended so it is in the job
    // before it can start anything.
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

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
                                  CREATE_SUSPENDED,    // dwCreationFlags
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
        if (job) {
            CloseHandle(job);
        }
        return false;
    }

    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        LOG_WARN("[" << provider_id_ << "] Could not put the provider in a job object (" << GetLastError()
                     << "); a forced stop will end only the provider process");
        CloseHandle(job);
        job = nullptr;
    }
    ResumeThread(pi.hThread);

    // Close thread handle (don't need it)
    CloseHandle(pi.hThread);

    // Store handles
    impl_->process_handle = pi.hProcess;
    impl_->job = job;

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

    impl_->pid = fork();
    if (impl_->pid < 0) {
        error_ = "Fork failed";
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return false;
    }

    if (impl_->pid == 0) {
        // Child process. Its own process group, so a forced termination ends
        // anything it starts as well (#563).
        setpgid(0, 0);

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

    // Parent process. Set the group here too, so it exists before either side
    // runs on; the call fails harmlessly once the child has exec'd.
    setpgid(impl_->pid, impl_->pid);
    impl_->pgid = impl_->pid;
    close(stdin_pipe[0]);   // Close read end of stdin pipe
    close(stdout_pipe[1]);  // Close write end of stdout pipe

    impl_->stdin_write_fd = stdin_pipe[1];
    impl_->stdout_read_fd = stdout_pipe[0];

    client_.set_handles(impl_->stdin_write_fd, impl_->stdout_read_fd);

    LOG_INFO("[" << provider_id_ << "] Process spawned successfully (PID=" << impl_->pid << ")");
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
    if (impl_->pid <= 0) return false;

    // Use waitpid(WNOHANG) for child liveness. kill(pid, 0) is not sufficient
    // because zombies still "exist" and would be misclassified as running.
    int status = 0;
    while (true) {
        pid_t result = waitpid(impl_->pid, &status, WNOHANG);
        if (result == 0) {
            return true;  // Child still running
        }
        if (result == impl_->pid) {
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

bool ProviderProcess::shutdown() {
    const bool running = is_running();
    bool exited = true;

    if (running) {
        LOG_INFO("[" << provider_id_ << "] Initiating shutdown");

        // Shutdown policy is ordered: first close stdin to signal EOF, then
        // wait for a clean exit, and only then escalate to termination.
        client_.close_stdin();

        exited = wait_for_exit(shutdown_timeout_ms_);

        if (exited) {
            LOG_INFO("[" << provider_id_ << "] Clean shutdown");
        } else {
            LOG_WARN("[" << provider_id_ << "] Timeout - forcing termination");
            force_terminate();
            exited = wait_for_exit(500);
        }
    } else {
#ifndef _WIN32
        // Normalize tracked PID state when process already exited before shutdown().
        wait_for_exit(0);
#endif
    }

    // A provider can exit, cleanly or not, and leave behind what it started:
    // a wrapper script's worker, say. That is ended too, so nothing of the old
    // provider runs beside its replacement (#563).
    if (group_alive()) {
        LOG_WARN("[" << provider_id_ << "] Processes the provider started are still running - terminating them");
        force_terminate();
    }
    const bool group_exited = wait_for_group_exit(500);
    if (!exited || !group_exited) {
        LOG_ERROR("[" << provider_id_ << "] " << (exited ? "Processes the provider started" : "The provider process")
                      << " did not exit after being killed");
    }

    // Always release pipe handles, even if the process already exited. This
    // keeps provider teardown idempotent from the caller's point of view.
    client_.close_stdin();
    client_.close_stdout();

#ifndef _WIN32
    impl_->stdin_write_fd = -1;
    impl_->stdout_read_fd = -1;
#endif

    // What has not exited stays tracked, so a later call can try again.
#ifdef _WIN32
    if (impl_->process_handle && exited) {
        CloseHandle(impl_->process_handle);
        impl_->process_handle = nullptr;
    }
    if (impl_->job && group_exited) {
        CloseHandle(impl_->job);
        impl_->job = nullptr;
    }
#else
    if (group_exited) {
        impl_->pgid = -1;
    }
#endif
    return exited && group_exited;
}

bool ProviderProcess::wait_for_exit(int timeout_ms) {
#ifdef _WIN32
    if (!impl_->process_handle) return true;
    DWORD result = WaitForSingleObject(impl_->process_handle, timeout_ms);
    return result == WAIT_OBJECT_0;
#else
    if (impl_->pid <= 0) {
        return true;
    }

    auto start = std::chrono::steady_clock::now();
    while (true) {
        int status;
        pid_t result = waitpid(impl_->pid, &status, WNOHANG);
        if (result == impl_->pid) {
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
    if (impl_->job) {
        TerminateJobObject(impl_->job, 1);
    } else if (impl_->process_handle) {
        TerminateProcess(impl_->process_handle, 1);
    }
#else
    if (impl_->pgid > 0) {
        kill(-impl_->pgid, SIGKILL);
    } else if (impl_->pid > 0) {
        kill(impl_->pid, SIGKILL);
    }
#endif
}

bool ProviderProcess::group_alive() const {
#ifdef _WIN32
    if (!impl_->job) {
        return false;
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info = {};
    if (!QueryInformationJobObject(impl_->job, JobObjectBasicAccountingInformation, &info, sizeof(info), NULL)) {
        return true;  // cannot tell; do not report it gone
    }
    return info.ActiveProcesses > 0;
#else
    if (impl_->pgid <= 0) {
        return false;
    }
    if (kill(-impl_->pgid, 0) == -1 && errno == ESRCH) {
        return false;
    }
#ifdef __linux__
    // kill() also finds zombies, and an orphaned member stays one until
    // whoever reaps orphans does so, which in a container may be never. A
    // zombie holds no device, so only a live member counts.
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator("/proc", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        std::ifstream stat(entry.path() / "stat");
        std::string line;
        if (!std::getline(stat, line)) {
            continue;
        }
        // pid (comm) state ppid pgrp ...; comm may hold spaces and parentheses.
        const auto close_paren = line.rfind(')');
        if (close_paren == std::string::npos) {
            continue;
        }
        char state = 0;
        long ppid = 0;
        long pgrp = 0;
        if (std::sscanf(line.c_str() + close_paren + 1, " %c %ld %ld", &state, &ppid, &pgrp) == 3 &&
            pgrp == impl_->pgid && state != 'Z') {
            return true;
        }
    }
    return ec ? true : false;  // without /proc, trust kill()
#else
    return true;
#endif
#endif
}

bool ProviderProcess::wait_for_group_exit(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (group_alive()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

}  // namespace provider
}  // namespace anolis
