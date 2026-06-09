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
                   ",\"state_label\":\"" + jsonEscape(events[i].stateLabel) + "\"}";
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

    // ── Asset scanning ─────────────────────────────────────────
    if (method == "scan_assets") {
        return handleScanAssets(id);
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
