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
static uint32_t extractHex(const std::string &json, const std::string &key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos != std::string::npos) {
        pos += search.size();
        char buf[20];
        int i = 0;
        while (pos < json.size() && json[pos] != '"' && i < 19) {
            buf[i++] = json[pos++];
        }
        buf[i] = 0;
        return (uint32_t)strtoul(buf, nullptr, 16);
    }
    // try decimal
    search = "\"" + key + "\":";
    pos = json.find(search);
    if (pos != std::string::npos) {
        pos += search.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        return (uint32_t)strtoul(json.c_str() + pos, nullptr, 10);
    }
    return 0;
}

static int extractInt(const std::string &json, const std::string &key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return atoi(json.c_str() + pos);
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
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"address\":%s,\"size\":%u,\"hex\":%s}",
                 hexStr(addr).c_str(), size, bytesToHex(data).c_str());
        return formatResponse(id, buf);
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
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"address\":\"0x80000000\",\"size\":%u,\"hex\":%s}",
                 (uint32_t)data.size(), bytesToHex(data, 4096).c_str());
        return formatResponse(id, buf);
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
