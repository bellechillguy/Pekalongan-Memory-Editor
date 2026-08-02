#include "process.hpp"
#include "scanner.hpp"
#include "value.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct FrozenValue {
    std::uint64_t address{};
    TypedValue value;
};

class FreezeManager {
public:
    explicit FreezeManager(std::shared_ptr<ProcessMemory> process)
        : process_(std::move(process)), worker_([this](std::stop_token stop) {
              while (!stop.stop_requested()) {
                  std::vector<FrozenValue> snapshot;
                  {
                      std::lock_guard lock(mutex_);
                      snapshot = values_;
                  }
                  for (const auto& frozen : snapshot) {
                      std::string ignored;
                      (void)process_->write(frozen.address,
                                            frozen.value.bytes.data(),
                                            frozen.value.size(),
                                            ignored);
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(50));
              }
          }) {}

    void set(std::uint64_t address, const TypedValue& value) {
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(values_.begin(), values_.end(),
                                        [address](const FrozenValue& frozen) {
                                            return frozen.address == address;
                                        });
        if (found == values_.end()) {
            values_.push_back({address, value});
        } else {
            found->value = value;
        }
    }

    bool remove(std::uint64_t address) {
        std::lock_guard lock(mutex_);
        const auto previous = values_.size();
        std::erase_if(values_, [address](const FrozenValue& frozen) {
            return frozen.address == address;
        });
        return values_.size() != previous;
    }

    void clear() {
        std::lock_guard lock(mutex_);
        values_.clear();
    }

    std::vector<FrozenValue> values() const {
        std::lock_guard lock(mutex_);
        return values_;
    }

private:
    std::shared_ptr<ProcessMemory> process_;
    mutable std::mutex mutex_;
    std::vector<FrozenValue> values_;
    std::jthread worker_;
};

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return text;
}

