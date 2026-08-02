#include "process.hpp"

#if !defined(__linux__)
#error "process_linux.cpp hanya boleh dikompilasi di Linux"
#endif

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

namespace {

std::string readTextFile(const std::filesystem::path& path, bool binary = false) {
    std::ifstream input(path, binary ? std::ios::binary : std::ios::in);
    if (!input) {
        return {};
    }
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::string processName(std::uint32_t pid) {
    auto name = readTextFile("/proc/" + std::to_string(pid) + "/comm");
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
        name.pop_back();
    }
    return name;
}

std::string processCommand(std::uint32_t pid) {
    auto command = readTextFile("/proc/" + std::to_string(pid) + "/cmdline", true);
    for (char& character : command) {
        if (character == '\0') {
            character = ' ';
        }
    }
    while (!command.empty() && command.back() == ' ') {
        command.pop_back();
    }
    return command;
}

std::string memoryError(const char* operation, std::uint32_t pid) {
    std::ostringstream message;
    message << operation << " PID " << pid << " gagal: " << std::strerror(errno);
    if (errno == EPERM || errno == EACCES) {
        message << ". Di Linux, gunakan perintah launch dari editor atau jalankan "
                   "editor dengan izin ptrace yang sesuai";
    }
    return message.str();
}

bool staleMemoryFileError(int error) {
    return error == ESRCH || error == EIO || error == EBADF;
}

class LinuxProcessMemory final : public ProcessMemory {
public:
    LinuxProcessMemory(std::uint32_t pid, std::string name, bool child)
        : pid_(pid), name_(std::move(name)), child_(child) {
        const char* requestedBackend = std::getenv("PEKALONGAN_IO");
        procfsOnly_ = requestedBackend != nullptr &&
                      std::string(requestedBackend) == "procfs";
        const char* requestedRegions = std::getenv("PEKALONGAN_REGIONS");
        smapsRegions_ = requestedRegions != nullptr &&
                        std::string(requestedRegions) == "smaps";
        (void)reopenMemoryFile();
    }

    ~LinuxProcessMemory() override {
        if (memoryFd_ >= 0) {
            ::close(memoryFd_);
        }
        if (child_ && !reaped_) {
            (void)::waitpid(static_cast<pid_t>(pid_), nullptr, WNOHANG);
        }
    }

    std::uint32_t pid() const override { return pid_; }
    std::string name() const override { return name_; }
    std::string backendName() const override {
        return std::string(procfsOnly_ ? "linux-procfs-only"
                                       : "linux-process-vm+procfs") +
               (smapsRegions_ ? "+smaps-regions" : "+maps-regions");
    }

    bool alive() const override {
        if (child_ && !reaped_) {
            const auto result = ::waitpid(static_cast<pid_t>(pid_), nullptr, WNOHANG);
            if (result == static_cast<pid_t>(pid_)) {
                reaped_ = true;
                return false;
            }
        }
        if (::kill(static_cast<pid_t>(pid_), 0) == 0 || errno == EPERM) {
            const auto stat = readTextFile("/proc/" + std::to_string(pid_) + "/stat");
            const auto closeParen = stat.rfind(')');
            if (closeParen != std::string::npos && closeParen + 2 < stat.size() &&
                stat[closeParen + 2] == 'Z') {
                return false;
            }
            return true;
        }
        return false;
    }

