#include "json_rpc_server.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

// Minimal JSON-RPC 2.0 implementation (not a full JSON parser — keep it simple)
// For production, use nlohmann/json or similar

static std::string jsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    return out;
}

static std::string formatResponse(int id, const std::string &resultJson) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":" + resultJson + "}\n";
}

static std::string formatError(int id, int code, const std::string &msg) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
           ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" +
           jsonEscape(msg) + "\"}}\n";
}

static std::string hexStr(uint32_t val) {
    char buf[20];
    snprintf(buf, sizeof(buf), "\"0x%08X\"", val);
    return buf;
}

static std::string hexStr64(uint64_t val) {
    char buf[20];
    snprintf(buf, sizeof(buf), "\"0x%016llX\"", (unsigned long long)val);
    return buf;
}

static std::string bytesToHex(const std::vector<uint8_t> &data, uint32_t maxLen = 0) {
    std::string out = "\"";
    uint32_t n = (maxLen > 0 && data.size() > maxLen) ? maxLen : (uint32_t)data.size();
    for (uint32_t i = 0; i < n; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", data[i]);
        out += buf;
    }
    if (n < data.size()) out += "...";
    out += "\"";
    return out;
}

// Extract a quoted string value from JSON (very simple parser)
static std::string extractString(const std::string &json, const std::string &key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    // skip optional whitespace after colon
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++; // skip opening quote
    std::string val;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            if (json[pos] == '"') val += '"';
            else if (json[pos] == '\\') val += '\\';
            else if (json[pos] == 'n') val += '\n';
            else { val += '\\'; val += json[pos]; }
        } else {
            val += json[pos];
        }
        pos++;
    }
    return val;
}

// Extract a hex/number value from JSON
// Handles: "key":"0x...", "key": "0x...", "key":1234, "key":0xABCD
static uint32_t extractHex(const std::string &json, const std::string &key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    // skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return 0;
    // quoted hex string: "0x..."
    if (json[pos] == '"') {
        pos++;
        char buf[20];
        int i = 0;
        while (pos < json.size() && json[pos] != '"' && i < 19) {
            buf[i++] = json[pos++];
        }
        buf[i] = 0;
        return (uint32_t)strtoul(buf, nullptr, 16);
    }
    // unquoted numeric (hex or decimal)
    char buf[20];
    int i = 0;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ' ' && i < 19) {
        buf[i++] = json[pos++];
    }
    buf[i] = 0;
    int base = (i > 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) ? 16 : 10;
    return (uint32_t)strtoul(buf, nullptr, base);
}

static int extractInt(const std::string &json, const std::string &key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    char buf[20];
    int i = 0;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ' ' && i < 19) {
        buf[i++] = json[pos++];
    }
    buf[i] = 0;
    if (i > 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X'))
        return (int)strtoul(buf, nullptr, 16);
    return atoi(buf);
}

static bool extractBool(const std::string &json, const std::string &key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return json.substr(pos, 4) == "true";
}

// Extract method name from request
static std::string extractMethod(const std::string &request) {
    return extractString(request, "method");
}

static int extractId(const std::string &request) {
    return extractInt(request, "id");
}

static std::string extractParams(const std::string &request) {
    auto pos = request.find("\"params\"");
    if (pos == std::string::npos) return "{}";
    pos = request.find('{', pos);
    if (pos == std::string::npos) return "{}";
    int depth = 0;
    std::string params;
    for (size_t i = pos; i < request.size(); i++) {
        if (request[i] == '{') depth++;
        else if (request[i] == '}') depth--;
        params += request[i];
        if (depth == 0) break;
    }
    return params;
}

// ── Helpers for asset scanning ──────────────────────────────────

