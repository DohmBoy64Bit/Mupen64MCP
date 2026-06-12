# Mupen64MCP

**Nintendo 64 ROM debugging via the Model Context Protocol (MCP).**

Inspect and control a running N64 ROM through Mupen64Plus's debugger API — breakpoints, memory, registers, display lists, OS detection, and more. Designed for runtime ROM analysis, asset reversing, and game-state labeling without instrumenting the binary.

## Prerequisites

- **Windows** (MSYS2 MINGW64 environment recommended)
- **CMake ≥ 3.16**, **gcc** (MSYS2) or MSVC
- **Python ≥ 3.11** with `uv` or `pip`
- **pkg-config**, **libopcodes**, **libbfd** (for Mupen64Plus build)

## Quick Start

```sh
# 1. Start the daemon (headless, dummy plugins)
start_daemon.bat

# 2. Start the MCP server
n64-debug-mcp

# 3. Configure your AI assistant (Claude Desktop, Cursor, etc.)
# See examples/ for config files
```

## Building

### Mupen64Plus Core (with DEBUGGER)

```sh
cd build/mupen64plus/mupen64plus-core/projects/unix
make clean
make all VULKAN=0 NO_ASM=1 DEBUGGER=1
cp mupen64plus.dll ../../lib/
```

Flags: `VULKAN=0` (avoids missing headers), `NO_ASM=1` (interpreter for reliable breakpoints), `DEBUGGER=1` (enables debugger API).

### Native Daemon

```sh
cd native/n64_debug_daemon
cmake -B build -DMUPEN64PLUS_DIR=../../build/mupen64plus
cmake --build build
```

### Python MCP Server

```sh
cd mcp/python
pip install -e .
# or: uv sync
```

### Optional: Rice Video Plugin (for framebuffer capture)

**Must be built in the MSYS2 MINGW64 shell** — not WinLibs or any other MinGW distribution. Plugins link against MSYS2's `libstdc++-6.dll` at runtime; building with a different toolchain creates ABI-incompatible DLLs that crash on load.

```sh
# Run this from an MSYS2 MINGW64 shell, not cmd or PowerShell
cd build/mupen64plus/mupen64plus-video-rice/projects/unix
make all
cp mupen64plus-video-rice.dll ../../../../plugins/
```

All other real plugins (RSP-HLE, audio-sdl, input-sdl) must be built the same way — using `make` from the MSYS2 MINGW64 shell.

## Usage

```sh
# Headless (dummy plugins) — best for debugging
native/n64_debug_daemon/build/n64-debug-daemon.exe ^
  --core build/mupen64plus/lib/mupen64plus.dll ^
  --rom roms/myrom.z64 ^
  --gfx dummy --audio dummy --rsp dummy ^
  --input native/n64_debug_daemon/build/mupen64plus-input-inject.dll ^
  --port 9876

# With real rendering (Rice + RSP-HLE) — for framebuffer capture
# Note: plugins must be built with MSYS2 MINGW64 toolchain (see "Rice Video Plugin" below)
native/n64_debug_daemon/build/n64-debug-daemon.exe ^
  --core build/mupen64plus/lib/mupen64plus.dll ^
  --rom roms/myrom.z64 ^
  --gfx plugins/mupen64plus-video-rice.dll ^
  --rsp plugins/mupen64plus-rsp-hle.dll ^
  --input native/n64_debug_daemon/build/mupen64plus-input-inject.dll ^
  --port 9876
```

### Config Options

| Option | Description | Default |
|--------|-------------|---------|
| `--core <path>` | Path to `mupen64plus.dll` | *(required)* |
| `--rom <path>` | ROM file to load | *(none)* |
| `--gfx <path>` | Video plugin DLL | `dummy` |
| `--audio <path>` | Audio plugin DLL | `dummy` |
| `--input <path>` | Input plugin DLL | `dummy` |
| `--rsp <path>` | RSP plugin DLL | `dummy` |
| `--datadir <dir>` | Shared data directory | `.` |
| `--configdir <dir>` | Config directory | `.` |
| `--port <n>` | JSON-RPC TCP port | `9876` |
| `--allow-write-memory` | Enable memory writes | *(disabled)* |

### AI Assistant Integration

Claude Desktop / Cursor:
```json
{
  "mcpServers": {
    "n64-debug-mcp": {
      "command": "uv",
      "args": ["--directory", "D:\\Mupen64MCP\\mcp\\python", "run", "n64-debug-mcp"]
    }
  }
}
```

### Status Dashboard (optional)

```sh
n64-viewer
# or: python start_viewer.py
```

## MCP Tools (47 total)

| Category | Tools |
|----------|-------|
| **Lifecycle** | `n64_start_daemon`, `n64_stop_daemon` |
| **Emulation** | `n64_get_status`, `n64_load_rom`, `n64_close_rom`, `n64_pause`, `n64_resume`, `n64_step_instruction`, `n64_step_frame` |
| **CPU** | `n64_get_pc`, `n64_get_registers` |
| **Memory** | `n64_read_memory`, `n64_write_memory`, `n64_dump_rdram`, `n64_translate_address`, `n64_translate_address_static` |
| **Breakpoints** | `n64_add_breakpoint`, `n64_remove_breakpoint`, `n64_list_breakpoints`, `n64_scan_functions` |
| **Tracing** | `n64_mark_game_state`, `n64_get_trace_events`, `n64_clear_trace_events`, `n64_trace_rom_reads`, `n64_wait_for_breakpoint`, `n64_export_trace`, `n64_track_struct`, `n64_decode_display_list`, `n64_trace_callchain`, `n64_trace_scheduler`, `n64_detect_os` |
| **Input** | `n64_set_controller` |
| **Framebuffer** | `n64_read_framebuffer`, `n64_set_frame_capture_interval`, `n64_get_frame_count`, `n64_get_frame_captures`, `n64_clear_frame_captures`, `n64_wait_for_frame` |
| **Assets** | `n64_discover_assets`, `n64_export_manifest` |
| **PI DMA** | `n64_capture_pi_dma`, `n64_trace_pi_dma` |
| **RSP/SP** | `n64_get_rsp_task`, `n64_trace_rsp_tasks`, `n64_read_sp_memory`, `n64_read_sp_registers`, `n64_check_rsp_health` |

## Project Structure

```
D:\Mupen64MCP\
├── mcp/python/              # Python MCP server (FastMCP)
├── native/                  # C++ daemon + input inject plugin
├── build/mupen64plus/       # Mupen64Plus core + plugins source + build
├── docs/                    # Architecture, engine reverse engineering
├── examples/                # Claude/Cursor config files
└── tests/                   # Test scripts
```

## License

MIT

See [TOOLS.md](TOOLS.md) for detailed MCP tool reference with parameters and return types,
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for design details,
[TROUBLESHOOTING.md](TROUBLESHOOTING.md) for common issues, and
[ROM_TESTING.md](ROM_TESTING.md) for tested ROMs and test results.
