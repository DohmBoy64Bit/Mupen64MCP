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
│  FastMCP server · 43 tools                        │
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

Use the Unix Makefile in MINGW64 shell:

```sh
cd build/mupen64plus/mupen64plus-core/projects/unix
make clean
make all VULKAN=0 NO_ASM=1 DEBUGGER=1
```

This produces `mupen64plus.dll` with the debugger API enabled. Copy it to the lib folder:

```sh
cp mupen64plus.dll ../../lib/
```

**Build flags:**
- `VULKAN=0` — disable Vulkan (avoids missing headers)
- `NO_ASM=1` — pure interpreter mode (required for reliable debugger breakpoints)
- `DEBUGGER=1` — enables the debugger API (`-DDBG`)
- `M64P_PARALLEL` — already set by default for parallel builds

### 2. Build native daemon

```sh
cd native/n64_debug_daemon
cmake -B build -DMUPEN64PLUS_DIR=../../build/mupen64plus
cmake --build build
```

This produces `native/n64_debug_daemon/build/n64-debug-daemon.exe`.

### 3. Build video plugin (Rice — optional, for real framebuffer)

If you want rendered pixels (not just RDRAM framebuffer), build the Rice video plugin:

```sh
cd build/mupen64plus/mupen64plus-video-rice/projects/unix
make all
```

Copy the DLL to the plugins folder:

```sh
cp mupen64plus-video-rice.dll ../../../../plugins/
```

The daemon auto-configures `Video-Rice` → `FrameBufferSetting=3` (writeback) so `n64_read_framebuffer` returns actual pixel data.

### 4. Build input inject plugin

The input injection plugin is built automatically alongside the daemon by `build.bat`.
To build manually:

```sh
cd native/input_inject
cmake -B build -DMUPEN64PLUS_DIR=../../build/mupen64plus
cmake --build build
```

Output: `native/input_inject/build/mupen64plus-input-inject.dll`

### 4. Install Python MCP server

```sh
cd mcp/python
pip install -e .
# or
uv sync
```

## Usage

### Start the daemon

```sh
# Headless (dummy plugins) — best for debugging & reversing
native/n64_debug_daemon/build/n64-debug-daemon.exe ^
  --core build/mupen64plus/lib/mupen64plus.dll ^
  --rom roms/myrom.z64 ^
  --datadir build/mupen64plus/share ^
  --configdir build/mupen64plus/config ^
  --gfx dummy --audio dummy --input native/n64_debug_daemon/build/mupen64plus-input-inject.dll ^
  --rsp dummy ^
  --port 9876

# With real video rendering (Rice + RSP-HLE) — for framebuffer capture
native/n64_debug_daemon/build/n64-debug-daemon.exe ^
  --core build/mupen64plus/lib/mupen64plus.dll ^
  --rom roms/myrom.z64 ^
  --datadir build/mupen64plus/share ^
  --configdir build/mupen64plus/config ^
  --gfx plugins/mupen64plus-video-rice.dll ^
  --audio dummy ^
  --input native/n64_debug_daemon/build/mupen64plus-input-inject.dll ^
  --rsp plugins/mupen64plus-rsp-hle.dll ^
  --port 9876
```

For input injection, pass the path to `mupen64plus-input-inject.dll` as `--input`.
Omit `--input` (or set `--input dummy`) to use the built-in dummy input plugin.

### Injecting controller input

```python
# Via MCP tool:
n64_set_controller(channel=0, buttons="START", sticky=False)      # one-shot press
n64_set_controller(channel=0, buttons="A", x=80, y=0, sticky=True) # hold A + stick right

# Symbolic button names: A B START Z L R U_DPAD D_DPAD L_DPAD R_DPAD U_C D_C L_C R_C
# Raw hex: "0x0010" = START, "0x0080" = A, "0x1090" = A + R + START
```

### Launch the status dashboard (optional)

```sh
n64-viewer
```

A standalone tkinter window that connects to an already-running daemon.
Shows live status, scene detection, speed/steering gauges, a 2D track
position trail, labeled game state fields, a live event feed, and quick
input injection buttons. The Game Data section is currently game-specific
to Cruis'n USA (addresses from `0x8013A000`). Launched via `n64-viewer`.

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

## MCP Tools (38 total)

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
| `n64_track_struct` | Memory write watcher — captures addr/offset/old/new on writes |
| `n64_dl_decode` | Display list decoder — pretty-prints F3DEX2 commands |
| `n64_trace_callchain` | Multi-BP function call tracer — captures RA/A0-A3 on call |
| `n64_trace_scheduler` | RTOS scheduler tracer — context switch + run queue (takes addresses) |
| `n64_detect_os` | Detect OS type (libultra/likely_libultra/custom_with_libultra_functions/custom), boot flow, RSP ucode, scheduler dispatch presence, thread function addresses, active PC context |

### Input Injection
| Tool | Description |
|------|-------------|
| `n64_set_controller` | Inject controller state (buttons, analog stick) into running emulator. Supports one-shot and sticky mode. Requires `mupen64plus-input-inject.dll` plugin. |

