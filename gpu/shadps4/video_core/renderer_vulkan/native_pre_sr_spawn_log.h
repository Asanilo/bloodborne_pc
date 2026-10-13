// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace Vulkan::NativePreSrSpawnLog {
// Owner-thread only. Keep both child streams off the renderer timing ledger.
// The new log stays as failure evidence; never reopen/truncate an existing path.
class Actions {
public:
    explicit Actions(const std::string& path) {
        try {
            fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0) Fail(errno, "preSR exclusive helper log open");
            // dup2 targets must not alias the source/close action if inherited
            // stdin/stdout/stderr were closed in this process.
            if (fd <= STDERR_FILENO) {
                const int high = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
                if (high < 0) Fail(errno, "preSR helper log move fd");
                ::close(fd);
                fd = high;
            }
            if (::fchmod(fd, 0600) != 0) Fail(errno, "preSR helper log permissions");
            struct stat st{};
            if (::fstat(fd, &st) != 0) Fail(errno, "preSR helper log stat");
            if (!S_ISREG(st.st_mode) || (st.st_mode & 0777) != 0600 || st.st_nlink != 1)
                Fail(EINVAL, "preSR helper log regular exclusive0600");
            Check(::posix_spawn_file_actions_init(&actions), "preSR log actions init");
            initialized = true;
            Check(::posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO), "preSR stdout dup2");
            Check(::posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO), "preSR stderr dup2");
            Check(::posix_spawn_file_actions_addclose(&actions, fd), "preSR child log source close");
        } catch (...) {
            Clear();
            throw;
        }
    }
    Actions(const Actions&) = delete;
    Actions& operator=(const Actions&) = delete;
    ~Actions() { Clear(); }
    const posix_spawn_file_actions_t* Get() const noexcept { return &actions; }
private:
    [[noreturn]] static void Fail(int code, const char* what) {
        throw std::system_error(code, std::generic_category(), what);
    }
    static void Check(int code, const char* what) { if (code != 0) Fail(code, what); }
    void Clear() noexcept {
        if (initialized) { ::posix_spawn_file_actions_destroy(&actions); initialized = false; }
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    int fd = -1;
    bool initialized = false;
    posix_spawn_file_actions_t actions{};
};
inline int Spawn(pid_t* child, const char* executable, char* const argv[],
                 char* const envp[], const std::string& log_path) {
    const Actions actions(log_path);
    // On success AND API failure, parent descriptor/actions are gone on return.
    // argv/environment/attributes/process ancestry are otherwise unchanged.
    return ::posix_spawn(child, executable, actions.Get(), nullptr, argv, envp);
}
} // namespace Vulkan::NativePreSrSpawnLog
