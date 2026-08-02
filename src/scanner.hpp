#pragma once

#include "process.hpp"
#include "value.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct Candidate {
    std::uint64_t address{};
    std::array<std::byte, 8> previous{};
};

enum class NextMode {
    Exact,
    Changed,
    Unchanged,
    Increased,
    Decreased,
};

enum class ScanKind {
    Typed,
    Signature,
    Pointer,
};

struct PointerChain {
    std::string moduleName;
    std::uint64_t anchorAddress{};
    std::uint64_t moduleOffset{};
    std::vector<std::uint64_t> offsets;
};

class MemoryScanner {
public:
    explicit MemoryScanner(std::shared_ptr<ProcessMemory> process);

    void setType(ValueType type);
    void setThreadCount(std::size_t count);
    void setCandidates(std::vector<Candidate> candidates);
    void setPointerChains(std::vector<PointerChain> chains);
    bool firstSignature(std::string_view pattern, std::string& error);
    bool firstPointer(std::uint64_t targetAddress,
                      std::size_t maxDepth,
                      std::uint64_t maxOffset,
                      std::size_t maxResults,
                      std::string& error);
    [[nodiscard]] ValueType type() const;
    [[nodiscard]] std::size_t threadCount() const;
    [[nodiscard]] ScanKind scanKind() const;
    [[nodiscard]] const std::string& signaturePattern() const;
    [[nodiscard]] const std::vector<PointerChain>& pointerChains() const;
    [[nodiscard]] std::size_t candidateCount() const;
    [[nodiscard]] const std::vector<Candidate>& candidates() const;
    [[nodiscard]] std::shared_ptr<ProcessMemory> process() const;

    bool firstExact(const TypedValue& value, std::string& error);
    bool firstUnknown(std::string& error);
    bool next(NextMode mode,
              const TypedValue* exactValue,
              std::string& error);
    void reset();
    void clearPointerChains();
    [[nodiscard]] bool resolvePointerChain(const PointerChain& chain,
                                           std::uint64_t& resolvedAddress,
                                           std::string& error) const;

    [[nodiscard]] bool readValue(std::uint64_t address,
                                 ValueType type,
                                 TypedValue& output,
                                 std::string& error) const;
    bool writeValue(std::uint64_t address,
                    const TypedValue& value,
                    std::string& error);

private:
    bool firstScan(const TypedValue* exactValue, std::string& error);

    std::shared_ptr<ProcessMemory> process_;
    ValueType type_{ValueType::Int32};
    std::size_t threadCount_{1};
    ScanKind scanKind_{ScanKind::Typed};
    std::string signaturePattern_;
    std::vector<PointerChain> pointerChains_;
    std::vector<Candidate> candidates_;
};
