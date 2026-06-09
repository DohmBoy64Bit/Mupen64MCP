# Mupen64MCP

**Nintendo 64 ROM debugging via the Model Context Protocol (MCP).**

Mupen64MCP lets AI assistants (Claude Desktop, Cursor, etc.) inspect and control a running N64 ROM through Mupen64Plus's debugger API. Designed for runtime ROM analysis, asset reversing, display-list tracing, and game-state labeling — without instrumenting the original binary.

## Architecture

```
┌──────────────────────────────────────────────────┐
│                AI Assistant (MCP client)          │
│  Claude Desktop / Cursor / custom MCP client      │
└────────────┬─────────────────────────────┬───────┘
             │ stdio (MCP protocol)         │
┌────────────▼─────────────────────────────▼───────┐
│           n64-debug-mcp  (Python)                 │
│  FastMCP server · 23 tools                        │
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

**Key design decisions:**
- Two-layer design: Python MCP server + native C++ daemon
- No emulator logic in the MCP layer — thin client-server only
- Daemon loads the core DLL dynamically — no link-time dependency
- Interpreter mode (`NO_ASM=1` / `R4300Emulator=0`) for reliable breakpoints
- Read-only memory by default; writes require `--allow-write-memory`
- One TCP connection per JSON-RPC request (simple, stateless)

## Prerequisites

- **Windows** (MSYS2 MINGW64 environment recommended)
- **CMake ≥ 3.16**
- **gcc** (MSYS2) or MSVC
- **Python ≥ 3.11** with `uv` or `pip`
- **pkg-config**, **libopcodes**, **libbfd** (for Mupen64Plus build)

## Building

### 1. Build Mupen64Plus Core (with DEBUGGER)

```sh
# From project root
cd build
cmake -G "MSYS Makefiles" .. -DCMAKE_BUILD_TYPE=Debug -DNO_ASM=1
make -j$(nproc)
```

This produces `build/mupen64plus/lib/mupen64plus.dll` with the debugger API enabled.

### 2. Build native daemon

```sh
cd native/n64_debug_daemon
cmake -B build -DMUPEN64PLUS_DIR=../../build/mupen64plus
cmake --build build
```

This produces `native/n64_debug_daemon/build/n64-debug-daemon.exe`.

### 3. Install Python MCP server

```sh
cd mcp/python
pip install -e .
# or
uv sync
```

## Usage

### Start the daemon

```sh
native/n64_debug_daemon/build/n64-debug-daemon.exe ^
  --core build/mupen64plus/lib/mupen64plus.dll ^
  --rom roms/myrom.z64 ^
  --datadir build/mupen64plus/share ^
  --configdir build/mupen64plus/config ^
  --gfx dummy --audio dummy --input dummy --rsp plugins\mupen64plus-rsp-hle.dll ^
  --port 9876 ^
  --allow-write-memory