// Classify a 16-byte ROM sample by content type
static const char* classifyRomSample(const uint8_t *data, size_t len) {
    if (len < 4) return "unknown";
    uint32_t first = (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
                     (uint32_t)data[2] << 8 | (uint32_t)data[3];
    // Check for zeros
    bool allZero = true;
    for (size_t i = 0; i < len; i++) { if (data[i] != 0) { allZero = false; break; } }
    if (allZero) return "empty";

    // Check header magic
    if (first == 0x80371240) return "rom_header";

    // Check for MIPS instructions (common opcodes: 0x00-0x1F SPECIAL, 0x08-0x0F J/COP, etc.)
    uint32_t op = first >> 26;
    if (op == 0x00 || op == 0x01 || op == 0x04 || op == 0x05 || op == 0x07 ||
        op == 0x08 || op == 0x09 || op == 0x0A || op == 0x0B || op == 0x0C ||
        op == 0x0D || op == 0x0E || op == 0x0F || op == 0x20 || op == 0x21 ||
        op == 0x22 || op == 0x23 || op == 0x24 || op == 0x25 || op == 0x26 ||
        op == 0x27 || op == 0x28 || op == 0x29 || op == 0x2A || op == 0x2B ||
        op == 0x2C || op == 0x2D || op == 0x2E || op == 0x2F || op == 0x30 ||
        op == 0x31 || op == 0x32 || op == 0x33 || op == 0x34 || op == 0x35 ||
        op == 0x36 || op == 0x37 || op == 0x38 || op == 0x39 || op == 0x3A ||
        op == 0x3B || op == 0x3C || op == 0x3D || op == 0x3E || op == 0x3F) {
        return "code";
    }

    // Check for ADPCM audio: repeating 73Ex pattern
    int adpcmMatch = 0;
    for (size_t i = 0; i < len - 1; i += 2) {
        if ((data[i] == 0x73 || data[i] == 0x6B || data[i] == 0x7B || data[i] == 0x63) &&
            (data[i+1] >= 0xE0 && data[i+1] <= 0xFF))
            adpcmMatch++;
    }
    if (adpcmMatch >= (int)(len / 8)) return "audio";

    // Check for RDP display-list commands
    if ((first & 0xFF000000) == 0xE7000000 || (first & 0xFF000000) == 0xE6000000 ||
        (first & 0xFF000000) == 0xBA000000 || (first & 0xFF000000) == 0xBF000000 ||
        (first & 0xFF000000) == 0xFD000000 || (first & 0xFF000000) == 0xF5000000 ||
        (first & 0xFF000000) == 0xF3000000 || (first & 0xFF000000) == 0xF2000000 ||
        (first & 0xFF000000) == 0xE8000000 || (first & 0xFF000000) == 0xFC000000)
        return "display_list";

    // High entropy = likely texture/asset data
    int uniqueBytes = 0;
    bool seen[256] = {false};
    for (size_t i = 0; i < len; i++) { if (!seen[data[i]]) { seen[data[i]] = true; uniqueBytes++; } }
    if (uniqueBytes > (int)(len / 2)) return "data_high_entropy";

    return "data";
}

// Format a region entry as JSON
static std::string regionJson(const char *addrLabel, uint32_t address, uint32_t size,
                               const char *type, const uint8_t *sample, uint32_t sampleLen) {
    std::string out = "{\"" + std::string(addrLabel) + "\":";
    char buf[128];
    snprintf(buf, sizeof(buf), "\"0x%08X\",\"size\":%u,\"type\":\"%s\"", address, size, type);
    out += buf;
    if (sample && sampleLen > 0) {
        out += ",\"sample\":";
        std::vector<uint8_t> sv(sample, sample + (sampleLen < 16 ? sampleLen : 16));
        out += bytesToHex(sv);
    }
    out += "}";
    return out;
}

// ── OS detection ─────────────────────────────────────────────
static std::string osTypeFromPatterns(bool hasDispatch, bool hasAnyLibultra, const char *bootType) {
    // __osDispatchThread (mtc0 $zero,$12) is the definitive libultra scheduler
    if (hasDispatch) return "libultra";
    // No dispatch + IPL3 boot but libultra fns found = likely libultra (dispatch undetected)
    if (strcmp(bootType, "ipl3") == 0 && hasAnyLibultra) return "likely_libultra";
    // rom_code_direct boot = custom RTOS even if some libultra fns are statically linked
    if (strcmp(bootType, "rom_code_direct") == 0) {
        if (hasAnyLibultra) return "custom_with_libultra_functions";
        return "custom";
    }
    if (hasAnyLibultra) return "likely_libultra";
    return "unknown";
}

static const char* classifyUcode(const uint8_t *data, size_t len) {
    if (len < 16) return "unknown";
    int op0 = data[0] >> 2;
    int cop2Count = 0, lwc2Count = 0, swc2Count = 0, total = (int)(len / 4);
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t instr = (uint32_t)data[i] << 24 | data[i+1] << 16 | data[i+2] << 8 | data[i+3];
        uint32_t op = instr >> 26;
        if (op == 0x12) cop2Count++;        // COP2
        else if (op == 0x32) lwc2Count++;   // LWC2
        else if (op == 0x3A) swc2Count++;   // SWC2
    }
    if (cop2Count + lwc2Count + swc2Count == 0) return "standard_f3d";
    int special = cop2Count + lwc2Count + swc2Count;
    if (special < total / 4) return "f3dex2_like";
    return "custom_f3d_variant";
}

// Scan ROM region for a specific byte pattern (reads 4KB chunks for speed)
static std::vector<uint32_t> scanRomPattern(EmulatorSession *session, uint32_t startVaddr,
                                             uint32_t size, const uint8_t *pattern, size_t patLen) {
    std::vector<uint32_t> results;
    const uint32_t STEP = 4096;
    for (uint32_t baseOff = 0; baseOff + patLen <= size; baseOff += STEP) {
        uint32_t readSize = (uint32_t)std::min((size_t)(STEP + patLen - 1), (size_t)(size - baseOff));
        auto chunk = session->readMemory(startVaddr + baseOff, readSize);
        if (chunk.size() < patLen) break;
        for (uint32_t off = 0; off + patLen <= chunk.size(); off += 4) {
            if (memcmp(&chunk[off], pattern, patLen) == 0)
                results.push_back(startVaddr + baseOff + off);
        }
    }
    return results;
}

