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

    // Configure video plugin for framebuffer writeback (required for read_framebuffer)
    if (mAPI.ConfigOpenSection && mAPI.ConfigSetParameter) {
        m64p_handle videoCfg = nullptr;
        if (mAPI.ConfigOpenSection("Video-Rice", &videoCfg) == M64ERR_SUCCESS && videoCfg) {
            int fbSetting = 3; // FRM_BUF_WRITEBACK
            mAPI.ConfigSetParameter(videoCfg, "FrameBufferSetting", M64TYPE_INT, &fbSetting);
            std::cerr << "Video-Rice FrameBufferSetting set to " << fbSetting << "\n";
        }
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
        std::cerr << "Loading plugin: " << path << "\n";
        out.handle = loadLibrary(path);
        if (!out.handle) {
            std::cerr << "LoadLibrary failed for: " << path << "\n";
            return false;
        }
        std::cerr << "  handle=" << out.handle << "\n";
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

    // Open ROM
    m64p_error rval = mAPI.CoreDoCommand(M64CMD_ROM_OPEN, (int)size, buffer.data());
    if (rval != M64ERR_SUCCESS) {
        const char *err = mAPI.CoreErrorMessage ? mAPI.CoreErrorMessage(rval) : "unknown";
        std::cerr << "ROM_OPEN failed: " << err << "\n";
        return false;
    }

    // Start plugins before attaching (required for real plugins to fetch core API)
    auto startPlugin = [&](const PluginLib &p) -> bool {
        if (p.handle && p.startup) {
            m64p_error r = p.startup((m64p_dynlib_handle)mCoreHandle, (void*)this, sDebugCallback);
            std::cerr << "  PluginStartup result=" << r << "\n";
            if (r != M64ERR_SUCCESS) {
                std::cerr << "PluginStartup failed for " << p.path << "\n";
                return false;
            }
        }
        return true;
    };
    if (!startPlugin(mPlugins.video)) return false;
    if (!startPlugin(mPlugins.audio)) return false;
    if (!startPlugin(mPlugins.input)) return false;
    if (!startPlugin(mPlugins.rsp)) return false;

    // Attach plugins in required order: Video → Audio → Input → RSP
    // Pass nullptr handle for dummy plugins; core provides built-in dummy
    // Attach RSP LAST — core requires GFX/AUDIO/INPUT to be attached first
    // (plugin_start_rsp reads function pointers from gfx/audio structs)
    auto attach = [&](const PluginLib &p, m64p_plugin_type type) -> bool {
        m64p_dynlib_handle h = (m64p_dynlib_handle)p.handle;
        std::cerr << "CoreAttachPlugin type=" << type << " handle=" << h << "\n";
        m64p_error r = M64ERR_INTERNAL;
        r = mAPI.CoreAttachPlugin(type, h);
        std::cerr << "  result=" << r << "\n";
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
    std::cerr << "All plugins attached, setting debugger callbacks\n";

    // Resolve SetControllerState from input plugin for injection
    if (mPlugins.input.handle) {
        mPlugins.input.setControllerState = (SetControllerStateFn)
            getSymbol(mPlugins.input.handle, "SetControllerState");
        std::cerr << "  input inject fn=" << (void*)mPlugins.input.setControllerState << "\n";
    }

    // Set debugger callbacks if available
    if (isDebuggerAvailable()) {
        mAPI.DebugSetCallbacks(sDbgInit, sDbgUpdate, sDbgVi);
        std::cerr << "Debugger callbacks set\n";
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

void EmulatorSession::setControllerState(int channel, unsigned int buttons, signed char x, signed char y, int sticky) {
    if (!mPlugins.input.setControllerState) {
        std::cerr << "setControllerState: input plugin has no SetControllerState export\n";
        return;
    }
    mPlugins.input.setControllerState(channel, buttons, x, y, sticky);
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

    uint32_t pc = getPC();

    // Temporarily remove any exec BP at the current PC so update_debugger
    // doesn't re-catch it before the instruction can execute.
    int bpIdx = -1;
    if (isDebuggerAvailable() && pc != 0) {
        bpIdx = mAPI.DebugBreakpointLookup(pc, 4, M64P_BKP_FLAG_EXEC | M64P_BKP_FLAG_ENABLED);
        if (bpIdx >= 0)
            mAPI.DebugBreakpointCommand(M64P_BKP_CMD_REMOVE_IDX, bpIdx, nullptr);
    }

    if (mAPI.DebugSetRunState)
        mAPI.DebugSetRunState(M64P_DBG_RUNSTATE_RUNNING);
    mAPI.CoreDoCommand(M64CMD_RESUME, 0, nullptr);
    if (mAPI.DebugStep)
        mAPI.DebugStep();
    mEmuPaused = false;

    // Re-add the BP — the emulator has already moved past it.
    if (bpIdx >= 0 && pc != 0) {
        m64p_breakpoint bp;
        bp.address = pc;
        bp.endaddr = pc;
        bp.flags = M64P_BKP_FLAG_EXEC | M64P_BKP_FLAG_ENABLED;
        mAPI.DebugBreakpointCommand(M64P_BKP_CMD_ADD_STRUCT, 0, &bp);
    }
}

bool EmulatorSession::stepInstruction() {
    if (!mEmuRunning || !mEmuPaused || !isDebuggerAvailable())
        return false;

    uint32_t pc = getPC();

    // Remove any exec BP at the current PC so the instruction can execute.
    mSteppingBpAddr = 0;
    mSteppingBpFlags = 0;
    if (pc != 0) {
        int bpIdx = mAPI.DebugBreakpointLookup(pc, 4, M64P_BKP_FLAG_EXEC | M64P_BKP_FLAG_ENABLED);
        if (bpIdx >= 0) {
            mSteppingBpAddr = pc;
            mSteppingBpFlags = M64P_BKP_FLAG_EXEC | M64P_BKP_FLAG_ENABLED;
            mAPI.DebugBreakpointCommand(M64P_BKP_CMD_REMOVE_IDX, bpIdx, nullptr);
        }
    }

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

std::vector<uint32_t> EmulatorSession::readRspTaskHeader() {
    // SP DMEM is at physical 0x04000000 → KSEG1 virtual 0xA4000000
    // Task header at DMEM + 0xFC0 (standard osSpTask placement)
    uint32_t base = 0xA4000FC0;
    std::vector<uint32_t> words;
    if (!isDebuggerAvailable() || !mEmuRunning) return words;
    for (int i = 0; i < 16; i++) {
        words.push_back(mAPI.DebugMemRead32(base + i * 4));
    }
    return words;
}

std::vector<uint8_t> EmulatorSession::readSpMemory(uint32_t offset, uint32_t size) {
    // SP DMEM at 0xA4000000, SP IMEM at 0xA4001000
    // offset 0x000-0xFFF = DMEM, offset 0x1000-0x1FFF = IMEM
    uint32_t vaddr = 0xA4000000 + (offset & 0x1FFF);
    if (size > 0x2000) size = 0x2000;
    return readMemory(vaddr, size);
}

std::vector<uint32_t> EmulatorSession::readSpRegisters() {
    // SP registers are at 0xA4040000 (physical 0x04040000 → KSEG1)
    std::vector<uint32_t> regs;
    if (!isDebuggerAvailable() || !mEmuRunning) return regs;
    uint32_t base = 0xA4040000;
    // Read first 8 SP registers: SP_MEM_ADDR, SP_DRAM_ADDR, SP_RD_LEN, SP_WR_LEN,
    // SP_STATUS, SP_DMA_FULL, SP_DMA_BUSY, SP_PC (at offset 0x8 in regs2)
    for (int i = 0; i < 8; i++) {
        regs.push_back(mAPI.DebugMemRead32(base + i * 4));
    }
    // SP_PC is at a different offset: SP core PC register
    regs.push_back(mAPI.DebugMemRead32(0xA4040000 + 0x8));  // SP_PC_REG
    return regs;
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

void EmulatorSession::enablePiDmaTrace(bool enabled) {
    mTracePiDma = enabled;
    if (enabled) {
        clearEvents();
        mLastPiStatus = 0;
    }
}

EmulatorSession::PiDmaRegs EmulatorSession::readPiDmaRegs() {
    PiDmaRegs regs = {0, 0, 0, 0, 0};
    if (!mCoreLoaded || !isDebuggerAvailable()) return regs;
    regs.dramAddr = mAPI.DebugMemRead32(0xA4600000);
    regs.cartAddr = mAPI.DebugMemRead32(0xA4600004);
    regs.rdLen    = mAPI.DebugMemRead32(0xA4600008);
    regs.wrLen    = mAPI.DebugMemRead32(0xA460000C);
    regs.status   = mAPI.DebugMemRead32(0xA4600010);
    return regs;
}

EmulatorSession::ViRegs EmulatorSession::readViRegs() {
    ViRegs regs = {0, 0, 0, 0, 0};
    if (!mCoreLoaded || !isDebuggerAvailable()) return regs;
    regs.status       = mAPI.DebugMemRead32(0xA4400000);
    regs.origin       = mAPI.DebugMemRead32(0xA4400004);
    regs.width        = mAPI.DebugMemRead32(0xA4400008);
    regs.vIntr        = mAPI.DebugMemRead32(0xA440000C);
    regs.vCurrentLine = mAPI.DebugMemRead32(0xA4400010);
    return regs;
}

std::vector<uint8_t> EmulatorSession::readFramebuffer(uint32_t &outWidth, uint32_t &outHeight, int &outBpp) {
    outWidth = 0; outHeight = 0; outBpp = 0;
    if (!mCoreLoaded || !isDebuggerAvailable()) return {};

    ViRegs vi = readViRegs();
    if (vi.origin == 0) return {};  // no framebuffer set

    // Determine pixel format from VI_STATUS bit 0
    // 0 = 32-bit (RGBA8888), 1 = 16-bit (RGBA5551)
    // Mupen64Plus VI_WIDTH is in 32-bit words; convert to pixels
    outBpp = (vi.status & 1) ? 2 : 4;
    outWidth = (vi.width * 4) / outBpp;

    // Height: typically 240 for NTSC, 288 for PAL
    // Detect PAL via VI_STATUS bit 7
    outHeight = (vi.status & 0x80) ? 288 : 240;

    // Convert physical framebuffer address to KSEG0 virtual
    uint32_t fbVaddr = (vi.origin & 0x00FFFFFF) | 0x80000000;

    uint32_t totalBytes = outWidth * outHeight * outBpp;
    return readMemory(fbVaddr, totalBytes);
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

// --- Struct tracking ---
int EmulatorSession::enableStructTracking(uint32_t addr, uint32_t size) {
    if (!isDebuggerAvailable()) return -1;
    if (mStructTrackEnabled) disableStructTracking();
    mStructTrackAddr = addr;
    mStructTrackSize = size;
    mStructTrackPrevData = readMemory(addr, size);
    mStructTrackBpIndex = addMemoryBreakpoint(addr, size, M64P_BKP_FLAG_WRITE);
    if (mStructTrackBpIndex >= 0) {
        mStructTrackEnabled = true;
    }
    return mStructTrackBpIndex;
}

void EmulatorSession::disableStructTracking() {
    if (!mStructTrackEnabled) return;
    if (mStructTrackBpIndex >= 0) {
        removeBreakpoint(mStructTrackBpIndex);
    }
    mStructTrackEnabled = false;
    mStructTrackAddr = 0;
    mStructTrackSize = 0;
    mStructTrackPrevData.clear();
    mStructTrackBpIndex = -1;
}

// --- Callchain tracing ---
int EmulatorSession::enableCallchainTrace(const std::vector<uint32_t> &addresses) {
    if (!isDebuggerAvailable()) return -1;
    if (mCallchainEnabled) disableCallchainTrace();
    mCallchainAddrs = addresses;
    for (auto addr : addresses) {
        int idx = addExecBreakpoint(addr);
        if (idx >= 0) {
            mCallchainBpIndices.push_back(idx);
        }
    }
    mCallchainEnabled = true;
    clearEvents();
    return (int)mCallchainBpIndices.size();
}

void EmulatorSession::disableCallchainTrace() {
    if (!mCallchainEnabled) return;
    for (auto idx : mCallchainBpIndices) {
        removeBreakpoint(idx);
    }
    // Clean up any remaining BPs at tracked addresses (BP re-catch may have
    // produced new indices after remove-and-re-add)
    if (mAPI.DebugBreakpointCommand) {
        for (auto addr : mCallchainAddrs) {
            int idx = mAPI.DebugBreakpointLookup(addr, 4, M64P_BKP_FLAG_EXEC | M64P_BKP_FLAG_ENABLED);
            if (idx >= 0)
                mAPI.DebugBreakpointCommand(M64P_BKP_CMD_REMOVE_IDX, idx, nullptr);
        }
    }
    mCallchainEnabled = false;
    mCallchainAddrs.clear();
    mCallchainBpIndices.clear();
}

// --- Scheduler tracing ---
int EmulatorSession::enableSchedulerTrace(uint32_t ctxSwitchAddr, uint32_t queueAddr) {
    if (!isDebuggerAvailable()) return -1;
    if (mSchedTraceEnabled) disableSchedulerTrace();
    // Clean up any stale BPs at these addresses
    if (mAPI.DebugBreakpointCommand) {
        if (ctxSwitchAddr != 0) {
            int idx = mAPI.DebugBreakpointLookup(ctxSwitchAddr, 4, M64P_BKP_FLAG_EXEC | M64P_BKP_FLAG_ENABLED);
            if (idx >= 0)
                mAPI.DebugBreakpointCommand(M64P_BKP_CMD_REMOVE_IDX, idx, nullptr);
        }
        if (queueAddr != 0) {
            int idx = mAPI.DebugBreakpointLookup(queueAddr, 16, M64P_BKP_FLAG_WRITE);
            if (idx >= 0)
                mAPI.DebugBreakpointCommand(M64P_BKP_CMD_REMOVE_IDX, idx, nullptr);
        }
    }
    mSchedCtxSwitchAddr = ctxSwitchAddr;
    mSchedQueueAddr = queueAddr;
    clearEvents();
    // BP on context switch function
    if (ctxSwitchAddr != 0)
        mSchedCtxSwitchBpIdx = addExecBreakpoint(ctxSwitchAddr);
    // BP on run queue head (memory write)
    if (queueAddr != 0) {
        m64p_breakpoint bp;
        bp.address = queueAddr;
        bp.endaddr = queueAddr + 15;
        bp.flags = M64P_BKP_FLAG_WRITE;
        int idx = mAPI.DebugBreakpointCommand(M64P_BKP_CMD_ADD_STRUCT, 0, &bp);
        if (idx >= 0) {
            mSchedQueueBpIdx = idx;
            // Capture baseline so the first actual write is detected as a change
            mSchedPrevQueueData = readMemory(queueAddr, 16);
        }
    }
    mSchedTraceEnabled = true;
    return (mSchedCtxSwitchBpIdx >= 0 || mSchedQueueBpIdx >= 0) ? 1 : -1;
}

void EmulatorSession::disableSchedulerTrace() {
    if (!mSchedTraceEnabled) return;
    if (mSchedCtxSwitchBpIdx >= 0) {
        removeBreakpoint(mSchedCtxSwitchBpIdx);
        mSchedCtxSwitchBpIdx = -1;
    }
    if (mSchedQueueBpIdx >= 0) {
        removeBreakpoint(mSchedQueueBpIdx);
        mSchedQueueBpIdx = -1;
    }
    mSchedTraceEnabled = false;
    mSchedPrevQueueData.clear();
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
    bool nowPaused = false;
    if (mAPI.DebugGetState && mAPI.DebugGetState(M64P_DBG_RUN_STATE) == M64P_DBG_RUNSTATE_PAUSED) {
        mEmuPaused = true;
        nowPaused = true;
    }

    // STEPPING mode: one instruction just executed — pause now.
    if (mAPI.DebugGetState && mAPI.DebugGetState(M64P_DBG_RUN_STATE) == M64P_DBG_RUNSTATE_STEPPING) {
        mAPI.DebugSetRunState(M64P_DBG_RUNSTATE_PAUSED);
        // Post an extra semaphore token so the current update_debugger call
        // unblocks (it will see PAUSED and wait, but this token satisfies it,
        // allowing InterpretOpcode to run exactly once before the next block).
        mAPI.DebugStep();
        // Re-add the BP we removed for this step
        if (mSteppingBpAddr != 0) {
            m64p_breakpoint bp;
            bp.address = mSteppingBpAddr;
            bp.endaddr = mSteppingBpAddr;
            bp.flags = mSteppingBpFlags;
            mAPI.DebugBreakpointCommand(M64P_BKP_CMD_ADD_STRUCT, 0, &bp);
            // If callchain tracing is active, auto-resume after re-adding BP
            if (mCallchainEnabled) {
                // We stepped past the BP; now resume running
                mSteppingBpAddr = 0;
                mSteppingBpFlags = 0;
                mAPI.DebugSetRunState(M64P_DBG_RUNSTATE_RUNNING);
                mAPI.DebugStep();
                mEmuPaused = false;
                return;
            }
            mSteppingBpAddr = 0;
            mSteppingBpFlags = 0;
        }
        mEmuPaused = true;
        nowPaused = true;
    }

    // Callchain tracing: capture on BP hit at tracked addresses, then auto-resume
    if (mCallchainEnabled && nowPaused) {
        bool hit = false;
        for (auto addr : mCallchainAddrs) {
            if (pc == addr) { hit = true; break; }
        }
        if (hit) {
            uint32_t ra = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            if (isDebuggerAvailable()) {
                a0 = readRegister(4);
                a1 = readRegister(5);
                a2 = readRegister(6);
                a3 = readRegister(7);
                ra = readRegister(31);
            }
            {
                std::lock_guard<std::mutex> lock(mEventMutex);
                TraceEvent ev;
                ev.frame = mFrameCounter;
                ev.type = "callchain";
                ev.pc = pc;
                ev.stateLabel = mCurrentStateLabel;
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "{\"ra\":\"0x%X\",\"a0\":\"0x%X\",\"a1\":\"0x%X\",\"a2\":\"0x%X\",\"a3\":\"0x%X\"}",
                         ra, a0, a1, a2, a3);
                ev.data.push_back({"callchain", buf});
                mEvents.push_back(ev);
                if (mEvents.size() > 100000) {
                    mEvents.erase(mEvents.begin(), mEvents.begin() + 50000);
                }
            }
            // Remove this BP to avoid re-catch, step past it, then re-add
            m64p_breakpoint bp;
            bp.address = pc;
            bp.endaddr = pc;
            bp.flags = M64P_BKP_FLAG_EXEC | M64P_BKP_FLAG_ENABLED;
            int bpIdx = mAPI.DebugBreakpointLookup(pc, 4, M64P_BKP_FLAG_EXEC | M64P_BKP_FLAG_ENABLED);
            if (bpIdx >= 0) {
                mAPI.DebugBreakpointCommand(M64P_BKP_CMD_REMOVE_IDX, bpIdx, nullptr);
            }
            mSteppingBpAddr = pc;
            mSteppingBpFlags = M64P_BKP_FLAG_EXEC | M64P_BKP_FLAG_ENABLED;
            mAPI.DebugSetRunState(M64P_DBG_RUNSTATE_STEPPING);
            mAPI.DebugStep();
            mEmuPaused = false;
            return;
        }
    }

    // Struct tracking: capture write data, then auto-resume
    if (mStructTrackEnabled && nowPaused && mStructTrackSize > 0) {
        auto current = readMemory(mStructTrackAddr, mStructTrackSize);
        if (current.size() == mStructTrackPrevData.size()) {
            for (uint32_t off = 0; off < current.size(); off++) {
                if (current[off] != mStructTrackPrevData[off]) {
                    std::lock_guard<std::mutex> lock(mEventMutex);
                    TraceEvent ev;
                    ev.frame = mFrameCounter;
                    ev.type = "struct_write";
                    ev.pc = pc;
                    ev.stateLabel = mCurrentStateLabel;
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "{\"addr\":\"0x%X\",\"offset\":%u,\"val\":\"0x%02X\",\"prev\":\"0x%02X\"}",
                             mStructTrackAddr + off, off, current[off], mStructTrackPrevData[off]);
                    ev.data.push_back({"struct_write", buf});
                    mEvents.push_back(ev);
                    if (mEvents.size() > 100000) {
                        mEvents.erase(mEvents.begin(), mEvents.begin() + 50000);
                    }
                }
            }
            mStructTrackPrevData = std::move(current);
        }
        // Auto-resume to capture next write transparently
        mAPI.DebugSetRunState(M64P_DBG_RUNSTATE_RUNNING);
        mAPI.DebugStep();
        mEmuPaused = false;
        return;
    }

    // Scheduler tracing: context switch or queue write hit
    if (mSchedTraceEnabled && nowPaused) {
        bool schedHit = false;
        // Context switch BP
        if (mSchedCtxSwitchAddr != 0 && pc == mSchedCtxSwitchAddr) {
            uint32_t ra = 0, a0 = 0, a1 = 0;
            if (isDebuggerAvailable()) {
                a0 = readRegister(4);
                a1 = readRegister(5);
                ra = readRegister(31);
            }
            {
                std::lock_guard<std::mutex> lock(mEventMutex);
                TraceEvent ev;
                ev.frame = mFrameCounter;
                ev.type = "sched_ctx_switch";
                ev.pc = pc;
                ev.stateLabel = mCurrentStateLabel;
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "{\"ra\":\"0x%X\",\"a0\":\"0x%X\",\"a1\":\"0x%X\"}",
                         ra, a0, a1);
                ev.data.push_back({"sched_ctx_switch", buf});
                mEvents.push_back(ev);
                if (mEvents.size() > 100000) {
                    mEvents.erase(mEvents.begin(), mEvents.begin() + 50000);
                }
            }
            schedHit = true;
        }
        // Queue write BP
        if (mSchedQueueBpIdx >= 0 && mSchedQueueAddr != 0) {
            auto current = readMemory(mSchedQueueAddr, 16);
            if (current.size() == 16 && !mSchedPrevQueueData.empty()) {
                bool changed = false;
                for (int i = 0; i < 16; i++) {
                    if (current[i] != mSchedPrevQueueData[i]) { changed = true; break; }
                }
                if (changed) {
                    std::lock_guard<std::mutex> lock(mEventMutex);
                    TraceEvent ev;
                    ev.frame = mFrameCounter;
                    ev.type = "sched_queue_write";
                    ev.pc = pc;
                    ev.stateLabel = mCurrentStateLabel;
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "{\"queue_head\":\"0x%02X%02X%02X%02X\"}",
                             current[3], current[2], current[1], current[0]);
                    ev.data.push_back({"sched_queue_write", buf});
                    mEvents.push_back(ev);
                    if (mEvents.size() > 100000) {
                        mEvents.erase(mEvents.begin(), mEvents.begin() + 50000);
                    }
                }
            }
            mSchedPrevQueueData = current;
            schedHit = true;
        }
        if (schedHit) {
            // Auto-resume — same pattern as struct tracking
            mAPI.DebugSetRunState(M64P_DBG_RUNSTATE_RUNNING);
            mAPI.DebugStep();
            mEmuPaused = false;
            return;
        }
    }

    std::lock_guard<std::mutex> lock(mEventMutex);
    
    // PI DMA tracing: when paused after a BP hit, check if PI DMA completed
    if (mTracePiDma && nowPaused && isDebuggerAvailable()) {
        uint32_t status = mAPI.DebugMemRead32(0xA4600010);
        // PI_STATUS bit 0 = DMA busy; when it transitions 1→0, a DMA just completed
        if ((mLastPiStatus & 1) && !(status & 1)) {
            TraceEvent dmaEv;
            dmaEv.frame = mFrameCounter;
            dmaEv.type = "pi_dma";
            dmaEv.pc = pc;
            dmaEv.stateLabel = mCurrentStateLabel;
            uint32_t dram = mAPI.DebugMemRead32(0xA4600000);
            uint32_t cart = mAPI.DebugMemRead32(0xA4600004);
            uint32_t len  = mAPI.DebugMemRead32(0xA4600008);
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"dram\":\"0x%X\",\"cart\":\"0x%X\",\"len\":%u}",
                     dram, cart, len + 1);
            dmaEv.data.push_back({"pi_dma", buf});
            mEvents.push_back(dmaEv);
        }
        mLastPiStatus = status;
    }
    
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
