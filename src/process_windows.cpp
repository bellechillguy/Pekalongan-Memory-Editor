#include "process.hpp"

#if !defined(_WIN32)
#error "process_windows.cpp hanya boleh dikompilasi di Windows"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <sstream>

namespace {

std::string windowsError(const char* operation) {
    const DWORD code = GetLastError();
    char* buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<char*>(&buffer),
        0,
        nullptr);
    std::string detail = length && buffer ? std::string(buffer, length)
                                          : "Win32 error " + std::to_string(code);
    if (buffer) {
        LocalFree(buffer);
    }
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) {
        detail.pop_back();
    }
    return std::string(operation) + " gagal: " + detail;
}

bool readableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) != 0 || protection == PAGE_NOACCESS) {
        return false;
    }
    const DWORD base = protection & 0xff;
    return base == PAGE_READONLY || base == PAGE_READWRITE ||
           base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
           base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

bool writableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) != 0) {
        return false;
    }
    const DWORD base = protection & 0xff;
    return base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
           base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

std::string quoteArgument(const std::string& argument) {
    if (argument.find_first_of(" \t\"") == std::string::npos) {
        return argument;
    }
    std::string result = "\"";
    std::size_t slashes = 0;
    for (char character : argument) {
        if (character == '\\') {
            ++slashes;
        } else if (character == '\"') {
            result.append(slashes * 2 + 1, '\\');
            result.push_back('\"');
            slashes = 0;
        } else {
            result.append(slashes, '\\');
            slashes = 0;
            result.push_back(character);
        }
    }
    result.append(slashes * 2, '\\');
    result.push_back('\"');
    return result;
}

class WindowsProcessMemory final : public ProcessMemory {
public:
    WindowsProcessMemory(DWORD pid, HANDLE handle, std::string name)
        : pid_(pid), handle_(handle), name_(std::move(name)) {}

    ~WindowsProcessMemory() override {
        if (handle_) {
            CloseHandle(handle_);
        }
    }

    std::uint32_t pid() const override { return pid_; }
    std::string name() const override { return name_; }
    std::string backendName() const override { return "windows-win32"; }

    bool alive() const override {
        DWORD exitCode = 0;
        return GetExitCodeProcess(handle_, &exitCode) && exitCode == STILL_ACTIVE;
    }

    std::vector<MemoryRegion> regions(std::string& error) const override {
        std::vector<MemoryRegion> result;
        MEMORY_BASIC_INFORMATION info{};
        std::uintptr_t address = 0;
        while (VirtualQueryEx(handle_,
                              reinterpret_cast<LPCVOID>(address),
                              &info,
                              sizeof(info)) == sizeof(info)) {
            const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
            if (info.State == MEM_COMMIT) {
                result.push_back(MemoryRegion{
                    static_cast<std::uint64_t>(base),
                    static_cast<std::uint64_t>(info.RegionSize),
                    readableProtection(info.Protect),
                    writableProtection(info.Protect),
                    info.Type == MEM_PRIVATE,
                    info.Type == MEM_PRIVATE ? "private" : "mapped",
                });
            }
            if (info.RegionSize == 0 ||
                base > std::numeric_limits<std::uintptr_t>::max() -
                           info.RegionSize) {
                break;
            }
            address = base + info.RegionSize;
        }
        if (result.empty() && GetLastError() != ERROR_INVALID_PARAMETER) {
            error = windowsError("VirtualQueryEx");
        }
        return result;
    }

    std::vector<ModuleInfo> modules(std::string& error) const override {
        std::vector<ModuleInfo> result;
        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid_);
        if (snapshot == INVALID_HANDLE_VALUE) {
            error = windowsError("CreateToolhelp32Snapshot(module)");
            return result;
        }

        MODULEENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Module32First(snapshot, &entry)) {
            do {
                result.push_back(ModuleInfo{
                    entry.szModule,
                    entry.szExePath,
                    reinterpret_cast<std::uint64_t>(entry.modBaseAddr),
                    static_cast<std::uint64_t>(entry.modBaseSize),
                });
            } while (Module32Next(snapshot, &entry));
        } else {
            error = windowsError("Module32First");
        }
        CloseHandle(snapshot);
        return result;
    }

    bool read(std::uint64_t address,
              void* destination,
              std::size_t size,
              std::string& error) const override {
        SIZE_T transferred = 0;
        if (ReadProcessMemory(handle_,
                              reinterpret_cast<LPCVOID>(
                                  static_cast<std::uintptr_t>(address)),
                              destination,
                              size,
                              &transferred) &&
            transferred == size) {
            return true;
        }
        error = windowsError("ReadProcessMemory");
        return false;
    }

    bool write(std::uint64_t address,
               const void* source,
               std::size_t size,
               std::string& error) override {
        SIZE_T transferred = 0;
        if (WriteProcessMemory(handle_,
                               reinterpret_cast<LPVOID>(
                                   static_cast<std::uintptr_t>(address)),
                               source,
                               size,
                               &transferred) &&
            transferred == size) {
            return true;
        }
        error = windowsError("WriteProcessMemory");
        return false;
    }

private:
    DWORD pid_{};
    HANDLE handle_{};
    std::string name_;
};

std::shared_ptr<ProcessMemory> openProcess(DWORD pid,
                                           const std::string& name,
                                           std::string& error) {
    const DWORD access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                         PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                         SYNCHRONIZE;
    HANDLE handle = OpenProcess(access, FALSE, pid);
    if (!handle) {
        error = windowsError("OpenProcess");
        return nullptr;
    }
    return std::make_shared<WindowsProcessMemory>(pid, handle, name);
}

}  // namespace

std::vector<ProcessInfo> ProcessMemory::list(std::string& error) {
    std::vector<ProcessInfo> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        error = windowsError("CreateToolhelp32Snapshot");
        return processes;
    }

    PROCESSENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        do {
            processes.push_back({entry.th32ProcessID,
                                 entry.szExeFile,
                                 entry.szExeFile});
        } while (Process32Next(snapshot, &entry));
    } else {
        error = windowsError("Process32First");
    }
    CloseHandle(snapshot);
    std::sort(processes.begin(), processes.end(),
              [](const ProcessInfo& left, const ProcessInfo& right) {
                  return left.pid < right.pid;
              });
    return processes;
}

std::shared_ptr<ProcessMemory> ProcessMemory::attach(std::uint32_t pid,
                                                     std::string& error) {
    std::string name = "PID " + std::to_string(pid);
    std::string listError;
    for (const auto& process : list(listError)) {
        if (process.pid == pid) {
            name = process.name;
            break;
        }
    }
    return openProcess(static_cast<DWORD>(pid), name, error);
}

std::shared_ptr<ProcessMemory> ProcessMemory::launch(
    const std::vector<std::string>& arguments,
    std::string& error) {
    if (arguments.empty()) {
        error = "launch membutuhkan path executable";
        return nullptr;
    }

    std::string commandLine;
    for (const auto& argument : arguments) {
        if (!commandLine.empty()) {
            commandLine.push_back(' ');
        }
        commandLine += quoteArgument(argument);
    }
    std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr,
                        mutableCommand.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        0,
                        nullptr,
                        nullptr,
                        &startup,
                        &process)) {
        error = windowsError("CreateProcess");
        return nullptr;
    }
    CloseHandle(process.hThread);
    auto name = std::filesystem::path(arguments.front()).filename().string();
    return std::make_shared<WindowsProcessMemory>(
        process.dwProcessId, process.hProcess, std::move(name));
}
