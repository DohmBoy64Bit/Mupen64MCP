#include "daemon.h"
#include "emulator_session.h"
#include "json_rpc_server.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

struct Config {
    std::string corePath = "libmupen64plus.dll";
    std::string romPath;
    PluginSet plugins;
    std::string dataDir = ".";
    std::string configDir = ".";
    int port = 9876;
    bool allowWriteMemory = false;
};

static Config parseArgs(int argc, char *argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--core") == 0 && i + 1 < argc)
            cfg.corePath = argv[++i];
        else if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc)
            cfg.romPath = argv[++i];
        else if (strcmp(argv[i], "--gfx") == 0 && i + 1 < argc)
            cfg.plugins.video.path = argv[++i];
        else if (strcmp(argv[i], "--audio") == 0 && i + 1 < argc)
            cfg.plugins.audio.path = argv[++i];
        else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            cfg.plugins.input.path = argv[++i];
        else if (strcmp(argv[i], "--rsp") == 0 && i + 1 < argc)
            cfg.plugins.rsp.path = argv[++i];
        else if (strcmp(argv[i], "--datadir") == 0 && i + 1 < argc)
            cfg.dataDir = argv[++i];
        else if (strcmp(argv[i], "--configdir") == 0 && i + 1 < argc)
            cfg.configDir = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            cfg.port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--allow-write-memory") == 0)
            cfg.allowWriteMemory = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            std::cout << "n64-debug-daemon v0.1.0\n"
                      << "  --core <path>        libmupen64plus path\n"
                      << "  --rom <path>         ROM file to load\n"
                      << "  --gfx <path>         Video plugin DLL\n"
                      << "  --audio <path>        Audio plugin DLL\n"
                      << "  --input <path>        Input plugin DLL\n"
                      << "  --rsp <path>          RSP plugin DLL\n"
                      << "  --datadir <dir>       Shared data directory\n"
                      << "  --configdir <dir>     Config directory\n"
                      << "  --port <n>            JSON-RPC port (default 9876)\n"
                      << "  --allow-write-memory  Enable memory writes\n"
                      << "  --help                This help\n";
            exit(0);
        }
    }
    return cfg;
}

int main(int argc, char *argv[]) {
    Config cfg = parseArgs(argc, argv);

    EmulatorSession session;
    if (!session.initCore(cfg.corePath)) {
        std::cerr << "Failed to load core: " << cfg.corePath << "\n";
        return 1;
    }
    if (!session.hasDebugger()) {
        std::cerr << "Core does not have debugger support (M64CAPS_DEBUGGER).\n";
        return 1;
    }
    if (!session.loadPlugins(cfg.plugins)) {
        std::cerr << "Failed to load plugins.\n";
        return 1;
    }
    if (!cfg.romPath.empty()) {
        if (!session.openRom(cfg.romPath)) {
            std::cerr << "Failed to open ROM: " << cfg.romPath << "\n";
            return 1;
        }
        if (!session.startEmulation()) {
            std::cerr << "Failed to start emulation.\n";
            return 1;
        }
    }

    session.setAllowMemoryWrite(cfg.allowWriteMemory);

    // Start JSON-RPC server on a separate thread
    JsonRpcServer server(&session, cfg.port);
    if (!server.start()) {
        std::cerr << "Failed to start JSON-RPC server on port " << cfg.port << "\n";
        return 1;
    }

    std::cout << "n64-debug-daemon running on port " << cfg.port << "\n";
    std::cout << "Press Ctrl+C to stop.\n";

    server.wait();
    server.stop();

    session.closeRom();
    session.shutdown();
    return 0;
}