    std::vector<MemoryRegion> regions(std::string& error) const override {
        std::vector<MemoryRegion> result;
        const auto regionFile = std::string("/proc/") + std::to_string(pid_) +
                                (smapsRegions_ ? "/smaps" : "/maps");
        std::ifstream maps(regionFile);
        if (!maps) {
            error = "tidak dapat membaca " + regionFile;
            return result;
        }

        std::string line;
        while (std::getline(maps, line)) {
            std::istringstream parser(line);
            std::string range;
            std::string permissions;
            std::string offset;
            std::string device;
            std::string inode;
            if (!(parser >> range >> permissions >> offset >> device >> inode)) {
                continue;
            }
            const auto dash = range.find('-');
            if (dash == std::string::npos || permissions.size() < 4) {
                continue;
            }

            std::string path;
            std::getline(parser, path);
            const auto first = path.find_first_not_of(' ');
            if (first != std::string::npos) {
                path.erase(0, first);
            } else {
                path.clear();
            }

            try {
                const auto begin = std::stoull(range.substr(0, dash), nullptr, 16);
                const auto end = std::stoull(range.substr(dash + 1), nullptr, 16);
                result.push_back(MemoryRegion{
                    begin,
                    end - begin,
                    permissions[0] == 'r',
                    permissions[1] == 'w',
                    permissions[3] == 'p',
                    path,
                });
            } catch (const std::exception&) {
                continue;
            }
        }
        return result;
    }

    std::vector<ModuleInfo> modules(std::string& error) const override {
        const auto mappedRegions = regions(error);
        if (!error.empty()) {
            return {};
        }

        struct Bounds {
            std::uint64_t begin{UINT64_MAX};
            std::uint64_t end{};
        };
        std::map<std::string, Bounds> grouped;
        for (const auto& region : mappedRegions) {
            if (region.name.empty() || region.name.front() != '/') {
                continue;
            }
            auto& bounds = grouped[region.name];
            bounds.begin = std::min(bounds.begin, region.base);
            bounds.end = std::max(bounds.end, region.base + region.size);
        }

        std::vector<ModuleInfo> result;
        result.reserve(grouped.size());
        for (const auto& [path, bounds] : grouped) {
            result.push_back(ModuleInfo{
                std::filesystem::path(path).filename().string(),
                path,
                bounds.begin,
                bounds.end - bounds.begin,
            });
        }
        return result;
    }

    bool read(std::uint64_t address,
              void* destination,
              std::size_t size,
              std::string& error) const override {
        int processVmError = 0;
        if (!procfsOnly_) {
            iovec local{destination, size};
            iovec remote{
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)),
                size};
            const auto transferred = static_cast<ssize_t>(::syscall(
                SYS_process_vm_readv,
                static_cast<pid_t>(pid_),
                &local,
                1UL,
                &remote,
                1UL,
                0UL));
            if (transferred == static_cast<ssize_t>(size)) {
                return true;
            }
            processVmError = errno;
        }

        if (memoryFd_ >= 0) {
            auto fallback = ::pread(memoryFd_,
                                    destination,
                                    size,
                                    static_cast<off_t>(address));
            if (fallback == static_cast<ssize_t>(size)) {
                return true;
            }
            const int fallbackError = errno;
            if (staleMemoryFileError(fallbackError) &&
                reopenMemoryFile()) {
                fallback = ::pread(memoryFd_,
                                   destination,
                                   size,
                                   static_cast<off_t>(address));
                if (fallback == static_cast<ssize_t>(size)) {
                    return true;
                }
            }
        } else if (reopenMemoryFile()) {
            const auto fallback = ::pread(memoryFd_,
                                          destination,
                                          size,
                                          static_cast<off_t>(address));
            if (fallback == static_cast<ssize_t>(size)) {
                return true;
            }
        }
        if (processVmError != 0) {
            errno = processVmError;
        }
        error = memoryError("membaca memory", pid_);
        return false;
    }

    bool write(std::uint64_t address,
               const void* source,
               std::size_t size,
               std::string& error) override {
        int processVmError = 0;
        if (!procfsOnly_) {
            iovec local{const_cast<void*>(source), size};
            iovec remote{
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)),
                size};
            const auto transferred = static_cast<ssize_t>(::syscall(
                SYS_process_vm_writev,
                static_cast<pid_t>(pid_),
                &local,
                1UL,
                &remote,
                1UL,
                0UL));
            if (transferred == static_cast<ssize_t>(size)) {
                return true;
            }
            processVmError = errno;
        }

        if (memoryFd_ >= 0) {
            auto fallback = ::pwrite(memoryFd_,
                                     source,
                                     size,
                                     static_cast<off_t>(address));
            if (fallback == static_cast<ssize_t>(size)) {
                return true;
            }
            const int fallbackError = errno;
            if (staleMemoryFileError(fallbackError) &&
                reopenMemoryFile()) {
                fallback = ::pwrite(memoryFd_,
                                    source,
                                    size,
                                    static_cast<off_t>(address));
                if (fallback == static_cast<ssize_t>(size)) {
                    return true;
                }
            }
        } else if (reopenMemoryFile()) {
            const auto fallback = ::pwrite(memoryFd_,
                                           source,
                                           size,
                                           static_cast<off_t>(address));
            if (fallback == static_cast<ssize_t>(size)) {
                return true;
            }
        }
        if (processVmError != 0) {
            errno = processVmError;
        }
        error = memoryError("menulis memory", pid_);
        return false;
    }