std::string JsonRpcServer::handleDetectOs(int id) {
    std::string out = "{";
    uint32_t romEntry = 0;
    const char *bootType = "unknown";

    // 1. ROM header
    auto headerData = mSession->readMemory(0xB0000000, 64);
    if (headerData.size() >= 64) {
        uint32_t magic = (uint32_t)headerData[0] << 24 | headerData[1] << 16 | headerData[2] << 8 | headerData[3];
        uint32_t clockrate = (uint32_t)headerData[4] << 24 | headerData[5] << 16 | headerData[6] << 8 | headerData[7];
        romEntry = (uint32_t)headerData[8] << 24 | headerData[9] << 16 | headerData[10] << 8 | headerData[11];
        uint32_t crc1 = (uint32_t)headerData[16] << 24 | headerData[17] << 16 | headerData[18] << 8 | headerData[19];
        uint32_t crc2 = (uint32_t)headerData[20] << 24 | headerData[21] << 16 | headerData[22] << 8 | headerData[23];
        uint8_t country = headerData[62];
        const char *countryStr = "unknown";
        if (country == 0x44) countryStr = "Germany";
        else if (country == 0x45) countryStr = "USA";
        else if (country == 0x46) countryStr = "France";
        else if (country == 0x49) countryStr = "Italy";
        else if (country == 0x4A) countryStr = "Japan";
        else if (country == 0x50) countryStr = "Europe";
        else if (country == 0x55) countryStr = "Australia";
        else if (country == 0x59) countryStr = "PAL";

        // Sanitize ROM code field (strip non-printable chars)
        char codeStr[8] = {0};
        for (int i = 0; i < 4 && i + 60 < 64 && headerData[60 + i] >= 0x20 && headerData[60 + i] < 0x7F; i++)
            codeStr[i] = headerData[60 + i];
        if (codeStr[0] == 0) snprintf(codeStr, sizeof(codeStr), "none");

        char headBuf[512];
        snprintf(headBuf, sizeof(headBuf),
                 "\"rom\":{\"name\":\"%.20s\",\"code\":\"%s\",\"crc1\":\"0x%08X\",\"crc2\":\"0x%08X\","
                 "\"entry\":\"0x%08X\",\"clockrate\":\"0x%08X\",\"country\":\"%s\",\"size_bytes\":%u}",
                 (const char*)&headerData[32], codeStr,
                 crc1, crc2, romEntry, clockrate, countryStr, mSession->isRomLoaded() ? 0x800000 : 0);
        out += std::string(headBuf);
    }

    // 2. Boot flow analysis — read from ROM via KSEG1
    if (romEntry != 0) {
        uint32_t romKseg1 = 0xB0000000 + (romEntry & 0x1FFFFFFF);
        auto entryMem = mSession->readMemory(romKseg1, 32);
        if (entryMem.size() >= 16) {
            uint32_t i0 = (uint32_t)entryMem[0]<<24|entryMem[1]<<16|entryMem[2]<<8|entryMem[3];
            uint32_t i1 = (uint32_t)entryMem[4]<<24|entryMem[5]<<16|entryMem[6]<<8|entryMem[7];
            uint32_t i2 = entryMem.size() >= 12 ? ((uint32_t)entryMem[8]<<24|entryMem[9]<<16|entryMem[10]<<8|entryMem[11]) : 0;
            uint32_t i3 = entryMem.size() >= 16 ? ((uint32_t)entryMem[12]<<24|entryMem[13]<<16|entryMem[14]<<8|entryMem[15]) : 0;
            bootType = "custom";
            if (romEntry == 0x80000400 ||
                (romEntry >= 0x80000000 && romEntry < 0x80001000 && (i0 >> 26) == 0x0F)) {
                bootType = "ipl3";
            } else if (romEntry >= 0x80000000 && romEntry < 0x80200000) {
                bootType = "rom_code_direct";
            }
            char bootBuf[256];
            snprintf(bootBuf, sizeof(bootBuf),
                     ",\"boot\":{\"type\":\"%s\",\"entry\":\"0x%08X\",\"first_instrs\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                     bootType, romEntry, i0, i1, i2, i3);
            out += std::string(bootBuf);
        }
    }

    // 3. OS type detection by scanning RDRAM (where code executes) and ROM
    {
        // libultra thread function prologues (big-endian z64)
        uint8_t osCreatePattern[] = {0x27,0xBD,0xFF,0xD0, 0xAF,0xBF,0x00,0x2C, 0xAF,0xB0,0x00,0x28};
        uint8_t osStartPattern[] = {0x27,0xBD,0xFF,0xD8, 0xAF,0xBF,0x00,0x24};
        uint8_t dispatchPattern[] = {0x40,0x80,0x60,0x00}; // mtc0 $zero,$12 (SR)
        uint8_t osYieldPattern[] = {0x27,0xBD,0xFF,0xE0, 0xAF,0xBF,0x00,0x1C};
        uint8_t customPattern[] = {0x27,0xBD,0xFF,0xE0, 0xAF,0xBF,0x00,0x1C, 0xAF,0xB0,0x00,0x18};

        // Scan RDRAM first (where code actually runs)
        auto scanBoth = [&](const uint8_t *pat, size_t patLen)
            -> std::pair<std::vector<uint32_t>, std::vector<uint32_t>> {
            auto ram = scanRomPattern(mSession, 0x80001000, 0x400000, pat, patLen);
            auto rom = scanRomPattern(mSession, 0xB0001000, 0x400000, pat, patLen);
            // If RDRAM has the pattern, prefer it (it's the execution address)
            if (!ram.empty()) return {ram, {}};
            return {{}, rom};
        };

        auto [osCreateRam, osCreateRom] = scanBoth(osCreatePattern, sizeof(osCreatePattern));
        auto [osStartRam, osStartRom] = scanBoth(osStartPattern, sizeof(osStartPattern));
        auto [dispatchRam, dispatchRom] = scanBoth(dispatchPattern, sizeof(dispatchPattern));
        auto [osYieldRam, osYieldRom] = scanBoth(osYieldPattern, sizeof(osYieldPattern));
        auto [customRam, customRom] = scanBoth(customPattern, sizeof(customPattern));

        // Build function list — always include ALL detected patterns
        std::string funcsJson;
        auto firstOf = [&](const std::string &name, const std::vector<uint32_t> &ram, const std::vector<uint32_t> &rom) {
            uint32_t addr = ram.empty() ? (rom.empty() ? 0 : rom[0]) : ram[0];
            if (addr == 0) return;
            if (!funcsJson.empty()) funcsJson += ",";
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"name\":\"%s\",\"addr\":\"0x%08X\",\"source\":\"%s\"}",
                     name.c_str(), addr, ram.empty() ? "rom" : "rdram");
            funcsJson += buf;
        };
        bool hasDispatch = !dispatchRam.empty() || !dispatchRom.empty();
        bool hasAnyLibultra = hasDispatch
                           || !osCreateRam.empty() || !osCreateRom.empty()
                           || !osStartRam.empty() || !osStartRom.empty()
                           || !osYieldRam.empty() || !osYieldRom.empty();
        firstOf("osCreateThread", osCreateRam, osCreateRom);
        firstOf("osStartThread", osStartRam, osStartRom);
        firstOf("osYieldThread", osYieldRam, osYieldRom);
        firstOf("__osDispatchThread", dispatchRam, dispatchRom);
        firstOf("custom_sched_fn", customRam, customRom);

        char osBuf[512];
        {
            std::string osType = osTypeFromPatterns(hasDispatch, hasAnyLibultra, bootType);
            snprintf(osBuf, sizeof(osBuf),
                     ",\"os\":{\"type\":\"%s\",\"has_dispatch\":%s,\"functions\":[%s]}",
                     osType.c_str(), hasDispatch ? "true" : "false", funcsJson.c_str());
        }
        out += std::string(osBuf);

        // 3b. Active context probe — check if current PC lands in a detected function
        {
            uint32_t curPc = mSession->getPC();
            // only meaningful if running past boot (PC in code space, not PIF)
            if (curPc >= 0x80000000 && curPc < 0x80400000) {
                // Build a small range map from detected addresses (assume ~0x200 byte functions)
                const std::pair<const char*, uint32_t> knownFns[] = {
                    {"osCreateThread",     osCreateRam.empty() ? (osCreateRom.empty() ? 0 : osCreateRom[0]) : osCreateRam[0]},
                    {"osStartThread",      osStartRam.empty() ? (osStartRom.empty() ? 0 : osStartRom[0]) : osStartRam[0]},
                    {"osYieldThread",      osYieldRam.empty() ? (osYieldRom.empty() ? 0 : osYieldRom[0]) : osYieldRam[0]},
                    {"__osDispatchThread", dispatchRam.empty() ? (dispatchRom.empty() ? 0 : dispatchRom[0]) : dispatchRam[0]},
                    {"custom_sched_fn",    customRam.empty() ? (customRom.empty() ? 0 : customRom[0]) : customRam[0]},
                };
                const char *activeFn = "unknown";
                for (auto &fn : knownFns) {
                    if (fn.second != 0 && curPc >= fn.second && curPc < fn.second + 0x200) {
                        activeFn = fn.first;
                        break;
                    }
                }
                char ctxBuf[128];
                snprintf(ctxBuf, sizeof(ctxBuf),
                         ",\"active_context\":{\"pc\":\"0x%08X\",\"in_function\":\"%s\"}",
                         curPc, activeFn);
                out += std::string(ctxBuf);
            }
        }
    }

    // 4. RSP ucode detection
    {
        auto ucode = mSession->readMemory(0xB0031000, 256);
        if (ucode.size() >= 64) {
            const char *ucodeType = classifyUcode(ucode.data(), ucode.size());
            // Check first 8 bytes as ucode_boot signature
            uint32_t boot0 = (uint32_t)ucode[0]<<24|ucode[1]<<16|ucode[2]<<8|ucode[3];
            uint32_t boot1 = (uint32_t)ucode[4]<<24|ucode[5]<<16|ucode[6]<<8|ucode[7];
            // Also check for SP DMEM resident ucode (F3DEX2 has 0xB807/0x0000 signature)
            auto dmem = mSession->readSpMemory(0, 16);

            char ucodeBuf[512];
            std::string dmemHex = bytesToHex(dmem, 16);
            snprintf(ucodeBuf, sizeof(ucodeBuf),
                     ",\"rsp\":{\"rom_offset\":\"0x31000\",\"type\":\"%s\",\"boot_bytes\":[\"0x%08X\",\"0x%08X\"],"
                     "\"dmem\":%s}",
                     ucodeType, boot0, boot1, dmemHex.c_str());
            out += std::string(ucodeBuf);
        }
    }

    out += "}";
    return formatResponse(id, out);
}

