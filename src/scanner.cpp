#include "scanner.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kChunkSize = 1024 * 1024;
constexpr std::uint64_t kComparisonPage = 64 * 1024;
constexpr std::size_t kMaxUnknownCandidates = 30'000'000;
constexpr std::size_t kPointerValueSize = 8;

struct SignatureByte {
    bool wildcard{};
    std::byte value{};
};

TypedValue candidateValue(ValueType type,
                          const std::array<std::byte, 8>& bytes) {
    TypedValue value;
    value.type = type;
    value.bytes = bytes;
    return value;
}

TypedValue bytesValue(ValueType type, const std::byte* bytes) {
    TypedValue value;
    value.type = type;
    std::memcpy(value.bytes.data(), bytes, value.size());
    return value;
}

std::size_t effectiveThreadCount(std::size_t requested, std::size_t workItems) {
    if (workItems <= 1) {
        return 1;
    }
    const auto hardware = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    const auto normalized = requested == 0 ? hardware : requested;
    return std::clamp<std::size_t>(normalized, 1, workItems);
}

struct ScanGroup {
    std::size_t begin{};
    std::size_t end{};
};

std::vector<ScanGroup> pageGroups(const std::vector<Candidate>& candidates) {
    std::vector<ScanGroup> groups;
    std::size_t begin = 0;
    while (begin < candidates.size()) {
        const auto pageEnd = ((candidates[begin].address / kComparisonPage) + 1) *
                             kComparisonPage;
        std::size_t end = begin + 1;
        while (end < candidates.size() && candidates[end].address < pageEnd) {
            ++end;
        }
        groups.push_back({begin, end});
        begin = end;
    }
    return groups;
}

std::uint64_t readUint64(const std::byte* bytes) {
    std::uint64_t value{};
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool parseSignatureByte(std::string_view text,
                        SignatureByte& output,
                        std::string& error) {
    if (text == "?" || text == "??") {
        output.wildcard = true;
        output.value = std::byte{};
        return true;
    }
    if (text.size() == 2 && std::isxdigit(static_cast<unsigned char>(text[0])) &&
        std::isxdigit(static_cast<unsigned char>(text[1]))) {
        const auto parsed = static_cast<unsigned int>(
            std::stoul(std::string(text), nullptr, 16));
        output.wildcard = false;
        output.value = static_cast<std::byte>(parsed);
        return true;
    }
    if (text.size() > 2 && text.size() % 2 == 0) {
        for (char character : text) {
            if (!std::isxdigit(static_cast<unsigned char>(character))) {
                error = "signature pattern hanya boleh berisi byte hex atau wildcard ?";
                return false;
            }
        }
        error = "byte signature harus dipisah spasi. Contoh: 48 8B ?? 89";
        return false;
    }
    error = "byte signature tidak valid: " + std::string(text);
    return false;
}

std::optional<std::vector<SignatureByte>> parseSignaturePattern(
    std::string_view pattern,
    std::string& error) {
    std::vector<SignatureByte> bytes;
    std::istringstream input{std::string(pattern)};
    std::string token;
    while (input >> token) {
        SignatureByte parsed{};
        if (token.size() > 2 && token.size() % 2 == 0) {
            for (std::size_t index = 0; index < token.size(); index += 2) {
                const auto pair = token.substr(index, 2);
                if (!parseSignatureByte(pair, parsed, error)) {
                    return std::nullopt;
                }
                bytes.push_back(parsed);
            }
            continue;
        }
        if (!parseSignatureByte(token, parsed, error)) {
            return std::nullopt;
        }
        bytes.push_back(parsed);
    }
    if (bytes.empty()) {
        error = "signature pattern tidak boleh kosong";
        return std::nullopt;
    }
    return bytes;
}

bool signatureMatches(const std::byte* data,
                      const std::vector<SignatureByte>& pattern) {
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        if (!pattern[index].wildcard && data[index] != pattern[index].value) {
            return false;
        }
    }
    return true;
}

void sortCandidates(std::vector<Candidate>& candidates) {
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left,
                                                      const Candidate& right) {
        return left.address < right.address;
    });
}

