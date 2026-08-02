#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ProcessInfo {
    std::uint32_t pid{};
    std::string name;
    std::string command;
};

struct MemoryRegion {
    std::uint64_t base{};
    std::uint64_t size{};
    bool readable{};
    bool writable{};
    bool privateMapping{};
    std::string name;
};

struct ModuleInfo {
    std::string name;
    std::string path;
    std::uint64_t base{};
    std::uint64_t size{};
};

class ProcessMemory {
public:
    virtual ~ProcessMemory() = default;

    [[nodiscard]] virtual std::uint32_t pid() const = 0;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::string backendName() const = 0;
    [[nodiscard]] virtual bool alive() const = 0;
    [[nodiscard]] virtual std::vector<MemoryRegion> regions(
        std::string& error) const = 0;
    [[nodiscard]] virtual std::vector<ModuleInfo> modules(
        std::string& error) const = 0;
    virtual bool read(std::uint64_t address,
                      void* destination,
                      std::size_t size,
                      std::string& error) const = 0;
    virtual bool write(std::uint64_t address,
                       const void* source,
                       std::size_t size,
                       std::string& error) = 0;

    [[nodiscard]] static std::vector<ProcessInfo> list(std::string& error);
    [[nodiscard]] static std::shared_ptr<ProcessMemory> attach(
        std::uint32_t pid,
        std::string& error);
    [[nodiscard]] static std::shared_ptr<ProcessMemory> launch(
        const std::vector<std::string>& arguments,
        std::string& error);
};
