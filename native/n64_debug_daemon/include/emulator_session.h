#pragma once
#include "daemon.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class EmulatorSession {
public:
    EmulatorSession();
    ~EmulatorSession();

    // Core lifecycle
    bool initCore(const std::string &corePath);
    bool hasDebugger() const;
    bool isDebuggerAvailable() const;
    bool loadPlugins(const PluginSet &plugins);
    bool openRom(const std::string &romPath);
    bool startEmulation();
    void stopEmulation();
    void pause();
    void resume();
    bool stepInstruction();
    bool stepFrame();
    void closeRom();
    void shutdown();

    // ROM metadata
    m64p_rom_header getRomHeader();
    m64p_rom_settings getRomSettings();

    // CPU / debugger
    uint32_t getPC();
    uint32_t readRegister(int regIndex);
    std::vector<uint32_t> readAllRegisters();
    std::vector<uint8_t> readMemory(uint32_t address, uint32_t size);
    bool writeMemory(uint32_t address, const uint8_t *data, uint32_t size);
    uint32_t translateAddress(uint32_t vaddr);
    int getDebugState(m64p_dbg_state state);
    std::vector<uint32_t> readRspTaskHeader();
    std::vector<uint8_t> readSpMemory(uint32_t offset, uint32_t size);
    std::vector<uint32_t> readSpRegisters();

    // Breakpoints
    int addExecBreakpoint(uint32_t vaddr);
    int addMemoryBreakpoint(uint32_t vaddr, uint32_t size, unsigned int flags);
    bool removeBreakpoint(int index);
    std::vector<BreakpointInfo> listBreakpoints();

    // Tracing
    void enableRomReadTrace(bool enabled);
    void enableRspTrace(bool enabled);
    void enablePiDmaTrace(bool enabled);
    std::vector<TraceEvent> getRecentEvents(uint32_t count);
    std::vector<TraceEvent> getAllEvents();
    void clearEvents();
    void markGameState(const std::string &label, const std::string &notes);
    std::string getCurrentStateLabel() const;
    
    // PI DMA
    struct PiDmaRegs { uint32_t dramAddr; uint32_t cartAddr; uint32_t rdLen; uint32_t wrLen; uint32_t status; };
    PiDmaRegs readPiDmaRegs();

    // Safety
    void setAllowMemoryWrite(bool allow) { mAllowMemoryWrite = allow; }
    bool isMemoryWriteAllowed() const { return mAllowMemoryWrite; }

    // Config
    void setConfigDir(const std::string &dir) { mConfigDir = dir; }
    void setDataDir(const std::string &dir) { mDataDir = dir; }

    // State queries
    bool isRomLoaded() const { return mRomLoaded; }
    bool isEmuRunning() const { return mEmuRunning; }
    bool isPaused() const { return mEmuPaused; }
    uint64_t frameCount() const { return mFrameCounter; }
    CoreAPI &api() { return mAPI; }

    // Callback trampoline target (static pointer)
    static EmulatorSession *sInstance;
    void onCoreLog(int level, const char *msg);
    void onCoreStateChange(m64p_core_param param, int value);
    void onDebuggerUpdate(unsigned int pc);
    void onDebuggerVi();

private:
    // Core library
    void *mCoreHandle;
    bool mCoreLoaded;
    bool mDebuggerAvailable;

    // Plugins
    PluginSet mPlugins;
    PluginSet mPluginPaths;

    // ROM
    std::string mRomPath;
    bool mRomLoaded;

    // Emulation state
    std::thread mEmulatorThread;
    std::atomic<bool> mEmuRunning;
    std::atomic<bool> mEmuPaused;
    std::atomic<uint64_t> mFrameCounter;

    // API function pointers
    CoreAPI mAPI;

    // Safety
    bool mAllowMemoryWrite;

    // Config paths
    std::string mConfigDir;
    std::string mDataDir;

    // Trace events and state labels
    std::mutex mEventMutex;
    std::vector<TraceEvent> mEvents;
    std::vector<GameStateLabel> mGameStateLabels;
    std::string mCurrentStateLabel;
    std::string mLastLog;

    // Trace flags
    bool mTraceRomRead = false;
    bool mTraceRsp = false;
    bool mTracePiDma = false;
    uint32_t mLastPiStatus = 0;

    // Stepping: temporarily removed BP tracking
    uint32_t mSteppingBpAddr = 0;
    unsigned int mSteppingBpFlags = 0;
};
