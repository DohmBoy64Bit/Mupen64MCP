#include "daemon.h"
#include "emulator_session.h"

// Breakpoint management helpers.
// The actual breakpoint operations are delegated to EmulatorSession via
// the Mupen64Plus Debug API (m64p_debugger.h).
//
// This file can be extended for:
// - Named breakpoint sets
// - Conditional breakpoints
// - Hit-count tracking
// - Breakpoint groups for specific trace scenarios