void sortPointerChains(std::vector<PointerChain>& chains) {
    std::sort(chains.begin(), chains.end(), [](const PointerChain& left,
                                              const PointerChain& right) {
        if (left.offsets.size() != right.offsets.size()) {
            return left.offsets.size() < right.offsets.size();
        }
        if (left.moduleName != right.moduleName) {
            return left.moduleName < right.moduleName;
        }
        return left.moduleOffset < right.moduleOffset;
    });
}

bool matches(NextMode mode,
             const TypedValue& previous,
             const TypedValue& current,
             const TypedValue* exact) {
    switch (mode) {
        case NextMode::Exact:
            return exact != nullptr && valuesEqual(current, *exact);
        case NextMode::Changed:
            return !valuesEqual(previous, current);
        case NextMode::Unchanged:
            return valuesEqual(previous, current);
        case NextMode::Increased:
            return valueIncreased(previous, current);
        case NextMode::Decreased:
            return valueDecreased(previous, current);
    }
    return false;
}

}  // namespace

MemoryScanner::MemoryScanner(std::shared_ptr<ProcessMemory> process)
    : process_(std::move(process)) {
    threadCount_ = std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

void MemoryScanner::setType(ValueType type) {
    if (type_ != type) {
        type_ = type;
        scanKind_ = ScanKind::Typed;
        signaturePattern_.clear();
        reset();
    }
}

void MemoryScanner::setThreadCount(std::size_t count) {
    threadCount_ = std::max<std::size_t>(1, count);
}

void MemoryScanner::setCandidates(std::vector<Candidate> candidates) {
    candidates_ = std::move(candidates);
    sortCandidates(candidates_);
}

void MemoryScanner::setPointerChains(std::vector<PointerChain> chains) {
    pointerChains_ = std::move(chains);
    sortPointerChains(pointerChains_);
    if (!pointerChains_.empty()) {
        scanKind_ = ScanKind::Pointer;
    }
}

void MemoryScanner::clearPointerChains() {
    pointerChains_.clear();
}

bool MemoryScanner::firstSignature(std::string_view pattern, std::string& error) {
    const auto parsed = parseSignaturePattern(pattern, error);
    if (!parsed) {
        return false;
    }

    candidates_.clear();
    pointerChains_.clear();
    scanKind_ = ScanKind::Signature;
    signaturePattern_ = std::string(pattern);

    const auto memoryRegions = process_->regions(error);
    if (!error.empty()) {
        return false;
    }

    std::vector<MemoryRegion> scanRegions;
    for (const auto& region : memoryRegions) {
        if (region.readable && region.size >= parsed->size()) {
            scanRegions.push_back(region);
        }
    }

    if (scanRegions.empty()) {
        error = "tidak ada readable memory yang cocok untuk signature scan";
        return false;
    }

    const auto workers = effectiveThreadCount(threadCount_, scanRegions.size());
    std::vector<std::vector<Candidate>> workerResults(workers);
    std::atomic<std::size_t> totalCandidates{0};
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> readableChunkCount{0};
    std::string lastReadError;
    std::mutex errorMutex;

    const auto scanRegion = [&](const MemoryRegion& region,
                                std::vector<Candidate>& localCandidates) {
        std::vector<std::byte> localBuffer(kChunkSize);
        std::vector<std::byte> window;
        std::vector<std::byte> carry;
        for (std::uint64_t offset = 0; offset < region.size && !stop.load();) {
            const auto remaining = region.size - offset;
            const std::size_t requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, localBuffer.size()));
            if (requested == 0) {
                break;
            }

            std::string readError;
            if (!process_->read(region.base + offset,
                                localBuffer.data(),
                                requested,
                                readError)) {
                {
                    std::lock_guard lock(errorMutex);
                    if (lastReadError.empty()) {
                        lastReadError = std::move(readError);
                    }
                }
                offset += requested;
                continue;
            }
            ++readableChunkCount;

            window.clear();
            window.reserve(carry.size() + requested);
            window.insert(window.end(), carry.begin(), carry.end());
            window.insert(window.end(), localBuffer.begin(),
                          localBuffer.begin() + requested);

            const std::size_t overlap = carry.size();
            if (window.size() >= parsed->size()) {
                for (std::size_t position = 0;
                     position + parsed->size() <= window.size() && !stop.load();
                     ++position) {
                    const auto absoluteStart =
                        region.base + offset - overlap + position;
                    if (absoluteStart + parsed->size() <= region.base + offset) {
                        continue;
                    }
                    if (!signatureMatches(window.data() + position, *parsed)) {
                        continue;
                    }
                    const auto previousCount = totalCandidates.fetch_add(1);
                    if (previousCount >= kMaxUnknownCandidates) {
                        stop = true;
                        break;
                    }
                    Candidate candidate;
                    candidate.address = absoluteStart;
                    std::memcpy(candidate.previous.data(),
                                window.data() + position,
                                std::min<std::size_t>(candidate.previous.size(),
                                                       parsed->size()));
                    localCandidates.push_back(candidate);
                }
            }

            carry.clear();
            const std::size_t keep =
                std::min<std::size_t>(parsed->size() - 1, window.size());
            carry.insert(carry.end(), window.end() - keep, window.end());
            offset += requested;
        }
    };

    if (workers == 1) {
        for (const auto& region : scanRegions) {
            scanRegion(region, workerResults.front());
            if (stop.load()) {
                break;
            }
        }
    } else {
        std::vector<std::jthread> pool;
        std::atomic<std::size_t> nextRegion{0};
        pool.reserve(workers);
        for (std::size_t index = 0; index < workers; ++index) {
            pool.emplace_back([&, index](std::stop_token) {
                auto& localCandidates = workerResults[index];
                while (!stop.load()) {
                    const auto regionIndex = nextRegion.fetch_add(1);
                    if (regionIndex >= scanRegions.size()) {
                        break;
                    }
                    scanRegion(scanRegions[regionIndex], localCandidates);
                }
            });
        }
        pool.clear();
    }

    for (auto& localCandidates : workerResults) {
        candidates_.insert(candidates_.end(),
                           std::make_move_iterator(localCandidates.begin()),
                           std::make_move_iterator(localCandidates.end()));
    }

    sortCandidates(candidates_);
    if (readableChunkCount == 0) {
        candidates_.clear();
        error = lastReadError.empty() ? "tidak ada readable memory yang dapat dibaca"
                                      : lastReadError;
        return false;
    }
    if (stop.load()) {
        error = "Signature scan melewati batas candidate. Kurangi pola yang terlalu umum.";
        candidates_.clear();
        return false;
    }
    return true;
}

