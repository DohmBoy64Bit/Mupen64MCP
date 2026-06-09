#pragma once
#include "emulator_session.h"
#include <atomic>
#include <string>
#include <thread>

class JsonRpcServer {
public:
    JsonRpcServer(EmulatorSession *session, int port);
    ~JsonRpcServer();

    bool start();
    void stop();
    void wait();

private:
    void serverThread();
    std::string handleRequest(const std::string &requestJson);
    std::string handleMethod(const std::string &method, const std::string &paramsJson, int id);
    std::string handleScanAssets(int id);
    std::string handleDetectOs(int id);

    EmulatorSession *mSession;
    int mPort;
    std::thread mThread;
    std::atomic<bool> mRunning;

    // Socket (platform-specific)
    void *mListenSocket; // SOCKET on Windows, int on Linux
};
