#include "emulator_session.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// Helper: load a dynamic library
static void *loadLibrary(const std::string &path) {
#ifdef _WIN32
    HMODULE mod = LoadLibraryA(path.c_str());
    return (void *)mod;
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

static void *getSymbol(void *handle, const std::string &name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)handle, name.c_str());
#else
    return dlsym(handle, name.c_str());
#endif
}

static void closeLibrary(void *handle) {
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

// --- Static callback trampolines ---
static void sDebugCallback(void *context, int level, const char *msg) {
    if (context) {
        static_cast<EmulatorSession *>(context)->onCoreLog(level, msg);
    }
}

static void sStateCallback(void *context, m64p_core_param param, int value) {
    if (context) {
        static_cast<EmulatorSession *>(context)->onCoreStateChange(param, value);
    }
}

// Debugger callbacks (registered via DebugSetCallbacks)
static void sDbgInit(void) {
    // Debugger initialized — signal ready
}

static void sDbgUpdate(unsigned int pc) {
    EmulatorSession *s = EmulatorSession::sInstance;
    if (s) s->onDebuggerUpdate(pc);
}

static void sDbgVi(void) {
    EmulatorSession *s = EmulatorSession::sInstance;
    if (s) s->onDebuggerVi();
}

// Static instance pointer for callback trampolines
EmulatorSession *EmulatorSession::sInstance = nullptr;

EmulatorSession::EmulatorSession()
    : mCoreHandle(nullptr), mCoreLoaded(false), mDebuggerAvailable(false),
      mRomLoaded(false), mEmuRunning(false), mEmuPaused(false),
      mFrameCounter(0), mAllowMemoryWrite(false) {
    std::memset(&mAPI, 0, sizeof(mAPI));
    sInstance = this;
}

EmulatorSession::~EmulatorSession() {
    shutdown();
    sInstance = nullptr;
}

bool EmulatorSession::initCore(const std::string &corePath) {
    mCoreHandle = loadLibrary(corePath);
    if (!mCoreHandle) {
        std::cerr << "Failed to load core library: " << corePath << "\n";
        return false;
    }

    // Resolve all function pointers from the core library
#define RESOLVE(name) \
    do { \
        auto ptr = getSymbol(mCoreHandle, "Core" name); \
        if (!ptr) ptr = getSymbol(mCoreHandle, name); \
        if (!ptr) { \
            std::cerr << "Missing symbol: " name "\n"; \
            closeLibrary(mCoreHandle); \
            mCoreHandle = nullptr; \
            return false; \
        } \
        std::memcpy(&mAPI.Core##name, &ptr, sizeof(ptr)); \
    } while (0)

    // Resolve the standard exports
    // We need to use the exact export names from m64p_frontend.h / m64p_debugger.h
    auto resolvePtr = [&](const char *name) -> void * {
        return getSymbol(mCoreHandle, name);
    };

    // Core API functions
    *(void **)&mAPI.PluginGetVersion = resolvePtr("PluginGetVersion");
    *(void **)&mAPI.CoreStartup = resolvePtr("CoreStartup");
    *(void **)&mAPI.CoreShutdown = resolvePtr("CoreShutdown");
    *(void **)&mAPI.CoreAttachPlugin = resolvePtr("CoreAttachPlugin");
    *(void **)&mAPI.CoreDetachPlugin = resolvePtr("CoreDetachPlugin");
    *(void **)&mAPI.CoreDoCommand = resolvePtr("CoreDoCommand");
    *(void **)&mAPI.CoreGetAPIVersions = resolvePtr("CoreGetAPIVersions");
    *(void **)&mAPI.CoreErrorMessage = resolvePtr("CoreErrorMessage");
    *(void **)&mAPI.CoreGetRomSettings = resolvePtr("CoreGetRomSettings");
    *(void **)&mAPI.ConfigGetSectionParamInt = resolvePtr("ConfigGetSectionParamInt");
    *(void **)&mAPI.ConfigOpenSection = resolvePtr("ConfigOpenSection");
    *(void **)&mAPI.ConfigSetParameter = resolvePtr("ConfigSetParameter");
    *(void **)&mAPI.ConfigGetParamBool = resolvePtr("ConfigGetParamBool");

    // Debugger API functions
    *(void **)&mAPI.DebugSetCallbacks = resolvePtr("DebugSetCallbacks");
    *(void **)&mAPI.DebugSetRunState = resolvePtr("DebugSetRunState");
    *(void **)&mAPI.DebugGetState = resolvePtr("DebugGetState");
    *(void **)&mAPI.DebugStep = resolvePtr("DebugStep");
    *(void **)&mAPI.DebugDecodeOp = resolvePtr("DebugDecodeOp");
    *(void **)&mAPI.DebugMemGetPointer = resolvePtr("DebugMemGetPointer");
    *(void **)&mAPI.DebugMemRead8 = resolvePtr("DebugMemRead8");
    *(void **)&mAPI.DebugMemRead16 = resolvePtr("DebugMemRead16");
    *(void **)&mAPI.DebugMemRead32 = resolvePtr("DebugMemRead32");
    *(void **)&mAPI.DebugMemRead64 = resolvePtr("DebugMemRead64");
    *(void **)&mAPI.DebugMemWrite8 = resolvePtr("DebugMemWrite8");
    *(void **)&mAPI.DebugMemWrite16 = resolvePtr("DebugMemWrite16");
    *(void **)&mAPI.DebugMemWrite32 = resolvePtr("DebugMemWrite32");
    *(void **)&mAPI.DebugMemWrite64 = resolvePtr("DebugMemWrite64");
    *(void **)&mAPI.DebugGetCPUDataPtr = resolvePtr("DebugGetCPUDataPtr");
    *(void **)&mAPI.DebugBreakpointLookup = resolvePtr("DebugBreakpointLookup");
    *(void **)&mAPI.DebugBreakpointCommand = resolvePtr("DebugBreakpointCommand");
    *(void **)&mAPI.DebugBreakpointTriggeredBy = resolvePtr("DebugBreakpointTriggeredBy");
    *(void **)&mAPI.DebugVirtualToPhysical = resolvePtr("DebugVirtualToPhysical");
    *(void **)&mAPI.DebugMemGetMemInfo = resolvePtr("DebugMemGetMemInfo");

    if (!mAPI.PluginGetVersion || !mAPI.CoreStartup || !mAPI.CoreShutdown ||
        !mAPI.CoreDoCommand) {
        std::cerr << "Core library missing required exports.\n";
        closeLibrary(mCoreHandle);
        mCoreHandle = nullptr;
        return false;
    }

    // Check for debugger capability
    m64p_plugin_type pluginType;
    int pluginVersion;
    const char *pluginName;
    int capabilities = 0;
    m64p_error rval = mAPI.PluginGetVersion(&pluginType, &pluginVersion,
                                             &capabilities, &pluginName, nullptr);
    if (rval == M64ERR_SUCCESS) {
        if (capabilities & M64CAPS_DEBUGGER) {
            mDebuggerAvailable = true;
        }
    }

    // Call CoreStartup
    rval = mAPI.CoreStartup(0x020100, // Front-end API version
                             mConfigDir.empty() ? nullptr : mConfigDir.c_str(),
                             mDataDir.empty() ? nullptr : mDataDir.c_str(),
                             (void *)this, sDebugCallback,
                             (void *)this, sStateCallback);
    if (rval != M64ERR_SUCCESS) {
        const char *err = mAPI.CoreErrorMessage ? mAPI.CoreErrorMessage(rval) : "unknown";
        std::cerr << "CoreStartup failed: " << err << "\n";
        closeLibrary(mCoreHandle);
        mCoreHandle = nullptr;
        return false;
    }

    mCoreLoaded = true;
    return true;
}

bool EmulatorSession::hasDebugger() const {
    return mDebuggerAvailable;
}

bool EmulatorSession::isDebuggerAvailable() const {
    return mDebuggerAvailable && mAPI.DebugSetCallbacks != nullptr;
}

bool EmulatorSession::loadPlugins(const PluginSet &plugins) {
    auto loadOne = [&](const std::string &path, PluginLib &out) -> bool {
        if (path.empty() || path == "dummy") return true; // dummy plugin — core handles it
        out.handle = loadLibrary(path);
        if (!out.handle) {
            std::cerr << "Failed to load plugin: " << path << "\n";
            return false;
        }
        out.startup = (PluginStartupFn)getSymbol(out.handle, "PluginStartup");
        out.shutdown = (PluginShutdownFn)getSymbol(out.handle, "PluginShutdown");
        out.getVersion = (PluginGetVersionFn)getSymbol(out.handle, "PluginGetVersion");
        if (!out.startup || !out.shutdown) {
            std::cerr << "Plugin missing startup/shutdown: " << path << "\n";
            closeLibrary(out.handle);
            out.handle = nullptr;
            return false;
        }
        out.path = path;
        return true;
    };

    if (!loadOne(plugins.video.path, mPlugins.video)) return false;
    if (!loadOne(plugins.audio.path, mPlugins.audio)) return false;
    if (!loadOne(plugins.input.path, mPlugins.input)) return false;
    if (!loadOne(plugins.rsp.path, mPlugins.rsp)) return false;
    mPluginPaths = plugins;
    return true;
}

bool EmulatorSession::openRom(const std::string &romPath) {
    if (!mCoreLoaded) return false;

    // Read ROM file
    std::ifstream file(romPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size)) return false;
    file.close();

    // Call PluginStartup on each loaded plugin (skip dummy/no-handle)
    auto startPlugin = [&](const PluginLib &p, m64p_plugin_type type) -> bool {
        if (!p.startup) return true;
        m64p_error r = p.startup(mCoreHandle, type, sDebugCallback);
        if (r != M64ERR_SUCCESS) {
            std::cerr << "PluginStartup failed for type " << type << "\n";
            return false;
        }
        return true;
    };
    if (!startPlugin(mPlugins.video, M64PLUGIN_GFX)) return false;
    if (!startPlugin(mPlugins.audio, M64PLUGIN_AUDIO)) return false;
    if (!startPlugin(mPlugins.input, M64PLUGIN_INPUT)) return false;
    if (!startPlugin(mPlugins.rsp, M64PLUGIN_RSP)) return false;

    // Open ROM
    m64p_error rval = mAPI.CoreDoCommand(M64CMD_ROM_OPEN, (int)size, buffer.data());
    if (rval != M64ERR_SUCCESS) {
        const char *err = mAPI.CoreErrorMessage ? mAPI.CoreErrorMessage(rval) : "unknown";
        std::cerr << "ROM_OPEN failed: " << err << "\n";
        return false;
    }

    // Attach plugins in required order: Video → Audio → Input → RSP
    // Pass nullptr handle for dummy plugins; core provides built-in dummy
    auto attach = [&](const PluginLib &p, m64p_plugin_type type) -> bool {
        m64p_dynlib_handle h = (m64p_dynlib_handle)p.handle;
        m64p_error r = mAPI.CoreAttachPlugin(type, h);
        if (r != M64ERR_SUCCESS) {
            std::cerr << "CoreAttachPlugin failed for type " << type << "\n";
            return false;
        }
        return true;
    };
    if (!attach(mPlugins.video, M64PLUGIN_GFX)) return false;
    if (!attach(mPlugins.audio, M64PLUGIN_AUDIO)) return false;
    if (!attach(mPlugins.input, M64PLUGIN_INPUT)) return false;
    if (!attach(mPlugins.rsp, M64PLUGIN_RSP)) return false;

    // Set debugger callbacks if available
    if (isDebuggerAvailable()) {
        mAPI.DebugSetCallbacks(sDbgInit, sDbgUpdate, sDbgVi);
    }

    mRomPath = romPath;
    mRomLoaded = true;
    return true;
}

bool EmulatorSession::startEmulation() {
    if (!mRomLoaded || mEmuRunning) return false;

    mEmuRunning = true;
    mEmuPaused = false;
    mFrameCounter = 0;

    // Enable the debugger and force pure interpreter mode in core config before M64CMD_EXECUTE
    if (mAPI.ConfigOpenSection && mAPI.ConfigSetParameter) {
        m64p_handle coreCfg = nullptr;
        if (mAPI.ConfigOpenSection("Core", &coreCfg) == M64ERR_SUCCESS && coreCfg) {
            int trueVal = 1;
            mAPI.ConfigSetParameter(coreCfg, "EnableDebugger", M64TYPE_BOOL, &trueVal);
            int zeroVal = 0;
            mAPI.ConfigSetParameter(coreCfg, "R4300Emulator", M64TYPE_INT, &zeroVal);
        }
    }

    // M64CMD_EXECUTE blocks — run on emulator thread
    mEmulatorThread = std::thread([this]() {
        mAPI.CoreDoCommand(M64CMD_EXECUTE, 0, nullptr);
        mEmuRunning = false;
    });

    // Wait briefly, then pause so debugger can inspect initial state
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pause();

    return true;
}

void EmulatorSession::stopEmulation() {
    if (!mEmuRunning) return;
    mAPI.CoreDoCommand(M64CMD_STOP, 0, nullptr);
    if (mEmulatorThread.joinable()) {
        mEmulatorThread.join();
    }
    mEmuRunning = false;
}

void EmulatorSession::pause() {
    if (!mEmuRunning || mEmuPaused) return;
    mAPI.CoreDoCommand(M64CMD_PAUSE, 0, nullptr);
    if (mAPI.DebugSetRunState)
        mAPI.DebugSetRunState(M64P_DBG_RUNSTATE_PAUSED);
    mEmuPaused = true;
}

void EmulatorSession::resume() {
    if (!mEmuRunning || !mEmuPaused) return;
    if (mAPI.DebugSetRunState)
        mAPI.DebugSetRunState(M64P_DBG_RUNSTATE_RUNNING);
    mAPI.CoreDoCommand(M64CMD_RESUME, 0, nullptr);
    mEmuPaused = false;
    // Post semaphore to unblock emulator thread
    if (mAPI.DebugStep)
        mAPI.DebugStep();
}

bool EmulatorSession::stepInstruction() {
    if (!mEmuRunning || !mEmuPaused || !isDebuggerAvailable())
        return false;
    mAPI.DebugSetRunState(M64P_DBG_RUNSTATE_STEPPING);
    m64p_error r = mAPI.DebugStep();
    return r == M64ERR_SUCCESS;
}

bool EmulatorSession::stepFrame() {
    if (!mEmuRunning) return false;
    m64p_error r = mAPI.CoreDoCommand(M64CMD_ADVANCE_FRAME, 0, nullptr);
    return r == M64ERR_SUCCESS;
}

void EmulatorSession::closeRom() {
    stopEmulation();
    if (!mRomLoaded) return;

    // Detach plugins
    auto detach = [&](m64p_plugin_type type) {
        mAPI.CoreDetachPlugin(type);
    };
    detach(M64PLUGIN_RSP);
    detach(M64PLUGIN_INPUT);
    detach(M64PLUGIN_AUDIO);
    detach(M64PLUGIN_GFX);

    // Shutdown plugin libraries (only if handle was loaded)
    auto shutdownPlugin = [&](PluginLib &p) {
        if (p.handle) {
            if (p.shutdown) p.shutdown();
            closeLibrary(p.handle);
        }
        p.handle = nullptr;
        p.startup = nullptr;
        p.shutdown = nullptr;
        p.getVersion = nullptr;
    };
    shutdownPlugin(mPlugins.rsp);
    shutdownPlugin(mPlugins.input);
    shutdownPlugin(mPlugins.audio);
    shutdownPlugin(mPlugins.video);

    mAPI.CoreDoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
    mRomLoaded = false;
}

void EmulatorSession::shutdown() {
    closeRom();
    if (mCoreLoaded) {
        mAPI.CoreShutdown();
        mCoreLoaded = false;
    }
    if (mCoreHandle) {
        closeLibrary(mCoreHandle);
        mCoreHandle = nullptr;
    }
}

// --- ROM metadata ---
m64p_rom_header EmulatorSession::getRomHeader() {
    m64p_rom_header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    if (mRomLoaded) {
        mAPI.CoreDoCommand(M64CMD_ROM_GET_HEADER, sizeof(hdr), &hdr);
    }
    return hdr;
}

m64p_rom_settings EmulatorSession::getRomSettings() {
    m64p_rom_settings s;
    std::memset(&s, 0, sizeof(s));
    if (mRomLoaded) {
        mAPI.CoreDoCommand(M64CMD_ROM_GET_SETTINGS, sizeof(s), &s);
    }
    return s;
}

// --- Debugger wrappers ---
uint32_t EmulatorSession::getPC() {
    if (!isDebuggerAvailable() || !mEmuRunning) return 0;
    void *pcPtr = mAPI.DebugGetCPUDataPtr(M64P_CPU_PC);
    if (pcPtr) {
        return *(uint32_t *)pcPtr;
    }
    return 0;
}

uint32_t EmulatorSession::readRegister(int regIndex) {
    if (!isDebuggerAvailable() || !mEmuRunning) return 0;
    void *regPtr = mAPI.DebugGetCPUDataPtr(M64P_CPU_REG_REG);
    if (regPtr) {
        return ((uint32_t *)regPtr)[regIndex];
    }
    return 0;
}

std::vector<uint32_t> EmulatorSession::readAllRegisters() {
    std::vector<uint32_t> regs(32, 0);
    if (!isDebuggerAvailable() || !mEmuRunning) return regs;
    void *regPtr = mAPI.DebugGetCPUDataPtr(M64P_CPU_REG_REG);
    if (regPtr) {
        std::memcpy(regs.data(), regPtr, 32 * sizeof(uint32_t));
    }
    return regs;
}

std::vector<uint8_t> EmulatorSession::readMemory(uint32_t address, uint32_t size) {
    std::vector<uint8_t> result;
    if (!isDebuggerAvailable() || !mEmuRunning) return result;
    result.resize(size);
    for (uint32_t i = 0; i < size; i++) {
        result[i] = mAPI.DebugMemRead8(address + i);
    }
    return result;
}

bool EmulatorSession::writeMemory(uint32_t address, const uint8_t *data, uint32_t size) {
    if (!mAllowMemoryWrite) return false;
    if (!isDebuggerAvailable() || !mEmuRunning) return false;
    for (uint32_t i = 0; i < size; i++) {
        mAPI.DebugMemWrite8(address + i, data[i]);
    }
    return true;
}

uint32_t EmulatorSession::translateAddress(uint32_t vaddr) {
    if (!isDebuggerAvailable()) return vaddr;
    return mAPI.DebugVirtualToPhysical(vaddr);
}

int EmulatorSession::getDebugState(m64p_dbg_state state) {
    if (!isDebuggerAvailable() || !mAPI.DebugGetState) return -1;
    return mAPI.DebugGetState(state);
}

// --- Breakpoints ---
int EmulatorSession::addExecBreakpoint(uint32_t vaddr) {
    if (!isDebuggerAvailable())
        return -1;

    m64p_breakpoint bp;
    bp.address = vaddr;
    bp.endaddr = vaddr;
    bp.flags = M64P_BKP_FLAG_EXEC | M64P_BKP_FLAG_ENABLED;
    int r = mAPI.DebugBreakpointCommand(M64P_BKP_CMD_ADD_STRUCT, 0, &bp);
    if (r == 0) {
        int index = mAPI.DebugBreakpointLookup(vaddr, 4, M64P_BKP_FLAG_EXEC | M64P_BKP_FLAG_ENABLED);
        int numBps = mAPI.DebugGetState(M64P_DBG_NUM_BREAKPOINTS);
        if (index >= 0) return index;
        if (numBps > 0) return numBps - 1;
        return 0;
    }
    return -1;
}

int EmulatorSession::addMemoryBreakpoint(uint32_t vaddr, uint32_t size, unsigned int flags) {
    if (!isDebuggerAvailable()) return -1;
    uint32_t paddr = translateAddress(vaddr);
    m64p_breakpoint bp;
    bp.address = paddr;
    bp.endaddr = paddr + size - 1;
    bp.flags = flags | M64P_BKP_FLAG_ENABLED;
    int r = mAPI.DebugBreakpointCommand(M64P_BKP_CMD_ADD_STRUCT, 0, &bp);
    if (r == 0) {
        int index = mAPI.DebugBreakpointLookup(paddr, size, flags | M64P_BKP_FLAG_ENABLED);
        if (index >= 0) return index;
        int numBps = mAPI.DebugGetState(M64P_DBG_NUM_BREAKPOINTS);
        return (numBps > 0) ? numBps - 1 : 0;
    }
    return -1;
}

bool EmulatorSession::removeBreakpoint(int index) {
    if (!isDebuggerAvailable()) return false;
    int r = mAPI.DebugBreakpointCommand(M64P_BKP_CMD_REMOVE_IDX, index, nullptr);
    return r == 0;
}

std::vector<BreakpointInfo> EmulatorSession::listBreakpoints() {
    std::vector<BreakpointInfo> result;
    if (!isDebuggerAvailable()) return result;
    int numBps = mAPI.DebugGetState(M64P_DBG_NUM_BREAKPOINTS);
    for (int i = 0; i < numBps; i++) {
        BreakpointInfo info;
        info.index = i;
        info.enabled = true;
        result.push_back(info);
    }
    return result;
}

// --- Tracing ---
void EmulatorSession::enableRomReadTrace(bool enabled) {
    mTraceRomRead = enabled;
}

void EmulatorSession::enableRspTrace(bool enabled) {
    mTraceRsp = enabled;
}

void EmulatorSession::enableAudioTrace(bool enabled) {
    mTraceAudio = enabled;
}

std::vector<TraceEvent> EmulatorSession::getRecentEvents(uint32_t count) {
    std::lock_guard<std::mutex> lock(mEventMutex);
    if (count == 0 || count >= mEvents.size()) return mEvents;
    return std::vector<TraceEvent>(mEvents.end() - count, mEvents.end());
}

std::vector<TraceEvent> EmulatorSession::getAllEvents() {
    std::lock_guard<std::mutex> lock(mEventMutex);
    return mEvents;
}

void EmulatorSession::clearEvents() {
    std::lock_guard<std::mutex> lock(mEventMutex);
    mEvents.clear();
}

void EmulatorSession::markGameState(const std::string &label, const std::string &notes) {
    std::lock_guard<std::mutex> lock(mEventMutex);
    GameStateLabel gs;
    gs.frame = mFrameCounter;
    gs.label = label;
    gs.notes = notes;
    mGameStateLabels.push_back(gs);
    // Tag subsequent events with this label
    mCurrentStateLabel = label;
}

std::string EmulatorSession::getCurrentStateLabel() const {
    return mCurrentStateLabel;
}

// --- Callbacks (called from emulator thread) ---
void EmulatorSession::onCoreLog(int level, const char *msg) {
    // Store last N log messages for debugging
    mLastLog = msg;
}

void EmulatorSession::onCoreStateChange(m64p_core_param param, int value) {
    if (param == M64CORE_EMU_STATE) {
        if (value == M64EMU_STOPPED) {
            mEmuRunning = false;
            mEmuPaused = false;
        } else if (value == M64EMU_PAUSED) {
            mEmuPaused = true;
            // mEmuRunning stays true — we are paused, not stopped
        } else if (value == M64EMU_RUNNING) {
            mEmuRunning = true;
            mEmuPaused = false;
        }
    }
}

void EmulatorSession::onDebuggerUpdate(unsigned int pc) {
    // Check if core debugger is paused (breakpoint hit)
    if (mAPI.DebugGetState && mAPI.DebugGetState(M64P_DBG_RUN_STATE) == M64P_DBG_RUNSTATE_PAUSED) {
        mEmuPaused = true;
    }
    std::lock_guard<std::mutex> lock(mEventMutex);
    TraceEvent ev;
    ev.frame = mFrameCounter;
    ev.type = "step";
    ev.pc = pc;
    ev.stateLabel = mCurrentStateLabel;
    mEvents.push_back(ev);
    if (mEvents.size() > 100000) {
        mEvents.erase(mEvents.begin(), mEvents.begin() + 50000);
    }
}

void EmulatorSession::onDebuggerVi() {
    mFrameCounter++;
}
