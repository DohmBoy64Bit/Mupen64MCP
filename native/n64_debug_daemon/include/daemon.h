#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <m64p_types.h>

// Convenience: host pointer to plugin startup/shutdown
using PluginStartupFn = m64p_error (*)(m64p_handle, m64p_plugin_type, void (*)(void *, int, const char *));
using PluginShutdownFn = m64p_error (*)(void);
using PluginGetVersionFn = m64p_error (*)(
    m64p_plugin_type *, int *, int *, const char **, int *);

struct PluginLib {
    std::string path;
    void *handle = nullptr;
    PluginStartupFn startup = nullptr;
    PluginShutdownFn shutdown = nullptr;
    PluginGetVersionFn getVersion = nullptr;
};

struct PluginSet {
    PluginLib video;
    PluginLib audio;
    PluginLib input;
    PluginLib rsp;
};

// Breakpoint representation for the JSON-RPC / MCP layer
struct BreakpointInfo {
    int index;
    uint32_t address;
    uint32_t endAddress;
    unsigned int flags;
    bool enabled;
};

// A single trace event
struct TraceEvent {
    uint64_t frame;
    std::string type; // "rom_read", "rsp_task", "audio_load", "bp_hit", "step"
    uint32_t pc;
    std::string stateLabel;
    // type-specific data stored as JSON-like key-value pairs
    std::vector<std::pair<std::string, std::string>> data;
};

// Game state label
struct GameStateLabel {
    uint64_t frame;
    std::string label;
    std::string notes;
};

// --- C++ wrappers around m64p function pointers ---
// These are resolved dynamically from libmupen64plus

struct CoreAPI {
    // Startup / shutdown
    m64p_error (*CoreStartup)(int, const char *, const char *, void *,
                               void (*)(void *, int, const char *), void *,
                               void (*)(void *, m64p_core_param, int)) = nullptr;
    m64p_error (*CoreShutdown)(void) = nullptr;
    m64p_error (*CoreAttachPlugin)(m64p_plugin_type, m64p_dynlib_handle) = nullptr;
    m64p_error (*CoreDetachPlugin)(m64p_plugin_type) = nullptr;

    // Commands
    m64p_error (*CoreDoCommand)(m64p_command, int, void *) = nullptr;

    // Version / error
    m64p_error (*PluginGetVersion)(m64p_plugin_type *, int *, int *, const char **, int *) = nullptr;
    m64p_error (*CoreGetAPIVersions)(int *, int *, int *, int *) = nullptr;
    const char *(*CoreErrorMessage)(m64p_error) = nullptr;
    m64p_error (*CoreGetRomSettings)(m64p_rom_settings *, int, int, int) = nullptr;

    // Config
    m64p_error (*ConfigGetSectionParamInt)(const char *, const char *, int32_t *) = nullptr;

    // Debugger (conditionally available; check M64CAPS_DEBUGGER)
    m64p_error (*DebugSetCallbacks)(void (*)(void), void (*)(unsigned int), void (*)(void)) = nullptr;
    m64p_error (*DebugSetRunState)(m64p_dbg_runstate) = nullptr;
    m64p_error (*DebugGetState)(m64p_dbg_state, int *) = nullptr;
    m64p_error (*DebugStep)(void) = nullptr;
    m64p_error (*DebugDecodeOp)(uint32_t, char *, char *, uint32_t) = nullptr;
    m64p_error (*DebugMemGetPointer)(m64p_dbg_memptr_type, void **) = nullptr;
    m64p_error (*DebugMemRead8)(uint32_t, uint8_t *) = nullptr;
    m64p_error (*DebugMemRead16)(uint32_t, uint16_t *) = nullptr;
    m64p_error (*DebugMemRead32)(uint32_t, uint32_t *) = nullptr;
    m64p_error (*DebugMemRead64)(uint32_t, uint64_t *) = nullptr;
    m64p_error (*DebugMemWrite8)(uint32_t, uint8_t) = nullptr;
    m64p_error (*DebugMemWrite16)(uint32_t, uint16_t) = nullptr;
    m64p_error (*DebugMemWrite32)(uint32_t, uint32_t) = nullptr;
    m64p_error (*DebugMemWrite64)(uint32_t, uint64_t) = nullptr;
    void *(*DebugGetCPUDataPtr)(m64p_dbg_cpu_data) = nullptr;
    m64p_error (*DebugBreakpointLookup)(uint32_t, uint32_t, unsigned int, int *) = nullptr;
    m64p_error (*DebugBreakpointCommand)(int, int, void *) = nullptr;
    m64p_error (*DebugBreakpointTriggeredBy)(unsigned int *, uint32_t *) = nullptr;
    m64p_error (*DebugVirtualToPhysical)(uint32_t, uint32_t *) = nullptr;
    m64p_error (*DebugMemGetMemInfo)(m64p_dbg_mem_info, uint32_t, int *) = nullptr;
};
