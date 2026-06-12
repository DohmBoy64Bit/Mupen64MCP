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

    // ROM offset <-> RDRAM address translation
    // For N64: boot code loaded from file offset 0x1000 to RDRAM KSEG0 0x80100000
    // Lower offsets (0x0000-0x0FFF) are IPL3 ROM mirror at KSEG0 0x80000000
    static uint32_t fileOffsetToRdram(uint32_t fileOffset);
    static uint32_t rdramToFileOffset(uint32_t rdramAddr);
    static uint32_t rdramToKseg1(uint32_t rdramAddr);

    // Function scanner
    struct FuncEntry { uint32_t address; uint32_t stackSize; uint32_t approxSize; };
    std::vector<FuncEntry> scanFunctions(uint32_t startAddr, uint32_t endAddr);

    // RSP health check
    struct RspHealth {
        bool rspHle;           // true if RSP-HLE plugin is used
        uint32_t spPc;
        uint32_t spStatus;
        uint32_t spDmaBusy;
        uint32_t ucodeHash;   // CRC32 of first 256 bytes of IMEM
        std::string ucodeType; // "f3dex2", "f3dex", "custom", "audio", "unknown"
        bool taskActive;
        uint32_t taskType;
    };
    RspHealth checkRspHealth();

    // Tracing
    void enableRomReadTrace(bool enabled);
    void enableRspTrace(bool enabled);
    void enablePiDmaTrace(bool enabled);
    std::vector<TraceEvent> getRecentEvents(uint32_t count);
    std::vector<TraceEvent> getAllEvents();
    void clearEvents();
    void markGameState(const std::string &label, const std::string &notes);
    std::string getCurrentStateLabel() const;
    
    // Struct tracking
    int enableStructTracking(uint32_t addr, uint32_t size);
    void disableStructTracking();

    // Callchain tracing
    int enableCallchainTrace(const std::vector<uint32_t> &addresses);
    void disableCallchainTrace();
    int enableSchedulerTrace(uint32_t ctxSwitchAddr, uint32_t queueAddr);
    void disableSchedulerTrace();

    // PI DMA
    struct PiDmaRegs { uint32_t dramAddr; uint32_t cartAddr; uint32_t rdLen; uint32_t wrLen; uint32_t status; };
    PiDmaRegs readPiDmaRegs();

    // Framebuffer
    struct ViRegs { uint32_t status; uint32_t origin; uint32_t width; uint32_t vIntr; uint32_t vCurrentLine; };
    ViRegs readViRegs();
    std::vector<uint8_t> readFramebuffer(uint32_t &outWidth, uint32_t &outHeight, int &outBpp);

    // Frame auto-capture
    void setFrameCaptureInterval(uint64_t interval) { mFrameCaptureInterval = interval; }
    uint64_t getFrameCaptureInterval() const { return mFrameCaptureInterval; }
    struct FrameCapture { uint64_t frame; uint32_t width; uint32_t height; int bpp; std::vector<uint8_t> pixels; };
    std::vector<FrameCapture> getFrameCaptures() const;
    void clearFrameCaptures();

    // Safety
    void setAllowMemoryWrite(bool allow) { mAllowMemoryWrite = allow; }
    bool isMemoryWriteAllowed() const { return mAllowMemoryWrite; }

    // Input injection
    void setControllerState(int channel, unsigned int buttons, signed char x, signed char y, int sticky);

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

    // Frame auto-capture
    std::atomic<uint64_t> mFrameCaptureInterval{0};
    mutable std::mutex mFrameCaptureMutex;
    std::vector<FrameCapture> mFrameCaptures;

    // Trace flags
    bool mTraceRomRead = false;
    bool mTraceRsp = false;
    bool mTracePiDma = false;
    uint32_t mLastPiStatus = 0;

    // BP tracking — own our BPs for reliable listing/removal
    struct TrackedBp { int index; uint32_t address; uint32_t endAddress; unsigned int flags; };
    std::vector<TrackedBp> mOwnedBps;

    // Stepping: temporarily removed BP tracking
    uint32_t mSteppingBpAddr = 0;
    unsigned int mSteppingBpFlags = 0;

    // Struct tracking
    bool mStructTrackEnabled = false;
    uint32_t mStructTrackAddr = 0;
    uint32_t mStructTrackSize = 0;
    std::vector<uint8_t> mStructTrackPrevData;
    int mStructTrackBpIndex = -1;

    // Callchain tracing
    bool mCallchainEnabled = false;
    std::vector<uint32_t> mCallchainAddrs;
    std::vector<int> mCallchainBpIndices;

    // Scheduler tracing
    bool mSchedTraceEnabled = false;
    uint32_t mSchedCtxSwitchAddr = 0;
    uint32_t mSchedQueueAddr = 0;
    int mSchedQueueBpIdx = -1;
    int mSchedCtxSwitchBpIdx = -1;
    std::vector<uint8_t> mSchedPrevQueueData;
};