std::string JsonRpcServer::handleScanAssets(int id) {
    std::string out = "{";

    // 1. ROM header (4KB at KSEG1 0xB0000000)
    auto headerData = mSession->readMemory(0xB0000000, 4096);
    if (headerData.size() >= 64) {
        char headStr[256];
        uint32_t magic = (uint32_t)headerData[0] << 24 | (uint32_t)headerData[1] << 16 |
                         (uint32_t)headerData[2] << 8 | headerData[3];
        snprintf(headStr, sizeof(headStr),
                 "\"magic\":\"0x%08X\",\"crc1\":\"0x%08X\",\"crc2\":\"0x%08X\",\"name\":\"%.20s\",\"code\":\"%.4s\"",
                 magic,
                 (uint32_t)(headerData[4]<<24|headerData[5]<<16|headerData[6]<<8|headerData[7]),
                 (uint32_t)(headerData[8]<<24|headerData[9]<<16|headerData[10]<<8|headerData[11]),
                 (const char*)&headerData[32],
                 (const char*)&headerData[60]);
        out += "\"rom_header\":{" + std::string(headStr) + "},";
    }

    // 2. Scan ROM in 64KB chunks (8MB = 128 chunks)
    out += "\"rom_regions\":[";
    bool first = true;
    // 8MB ROM, 64KB strides
    for (uint32_t off = 0; off < 0x800000; off += 0x10000) {
        uint32_t vaddr = 0xB0000000 + off;
        auto chunk = mSession->readMemory(vaddr, 16);
        if (chunk.size() < 16) continue;
        const char *type = classifyRomSample(chunk.data(), 16);
        if (strcmp(type, "empty") == 0) continue;
        if (!first) out += ",";
        first = false;
        out += regionJson("vaddr", vaddr, 0x10000, type, chunk.data(), 16);
    }
    out += "],";

    // 3. Scan RDRAM in 128KB chunks (8MB = 64 chunks)
    out += "\"rdram_regions\":[";
    first = true;
    for (uint32_t off = 0; off < 0x800000; off += 0x20000) {
        uint32_t vaddr = 0x80000000 + off;
        auto chunk = mSession->readMemory(vaddr, 16);
        if (chunk.size() < 16) continue;
        const char *type = classifyRomSample(chunk.data(), 16);
        bool allZero = true;
        for (auto b : chunk) { if (b != 0) { allZero = false; break; } }
        if (allZero && off > 0x100000) continue; // skip large empty regions
        if (!first) out += ",";
        first = false;
        out += regionJson("vaddr", vaddr, 0x20000, type, chunk.data(), 16);
    }
    out += "],";

    // 4. Boot flow (PIF jump target)
    {
        auto pifMem = mSession->readMemory(0xA4000040, 16);
        auto resetVec = mSession->readMemory(0x80000000, 8);
        char bootBuf[256];
        if (resetVec.size() >= 8) {
            uint32_t instr1 = (uint32_t)resetVec[0]<<24|resetVec[1]<<16|resetVec[2]<<8|resetVec[3];
            uint32_t instr2 = (uint32_t)resetVec[4]<<24|resetVec[5]<<16|resetVec[6]<<8|resetVec[7];
            uint32_t pc = 0;
            // jr $k0: opcode 0x00, funct 0x08, rs $k0(26)
            if ((instr1 >> 26) == 0 && (instr1 & 0x3F) == 0x08 && ((instr1 >> 21) & 0x1F) == 26) {
                pc = instr2; // nop delay slot = target already in $k0 from lui+addiu
            }
            snprintf(bootBuf, sizeof(bootBuf),
                     "\"boot_flow\":{\"pif_jump\":\"0x%08X\",\"reset_trampoline\":[\"0x%08X\",\"0x%08X\"],\"detected_target\":\"0x%08X\"}",
                     (uint32_t)(pifMem[4]<<24|pifMem[5]<<16|pifMem[6]<<8|pifMem[7]),
                     instr1, instr2, pc);
            out += std::string(bootBuf) + ",";
        }
    }

    // 5. RSP task (if paused mid-execution)
    {
        auto task = mSession->readRspTaskHeader();
        if (!task.empty() && task[0] != 0) {
            char rspBuf[512];
            snprintf(rspBuf, sizeof(rspBuf),
                     "\"rsp_task\":{\"type\":\"0x%08X\",\"ucode_boot\":\"0x%08X\",\"ucode\":\"0x%08X\",\"ucode_data\":\"0x%08X\",\"data_ptr\":\"0x%08X\",\"data_size\":\"0x%08X\"}",
                     task[0], task[2], task[4], task[6], task[12], task[13]);
            out += std::string(rspBuf) + ",";
        }
    }

    // 6. Emulator state
    {
        char stateBuf[128];
        snprintf(stateBuf, sizeof(stateBuf),
                 "\"state\":{\"frame\":%llu,\"pc\":%s,\"paused\":%s}",
                 (unsigned long long)mSession->frameCount(),
                 hexStr(mSession->getPC()).c_str(),
                 mSession->isPaused() ? "true" : "false");
        out += std::string(stateBuf);
    }

    out += "}";
    return formatResponse(id, out);
}