bool MemoryScanner::firstPointer(std::uint64_t targetAddress,
                                 std::size_t maxDepth,
                                 std::uint64_t maxOffset,
                                 std::size_t maxResults,
                                 std::string& error) {
    if (maxDepth == 0) {
        error = "depth pointer scan harus lebih besar dari nol";
        return false;
    }
    candidates_.clear();
    pointerChains_.clear();
    scanKind_ = ScanKind::Pointer;
    signaturePattern_.clear();

    const auto memoryRegions = process_->regions(error);
    if (!error.empty()) {
        return false;
    }

    const auto loadedModules = process_->modules(error);
    if (!error.empty()) {
        return false;
    }
    if (loadedModules.empty()) {
        error = "module target tidak ditemukan untuk anchor pointer chain";
        return false;
    }

    std::vector<MemoryRegion> scanRegions;
    for (const auto& region : memoryRegions) {
        if (region.readable && region.writable &&
            region.size >= kPointerValueSize) {
            scanRegions.push_back(region);
        }
    }
    if (scanRegions.empty()) {
        error = "tidak ada readable memory yang dapat dipindai untuk pointer chain";
        return false;
    }

    struct SearchNode {
        std::uint64_t address{};
        std::vector<std::uint64_t> offsets;
    };

    const auto containingModule = [&](std::uint64_t address)
        -> const ModuleInfo* {
        for (const auto& module : loadedModules) {
            if (module.base <= address &&
                address < module.base + module.size) {
                return &module;
            }
        }
        return nullptr;
    };

    std::vector<SearchNode> frontier{{targetAddress, {}}};
    std::size_t totalFound = 0;
    std::string lastReadError;
    std::atomic<std::size_t> readableChunkCount{0};
    std::mutex errorMutex;

    for (std::size_t depth = 1; depth <= maxDepth && !frontier.empty(); ++depth) {
        std::sort(frontier.begin(), frontier.end(),
                  [](const SearchNode& left, const SearchNode& right) {
                      return left.address < right.address;
                  });
        std::vector<std::vector<PointerChain>> workerChains;
        std::vector<std::vector<SearchNode>> workerFrontiers;

        const auto workers = effectiveThreadCount(threadCount_, scanRegions.size());
        workerChains.resize(workers);
        workerFrontiers.resize(workers);
        auto scanRegion = [&](const MemoryRegion& region,
                              std::vector<PointerChain>& localChains,
                              std::vector<SearchNode>& localFrontier) {
            std::vector<std::byte> localBuffer(kChunkSize);
            for (std::uint64_t offset = 0; offset < region.size;) {
                const auto remaining = region.size - offset;
                const std::size_t requested = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, localBuffer.size()));
                if (requested < kPointerValueSize) {
                    break;
                }

                std::string readError;
                if (!process_->read(region.base + offset,
                                    localBuffer.data(),
                                    requested,
                                    readError)) {
                    std::lock_guard lock(errorMutex);
                    if (lastReadError.empty()) {
                        lastReadError = std::move(readError);
                    }
                    offset += requested;
                    continue;
                }
                ++readableChunkCount;

                const std::size_t usable = requested - (requested % kPointerValueSize);
                for (std::size_t position = 0;
                 position + kPointerValueSize <= usable;
                 position += kPointerValueSize) {
                    const auto pointerValue = readUint64(localBuffer.data() + position);
                    if (pointerValue == 0 ||
                        pointerValue > UINT64_MAX - maxOffset) {
                        continue;
                    }

                    const auto upper = pointerValue + maxOffset;
                    auto match = std::lower_bound(
                        frontier.begin(), frontier.end(), pointerValue,
                        [](const SearchNode& node, std::uint64_t value) {
                            return node.address < value;
                        });
                    for (; match != frontier.end() &&
                           match->address <= upper; ++match) {
                        const auto pointerAddress =
                            region.base + offset + position;
                        SearchNode next;
                        next.address = pointerAddress;
                        next.offsets.reserve(match->offsets.size() + 1);
                        next.offsets.push_back(match->address - pointerValue);
                        next.offsets.insert(next.offsets.end(),
                                            match->offsets.begin(),
                                            match->offsets.end());
                        localFrontier.push_back(next);

                        if (const auto* module =
                                containingModule(pointerAddress)) {
                            PointerChain chain;
                            chain.moduleName = module->path.empty()
                                                   ? module->name
                                                   : module->path;
                            chain.anchorAddress = pointerAddress;
                            chain.moduleOffset =
                                pointerAddress - module->base;
                            chain.offsets = std::move(next.offsets);
                            localChains.push_back(std::move(chain));
                        }
                    }
                }
                offset += requested;
            }
        };

        if (workers == 1) {
            for (const auto& region : scanRegions) {
                scanRegion(region,
                           workerChains.front(),
                           workerFrontiers.front());
            }
        } else {
            std::atomic<std::size_t> nextRegion{0};
            std::vector<std::jthread> pool;
            pool.reserve(workers);
            for (std::size_t index = 0; index < workers; ++index) {
                pool.emplace_back([&, index](std::stop_token) {
                    auto& localChains = workerChains[index];
                    auto& localFrontier = workerFrontiers[index];
                    while (true) {
                        const auto regionIndex = nextRegion.fetch_add(1);
                        if (regionIndex >= scanRegions.size()) {
                            break;
                        }
                        scanRegion(scanRegions[regionIndex],
                                   localChains,
                                   localFrontier);
                    }
                });
            }
            pool.clear();
        }

        std::vector<SearchNode> nextFrontier;
        for (auto& localChains : workerChains) {
            for (auto& chain : localChains) {
                pointerChains_.push_back(std::move(chain));
                ++totalFound;
                if (totalFound >= maxResults) {
                    break;
                }
            }
            if (totalFound >= maxResults) {
                break;
            }
        }
        for (auto& localFrontier : workerFrontiers) {
            nextFrontier.insert(
                nextFrontier.end(),
                std::make_move_iterator(localFrontier.begin()),
                std::make_move_iterator(localFrontier.end()));
        }
        std::sort(nextFrontier.begin(), nextFrontier.end(),
                  [](const SearchNode& left, const SearchNode& right) {
                      return left.address < right.address;
                  });
        nextFrontier.erase(
            std::unique(nextFrontier.begin(), nextFrontier.end(),
                        [](const SearchNode& left, const SearchNode& right) {
                            return left.address == right.address;
                        }),
            nextFrontier.end());
        constexpr std::size_t kMaxPointerFrontier = 20'000;
        if (nextFrontier.size() > kMaxPointerFrontier) {
            nextFrontier.resize(kMaxPointerFrontier);
        }
        frontier = std::move(nextFrontier);
        if (totalFound >= maxResults) {
            break;
        }
        if (frontier.empty()) {
            break;
        }
    }

    sortPointerChains(pointerChains_);
    if (pointerChains_.empty()) {
        error = readableChunkCount == 0 ? "tidak ada readable memory yang dapat dibaca"
                                        : "pointer chain tidak ditemukan";
        return false;
    }
    Candidate candidate;
    candidate.address = targetAddress;
    TypedValue current;
    if (readValue(targetAddress, type_, current, error)) {
        candidate.previous = current.bytes;
    } else {
        error.clear();
    }
    candidates_.push_back(candidate);
    return true;
}

