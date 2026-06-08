#include "daemon.h"
#include "emulator_session.h"
#include <algorithm>
#include <cstring>
#include <vector>

// Memory helper utilities.
// The actual read/write operations are delegated to EmulatorSession.
// This file can be extended for cached reads and memory search.

// Search RDRAM for a byte pattern
struct MemSearchResult {
    uint32_t address;
    std::vector<uint8_t> context;
};

std::vector<MemSearchResult> searchMemory(EmulatorSession *session,
                                           const std::vector<uint8_t> &pattern,
                                           uint32_t startAddr, uint32_t endAddr,
                                           uint32_t maxResults) {
    std::vector<MemSearchResult> results;
    if (!session || pattern.empty()) return results;

    const uint32_t step = 4096;
    std::vector<uint8_t> buf(step);
    for (uint32_t addr = startAddr; addr < endAddr && results.size() < maxResults; addr += step) {
        uint32_t readSize = std::min(step, endAddr - addr);
        auto data = session->readMemory(addr, readSize);
        if (data.empty()) break;

        // Scan for pattern
        for (size_t i = 0; i + pattern.size() <= data.size(); i++) {
            if (std::memcmp(data.data() + i, pattern.data(), pattern.size()) == 0) {
                MemSearchResult r;
                r.address = addr + (uint32_t)i;
                results.push_back(r);
                if (results.size() >= maxResults) break;
            }
        }
    }
    return results;
}

// Compare two memory ranges
std::vector<std::pair<uint32_t, std::pair<uint8_t, uint8_t>>>
compareMemory(EmulatorSession *session, uint32_t addrA, uint32_t addrB, uint32_t size) {
    std::vector<std::pair<uint32_t, std::pair<uint8_t, uint8_t>>> diffs;
    if (!session) return diffs;

    auto dataA = session->readMemory(addrA, size);
    auto dataB = session->readMemory(addrB, size);
    uint32_t minSize = std::min((uint32_t)dataA.size(), (uint32_t)dataB.size());

    for (uint32_t i = 0; i < minSize; i++) {
        if (dataA[i] != dataB[i]) {
            diffs.push_back({addrA + i, {dataA[i], dataB[i]}});
        }
    }
    return diffs;
}
