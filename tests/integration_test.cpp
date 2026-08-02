#include "process.hpp"
#include "scanner.hpp"
#include "value.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

bool waitForPointerValue(MemoryScanner& scanner,
                         const PointerChain& chain,
                         const TypedValue& expected,
                         std::uint64_t& resolvedAddress,
                         std::string& error) {
    std::string lastFailure;
    for (int attempt = 0; attempt < 50; ++attempt) {
        TypedValue current;
        error.clear();
        if (!scanner.resolvePointerChain(chain, resolvedAddress, error)) {
            lastFailure = "resolve gagal: " + error;
        } else if (!scanner.readValue(
                       resolvedAddress, expected.type, current, error)) {
            lastFailure = "read gagal: " + error;
        } else if (!valuesEqual(current, expected)) {
            lastFailure = "value hasil chain bukan value yang diharapkan";
        } else {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cerr << "Pointer replay gagal untuk module " << chain.moduleName
              << "+0x" << std::hex << chain.moduleOffset << std::dec
              << ", depth " << chain.offsets.size() << ": "
              << lastFailure << '\n';
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    std::string error;
    auto process = ProcessMemory::launch({argv[1]}, error);
    assert(process && error.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    MemoryScanner scanner(process);
    const auto magic = parseValue(ValueType::Int32, "42424242", error);
    assert(magic);
    assert(scanner.firstExact(*magic, error));
    assert(scanner.candidateCount() >= 1);

    const auto targetModuleName =
        std::filesystem::path(argv[1]).filename().string();
    std::optional<PointerChain> persistentChain;
    for (const auto& candidate : scanner.candidates()) {
        MemoryScanner pointerScanner(process);
        pointerScanner.setThreadCount(4);
        error.clear();
        if (!pointerScanner.firstPointer(
                candidate.address, 3, 0x100, 100, error)) {
            continue;
        }
        for (const auto& chain : pointerScanner.pointerChains()) {
            if (std::filesystem::path(chain.moduleName).filename().string() !=
                targetModuleName) {
                continue;
            }
            std::uint64_t resolved = 0;
            TypedValue current;
            error.clear();
            if (pointerScanner.resolvePointerChain(chain, resolved, error) &&
                pointerScanner.readValue(
                    resolved, ValueType::Int32, current, error) &&
                valuesEqual(current, *magic)) {
                persistentChain = chain;
                break;
            }
        }
        if (persistentChain) {
            break;
        }
    }
    assert(persistentChain);

    const auto replacement = parseValue(ValueType::Int32, "1337", error);
    assert(replacement);
    std::uint64_t firstResolved = 0;
    assert(scanner.resolvePointerChain(
        *persistentChain, firstResolved, error));
    assert(scanner.writeValue(firstResolved, *replacement, error));

    for (int attempt = 0; attempt < 30 && process->alive(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    assert(!process->alive());

    auto restarted = ProcessMemory::launch({argv[1]}, error);
    assert(restarted && error.empty());
    MemoryScanner replay(restarted);
    std::uint64_t replayedAddress = 0;
    assert(waitForPointerValue(
        replay, *persistentChain, *magic, replayedAddress, error));
    assert(replayedAddress != 0);
    assert(replay.writeValue(replayedAddress, *replacement, error));
    for (int attempt = 0; attempt < 30 && restarted->alive(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    assert(!restarted->alive());

    std::cout << "integration_test: pointer replay after restart OK\n";
    return 0;
}