std::vector<std::string> tokenize(const std::string& line, std::string& error) {
    std::vector<std::string> tokens;
    std::string current;
    char quote = '\0';
    bool escaping = false;
    for (char character : line) {
        if (escaping) {
            current.push_back(character);
            escaping = false;
            continue;
        }
        if (character == '\\' && quote != '\'') {
            escaping = true;
            continue;
        }
        if (quote != '\0') {
            if (character == quote) {
                quote = '\0';
            } else {
                current.push_back(character);
            }
            continue;
        }
        if (character == '\'' || character == '\"') {
            quote = character;
        } else if (std::isspace(static_cast<unsigned char>(character))) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (escaping || quote != '\0') {
        error = "kutip atau escape pada command belum ditutup";
        return {};
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string joinArguments(const std::vector<std::string>& arguments,
                          std::size_t start = 0) {
    std::string result;
    for (std::size_t index = start; index < arguments.size(); ++index) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += arguments[index];
    }
    return result;
}

std::optional<std::uint64_t> parseUnsigned(std::string_view text,
                                           std::string& error) {
    try {
        std::size_t consumed = 0;
        const auto copy = std::string(text);
        const auto value = std::stoull(copy, &consumed, 0);
        if (consumed != copy.size()) {
            error = "angka tidak valid: " + copy;
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        error = "angka tidak valid: " + std::string(text);
        return std::nullopt;
    }
}

void printAddress(std::uint64_t address) {
    std::cout << "0x" << std::hex << std::uppercase << address
              << std::dec << std::nouppercase;
}

void printHelp() {
    std::cout
        << "\nCommand utama:\n"
        << "  ps [filter]                         daftar process\n"
        << "  attach <pid>                       pilih process yang sudah hidup\n"
        << "  launch <path> [arg...]             jalankan target sebagai child\n"
        << "  type <i8|u8|i16|u16|i32|u32|i64|u64|f32|f64>\n"
        << "                                    pilih tipe data scan\n"
        << "  threads <auto|n>                  atur jumlah worker scan\n"
        << "  benchmark scan <exact|unknown>... bandingkan single vs multi-thread\n"
        << "  scan exact <value>                 First Scan exact value\n"
        << "  scan unknown                       First Scan unknown initial value\n"
        << "  scan signature <hex|?? ...>        cari byte pattern dan wildcard\n"
        << "  scan pointer <address|#index> [depth] [max-offset] [limit]\n"
        << "  next exact <value>                 Next Scan exact value\n"
        << "  next <changed|unchanged|increased|decreased>\n"
        << "  results [limit]                    tampilkan candidate dan nilainya\n"
        << "  pointers [limit]                   daftar pointer chain\n"
        << "  follow <index>                     resolve pointer chain ke candidate\n"
        << "  read <address|#index> [type]       baca value\n"
        << "  write <address|#index> <value> [type]\n"
        << "  freeze <address|#index> <value> [type]\n"
        << "  unfreeze <address|#index|all>      hentikan freeze\n"
        << "  freezes                            daftar value yang sedang di-freeze\n"
        << "  regions                            ringkasan mapping memory\n"
        << "  save config <file>                simpan konfigurasi scan\n"
        << "  load config <file>                muat konfigurasi scan\n"
        << "  reset                              hapus hasil scan\n"
        << "  status                             tampilkan target dan scan aktif\n"
        << "  help | quit\n\n"
        << "Alamat dapat ditulis dalam desimal atau heksadesimal, misalnya "
           "0x7FF01234. #0 berarti hasil scan nomor 0.\n\n";
}

class Console {
public:
    int run() {
        std::cout << "Pekalongan Memory Editor 1.0\n"
                  << "Linux x64 / Windows x64, scan runtime tanpa hardcoded address\n"
                  << "Ketik help untuk melihat command.\n";
        std::string line;
        while (true) {
            std::cout << "\nmemedit> " << std::flush;
            if (!std::getline(std::cin, line)) {
                std::cout << '\n';
                return 0;
            }
            std::string parseError;
            auto arguments = tokenize(line, parseError);
            if (!parseError.empty()) {
                printError(parseError);
                continue;
            }
            if (arguments.empty()) {
                continue;
            }
            const auto command = lowercase(arguments.front());
            arguments.erase(arguments.begin());
            if (command == "quit" || command == "exit") {
                return 0;
            }
            execute(command, arguments);
        }
    }

    bool selectProcess(std::shared_ptr<ProcessMemory> process) {
        if (!process) {
            return false;
        }
        freeze_.reset();
        scanner_ = std::make_unique<MemoryScanner>(process);
        freeze_ = std::make_unique<FreezeManager>(process);
        std::cout << "Target dipilih: " << process->name()
                  << " (PID " << process->pid() << ")\n";
        return true;
    }

private:
    void execute(const std::string& command,
                 const std::vector<std::string>& arguments) {
        if (command == "help" || command == "?") {
            printHelp();
        } else if (command == "ps") {
            listProcesses(arguments);
        } else if (command == "attach") {
            attach(arguments);
        } else if (command == "launch") {
            launch(arguments);
        } else if (command == "type") {
            changeType(arguments);
        } else if (command == "threads") {
            changeThreads(arguments);
        } else if (command == "benchmark") {
            benchmark(arguments);
        } else if (command == "scan") {
            firstScan(arguments);
        } else if (command == "next") {
            nextScan(arguments);
        } else if (command == "results" || command == "list") {
            showResults(arguments);
        } else if (command == "pointers") {
            showPointers(arguments);
        } else if (command == "follow") {
            followPointer(arguments);
        } else if (command == "read") {
            readValue(arguments);
        } else if (command == "write") {
            writeValue(arguments);
        } else if (command == "freeze") {
            freezeValue(arguments);
        } else if (command == "unfreeze") {
            unfreezeValue(arguments);
        } else if (command == "freezes") {
            showFreezes();
        } else if (command == "regions") {
            showRegions();
        } else if (command == "save") {
            save(arguments);
        } else if (command == "load") {
            load(arguments);
        } else if (command == "reset") {
            if (requireTarget()) {
                scanner_->reset();
                std::cout << "Hasil scan dihapus.\n";
            }
        } else if (command == "status") {
            showStatus();
        } else {
            printError("command tidak dikenal. Ketik help");
        }
    }

    void listProcesses(const std::vector<std::string>& arguments) {
        const auto filter = arguments.empty() ? std::string{}
                                              : lowercase(arguments.front());
        std::string error;
        const auto processes = ProcessMemory::list(error);
        if (!error.empty()) {
            printError(error);
            return;
        }
        std::cout << std::left << std::setw(9) << "PID" << std::setw(28)
                  << "NAME" << "COMMAND\n";
        std::size_t shown = 0;
        for (const auto& process : processes) {
            const auto searchable = lowercase(process.name + " " + process.command);
            if (!filter.empty() && searchable.find(filter) == std::string::npos) {
                continue;
            }
            std::cout << std::left << std::setw(9) << process.pid
                      << std::setw(28) << process.name
                      << process.command.substr(0, 90) << '\n';
            ++shown;
        }
        std::cout << shown << " process ditampilkan.\n";
    }

    void attach(const std::vector<std::string>& arguments) {
        if (arguments.size() != 1) {
            printError("pemakaian: attach <pid>");
            return;
        }
        std::string error;
        const auto parsed = parseUnsigned(arguments[0], error);
        if (!parsed || *parsed > UINT32_MAX) {
            printError(error.empty() ? "PID berada di luar rentang" : error);
            return;
        }
        auto process = ProcessMemory::attach(static_cast<std::uint32_t>(*parsed), error);
        if (!process) {
            printError(error);
            return;
        }
        selectProcess(std::move(process));
    }

    void launch(const std::vector<std::string>& arguments) {
        if (arguments.empty()) {
            printError("pemakaian: launch <path> [arg...]");
            return;
        }
        std::string error;
        auto process = ProcessMemory::launch(arguments, error);
        if (!process) {
            printError(error);
            return;
        }
        selectProcess(std::move(process));
    }

    void changeType(const std::vector<std::string>& arguments) {
        if (!requireTarget()) {
            return;
        }
        if (arguments.size() != 1) {
            printError("pemakaian: type <i8|u8|i16|u16|i32|u32|i64|u64|f32|f64>");
            return;
        }
        const auto type = parseValueType(lowercase(arguments.front()));
        if (!type) {
            printError("tipe tidak dikenal. Pilih i8, u8, i16, u16, i32, u32, i64, u64, f32, atau f64");
            return;
        }
        scanner_->setType(*type);
        std::cout << "Tipe scan: " << valueTypeName(*type)
                  << ". Hasil scan sebelumnya dihapus jika tipe berubah.\n";
    }

    void changeThreads(const std::vector<std::string>& arguments) {
        if (!requireTarget()) {
            return;
        }
        if (arguments.size() > 1) {
            printError("pemakaian: threads <auto|n>");
            return;
        }
        if (arguments.empty() || lowercase(arguments[0]) == "auto") {
            const auto hardware = std::max<std::size_t>(1, std::thread::hardware_concurrency());
            scanner_->setThreadCount(hardware);
            std::cout << "Thread scan disetel ke auto (" << hardware << ").\n";
            return;
        }
        std::string error;
        const auto parsed = parseUnsigned(arguments[0], error);
        if (!parsed || *parsed == 0) {
            printError(error.empty() ? "jumlah thread harus lebih besar dari nol" : error);
            return;
        }
        scanner_->setThreadCount(static_cast<std::size_t>(*parsed));
        std::cout << "Thread scan disetel ke " << scanner_->threadCount() << ".\n";
    }

    void benchmark(const std::vector<std::string>& arguments) {
        if (!requireTarget()) {
            return;
        }
        if (arguments.empty() || lowercase(arguments[0]) != "scan") {
            printError("pemakaian: benchmark scan <exact|unknown> [value]");
            return;
        }

        bool useExact = false;
        std::optional<TypedValue> value;
        std::string error;
        if (arguments.size() == 3 && lowercase(arguments[1]) == "exact") {
            useExact = true;
            value = parseValue(scanner_->type(), arguments[2], error);
            if (!value) {
                printError(error);
                return;
            }
        } else if (arguments.size() == 2 && lowercase(arguments[1]) == "unknown") {
            useExact = false;
        } else {
            printError("pemakaian: benchmark scan <exact|unknown> [value]");
            return;
        }

        const auto process = scanner_->process();
        const auto hardware = std::max<std::size_t>(1, std::thread::hardware_concurrency());
        const auto originalThreads = scanner_->threadCount();
        const auto runScan = [&](std::size_t threads, std::size_t& candidatesOut) {
            MemoryScanner temp(process);
            temp.setType(scanner_->type());
            temp.setThreadCount(threads);
            const auto started = std::chrono::steady_clock::now();
            bool ok = false;
            std::string scanError;
            if (useExact) {
                ok = temp.firstExact(*value, scanError);
            } else {
                ok = temp.firstUnknown(scanError);
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            if (!ok) {
                throw std::runtime_error(scanError);
            }
            candidatesOut = temp.candidateCount();
            return elapsed.count();
        };

        try {
            std::size_t singleCandidates = 0;
            std::size_t multiCandidates = 0;
            const auto singleMs = runScan(1, singleCandidates);
            const auto multiMs = runScan(hardware, multiCandidates);

            std::cout << "Benchmark scan (" << (useExact ? "exact" : "unknown") << ")\n";
            std::cout << std::left << std::setw(14) << "Mode"
                      << std::setw(12) << "Threads"
                      << std::setw(14) << "Candidates"
                      << "Time (ms)\n";
            std::cout << std::left << std::setw(14) << "single-thread"
                      << std::setw(12) << 1
                      << std::setw(14) << singleCandidates
                      << singleMs << '\n';
            std::cout << std::left << std::setw(14) << "multi-thread"
                      << std::setw(12) << hardware
                      << std::setw(14) << multiCandidates
                      << multiMs << '\n';
            std::cout << "Perbandingan ini menjalankan scan yang sama dua kali dengan thread berbeda.\n";
        } catch (const std::exception& exc) {
            printError(exc.what());
        }

        scanner_->setThreadCount(originalThreads);
    }

    void firstScan(const std::vector<std::string>& arguments) {
        if (!requireTarget()) {
            return;
        }
        if (arguments.empty()) {
            printError("pemakaian: scan exact <value> | scan unknown | scan signature <pattern> | scan pointer <address|#index> [depth] [max-offset] [limit]");
            return;
        }
        const auto mode = lowercase(arguments[0]);
        std::string error;
        bool ok = false;
        const auto started = std::chrono::steady_clock::now();
        std::cout << "Memindai virtual memory target...\n";
        if (mode == "unknown" && arguments.size() == 1) {
            ok = scanner_->firstUnknown(error);
        } else if (mode == "exact" && arguments.size() == 2) {
            auto value = parseValue(scanner_->type(), arguments[1], error);
            if (value) {
                ok = scanner_->firstExact(*value, error);
            }
        } else if ((mode == "signature" || mode == "pattern" || mode == "bytes") &&
                   arguments.size() >= 2) {
            ok = scanner_->firstSignature(joinArguments(arguments, 1), error);
        } else if (mode == "pointer" && arguments.size() >= 2) {
            const auto address = resolveAddress(arguments[1]);
            if (!address) {
                return;
            }
            std::size_t depth = 3;
            std::uint64_t maxOffset = 0x400;
            std::size_t limit = 100;
            if (arguments.size() >= 3) {
                std::string depthError;
                const auto parsedDepth = parseUnsigned(arguments[2], depthError);
                if (!parsedDepth || *parsedDepth == 0) {
                    printError(depthError.empty() ? "depth pointer harus lebih besar dari nol"
                                                 : depthError);
                    return;
                }
                depth = static_cast<std::size_t>(*parsedDepth);
            }
            if (arguments.size() >= 4) {
                std::string offsetError;
                const auto parsedOffset =
                    parseUnsigned(arguments[3], offsetError);
                if (!parsedOffset) {
                    printError(offsetError);
                    return;
                }
                maxOffset = *parsedOffset;
            }
            if (arguments.size() >= 5) {
                std::string limitError;
                const auto parsedLimit = parseUnsigned(arguments[4], limitError);
                if (!parsedLimit || *parsedLimit == 0) {
                    printError(limitError.empty() ? "limit pointer harus lebih besar dari nol"
                                                  : limitError);
                    return;
                }
                limit = static_cast<std::size_t>(*parsedLimit);
            }
            if (arguments.size() > 5) {
                printError("terlalu banyak argumen pointer scan");
                return;
            }
            ok = scanner_->firstPointer(
                *address, depth, maxOffset, limit, error);
        } else {
            printError("pemakaian: scan exact <value> | scan unknown | scan signature <pattern> | scan pointer <address|#index> [depth] [max-offset] [limit]");
            return;
        }
        if (!ok) {
            printError(error);
            return;
        }
        printScanSummary(started);
    }

    void nextScan(const std::vector<std::string>& arguments) {
        if (!requireTarget()) {
            return;
        }
        if (arguments.empty()) {
            printError("pemakaian: next exact <value> | next <changed|unchanged|increased|decreased>");
            return;
        }

        const auto modeText = lowercase(arguments[0]);
        NextMode mode{};
        std::optional<TypedValue> exact;
        std::string error;
        if (modeText == "exact" && arguments.size() == 2) {
            mode = NextMode::Exact;
            exact = parseValue(scanner_->type(), arguments[1], error);
            if (!exact) {
                printError(error);
                return;
            }
        } else if (arguments.size() == 1 && modeText == "changed") {
            mode = NextMode::Changed;
        } else if (arguments.size() == 1 && modeText == "unchanged") {
            mode = NextMode::Unchanged;
        } else if (arguments.size() == 1 && modeText == "increased") {
            mode = NextMode::Increased;
        } else if (arguments.size() == 1 && modeText == "decreased") {
            mode = NextMode::Decreased;
        } else {
            printError("mode Next Scan tidak valid");
            return;
        }

        const auto started = std::chrono::steady_clock::now();
        if (!scanner_->next(mode, exact ? &*exact : nullptr, error)) {
            printError(error);
            return;
        }
        printScanSummary(started);
    }

    void showResults(const std::vector<std::string>& arguments) {
        if (!requireTarget()) {
            return;
        }
        std::size_t limit = 25;
        if (arguments.size() > 1) {
            printError("pemakaian: results [limit]");
            return;
        }
        if (!arguments.empty()) {
            std::string error;
            const auto parsed = parseUnsigned(arguments[0], error);
            if (!parsed) {
                printError(error);
                return;
            }
            limit = static_cast<std::size_t>(*parsed);
        }

        const auto& candidates = scanner_->candidates();
        const auto shown = std::min(limit, candidates.size());
        for (std::size_t index = 0; index < shown; ++index) {
            TypedValue current;
            std::string error;
            std::cout << '#' << index << "  ";
            printAddress(candidates[index].address);
            if (scanner_->readValue(candidates[index].address,
                                    scanner_->type(),
                                    current,
                                    error)) {
                std::cout << "  " << formatValue(current);
            } else {
                std::cout << "  <tidak dapat dibaca>";
            }
            std::cout << '\n';
        }
        std::cout << shown << " dari " << candidates.size()
                  << " candidate ditampilkan.\n";
    }

    void showPointers(const std::vector<std::string>& arguments) const {
        if (!requireTarget()) {
            return;
        }
        std::size_t limit = 25;
        if (arguments.size() > 1) {
            printError("pemakaian: pointers [limit]");
            return;
        }
        if (!arguments.empty()) {
            std::string error;
            const auto parsed = parseUnsigned(arguments[0], error);
            if (!parsed) {
                printError(error);
                return;
            }
            limit = static_cast<std::size_t>(*parsed);
        }

        const auto& chains = scanner_->pointerChains();
        const auto shown = std::min(limit, chains.size());
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& chain = chains[index];
            std::uint64_t resolved = 0;
            std::string error;
            std::cout << '#' << index << "  "
                      << std::filesystem::path(chain.moduleName)
                             .filename()
                             .string()
                      << "+0x" << std::hex << std::uppercase
                      << chain.moduleOffset << std::dec << std::nouppercase
                      << " offsets=[";
            for (std::size_t offsetIndex = 0;
                 offsetIndex < chain.offsets.size();
                 ++offsetIndex) {
                if (offsetIndex != 0) {
                    std::cout << ", ";
                }
                std::cout << "0x" << std::hex << std::uppercase
                          << chain.offsets[offsetIndex] << std::dec
                          << std::nouppercase;
            }
            std::cout << "] -> ";
            if (scanner_->resolvePointerChain(chain, resolved, error)) {
                printAddress(resolved);
            } else {
                std::cout << '<' << error << '>';
            }
            std::cout << '\n';
        }
        std::cout << shown << " dari " << chains.size() << " pointer chain ditampilkan.\n";
    }

    void followPointer(const std::vector<std::string>& arguments) {
        if (!requireTarget()) {
            return;
        }
        if (arguments.size() != 1) {
            printError("pemakaian: follow <index>");
            return;
        }
        std::string error;
        const auto parsed = parseUnsigned(arguments[0], error);
        if (!parsed || *parsed >= scanner_->pointerChains().size()) {
            printError(parsed ? "index pointer chain di luar rentang" : error);
            return;
        }

        std::uint64_t resolved = 0;
        if (!scanner_->resolvePointerChain(scanner_->pointerChains()[static_cast<std::size_t>(*parsed)],
                                           resolved,
                                           error)) {
            printError(error);
            return;
        }

        Candidate candidate;
        candidate.address = resolved;
        TypedValue current;
        if (scanner_->readValue(resolved, scanner_->type(), current, error)) {
            candidate.previous = current.bytes;
        }
        scanner_->setCandidates({candidate});
        std::cout << "Pointer chain #" << *parsed << " resolved ke ";
        printAddress(resolved);
        std::cout << " dan dipasang sebagai candidate aktif.\n";
    }

    void readValue(const std::vector<std::string>& arguments) {
        if (!requireTarget()) {
            return;
        }
        if (arguments.empty() || arguments.size() > 2) {
            printError("pemakaian: read <address|#index> [type]");
            return;
        }
        const auto address = resolveAddress(arguments[0]);
        const auto type = resolveType(arguments, 1);
        if (!address || !type) {
            return;
        }
        TypedValue value;
        std::string error;
        if (!scanner_->readValue(*address, *type, value, error)) {
            printError(error);
            return;
        }
        printAddress(*address);
        std::cout << " [" << valueTypeName(*type) << "] = "
                  << formatValue(value) << '\n';
    }

    void writeValue(const std::vector<std::string>& arguments) {
        if (!requireTarget()) {
            return;
        }
        if (arguments.size() < 2 || arguments.size() > 3) {
            printError("pemakaian: write <address|#index> <value> [type]");
            return;
        }
        const auto address = resolveAddress(arguments[0]);
        const auto type = resolveType(arguments, 2);
        if (!address || !type) {
            return;
        }
        std::string error;
        const auto value = parseValue(*type, arguments[1], error);
        if (!value) {
            printError(error);
            return;
        }
        if (!scanner_->writeValue(*address, *value, error)) {
            printError(error);
            return;
        }
        std::cout << "Berhasil menulis " << formatValue(*value) << " ke ";
        printAddress(*address);
        std::cout << ".\n";
    }

    void freezeValue(const std::vector<std::string>& arguments) {
        if (!requireTarget()) {
            return;
        }
        if (arguments.size() < 2 || arguments.size() > 3) {
            printError("pemakaian: freeze <address|#index> <value> [type]");
            return;
        }
        const auto address = resolveAddress(arguments[0]);
        const auto type = resolveType(arguments, 2);
        if (!address || !type) {
            return;
        }
        std::string error;
        const auto value = parseValue(*type, arguments[1], error);
        if (!value) {
            printError(error);
            return;
        }
        if (!scanner_->writeValue(*address, *value, error)) {
            printError(error);
            return;
        }
        freeze_->set(*address, *value);
        std::cout << "Freeze aktif di ";
        printAddress(*address);
        std::cout << " dengan nilai " << formatValue(*value) << ".\n";
    }

    void unfreezeValue(const std::vector<std::string>& arguments) {
        if (!requireTarget()) {
            return;
        }
        if (arguments.size() != 1) {
            printError("pemakaian: unfreeze <address|#index|all>");
            return;
        }
        if (lowercase(arguments[0]) == "all") {
            freeze_->clear();
            std::cout << "Semua freeze dihentikan.\n";
            return;
        }
        const auto address = resolveAddress(arguments[0]);
        if (!address) {
            return;
        }
        if (freeze_->remove(*address)) {
            std::cout << "Freeze dihentikan.\n";
        } else {
            printError("alamat tersebut tidak sedang di-freeze");
        }
    }

    void showFreezes() const {
        if (!requireTarget()) {
            return;
        }
        const auto values = freeze_->values();
        for (const auto& frozen : values) {
            printAddress(frozen.address);
            std::cout << " [" << valueTypeName(frozen.value.type) << "] = "
                      << formatValue(frozen.value) << '\n';
        }
        std::cout << values.size() << " freeze aktif.\n";
    }

    void showRegions() const {
        if (!requireTarget()) {
            return;
        }
        std::string error;
        const auto regions = scanner_->process()->regions(error);
        if (!error.empty()) {
            printError(error);
            return;
        }
        std::size_t scanRegions = 0;
        std::uint64_t scanBytes = 0;
        for (const auto& region : regions) {
            if (region.readable && region.writable && region.privateMapping) {
                ++scanRegions;
                scanBytes += region.size;
            }
        }
        std::cout << regions.size() << " mapping ditemukan. " << scanRegions
                  << " writable private mapping akan dipindai ("
                  << (scanBytes / (1024 * 1024)) << " MiB).\n";
    }

    void save(const std::vector<std::string>& arguments) const {
        if (!requireTarget()) {
            return;
        }
        if (arguments.size() != 2 || lowercase(arguments[0]) != "config") {
            printError("pemakaian: save config <file>");
            return;
        }
        const std::filesystem::path path = arguments[1];
        if (path.has_parent_path()) {
            std::error_code ignored;
            std::filesystem::create_directories(path.parent_path(), ignored);
        }

        std::ofstream output(path);
        if (!output) {
            printError("tidak dapat menulis file konfigurasi");
            return;
        }

        std::string kind = "typed";
        if (scanner_->scanKind() == ScanKind::Signature) {
            kind = "signature";
        } else if (scanner_->scanKind() == ScanKind::Pointer) {
            kind = "pointer";
        }
        output << "kind=" << kind << '\n';
        output << "type=" << valueTypeName(scanner_->type()) << '\n';
        output << "threads=" << scanner_->threadCount() << '\n';
        if (scanner_->scanKind() == ScanKind::Signature) {
            output << "pattern=" << scanner_->signaturePattern() << '\n';
        }
        if (scanner_->scanKind() == ScanKind::Pointer) {
            for (const auto& chain : scanner_->pointerChains()) {
                output << "pointer_chain=" << chain.moduleName << '|'
                       << std::hex << std::uppercase << chain.moduleOffset
                       << '|';
                for (std::size_t index = 0;
                     index < chain.offsets.size();
                     ++index) {
                    if (index != 0) {
                        output << ',';
                    }
                    output << chain.offsets[index];
                }
                output << std::dec << std::nouppercase << '\n';
            }
        }
        output << "candidate_count=" << scanner_->candidateCount() << '\n';
        for (const auto& candidate : scanner_->candidates()) {
            output << "candidate=0x" << std::hex << std::uppercase << candidate.address
                   << std::dec << std::nouppercase << '\n';
        }
        std::cout << "Konfigurasi disimpan ke " << path.string() << ".\n";
    }

    void load(const std::vector<std::string>& arguments) {
        if (!requireTarget()) {
            return;
        }
        if (arguments.size() != 2 || lowercase(arguments[0]) != "config") {
            printError("pemakaian: load config <file>");
            return;
        }

        const std::filesystem::path path = arguments[1];
        std::ifstream input(path);
        if (!input) {
            printError("file konfigurasi tidak ditemukan");
            return;
        }

        std::string kind = "typed";
        std::string typeText = valueTypeName(scanner_->type());
        std::string pattern;
        std::size_t threads = scanner_->threadCount();
        std::vector<PointerChain> pointerChains;
        std::vector<std::uint64_t> addresses;

        std::string line;
        while (std::getline(input, line)) {
            const auto normalized = trim(line);
            if (normalized.empty() || normalized.front() == '#') {
                continue;
            }
            const auto equals = normalized.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            const auto key = lowercase(trim(normalized.substr(0, equals)));
            const auto value = trim(normalized.substr(equals + 1));
            if (key == "kind") {
                kind = lowercase(value);
            } else if (key == "type") {
                typeText = lowercase(value);
            } else if (key == "pattern") {
                pattern = value;
            } else if (key == "threads") {
                std::string error;
                const auto parsed = parseUnsigned(value, error);
                if (parsed && *parsed > 0) {
                    threads = static_cast<std::size_t>(*parsed);
                }
            } else if (key == "candidate") {
                std::string error;
                const auto parsed = parseUnsigned(value, error);
                if (parsed) {
                    addresses.push_back(*parsed);
                }
            } else if (key == "pointer_chain") {
                std::vector<std::string> parts;
                std::string part;
                std::istringstream parser(value);
                while (std::getline(parser, part, '|')) {
                    parts.push_back(part);
                }
                if (parts.size() == 3) {
                    std::string error;
                    const auto moduleOffset =
                        parseUnsigned("0x" + parts[1], error);
                    std::vector<std::uint64_t> offsets;
                    std::istringstream offsetParser(parts[2]);
                    std::string offsetText;
                    while (std::getline(offsetParser, offsetText, ',')) {
                        const auto parsedOffset =
                            parseUnsigned("0x" + offsetText, error);
                        if (!parsedOffset) {
                            offsets.clear();
                            break;
                        }
                        offsets.push_back(*parsedOffset);
                    }
                    if (moduleOffset && !offsets.empty()) {
                        pointerChains.push_back(PointerChain{
                            parts[0],
                            0,
                            *moduleOffset,
                            std::move(offsets),
                        });
                    }
                }
            }
        }

        scanner_->setThreadCount(threads);

        const auto type = parseValueType(typeText);
        if (!type) {
            printError("tipe pada file konfigurasi tidak valid");
            return;
        }
        scanner_->setType(*type);

        if (kind == "signature" && !pattern.empty()) {
            std::string error;
            if (!scanner_->firstSignature(pattern, error)) {
                printError(error);
                return;
            }
            std::cout << "Konfigurasi signature dimuat ulang dari " << path.string()
                      << ". Candidate baru: " << scanner_->candidateCount() << "\n";
            return;
        }

        if (kind == "pointer" && !pointerChains.empty()) {
            std::vector<Candidate> resolved;
            std::string error;
            for (const auto& chain : pointerChains) {
                std::uint64_t address = 0;
                if (!scanner_->resolvePointerChain(chain, address, error)) {
                    continue;
                }
                Candidate candidate;
                candidate.address = address;
                TypedValue current;
                if (scanner_->readValue(address, scanner_->type(), current, error)) {
                    candidate.previous = current.bytes;
                }
                resolved.push_back(candidate);
            }
            if (resolved.empty()) {
                printError("pointer chain pada config tidak dapat direstore");
                return;
            }
            scanner_->setPointerChains(std::move(pointerChains));
            scanner_->setCandidates(std::move(resolved));
            std::cout << "Konfigurasi pointer dimuat ulang dari " << path.string()
                      << " (" << scanner_->candidateCount() << " candidate aktif).\n";
            return;
        }

        std::vector<Candidate> candidates;
        candidates.reserve(addresses.size());
        for (const auto address : addresses) {
            Candidate candidate;
            candidate.address = address;
            TypedValue current;
            std::string error;
            if (scanner_->readValue(address, *type, current, error)) {
                candidate.previous = current.bytes;
            }
            candidates.push_back(candidate);
        }
        scanner_->setCandidates(std::move(candidates));
        std::cout << "Snapshot candidate dimuat dari " << path.string()
                  << " (" << scanner_->candidateCount() << " address).\n";
    }

    void showStatus() const {
        if (!scanner_) {
            std::cout << "Belum ada target. Gunakan ps lalu attach, atau launch.\n";
            return;
        }
        const auto process = scanner_->process();
        std::cout << "Target: " << process->name() << " (PID " << process->pid()
                  << "), status " << (process->alive() ? "aktif" : "berhenti")
                  << ", backend " << process->backendName()
                  << "\nTipe: " << valueTypeName(scanner_->type())
                  << ", scan: "
                  << (scanner_->scanKind() == ScanKind::Signature ? "signature"
                      : scanner_->scanKind() == ScanKind::Pointer ? "pointer"
                                                                 : "typed")
                  << ", candidate: " << scanner_->candidateCount()
                  << ", pointer chain: " << scanner_->pointerChains().size()
                  << ", thread: " << scanner_->threadCount()
                  << ", freeze: " << freeze_->values().size() << '\n';
        if (scanner_->scanKind() == ScanKind::Signature && !scanner_->signaturePattern().empty()) {
            std::cout << "Pattern: " << scanner_->signaturePattern() << '\n';
        }
    }

    std::optional<std::uint64_t> resolveAddress(const std::string& token) const {
        if (!token.empty() && token.front() == '#') {
            std::string error;
            const auto index = parseUnsigned(std::string_view(token).substr(1), error);
            if (!index || *index >= scanner_->candidates().size()) {
                printError(index ? "index candidate di luar rentang" : error);
                return std::nullopt;
            }
            return scanner_->candidates()[static_cast<std::size_t>(*index)].address;
        }
        std::string error;
        const auto address = parseUnsigned(token, error);
        if (!address) {
            printError(error);
        }
        return address;
    }

    std::optional<ValueType> resolveType(
        const std::vector<std::string>& arguments,
        std::size_t position) const {
        if (arguments.size() <= position) {
            return scanner_->type();
        }
        const auto type = parseValueType(lowercase(arguments[position]));
        if (!type) {
            printError("tipe tidak dikenal. Pilih i8, u8, i16, u16, i32, u32, i64, u64, f32, atau f64");
        }
        return type;
    }

    bool requireTarget() const {
        if (!scanner_) {
            printError("pilih target lebih dulu dengan attach atau launch");
            return false;
        }
        if (!scanner_->process()->alive()) {
            printError("process target sudah berhenti");
            return false;
        }
        return true;
    }

    void printScanSummary(std::chrono::steady_clock::time_point started) const {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        std::cout << "Scan selesai: " << scanner_->candidateCount()
                  << " candidate dalam " << milliseconds.count() << " ms.\n";
    }

    static void printError(const std::string& message) {
        std::cerr << "Error: " << message << '\n';
    }

    std::unique_ptr<MemoryScanner> scanner_;
    std::unique_ptr<FreezeManager> freeze_;
};

}  // namespace

int main(int argc, char** argv) {
    Console console;
    if (argc > 1) {
        std::vector<std::string> arguments(argv + 1, argv + argc);
        std::string error;
        std::shared_ptr<ProcessMemory> process;
        if (arguments.size() == 2 && arguments[0] == "--pid") {
            const auto pid = parseUnsigned(arguments[1], error);
            if (pid && *pid <= UINT32_MAX) {
                process = ProcessMemory::attach(static_cast<std::uint32_t>(*pid), error);
            }
        } else if (arguments[0] == "--launch" && arguments.size() >= 2) {
            arguments.erase(arguments.begin());
            process = ProcessMemory::launch(arguments, error);
        } else {
            std::cerr << "Pemakaian: memory_editor [--pid PID | --launch PATH [ARG...]]\n";
            return 2;
        }
        if (!process || !console.selectProcess(std::move(process))) {
            std::cerr << "Error: " << error << '\n';
            return 1;
        }
    }
    return console.run();
}