// --- JSON-RPC method implementations ---

std::string JsonRpcServer::handleMethod(const std::string &method,
                                         const std::string &paramsJson, int id) {
    // Session
    if (method == "ping") {
        return formatResponse(id, "{\"pong\":true}");
    }
    if (method == "version") {
        return formatResponse(id, "{\"daemon\":\"n64-debug-daemon\",\"version\":\"0.1.0\"}");
    }
    if (method == "status") {
        char buf[512];
        int dbgState = mSession->getDebugState(M64P_DBG_RUN_STATE);
        snprintf(buf, sizeof(buf),
                 "{\"core_loaded\":%s,\"debugger_available\":%s,\"rom_loaded\":%s,"
                 "\"running\":%s,\"paused\":%s,\"frame\":%llu,\"pc\":%s,\"dbg_run_state\":%d}",
                 mSession->hasDebugger() ? "true" : "false",
                 mSession->isDebuggerAvailable() ? "true" : "false",
                 mSession->isRomLoaded() ? "true" : "false",
                 mSession->isEmuRunning() ? "true" : "false",
                 mSession->isPaused() ? "true" : "false",
                 (unsigned long long)mSession->frameCount(),
                 hexStr(mSession->getPC()).c_str(),
                 dbgState);
        return formatResponse(id, buf);
    }
    if (method == "load_rom") {
        std::string path = extractString(paramsJson, "path");
        if (path.empty()) return formatError(id, -32602, "Missing 'path' parameter");
        if (!mSession->openRom(path)) return formatError(id, -32000, "Failed to open ROM");
        mSession->startEmulation();
        return formatResponse(id, "{\"ok\":true}");
    }
    if (method == "close_rom") {
        mSession->closeRom();
        return formatResponse(id, "{\"ok\":true}");
    }
    if (method == "shutdown") {
        mSession->shutdown();
        return formatResponse(id, "{\"ok\":true}");
    }

    // Execution control
    if (method == "pause") {
        mSession->pause();
        return formatResponse(id, "{\"ok\":true}");
    }
    if (method == "resume") {
        mSession->resume();
        return formatResponse(id, "{\"ok\":true}");
    }
    if (method == "step_instruction") {
        bool ok = mSession->stepInstruction();
        if (!ok) return formatError(id, -32000, "Cannot step (not paused or no debugger)");
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"pc\":%s}", hexStr(mSession->getPC()).c_str());
        return formatResponse(id, buf);
    }
    if (method == "step_frame") {
        bool ok = mSession->stepFrame();
        if (!ok) return formatError(id, -32000, "Cannot step frame");
        return formatResponse(id, "{\"ok\":true}");
    }

    // Input injection
    if (method == "set_controller_state") {
        int channel = extractInt(paramsJson, "channel");
        unsigned int buttons = (unsigned int)extractHex(paramsJson, "buttons");
        signed char x = (signed char)extractInt(paramsJson, "x");
        signed char y = (signed char)extractInt(paramsJson, "y");
        int sticky = extractBool(paramsJson, "sticky") ? 1 : 0;
        mSession->setControllerState(channel, buttons, x, y, sticky);
        return formatResponse(id, "{\"ok\":true}");
    }

    // Framebuffer
    if (method == "read_framebuffer") {
        uint32_t w = 0, h = 0;
        int bpp = 0;
        auto pixels = mSession->readFramebuffer(w, h, bpp);
        if (pixels.empty())
            return formatError(id, -32000, "No framebuffer available");
        std::string json = "{\"width\":" + std::to_string(w) +
                           ",\"height\":" + std::to_string(h) +
                           ",\"bpp\":" + std::to_string(bpp) +
                           ",\"size\":" + std::to_string(pixels.size()) +
                           ",\"pixels\":" + bytesToHex(pixels) + "}";
        return formatResponse(id, json);
    }

    // CPU queries
    if (method == "get_pc") {
        return formatResponse(id, hexStr(mSession->getPC()));
    }
    if (method == "get_registers") {
        auto regs = mSession->readAllRegisters();
        std::string out = "{\"gpr\":[";
        for (int i = 0; i < 32; i++) {
            if (i > 0) out += ",";
            out += hexStr(regs[i]);
        }
        out += "],\"pc\":";
        out += hexStr(mSession->getPC());
        out += "}";
        return formatResponse(id, out);
    }

    // Memory
    if (method == "read_mem") {
        uint32_t addr = extractHex(paramsJson, "address");
        uint32_t size = extractInt(paramsJson, "size");
        if (size == 0) size = 64;
        if (size > 1048576) size = 1048576;
        auto data = mSession->readMemory(addr, size);
        std::string json = "{\"address\":" + hexStr(addr) +
                           ",\"size\":" + std::to_string(size) +
                           ",\"hex\":" + bytesToHex(data) + "}";
        return formatResponse(id, json);
    }
    if (method == "write_mem") {
        if (!mSession->isMemoryWriteAllowed())
            return formatError(id, -32001, "Memory writes disabled");
        uint32_t addr = extractHex(paramsJson, "address");
        std::string hexData = extractString(paramsJson, "data");
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < hexData.size(); i += 2) {
            char buf[3] = {hexData[i], hexData[i+1], 0};
            bytes.push_back((uint8_t)strtoul(buf, nullptr, 16));
        }
        bool ok = mSession->writeMemory(addr, bytes.data(), (uint32_t)bytes.size());
        return formatResponse(id, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    }
    if (method == "dump_rdram") {
        // Dump full RDRAM (8 MB default)
        uint32_t size = extractInt(paramsJson, "size");
        if (size == 0 || size > 0x800000) size = 0x800000;
        auto data = mSession->readMemory(0x80000000, size);
        std::string json = "{\"address\":\"0x80000000\",\"size\":" +
                           std::to_string(data.size()) + ",\"hex\":" +
                           bytesToHex(data, 4096) + "}";
        return formatResponse(id, json);
    }
    if (method == "translate_address") {
        uint32_t vaddr = extractHex(paramsJson, "vaddr");
        uint32_t paddr = mSession->translateAddress(vaddr);
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"vaddr\":%s,\"paddr\":%s}",
                 hexStr(vaddr).c_str(), hexStr(paddr).c_str());
        return formatResponse(id, buf);
    }

    // Breakpoints
    if (method == "add_exec_breakpoint") {
        uint32_t vaddr = extractHex(paramsJson, "address");
        int index = mSession->addExecBreakpoint(vaddr);
        if (index < 0) return formatError(id, -32000, "Failed to add breakpoint");
        return formatResponse(id, "{\"index\":" + std::to_string(index) + "}");
    }
    if (method == "add_mem_breakpoint") {
        uint32_t vaddr = extractHex(paramsJson, "address");
        uint32_t size = extractInt(paramsJson, "size");
        if (size == 0) size = 4;
        std::string type = extractString(paramsJson, "type");
        unsigned int flags = 0;
        if (type == "read") flags = M64P_BKP_FLAG_READ;
        else if (type == "write") flags = M64P_BKP_FLAG_WRITE;
        else flags = M64P_BKP_FLAG_READ | M64P_BKP_FLAG_WRITE;
        int index = mSession->addMemoryBreakpoint(vaddr, size, flags);
        if (index < 0) return formatError(id, -32000, "Failed to add memory breakpoint");
        return formatResponse(id, "{\"index\":" + std::to_string(index) + "}");
    }
    if (method == "remove_breakpoint") {
        int index = extractInt(paramsJson, "index");
        bool ok = mSession->removeBreakpoint(index);
        return formatResponse(id, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    }
    if (method == "list_breakpoints") {
        auto bps = mSession->listBreakpoints();
        std::string out = "[";
        for (size_t i = 0; i < bps.size(); i++) {
            if (i > 0) out += ",";
            out += "{\"index\":" + std::to_string(bps[i].index) + "}";
        }
        out += "]";
        return formatResponse(id, out);
    }

    // Tracing
    if (method == "mark_game_state") {
        std::string label = extractString(paramsJson, "label");
        std::string notes = extractString(paramsJson, "notes");
        mSession->markGameState(label, notes);
        return formatResponse(id, "{\"ok\":true}");
    }
    if (method == "get_trace_events") {
        uint32_t count = extractInt(paramsJson, "count");
        auto events = mSession->getRecentEvents(count ? count : 100);
        std::string out = "[";
        for (size_t i = 0; i < events.size(); i++) {
            if (i > 0) out += ",";
            out += "{\"frame\":" + std::to_string(events[i].frame) +
                   ",\"type\":\"" + jsonEscape(events[i].type) +
                   "\",\"pc\":" + hexStr(events[i].pc) +
                   ",\"state_label\":\"" + jsonEscape(events[i].stateLabel) + "\"";
            if (!events[i].data.empty()) {
                out += ",\"data\":[";
                for (size_t d = 0; d < events[i].data.size(); d++) {
                    if (d > 0) out += ",";
                    out += "{\"key\":\"" + jsonEscape(events[i].data[d].first) +
                           "\",\"value\":\"" + jsonEscape(events[i].data[d].second) + "\"}";
                }
                out += "]";
            }
            out += "}";
        }
        out += "]";
        return formatResponse(id, out);
    }
    if (method == "trace_rom_reads") {
        bool enable = extractBool(paramsJson, "enable");
        mSession->enableRomReadTrace(enable);
        return formatResponse(id, "{\"ok\":true}");
    }

    // PI DMA
    if (method == "capture_pi_dma") {
        auto regs = mSession->readPiDmaRegs();
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "{\"dram_addr\":%s,\"cart_addr\":%s,\"rd_len\":%s,\"wr_len\":%s,\"status\":%s}",
                 hexStr(regs.dramAddr).c_str(), hexStr(regs.cartAddr).c_str(),
                 hexStr(regs.rdLen).c_str(), hexStr(regs.wrLen).c_str(),
                 hexStr(regs.status).c_str());
        return formatResponse(id, buf);
    }
    if (method == "enable_pi_dma_trace") {
        bool enable = extractBool(paramsJson, "enable");
        mSession->enablePiDmaTrace(enable);
        return formatResponse(id, "{\"ok\":true}");
    }

    // SP / RSP
    if (method == "read_sp_mem") {
        uint32_t offset = extractHex(paramsJson, "offset");
        uint32_t size = extractInt(paramsJson, "size");
        if (size == 0 || size > 0x2000) size = 0x2000;
        auto data = mSession->readSpMemory(offset, size);
        std::string json = "{\"offset\":" + hexStr(offset) +
                           ",\"size\":" + std::to_string(size) +
                           ",\"hex\":" + bytesToHex(data) + "}";
        return formatResponse(id, json);
    }
    if (method == "read_sp_regs") {
        auto regs = mSession->readSpRegisters();
        if (regs.size() < 9) return formatError(id, -32000, "Cannot read SP registers");
        char buf[1024];
        snprintf(buf, sizeof(buf),
                 "{\"mem_addr\":%s,\"dram_addr\":%s,\"rd_len\":%s,\"wr_len\":%s,"
                 "\"status\":%s,\"dma_full\":%s,\"dma_busy\":%s,\"reserved\":%s,\"pc\":%s}",
                 hexStr(regs[0]).c_str(), hexStr(regs[1]).c_str(),
                 hexStr(regs[2]).c_str(), hexStr(regs[3]).c_str(),
                 hexStr(regs[4]).c_str(), hexStr(regs[5]).c_str(),
                 hexStr(regs[6]).c_str(), hexStr(regs[7]).c_str(),
                 hexStr(regs[8]).c_str());
        return formatResponse(id, buf);
    }
    if (method == "get_rsp_task") {
        auto task = mSession->readRspTaskHeader();
        if (task.empty())
            return formatError(id, -32000, "Cannot read RSP task header");
        char buf[1024];
        snprintf(buf, sizeof(buf),
                 "{\"type\":%s,\"flags\":%s,\"ucode_boot\":%s,\"ucode_boot_size\":%s,"
                 "\"ucode\":%s,\"ucode_size\":%s,\"ucode_data\":%s,\"ucode_data_size\":%s,"
                 "\"dram_stack\":%s,\"dram_stack_size\":%s,\"output_buff\":%s,\"output_buff_size\":%s,"
                 "\"data_ptr\":%s,\"data_size\":%s,\"yield_data_ptr\":%s,\"yield_data_size\":%s}",
                 hexStr(task[0]).c_str(), hexStr(task[1]).c_str(),
                 hexStr(task[2]).c_str(), hexStr(task[3]).c_str(),
                 hexStr(task[4]).c_str(), hexStr(task[5]).c_str(),
                 hexStr(task[6]).c_str(), hexStr(task[7]).c_str(),
                 hexStr(task[8]).c_str(), hexStr(task[9]).c_str(),
                 hexStr(task[10]).c_str(), hexStr(task[11]).c_str(),
                 hexStr(task[12]).c_str(), hexStr(task[13]).c_str(),
                 hexStr(task[14]).c_str(), hexStr(task[15]).c_str());
        return formatResponse(id, buf);
    }
    if (method == "trace_rsp_tasks") {
        bool enable = extractBool(paramsJson, "enable");
        mSession->enableRspTrace(enable);
        return formatResponse(id, "{\"ok\":true}");
    }

    // ── Callchain tracing ───────────────────────────────────────
    if (method == "trace_callchain") {
        // Addresses as comma-separated hex, e.g. "0x8011C450,0x80124C60"
        std::string addrsStr = extractString(paramsJson, "addresses");
        std::vector<uint32_t> addrs;
        if (!addrsStr.empty()) {
            size_t p = 0;
            while ((p = addrsStr.find("0x", p)) != std::string::npos) {
                addrs.push_back((uint32_t)strtoul(addrsStr.c_str() + p, nullptr, 16));
                p += 2;
            }
        }
        int count = mSession->enableCallchainTrace(addrs);
        if (count < 0)
            return formatError(id, -32000, "Cannot enable callchain trace");
        return formatResponse(id, "{\"ok\":true,\"bps_set\":" + std::to_string(count) + "}");
    }
    if (method == "trace_callchain_stop") {
        mSession->disableCallchainTrace();
        return formatResponse(id, "{\"ok\":true}");
    }

    // ── Scheduler tracing ──────────────────────────────────────
    if (method == "trace_scheduler") {
        uint32_t ctxAddr = extractHex(paramsJson, "ctx_switch_addr");
        uint32_t qAddr = extractHex(paramsJson, "queue_addr");
        int result = mSession->enableSchedulerTrace(ctxAddr, qAddr);
        if (result < 0)
            return formatError(id, -32000, "Cannot enable scheduler trace");
        return formatResponse(id, "{\"ok\":true}");
    }
    if (method == "trace_scheduler_stop") {
        mSession->disableSchedulerTrace();
        return formatResponse(id, "{\"ok\":true}");
    }

    // ── Struct tracking ─────────────────────────────────────────
    if (method == "track_struct") {
        uint32_t addr = extractHex(paramsJson, "address");
        uint32_t size = extractInt(paramsJson, "size");
        if (size == 0) size = 16;
        if (size > 4096) size = 4096;
        int idx = mSession->enableStructTracking(addr, size);
        if (idx < 0)
            return formatError(id, -32000, "Cannot set struct tracking BP");
        // Clear previous events to avoid noise
        mSession->clearEvents();
        return formatResponse(id, "{\"ok\":true,\"bp_index\":" + std::to_string(idx) + "}");
    }
    if (method == "track_struct_stop") {
        mSession->disableStructTracking();
        return formatResponse(id, "{\"ok\":true}");
    }

    // ── Asset scanning ─────────────────────────────────────────
    if (method == "scan_assets") {
        return handleScanAssets(id);
    }

    // ── OS detection ─────────────────────────────────────────
    if (method == "detect_os") {
        return handleDetectOs(id);
    }

    return formatError(id, -32601, "Method not found: " + method);
}

