#include "daemon.h"
#include "emulator_session.h"

// Tracing helpers for ROM-read, RSP task, and audio load capture.
// The trace event buffer is managed by EmulatorSession.
// This file can be extended for:
// - Breakpoint-based ROM read tracing (register mapping config)
// - RSP task submission tracing
// - Audio bank/sequence load tracing
// - CSV/JSON export formatting
// - Trace-to-asset-manifest conversion

struct TraceConfig {
    std::string name;
    uint32_t breakpointAddr;
    std::string romSrcReg;  // register name for ROM source
    std::string ramDstReg;  // register name for RAM destination
    std::string sizeReg;    // register name for size
};

// In a full implementation, the daemon would:
// 1. Set an exec breakpoint on the suspected DMA function
// 2. On hit, read the configured registers
// 3. Record the trace event with ROM src, RAM dst, size
// 4. Resume execution