ValueType MemoryScanner::type() const {
    return type_;
}

std::size_t MemoryScanner::threadCount() const {
    return threadCount_;
}

ScanKind MemoryScanner::scanKind() const {
    return scanKind_;
}

const std::string& MemoryScanner::signaturePattern() const {
    return signaturePattern_;
}

const std::vector<PointerChain>& MemoryScanner::pointerChains() const {
    return pointerChains_;
}

std::size_t MemoryScanner::candidateCount() const {
    return candidates_.size();
}

const std::vector<Candidate>& MemoryScanner::candidates() const {
    return candidates_;
}

std::shared_ptr<ProcessMemory> MemoryScanner::process() const {
    return process_;
}

bool MemoryScanner::firstExact(const TypedValue& value, std::string& error) {
    if (value.type != type_) {
        error = "tipe nilai tidak sama dengan tipe scan";
        return false;
    }
    return firstScan(&value, error);
}

bool MemoryScanner::firstUnknown(std::string& error) {
    return firstScan(nullptr, error);
}

bool MemoryScanner::firstScan(const TypedValue* exactValue,
                              std::string& error) {
    candidates_.clear();
    scanKind_ = ScanKind::Typed;
    signaturePattern_.clear();
    pointerChains_.clear();
    const auto memoryRegions = process_->regions(error);
    if (!error.empty()) {
        return false;
    }

    TypedValue sizeProbe;
    sizeProbe.type = type_;
    const std::size_t valueSize = sizeProbe.size();
    std::string lastReadError;
    std::mutex errorMutex;
    std::atomic<std::size_t> totalCandidates{0};
    std::atomic<std::size_t> readableChunkCount{0};
    std::atomic<bool> stop{false};

    const auto scanRegion = [&](const MemoryRegion& region,
                                std::vector<Candidate>& localCandidates) {
        std::vector<std::byte> localBuffer(kChunkSize);
        for (std::uint64_t offset = 0; offset < region.size && !stop.load();) {
            const auto remaining = region.size - offset;
            const std::size_t requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, localBuffer.size()));
            if (requested < valueSize) {
                break;
            }

            std::string readError;
            if (!process_->read(region.base + offset,
                                localBuffer.data(),
                                requested,
                                readError)) {
                {
                    std::lock_guard lock(errorMutex);
                    if (lastReadError.empty()) {
                        lastReadError = std::move(readError);
                    }
                }
                offset += requested;
                continue;
            }
            ++readableChunkCount;

            const std::size_t usable = requested - (requested % valueSize);
            for (std::size_t position = 0;
                 position + valueSize <= usable && !stop.load();
                 position += valueSize) {
                const auto current = bytesValue(type_, localBuffer.data() + position);
                if (exactValue == nullptr || valuesEqual(current, *exactValue)) {
                    const auto previousCount = totalCandidates.fetch_add(1);
                    if (exactValue == nullptr && previousCount >= kMaxUnknownCandidates) {
                        stop = true;
                        break;
                    }
                    Candidate candidate;
                    candidate.address = region.base + offset + position;
                    candidate.previous = current.bytes;
                    localCandidates.push_back(candidate);
                }
            }
            offset += requested;
        }
    };

    std::vector<MemoryRegion> scanRegions;
    for (const auto& region : memoryRegions) {
        if (region.readable && region.writable && region.privateMapping &&
            region.size >= valueSize) {
            scanRegions.push_back(region);
        }
    }

    const auto workers = effectiveThreadCount(threadCount_, scanRegions.size());
    std::vector<std::vector<Candidate>> workerResults(workers);

    if (workers == 1) {
        for (const auto& region : scanRegions) {
            scanRegion(region, workerResults.front());
            if (stop.load()) {
                break;
            }
        }
    } else {
        std::atomic<std::size_t> nextRegion{0};
        std::vector<std::jthread> pool;
        pool.reserve(workers);
        for (std::size_t index = 0; index < workers; ++index) {
            pool.emplace_back([&, index](std::stop_token) {
                auto& localCandidates = workerResults[index];
                while (!stop.load()) {
                    const auto regionIndex = nextRegion.fetch_add(1);
                    if (regionIndex >= scanRegions.size()) {
                        break;
                    }
                    scanRegion(scanRegions[regionIndex], localCandidates);
                }
            });
        }
        pool.clear();
    }

    for (auto& localCandidates : workerResults) {
        if (!localCandidates.empty()) {
            candidates_.insert(candidates_.end(),
                               std::make_move_iterator(localCandidates.begin()),
                               std::make_move_iterator(localCandidates.end()));
        }
    }

    sortCandidates(candidates_);
    if (readableChunkCount == 0) {
        candidates_.clear();
        error = lastReadError.empty()
                    ? "tidak ada writable private memory yang dapat dibaca"
                    : lastReadError;
        return false;
    }
    if (stop.load() && exactValue == nullptr) {
        error = "Unknown Initial Value melewati batas 30 juta candidate. "
                "Kurangi mapping target atau gunakan Exact Value.";
        candidates_.clear();
        return false;
    }
    return true;
}