```

### Run the MCP server (standalone)

```sh
n64-debug-mcp
```

Or run via `uv`:

```sh
uv --directory mcp/python run n64-debug-mcp
```

### Claude Desktop integration

Add to your `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "n64-debug-mcp": {
      "command": "uv",
      "args": [
        "--directory",
        "D:\\Mupen64MCP\\mcp\\python",
        "run",
        "n64-debug-mcp"
      ]
    }
  }
}
```

### Cursor integration

Add to your Cursor MCP config:

```json
{
  "mcpServers": {
    "n64-debug-mcp": {
      "type": "command",
      "command": "uv",
      "args": [
        "--directory",
        "D:\\Mupen64MCP\\mcp\\python",
        "run",
        "n64-debug-mcp"
      ]
    }
  }
}
```

## MCP Tools (27 total)

### Lifecycle
| Tool | Description |
|------|-------------|
| `n64_start_daemon` | Start daemon subprocess and connect |
| `n64_stop_daemon` | Shut down daemon and release resources |

### Emulation control
| Tool | Description |
|------|-------------|
| `n64_status` | Core/ROM/emulator state + pc, frame, paused |
| `n64_load_rom` | Load a .z64 ROM and start emulation |
| `n64_close_rom` | Close ROM and stop emulation |
| `n64_pause` | Pause emulation |
| `n64_resume` | Resume from pause |
| `n64_step_instruction` | Execute one instruction |
| `n64_step_frame` | Execute until next VBI |

### CPU inspection
| Tool | Description |
|------|-------------|
| `n64_get_pc` | Current program counter |
| `n64_get_registers` | All 32 GPRs + PC |

### Memory
| Tool | Description |
|------|-------------|
| `n64_read_memory` | Read bytes from N64 address space |
| `n64_write_memory` | Write bytes (disabled by default) |
| `n64_dump_rdram` | Dump RDRAM from 0x80000000 |
| `n64_translate_address` | Virtual → physical address translation |

### Breakpoints
| Tool | Description |
|------|-------------|
| `n64_add_exec_breakpoint` | Set execution breakpoint at virtual address |
| `n64_remove_breakpoint` | Remove breakpoint by index |
| `n64_list_breakpoints` | List all active breakpoints |

### Tracing
| Tool | Description |
|------|-------------|
| `n64_mark_game_state` | Tag current state (e.g. "title_screen") |
| `n64_get_trace_events` | Recent trace events |
| `n64_trace_rom_reads` | Enable/disable PI DMA tracing |
| `n64_wait_for_breakpoint` | Block until breakpoint fires |
| `n64_export_trace` | Export trace events to JSON file |

### RSP / SP
| Tool | Description |
|------|-------------|
| `n64_get_rsp_task` | Read current RSP task header from SP DMEM |
| `n64_trace_rsp_tasks` | Enable/disable RSP task submission tracing |
| `n64_read_sp_mem` | Read bytes from SP DMEM or IMEM |
| `n64_read_sp_regs` | Read SP control registers (status, DMA, PC) |

## Project Structure

```
D:\Mupen64MCP\
├── mcp/
│   └── python/
│       ├── pyproject.toml           # Python package config
│       └── n64_debug_mcp/
│           ├── __init__.py
│           ├── server.py            # 23 MCP tools (FastMCP)
│           └── daemon_client.py     # TCP JSON-RPC client
├── native/
│   └── n64_debug_daemon/
│       ├── CMakeLists.txt
│       ├── include/
│       │   ├── daemon.h             # CoreAPI struct, types
│       │   ├── emulator_session.h   # EmulatorSession class
│       │   └── json_rpc_server.h    # TCP server
│       ├── src/
│       │   ├── main.cpp             # Entry point, arg parsing
│       │   ├── emulator_session.cpp # Lifecycle, debug API wrappers
│       │   ├── json_rpc_server.cpp  # TCP server + handlers
│       │   ├── mupen_core_loader.cpp
│       │   ├── memory.cpp
│       │   ├── breakpoints.cpp
│       │   └── tracing.cpp
│       └── build/                   # CMake output
├── plugins/                         # Plugin DLLs
├── build/
│   └── mupen64plus/                 # Mupen64Plus source + build
│       ├── mupen64plus-core/
│       ├── mupen64plus-audio-sdl/
│       ├── mupen64plus-input-sdl/
│       ├── mupen64plus-rsp-hle/
│       ├── mupen64plus-video-rice/
│       └── include/
│           └── m64p_debugger.h      # Debugger API
├── examples/
│   ├── claude_desktop_config.json
│   └── cursor_mcp.json
├── tests/                           # Test scripts
├── docs/                            # Documentation
└── protocols/                       # JSON-RPC protocol docs
```

## Development Status

### Implemented
- Mupen64Plus core built from source with DEBUGGER flag
- Four plugins compiled; RSP-HLE loads via CoreAttachPlugin (fixed: PluginStartup context param must be `void*`, not `m64p_plugin_type`)
- SP memory (DMEM/IMEM) and register reads via debugger — inspect RSP task headers and status without plugin attachment
- Native daemon: core loading, plugin lifecycle, debug API, breakpoints, memory R/W, tracing
- Config auto-set: `EnableDebugger=1`, `R4300Emulator=0` (Pure Interpreter)
- `onDebuggerUpdate` callback propagates pause state via semaphore
- JSON-RPC over TCP with space-tolerant parser
- 23 MCP tools via FastMCP
- Python daemon client with one-connection-per-call pattern
- Breakpoint → resume → wait loop for runtime debugging
- ROM-read DMA tracing (PI register capture)
- Game state labeling
- Trace export to JSON
- Verified boot flow, register access, stepping, and memory reads on Cruis'n USA

### Known Limitations
- `DebugStep()` in resume may execute one instruction before breakpoint check (observed: BP PC vs actual PC mismatch)
- One TCP connection per request — no daemon-side blocking for `wait_for_breakpoint` (implemented as client-side poll loop)
- Only interpreter mode produces reliable debugger callbacks

### Tested ROM
- **Cruis'n USA** (NCUE) — CRC `FF2F2FB4 D161149A`, 8 MB
- Custom Midway engine (no libultra — PIF jumps to `0x8011C450`, bypassing IPL3)
- Custom F3DEX-based RSP microcode at ROM offset `0x31000`

## License

MIT