private:
    bool reopenMemoryFile() const {
        if (memoryFd_ >= 0) {
            ::close(memoryFd_);
        }
        const auto path = "/proc/" + std::to_string(pid_) + "/mem";
        memoryFd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        return memoryFd_ >= 0;
    }

    std::uint32_t pid_;
    std::string name_;
    bool child_{};
    mutable bool reaped_{};
    mutable int memoryFd_{-1};
    bool procfsOnly_{};
    bool smapsRegions_{};
};

}  // namespace

std::vector<ProcessInfo> ProcessMemory::list(std::string& error) {
    std::vector<ProcessInfo> processes;
    std::error_code filesystemError;
    for (const auto& entry :
         std::filesystem::directory_iterator("/proc", filesystemError)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto filename = entry.path().filename().string();
        if (filename.empty() ||
            filename.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        try {
            const auto pid = static_cast<std::uint32_t>(std::stoul(filename));
            auto name = processName(pid);
            if (!name.empty()) {
                processes.push_back({pid, std::move(name), processCommand(pid)});
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    if (filesystemError) {
        error = "gagal membaca /proc: " + filesystemError.message();
    }
    std::sort(processes.begin(), processes.end(),
              [](const ProcessInfo& left, const ProcessInfo& right) {
                  return left.pid < right.pid;
              });
    return processes;
}

std::shared_ptr<ProcessMemory> ProcessMemory::attach(std::uint32_t pid,
                                                     std::string& error) {
    const auto name = processName(pid);
    if (name.empty()) {
        error = "PID " + std::to_string(pid) + " tidak ditemukan";
        return nullptr;
    }
    return std::make_shared<LinuxProcessMemory>(pid, name, false);
}

std::shared_ptr<ProcessMemory> ProcessMemory::launch(
    const std::vector<std::string>& arguments,
    std::string& error) {
    if (arguments.empty()) {
        error = "launch membutuhkan path executable";
        return nullptr;
    }

    int statusPipe[2]{};
    if (::pipe2(statusPipe, O_CLOEXEC) != 0) {
        error = std::string("gagal membuat pipe: ") + std::strerror(errno);
        return nullptr;
    }

    const pid_t child = ::fork();
    if (child < 0) {
        error = std::string("fork gagal: ") + std::strerror(errno);
        ::close(statusPipe[0]);
        ::close(statusPipe[1]);
        return nullptr;
    }
    if (child == 0) {
        ::close(statusPipe[0]);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv.front(), argv.data());
        const int execError = errno;
        const auto reported =
            ::write(statusPipe[1], &execError, sizeof(execError));
        (void)reported;
        _exit(127);
    }

    ::close(statusPipe[1]);
    int execError = 0;
    const auto received = ::read(statusPipe[0], &execError, sizeof(execError));
    ::close(statusPipe[0]);
    if (received > 0) {
        (void)::waitpid(child, nullptr, 0);
        error = "exec gagal: " + std::string(std::strerror(execError));
        return nullptr;
    }

    auto name = std::filesystem::path(arguments.front()).filename().string();
    return std::make_shared<LinuxProcessMemory>(
        static_cast<std::uint32_t>(child), std::move(name), true);
}