bool MemoryScanner::next(NextMode mode,
                         const TypedValue* exactValue,
                         std::string& error) {
    if (scanKind_ == ScanKind::Signature || scanKind_ == ScanKind::Pointer) {
        error = "signature dan pointer scan tidak mendukung Next Scan. "
                "Jalankan scan lagi atau gunakan save/load config.";
        return false;
    }
    if (candidates_.empty()) {
        error = "belum ada candidate. Jalankan First Scan lebih dulu";
        return false;
    }
    if (mode == NextMode::Exact &&
        (exactValue == nullptr || exactValue->type != type_)) {
        error = "Next Scan exact membutuhkan nilai dengan tipe yang sama";
        return false;
    }

    TypedValue sizeProbe;
    sizeProbe.type = type_;
    const std::size_t valueSize = sizeProbe.size();
    std::vector<Candidate> filtered;
    filtered.reserve(candidates_.size());

    const auto groups = pageGroups(candidates_);
    const auto workers = effectiveThreadCount(threadCount_, groups.size());
    std::vector<std::jthread> pool;
    std::vector<std::vector<Candidate>> workerResults(workers);

    const auto processGroup = [&](const ScanGroup& group,
                                 std::vector<Candidate>& localFiltered) {
        const std::uint64_t blockStart = candidates_[group.begin].address;
        const std::uint64_t blockEnd =
            candidates_[group.end - 1].address + valueSize;
        std::vector<std::byte> block(
            static_cast<std::size_t>(blockEnd - blockStart));
        std::string readError;
        const bool bulkRead = process_->read(blockStart,
                                             block.data(),
                                             block.size(),
                                             readError);

        for (std::size_t index = group.begin; index < group.end; ++index) {
            TypedValue current;
            current.type = type_;
            bool readOk = false;
            if (bulkRead) {
                const auto offset = static_cast<std::size_t>(
                    candidates_[index].address - blockStart);
                std::memcpy(current.bytes.data(),
                            block.data() + offset,
                            valueSize);
                readOk = true;
            } else {
                std::string singleError;
                readOk = process_->read(candidates_[index].address,
                                        current.bytes.data(),
                                        valueSize,
                                        singleError);
            }

            if (!readOk) {
                continue;
            }
            const auto previous = candidateValue(type_,
                                                 candidates_[index].previous);
            if (matches(mode, previous, current, exactValue)) {
                auto candidate = candidates_[index];
                candidate.previous = current.bytes;
                localFiltered.push_back(candidate);
            }
        }
    };

    if (workers == 1) {
        for (const auto& group : groups) {
            processGroup(group, workerResults.front());
        }
    } else {
        std::atomic<std::size_t> nextGroup{0};
        pool.reserve(workers);
        for (std::size_t index = 0; index < workers; ++index) {
            pool.emplace_back([&, index](std::stop_token) {
                auto& localFiltered = workerResults[index];
                while (true) {
                    const auto groupIndex = nextGroup.fetch_add(1);
                    if (groupIndex >= groups.size()) {
                        break;
                    }
                    processGroup(groups[groupIndex], localFiltered);
                }
            });
        }
        pool.clear();
    }

    for (auto& localFiltered : workerResults) {
        filtered.insert(filtered.end(),
                        std::make_move_iterator(localFiltered.begin()),
                        std::make_move_iterator(localFiltered.end()));
    }

    sortCandidates(filtered);
    candidates_ = std::move(filtered);
    return true;
}

