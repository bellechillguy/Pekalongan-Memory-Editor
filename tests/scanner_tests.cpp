#include "scanner.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

class MockProcess final : public ProcessMemory {
public:
    static constexpr std::uint64_t kBase = 0x10000;

    MockProcess() : memory_(512) {}

    std::uint32_t pid() const override { return 123; }
    std::string name() const override { return "mock"; }
    std::string backendName() const override { return "mock"; }
    bool alive() const override { return true; }

    std::vector<MemoryRegion> regions(std::string&) const override {
        return {{base_, memory_.size(), true, true, true, "mock-module"}};
    }

    std::vector<ModuleInfo> modules(std::string&) const override {
        return {{"mock-module", "mock-module", base_, memory_.size()}};
    }

    bool read(std::uint64_t address,
              void* destination,
              std::size_t size,
              std::string& error) const override {
        if (address < base_ || address - base_ + size > memory_.size()) {
            error = "out of range";
            return false;
        }
        std::memcpy(destination, memory_.data() + (address - base_), size);
        return true;
    }

    bool write(std::uint64_t address,
               const void* source,
               std::size_t size,
               std::string& error) override {
        if (address < base_ || address - base_ + size > memory_.size()) {
            error = "out of range";
            return false;
        }
        std::memcpy(memory_.data() + (address - base_), source, size);
        return true;
    }

    void put(std::size_t offset, std::int32_t value) {
        std::memcpy(memory_.data() + offset, &value, sizeof(value));
    }

    void putBytes(std::size_t offset, std::initializer_list<std::byte> bytes) {
        std::memcpy(memory_.data() + offset, bytes.begin(), bytes.size());
    }

    void putPointer(std::size_t offset, std::uint64_t value) {
        std::memcpy(memory_.data() + offset, &value, sizeof(value));
    }

    void relocate(std::uint64_t base) { base_ = base; }

    std::uint64_t base() const { return base_; }

private:
    std::uint64_t base_{kBase};
    std::vector<std::byte> memory_;
};

TypedValue i32(std::int32_t number) {
    std::string error;
    const auto value = parseValue(ValueType::Int32,
                                  std::to_string(number),
                                  error);
    assert(value && error.empty());
    return *value;
}

template <typename T>
TypedValue typed(ValueType type, T number) {
    std::string error;
    const auto value = parseValue(type, std::to_string(number), error);
    assert(value && error.empty());
    return *value;
}

}  // namespace

int main() {
    auto process = std::make_shared<MockProcess>();
    process->put(40, 100);
    process->put(80, 100);
    process->put(120, 100);
    process->putBytes(300,
                      {std::byte{0xAA}, std::byte{0xBB}, std::byte{0x11},
                       std::byte{0xDD}});

    MemoryScanner scanner(process);
    std::string error;

    scanner.setThreadCount(1);
    assert(scanner.firstExact(i32(100), error));
    assert(scanner.candidateCount() == 3);

    assert(formatValue(typed<std::int8_t>(ValueType::Int8, -7)) == "-7");
    assert(formatValue(typed<std::uint8_t>(ValueType::UInt8, 250)) == "250");
    assert(formatValue(typed<std::int16_t>(ValueType::Int16, -1234)) == "-1234");
    assert(formatValue(typed<std::uint16_t>(ValueType::UInt16, 65530)) == "65530");
    assert(formatValue(typed<std::int64_t>(ValueType::Int64, -9000000000LL)) == "-9000000000");
    assert(formatValue(typed<std::uint32_t>(ValueType::UInt32, 4000000000ULL)) == "4000000000");
    assert(formatValue(typed<std::uint64_t>(ValueType::UInt64, 9000000000ULL)) == "9000000000");
    assert(formatValue(typed<float>(ValueType::Float, 12.5F)) == "12.5");
    assert(formatValue(typed<double>(ValueType::Double, -25.25)) == "-25.25");

    scanner.reset();
    scanner.setThreadCount(4);
    assert(scanner.firstExact(i32(100), error));
    assert(scanner.candidateCount() == 3);

    process->put(40, 125);
    process->put(80, 90);
    assert(scanner.next(NextMode::Increased, nullptr, error));
    assert(scanner.candidateCount() == 1);
    assert(scanner.candidates()[0].address == MockProcess::kBase + 40);

    process->put(40, 100);
    process->put(80, 100);
    process->put(120, 100);
    scanner.reset();
    assert(scanner.firstExact(i32(100), error));
    process->put(80, 75);
    assert(scanner.next(NextMode::Decreased, nullptr, error));
    assert(scanner.candidateCount() == 1);
    assert(scanner.candidates()[0].address == MockProcess::kBase + 80);

    scanner.reset();
    assert(scanner.firstSignature("AA BB ?? DD", error));
    assert(scanner.candidateCount() == 1);
    assert(scanner.candidates()[0].address == MockProcess::kBase + 300);

    process->put(200, 4242);
    process->putPointer(16, process->base() + 192);
    scanner.setType(ValueType::Int32);
    assert(scanner.firstPointer(process->base() + 200, 2, 0x20, 20, error));
    assert(!scanner.pointerChains().empty());
    const auto persistentChain = scanner.pointerChains().front();
    std::uint64_t resolved = 0;
    assert(scanner.resolvePointerChain(persistentChain, resolved, error));
    assert(resolved == process->base() + 200);

    process->relocate(0x20000);
    process->putPointer(16, process->base() + 192);
    assert(scanner.resolvePointerChain(persistentChain, resolved, error));
    assert(resolved == process->base() + 200);

    assert(scanner.writeValue(process->base() + 40, i32(777), error));
    TypedValue readBack;
    assert(scanner.readValue(process->base() + 40,
                             ValueType::Int32,
                             readBack,
                             error));
    assert(formatValue(readBack) == "777");

    scanner.reset();
    assert(scanner.firstUnknown(error));
    const auto unknownCount = scanner.candidateCount();
    process->put(200, 55);
    assert(scanner.next(NextMode::Changed, nullptr, error));
    assert(scanner.candidateCount() >= 1);
    assert(scanner.candidateCount() < unknownCount);

    std::cout << "scanner_tests: OK\n";
    return 0;
}