std::string JsonRpcServer::handleRequest(const std::string &requestJson) {
    std::string method = extractMethod(requestJson);
    int id = extractId(requestJson);
    std::string params = extractParams(requestJson);
    return handleMethod(method, params, id);
}

void JsonRpcServer::serverThread() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    SOCKET listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd == INVALID_SOCKET) return;

    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(mPort);

    if (bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listenFd);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    listen(listenFd, 5);
    mListenSocket = (void *)(intptr_t)listenFd;
    std::cout << "JSON-RPC listening on 127.0.0.1:" << mPort << "\n";

    while (mRunning) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        SOCKET clientFd = accept(listenFd, (struct sockaddr *)&clientAddr, &clientLen);
        if (clientFd == INVALID_SOCKET) break;

        // Read request (max 64KB)
        std::string request;
        char buf[4096];
        int n;
        while ((n = recv(clientFd, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[n] = 0;
            request += buf;
            if (request.size() > 65536) break;
            // Check for JSON-RPC delimiter — we just read until newline or timeout
            if (request.find('\n') != std::string::npos) break;
        }

        if (!request.empty()) {
            std::string response = handleRequest(request);
            send(clientFd, response.c_str(), (int)response.size(), 0);
        }

        closesocket(clientFd);
    }

    closesocket(listenFd);
#ifdef _WIN32
    WSACleanup();
#endif
    mRunning = false;
}

JsonRpcServer::JsonRpcServer(EmulatorSession *session, int port)
    : mSession(session), mPort(port), mRunning(false), mListenSocket(nullptr) {}

JsonRpcServer::~JsonRpcServer() { stop(); }

bool JsonRpcServer::start() {
    if (mRunning) return false;
    mRunning = true;
    mThread = std::thread(&JsonRpcServer::serverThread, this);
    return true;
}

void JsonRpcServer::stop() {
    mRunning = false;
    if (mListenSocket) {
#ifdef _WIN32
        closesocket((SOCKET)(intptr_t)mListenSocket);
#else
        closesocket((int)(intptr_t)mListenSocket);
#endif
        mListenSocket = nullptr;
    }
    if (mThread.joinable()) mThread.join();
}

void JsonRpcServer::wait() {
    if (mThread.joinable()) mThread.join();
}
