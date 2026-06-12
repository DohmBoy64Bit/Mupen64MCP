# Architecture

```
┌──────────────────────────────────────────────────┐
│                AI Assistant (MCP client)          │
│  Claude Desktop / Cursor / custom MCP client      │
└────────────┬─────────────────────────────┬───────┘
             │ stdio (MCP protocol)         │
┌────────────▼─────────────────────────────▼───────┐
│           n64-debug-mcp  (Python)                 │
│  FastMCP server · 47 tools                        │
│  Thin translation layer → JSON-RPC                │
└────────────┬─────────────────────────────┬───────┘
             │ TCP 127.0.0.1:9876           │
┌────────────▼─────────────────────────────▼───────┐
│         n64-debug-daemon  (C++17)                 │
│  Loads libmupen64plus dynamically                 │
│  JSON-RPC over TCP (one-request-per-connection)   │
│  Breakpoints · Memory R/W · Step · Trace          │
└────────────┬─────────────────────────────┬───────┘
             │ Core API (m64p_debugger.h)   │
┌────────────▼─────────────────────────────▼───────┐
│         Mupen64Plus Core (with DEBUGGER)          │
│  Pure Interpreter mode (R4300Emulator=0)          │
│  DebugVirtualToPhysical · check_breakpoints       │
│  update_debugger · DebugStep · SDL plugins        │
└──────────────────────────────────────────────────┘
```

## Design Decisions

- **Two-layer design**: Python MCP server + native C++ daemon. No emulator logic in the MCP layer — thin client-server only.
- **Dynamic core loading**: Daemon loads `libmupen64plus` via `LoadLibrary` — no link-time dependency.
- **Interpreter mode**: Built with `NO_ASM=1` and `R4300Emulator=0` for reliable debugger breakpoints.
- **Read-only memory by default**: Writes require `--allow-write-memory`.
- **One TCP connection per JSON-RPC request**: Simple, stateless.

## Implemented Features

- Mupen64Plus core built from source with `DEBUGGER=1` and `NO_ASM=1`
- Rice video plugin with real OpenGL rendering
- RSP-HLE plugin (fixed: `PluginStartup` context must be `void*`, not `m64p_plugin_type`)
- SP memory (DMEM/IMEM) and register reads via debugger
- Config auto-set: `EnableDebugger=1`, `R4300Emulator=0`, `Video-Rice.FrameBufferSetting=3`
- `onDebuggerUpdate` callback propagates pause state via semaphore
- JSON-RPC over TCP with space-tolerant parser
- 47 MCP tools (n64_verb_noun convention)
- Resume/step breakpoint escape: temporarily removes BP at current PC to prevent re-catch
- Runtime asset discovery via non-invasive ROM/RDRAM content fingerprinting
- Input injection: custom `mupen64plus-input-inject.dll` with `SetControllerState` export
- Framebuffer capture via VI register reads and RDRAM access
- Scheduler queue-write detection with baseline read initialization
- Breakpoint management via `mOwnedBps` vector with `REMOVE_ADDR`/`REMOVE_IDX` fallback
- Static address translation (file offset ↔ KSEG0 ↔ KSEG1)
- Function scanner for MIPS prologues (`ADDIU` + `SW ra`)
- RSP health check (IMEM CRC32, ucode type inference, RSP-HLE detection)
- OS detector (libultra / custom engine classification)

## Key Files

| File | Purpose |
|------|---------|
| `native/n64_debug_daemon/src/main.cpp` | Entry point, argument parsing |
| `native/n64_debug_daemon/src/emulator_session.cpp` | Core lifecycle, debug API wrappers |
| `native/n64_debug_daemon/src/json_rpc_server.cpp` | TCP server + JSON-RPC method handlers |
| `native/n64_debug_daemon/src/mupen_core_loader.cpp` | Dynamic loading of mupen64plus.dll |
| `mcp/python/n64_debug_mcp/server.py` | 47 MCP tools (FastMCP) |
| `mcp/python/n64_debug_mcp/daemon_client.py` | TCP JSON-RPC client |