void MemoryScanner::reset() {
    candidates_.clear();
    pointerChains_.clear();
}

bool MemoryScanner::resolvePointerChain(const PointerChain& chain,
                                        std::uint64_t& resolvedAddress,
                                        std::string& error) const {
    const auto loadedModules = process_->modules(error);
    if (!error.empty()) {
        return false;
    }

    const auto requestedBaseName =
        std::filesystem::path(chain.moduleName).filename().string();
    const auto module = std::find_if(
        loadedModules.begin(), loadedModules.end(),
        [&](const ModuleInfo& entry) {
            return entry.path == chain.moduleName ||
                   entry.name == chain.moduleName ||
                   entry.name == requestedBaseName;
        });
    if (module == loadedModules.end()) {
        error = "module pointer chain tidak ditemukan: " + chain.moduleName;
        return false;
    }

    if (chain.moduleOffset >= module->size) {
        error = "module offset pointer chain berada di luar module";
        return false;
    }

    std::uint64_t address = module->base + chain.moduleOffset;
    for (const auto offset : chain.offsets) {
        std::uint64_t nextAddress = 0;
        if (!process_->read(address,
                            &nextAddress,
                            sizeof(nextAddress),
                            error)) {
            return false;
        }
        if (nextAddress > UINT64_MAX - offset) {
            error = "pointer chain overflow";
            return false;
        }
        address = nextAddress + offset;
    }
    resolvedAddress = address;
    return true;
}

bool MemoryScanner::readValue(std::uint64_t address,
                              ValueType type,
                              TypedValue& output,
                              std::string& error) const {
    output = {};
    output.type = type;
    return process_->read(address,
                          output.bytes.data(),
                          output.size(),
                          error);
}

bool MemoryScanner::writeValue(std::uint64_t address,
                               const TypedValue& value,
                               std::string& error) {
    return process_->write(address,
                           value.bytes.data(),
                           value.size(),
                           error);
}