### Framebuffer
| Tool | Description |
|------|-------------|
| `n64_read_framebuffer` | Read current framebuffer via VI registers. Returns width/height/bpp and raw pixel data. Pixels are zero with dummy gfx plugin (requires real video plugin for rendered output). |
| `n64_set_frame_capture_interval` | Auto-capture framebuffer every N frames (0=disable). Captures stored internally, max 100. |
| `n64_get_frame_captures` | List auto-captured frames (frame, width, height, bpp, size). |
| `n64_clear_frame_captures` | Clear the auto-capture buffer. |
| `n64_wait_for_frame` | Wait until emulator reaches target frame count (with timeout). |
| `n64_frame_count` | Get current VI frame counter. |

### Asset Discovery
| Tool | Description |
|------|-------------|
| `n64_discover_assets` | Scan ROM, RDRAM, and RSP state for structured asset manifest |
| `n64_export_manifest` | Scan + export asset manifest to JSON file |

### PI DMA
| Tool | Description |
|------|-------------|
| `n64_capture_pi_dma` | Read current PI DMA registers (source/dest/size/status) |
| `n64_trace_pi_dma` | Enable/disable automatic PI DMA tracing on breakpoint hit |

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
│           ├── server.py            # 43 MCP tools (FastMCP)
│           ├── daemon_client.py     # TCP JSON-RPC client
│           └── n64_viewer.py        # Live status dashboard (tkinter)
├── native/
│   ├── n64_debug_daemon/
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   ├── daemon.h             # CoreAPI struct, types
│   │   │   ├── emulator_session.h   # EmulatorSession class
│   │   │   └── json_rpc_server.h    # TCP server
│   │   ├── src/
│   │   │   ├── main.cpp             # Entry point, arg parsing
│   │   │   ├── emulator_session.cpp # Lifecycle, debug API wrappers
│   │   │   ├── json_rpc_server.cpp  # TCP server + handlers
│   │   │   ├── mupen_core_loader.cpp
│   │   │   ├── memory.cpp
│   │   │   ├── breakpoints.cpp
│   │   │   └── tracing.cpp
│   │   └── build/                   # CMake output
│   └── input_inject/                # Input injection plugin (replaces dummy input)
│       ├── CMakeLists.txt
│       ├── plugin.h
│       └── plugin.c
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
- Mupen64Plus core built from source with `DEBUGGER=1` and `NO_ASM=1` (interpreter only)
- Rice video plugin built and working with real OpenGL rendering
- RSP-HLE plugin loads via CoreAttachPlugin (fixed: PluginStartup context param must be `void*`, not `m64p_plugin_type`)
- SP memory (DMEM/IMEM) and register reads via debugger — inspect RSP task headers and status without plugin attachment
- Native daemon: core loading, plugin lifecycle, debug API, breakpoints, memory R/W, tracing
- Config auto-set: `EnableDebugger=1`, `R4300Emulator=0` (Pure Interpreter), `Video-Rice.FrameBufferSetting=3`
- `onDebuggerUpdate` callback propagates pause state via semaphore
- JSON-RPC over TCP with space-tolerant parser
- 43 MCP tools via FastMCP
- Python daemon client with one-connection-per-call pattern
- Breakpoint → resume → wait loop for runtime debugging
- ROM-read DMA tracing (PI register capture)
- Game state labeling
- Trace export to JSON
- Verified boot flow, register access, stepping, and memory reads on Cruis'n USA
- Resume/step now correctly escapes breakpoint at current PC (temporarily removes BP to prevent re-catch in `update_debugger`)
- Single-step advances PC correctly through arbitrary MIPS instructions including branches
- **Runtime asset discovery**: non-invasive ROM/RDRAM scan identifies regions by content fingerprint
- **Input injection**: custom `mupen64plus-input-inject.dll` plugin replaces the dummy input plugin. Exports `SetControllerState` for the daemon to call, stores 4 channels of `BUTTONS` state, supports one-shot and sticky modes. All required Mupen64Plus input plugin exports (`SDL_KeyDown`/`SDL_KeyUp`, `GetKeys`, `InitiateControllers`, etc.)
- **Framebuffer capture**: reads VI registers and RDRAM framebuffer via `read_framebuffer`. With dummy gfx, the RDP never processes display lists and the framebuffer stays zero. With Rice video + RSP-HLE, the framebuffer contains actual rendered pixels (daemon auto-sets `Video-Rice.FrameBufferSetting=3` for writeback).
- **n64-viewer** (optional): standalone live status dashboard with scene detection (PC-range heuristic), labeled game state display, speed/steering gauges, 2D track position trail, event feed, and input injection buttons. Game Data section is Cruis'n USA-specific. Launched via `n64-viewer`.
- **Scheduler queue-write detection**: `mSchedPrevQueueData` is now initialized with a baseline read when the trace is enabled, so the first actual write is detected as a change. `queue_addr` is optional — omit it for games with custom schedulers (e.g. Cruis'n USA) where the run queue structure is not a standard libultra `__osRunQueue`.

### Known Limitations
- One TCP connection per request — no daemon-side blocking for `wait_for_breakpoint` (implemented as client-side poll loop)
- Only interpreter mode produces reliable debugger callbacks
- Frame counter requires VI interrupts (dummy gfx plugins may not increment it)
- Input injection requires passing `--input path/to/mupen64plus-input-inject.dll` at daemon startup

### Tested ROMs
- **Cruis'n USA** (NCUE) — CRC `FF2F2FB4 D161149A`, 8 MB
  - Custom Midway engine (PIF jumps to `0x8011C450`, bypassing IPL3)
  - Custom F3DEX-based RSP microcode at ROM offset `0x31000` (custom implementation, standard F3DEX2 GBI commands)
  - Boot flow: `PIF (0xA4000040)` → `0x80000000` trampoline → `0x80124C60` → `0x8011C450`
- **Star Fox 64** (LZ-type) — CRC `BA780BA0 0F21DB34`, 12 MB
  - Standard IPL3 boot (`0x80000400` entry)
  - libultra functions detected: `osCreateThread @ 0x8001C3EC`, `osStartThread @ 0x80006FD8`, `osYieldThread @ 0x800049D4`

### Future Improvements
- **OS Detector**: Currently supports libultra-based ROMs (IPL3 boot) and custom engines with libultra function signatures. Future work should add:
  - Custom scheduler detection for non-libultra engines (e.g., Cruis'n USA's custom RTOS)
  - Additional boot pattern recognition (e.g., `0x80000180` direct entry without trampoline)
  - Function signature database expansion for games with static-linked libultra variants
  - Detection of custom RSP ucode types beyond F3D/F3DEX2 (e.g., Boss Game Studios, Factor 5)
  - Support for ROMs that use custom IPL3 replacements (e.g., Nintendo 64DD IPL, iQue Player)

### Comprehensive Test Results (53/53 PASS)
All 43 MCP tools verified on Cruis'n USA in a single end-to-end test:

| # | Test | Result |
|---|------|--------|
| 1 | Core connectivity (ping, status) | PASS |
| 2 | CPU registers (32 GPRs, PC) | PASS |
| 3 | Memory reads (ROM header, name, magic) | PASS |
| 4 | Breakpoints (add, list, remove) | PASS |
| 5 | Boot flow (entry BP → resume → hit → step past) | PASS |
| 6 | Single-step through arbitrary MIPS code | PASS |
| 7 | OS detection (libultra functions, boot type, ucode) | PASS |
| 8 | RSP/SP memory, registers, task header | PASS |
| 9 | PI DMA register capture | PASS |
| 10 | Struct tracking (memory write watcher + BP escape) | PASS |
| 11 | Callchain trace (200 events at context switch) | PASS |
| 12 | Display list decode (F3DEX2 commands at 0x802C0000) | PASS |
| 13 | Scheduler trace (200 ctx switches) | PASS |
| 14 | Asset discovery (ROM/RDRAM scans) | PASS |
| 15 | State labeling | PASS |
| 16 | Virtual-to-physical address translation | PASS |
| 17 | ROM read tracing (PI DMA) | PASS |
| 18 | Cleanup (no stale breakpoints) | PASS |
| 19 | Framebuffer capture (Rice + RSP-HLE) | PASS |
| 20 | Input injection (A/B/START with sticky) | PASS |
| 17 | ROM read tracing (PI DMA) | PASS |
| 18 | Cleanup (no stale breakpoints) | PASS |

### Asset Discovery (Cruis'n USA)
Runtime ROM/RDRAM scans via debugger memory reads identified:

| ROM Offset | Contents | Identification |
|---|---|---|
| `0x000000` | `80371240` magic, CRC, "Cruis'n USA", "NCUE" | N64 ROM header |
| `0x001000` | Memory clear loop → boot init | IPL3 / boot code |
| `0x040000-0x0BFFFF` | MIPS instructions (`3C0A8001...`) | Game code (~512 KB) |
| `0x0C0000` | `63634E4E` ("ccNN") | Data segment |
| `0x180000` | `73E5 73E5 73E5 6BE7...` repeating | **ADPCM audio data** |
| `0x1C0000-0x7FFFFF` | Dense high-entropy data | Textures, levels, models |
| `0x31000` | 8KB blob: 34% COP2, 19% LWC2 | **Custom F3DEX RSP microcode** (standard F3DEX2 GBI commands) |

RDRAM after running to frame ~200:

| Address | Contents | Identification |
|---|---|---|
| `0x80000000` | `3C1A8012 275A4C60 03400008` → `jr 0x80124C60` | **Reset trampoline** |
| `0x80000000-0x801FFFFF` | MIPS instructions | Loaded game code |
| `0x802C0000-0x802FFFFF` | `E6000000`, `BA000E02`, `BF...`, `F5500000` | **Active F3DEX2 display lists** (standard commands) |
| `0x80330000` | `89868685...` | **CI8 palette data** |
| `0x80340000` | `479E9E9E...` smooth gradient | **Texture color data** |

## License

MIT
