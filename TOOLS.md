# MCP Tools Reference

47 tools organized into 12 categories. Each tool lists its purpose, parameters, return values, and how it works.

---

## Lifecycle

### `n64_start_daemon`

Start the native n64-debug-daemon subprocess and connect to it.

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `core_path` | `string` | — | Path to `mupen64plus.dll` |
| `rom_path` | `string` | `""` | Optional ROM to load immediately |
| `data_dir` | `string` | `""` | Shared data directory |
| `config_dir` | `string` | `""` | Config directory |
| `port` | `int` | `9876` | TCP port for JSON-RPC |
| `allow_write` | `bool` | `false` | Enable memory writes |
| `input_path` | `string` | `""` | Input plugin DLL path |

**Returns** `dict` — daemon status after startup (same as `n64_get_status`).

**How it works** Spawns `n64-debug-daemon.exe` as a subprocess with the given args, waits for it to open its TCP port, then sends a `ping` to confirm connectivity.

---

### `n64_stop_daemon`

Shut down the native daemon and release resources.

**Parameters** None

**Returns** `{"ok": true}` or `{"ok": false, "error": "Not running"}`

**How it works** Sends a `shutdown` JSON-RPC command, then terminates the subprocess and waits for it to exit.

---

## Emulation Control

### `n64_get_status`

Return current daemon and emulator state.

**Parameters** None

**Returns**
| Field | Type | Description |
|-------|------|-------------|
| `core_loaded` | `bool` | Core DLL is loaded |
| `debugger_available` | `bool` | Debugger API is enabled |
| `rom_loaded` | `bool` | ROM is loaded |
| `running` | `bool` | Emulator is executing |
| `paused` | `bool` | Emulator is paused in debugger |
| `frame` | `int` | Current VI frame counter |
| `pc` | `string` | Current program counter (hex) |
| `dbg_run_state` | `int` | Raw Mupen64Plus debug run state |

---

### `n64_load_rom`

Load a `.z64` ROM file and start emulation.

**Parameters**
| Name | Type | Description |
|------|------|-------------|
| `path` | `string` | Path to the ROM file |

**Returns** `{"ok": true}`

**How it works** The daemon calls `CoreDoCommand(M64CMD_ROM_OPEN, ...)`, which resets the CPU, loads the ROM into RDRAM, and starts executing from the reset vector. After loading, the daemon auto-pauses so tools can inspect the initial state.

---

### `n64_close_rom`

Close the currently loaded ROM and stop emulation.

**Parameters** None

**Returns** `{"ok": true}`

---

### `n64_pause`

Pause emulation so registers and memory can be inspected.

**Parameters** None

**Returns** `{"ok": true}`

**How it works** Sends `CoreDoCommand(M64CMD_CORE_STATE_SET, M64CORE_EMU_STATE, ...)` to pause the R4300i core. The core's `onDebuggerUpdate` callback signals a semaphore that the daemon waits on.

---

### `n64_resume`

Resume emulation from a paused state.

**Parameters** None

**Returns** `{"ok": true}`

**How it works** Sends `CoreDoCommand(M64CMD_CORE_STATE_SET, M64CORE_EMU_STATE, ...)` to resume. If a breakpoint is set at the current PC, the daemon temporarily removes it before resuming and re-adds it after the core steps past it, preventing an immediate re-catch.

---

### `n64_step_instruction`

Execute a single instruction and return the new PC.

**Parameters** None

**Returns** `{"pc": "0x..."}`

**Requires** Emulator must be paused.

**How it works** Calls `CoreDoCommand(M64CMD_CORE_STATE_SET, M64CORE_EMU_STATE, ...)` with step mode, then reads the updated PC from registers.

---

### `n64_step_frame`

Execute until the next vertical blanking interval (one frame).

**Parameters** None

**Returns** `{"ok": true}`

**Requires** Emulator must be paused.

**How it works** Resumes execution and waits for the next VI interrupt, then pauses again. With dummy video plugins the VI may not fire, causing step_frame to hang.

---

## CPU Inspection

### `n64_get_pc`

Return the current program counter as a hex string.

**Parameters** None

**Returns** `"0x80123456"` (raw string, not a dict)

**How it works** Reads the CPU's PC register via `CoreDoCommand(M64CMD_READ_REGISTER, ...)`.

---

### `n64_get_registers`

Return all 32 general-purpose registers and the PC.

**Parameters** None

**Returns**
```json
{
  "gpr": ["0x00000000", "0x8014E400", ...],
  "pc": "0x80123456"
}
```

GPRs are hex strings ordered $0 (zero) through $31 (ra). GPR $0 is always `0x00000000`.

---

## Memory

### `n64_read_memory`

Read bytes from the emulated N64 address space.

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `address` | `string` | — | Hex virtual or physical address |
| `size` | `int` | `64` | Number of bytes (max 1 MB) |

**Returns**
```json
{
  "address": "0x80000000",
  "size": 64,
  "hex": "3C1A8012275A4C60..."
}
```

**How it works** Uses `CoreDoCommand(M64CMD_READ_MEM, ...)` to read from the emulated memory bus. Addresses in KSEG0 (`0x80000000-0x9FFFFFFF`) are identity-mapped to physical RDRAM. Addresses in KSEG1 (`0xA0000000-0xBFFFFFFF`) are uncached equivalents.

---

### `n64_write_memory`

Write bytes to emulated N64 memory (disabled by default).

**Parameters**
| Name | Type | Description |
|------|------|-------------|
| `address` | `string` | Hex virtual address to write to |
| `data` | `string` | Hex-encoded bytes, e.g. `"AABBCCDD"` |

**Returns** `{"ok": true}` or error if writes are not enabled.

**How it works** Calls `CoreDoCommand(M64CMD_WRITE_MEM, ...)`. Only works if the daemon was started with `--allow-write-memory` or `allow_write=true`.

---

### `n64_dump_rdram`

Dump a range of RDRAM starting at `0x80000000`.

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `size` | `int` | `8388608` | Number of bytes (default = full 8 MB RDRAM) |

**Returns** First 4 KB as hex string. Useful for capturing RDRAM snapshots for offline analysis.

---

### `n64_translate_address`

Translate a virtual address to a physical address using the core's TLB.

**Parameters**
| Name | Type | Description |
|------|------|-------------|
| `vaddr` | `string` | Virtual address to translate |

**Returns** `{"physical_address": "0x..."}`. For KSEG0 addresses this returns the identity mapping. For TLB-mapped addresses (e.g. game code in `0x80000000` range), returns the physical RDRAM address.

---

### `n64_translate_address_static`

Translate between ROM file offset and RDRAM KSEG0/KSEG1 addresses using the standard N64 memory mapping.

**Parameters** (provide one, get the other)
| Name | Type | Description |
|------|------|-------------|
| `file_offset` | `string` | ROM file offset (hex) |
| `kseg0` | `string` | RDRAM KSEG0 address (hex) |

**Returns**
```json
{
  "file_offset": "0x268C0",
  "kseg0": "0x801258C0",
  "kseg1": "0xA01258C0"
}
```

**How it works** Uses the standard N64 mapping formula: `RDRAM = 0x80100000 + (file_offset - 0x1000)`. The first `0x1000` bytes of the ROM (header + IPL3) are mapped to `0x80000000-0x80000FFF`. No emulator state required.

---

## Breakpoints & Code Analysis

### `n64_add_breakpoint`

Set an execution breakpoint at a virtual address.

**Parameters**
| Name | Type | Description |
|------|------|-------------|
| `address` | `string` | Hex address to break on |

**Returns** `{"index": N}` — the daemon's internal breakpoint index.

**How it works** Calls the core's `add_breakpoint_struct` API. The index is tracked in the daemon's `mOwnedBps` vector for reliable remove/list operations. The core auto-removes the BP at the current PC when resuming (to prevent immediate re-catch).

---

### `n64_remove_breakpoint`

Remove a breakpoint by its daemon index.

**Parameters**
| Name | Type | Description |
|------|------|-------------|
| `index` | `int` | Breakpoint index from `n64_add_breakpoint` |

**Returns** `{"ok": true}`

**How it works** First tries `CoreDoCommand(M64CMD_BREAKPOINT, M64BP_DELETE_BY_ADDR, ...)`. If that fails, falls back to `M64BP_DELETE_BY_INDEX`. This two-step approach handles cases where the core's index tracking has shifted.

---

### `n64_list_breakpoints`

List all active breakpoints.

**Parameters** None

**Returns**
```json
[
  {"index": 0, "address": "0x80123456", "flags": 1, "enabled": true},
  {"index": 1, "address": "0x801789AB", "flags": 1, "enabled": true}
]
```

**How it works** Iterates the daemon's `mOwnedBps` vector and queries each breakpoint's current state from the core.

---

### `n64_scan_functions`

Scan RDRAM for MIPS function prologues and return all entry points.

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `start_addr` | `string` | `0x80100000` | Start of scan range |
| `end_addr` | `string` | `0x80800000` | End of scan range |

**Returns**
```json
[
  {"address": "0x80100050", "stack_size": 32, "approx_size": 40},
  {"address": "0x80100080", "stack_size": 64, "approx_size": 80}
]
```

**How it works** Reads RDRAM within the scan range and looks for the standard MIPS function prologue pattern: `ADDIU sp, sp, -N` followed by `SW ra, N-4(sp)`. The `stack_size` is extracted from the `ADDIU` immediate, and `approx_size` is estimated by scanning forward until the next prologue or 200 instructions.

**Note** The emulator must have loaded game code into RDRAM (PC in KSEG0 range). Returns 0 results if PC is in SP boot area (`0xA4000000`).

---

## Tracing & Events

### `n64_mark_game_state`

Tag the current game state so subsequent trace events are labelled.

**Parameters**
| Name | Type | Description |
|------|------|-------------|
| `label` | `string` | Short name like `"title_screen"` |
| `notes` | `string` | Optional description |

**Returns** `{"ok": true}`

**How it works** Sets an internal label string in the daemon that gets prepended to all subsequent trace events. Useful for filtering events by game state during analysis.

---

### `n64_get_trace_events`

Return recent trace events (breakpoint hits, DMA transfers, etc.).

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `count` | `int` | `100` | Number of recent events to return |

**Returns** `[{event objects}]`

**How it works** Reads from the daemon's in-memory ring buffer of trace events. Events include type (breakpoint, DMA, scheduler, struct write), timestamp, and event-specific data.

---

### `n64_clear_trace_events`

Clear the trace event buffer.

**Parameters** None

**Returns** `{"ok": true}`

---

### `n64_trace_rom_reads`

Enable or disable ROM-read tracing.

**Parameters**
| Name | Type | Description |
|------|------|-------------|
| `enable` | `bool` | True to enable, False to disable |

**Returns** `{"ok": true}`

**How it works** When enabled, the daemon captures every PI DMA completion event (ROM→RDRAM transfer) and logs it as a trace event with source cart address, destination DRAM address, and transfer size.

---

### `n64_wait_for_breakpoint`

Block until a breakpoint fires (emulator becomes paused).

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `timeout` | `float` | `10.0` | Max seconds to wait |
| `poll_interval` | `float` | `0.05` | Seconds between status polls |

**Returns** Status dict when breakpoint hits, or error on timeout.

**How it works** Client-side polling loop: calls `n64_get_status` at `poll_interval` until `paused=true`. Use after `n64_resume` to wait for the CPU to hit a breakpoint.

---

### `n64_export_trace`

Export trace events to a JSON file on disk.

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `path` | `string` | — | Output file path |
| `count` | `int` | `0` | Number of events (0 = all) |

**Returns** `{"ok": true, "path": "...", "events_written": N}`

---

### `n64_track_struct`

Watch writes to a memory region and log every change.

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `address` | `string` | — | Hex address of the struct to watch |
| `size` | `int` | `16` | Number of bytes to watch (max 4096) |
| `enable` | `bool` | `true` | Enable or disable tracking |

**Returns** `{"ok": true, "bp_index": N}` on enable.

**How it works** Sets a write breakpoint on the address range. Each write is captured transparently (auto-resume) with offset, old value, new value, and PC of the writing instruction. Read captured writes via `n64_get_trace_events`.

---

### `n64_decode_display_list`

Read a display list from memory and decode each GBI command.

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `address` | `string` | — | Hex address of the display list |
| `size` | `int` | `256` | Bytes to decode (must be multiple of 8) |

**Returns**
```json
{
  "address": "0x802C0000",
  "command_count": 12,
  "commands": [
    {"offset": "0x0", "domain": "RSP", "name": "G_VTX", "args": "n=5 v0=0 vaddr=0x04000000", "raw": "0xBA000005 0x04000000"},
    {"offset": "0x8", "domain": "RSP", "name": "G_TRI1", "args": "i0=0 i1=1 i2=2 flag=0", "raw": "0xBF000000 0x00010200"},
    {"offset": "0x10", "domain": "RSP", "name": "G_ENDDL", "args": "", "raw": "0xDF000000 0x00000000"}
  ]
}
```

**How it works** Reads the raw bytes from the given address, interprets each 8-byte pair as a GBI command (opcode + operand), and looks up the opcode in a table of known F3D/F3DEX2 commands. Supports RDP commands (texture operations, sync, color) and RSP commands (vertex, triangle, matrix, display list branching). Stops at `G_ENDDL`.

---

### `n64_trace_callchain`

Set execution breakpoints at multiple function addresses and capture call context.

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `addresses` | `string` | — | Comma-separated hex addresses |
| `enable` | `bool` | `true` | Enable or disable tracing |

**Returns** `{"ok": true, "bps_set": N}` on enable.

**How it works** Sets exec breakpoints at each given address. On each hit, captures RA (return address) and A0-A3 (first 4 arguments), then auto-resumes. Read captured calls via `n64_get_trace_events`. Use `n64_detect_os` to find function addresses.

---

### `n64_trace_scheduler`

Trace the game's RTOS scheduler by monitoring context switches and run queue.

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `ctx_switch_addr` | `string` | — | Address of context-switch function (or `"0"` to skip) |
| `queue_addr` | `string` | `""` | Address of run queue head (or `""` to skip) |
| `enable` | `bool` | `true` | Enable or disable tracing |

**Returns** `{"ok": true}` on enable.

**How it works** Sets an exec breakpoint on the context-switch function. On each hit captures the old TCB (A0) and new TCB (A1), plus a timestamp. If `queue_addr` is provided, also monitors writes to the run queue head pointer. Auto-resumes after each capture.

---

### `n64_detect_os`

Detect the OS type, boot flow, RSP microcode, and thread function addresses.

**Parameters** None

**Returns**
```json
{
  "rom": {"name": "CRUISN USA", "crc": "FF2F2FB4 D161149A", "entry": "0x8011C450", "country": "USA"},
  "boot": {"boot_type": "custom", "pif_jump": "0x8011C450", "reset_trampoline": ["0x3C1A8012", "0x275A4C60"]},
  "os": {"type": "custom", "detected_functions": []},
  "rsp": {"ucode_type": "custom_gfx", "ucode_boot": "0x80000000", "dmem_set": true}
}
```

**How it works** Reads the ROM header, inspects the reset vector at `0x80000000`, checks the PIF ROM jump target, scans for libultra function signatures (`osCreateThread`, `osStartThread`, `osYieldThread`, `__osRunQueue`), reads the RSP task header from SP DMEM at `0xA4000FC0`, and classifies the RSP microcode by checking known ucode boot hashes.

---

## PI DMA

### `n64_capture_pi_dma`

Read the current PI DMA registers (last transfer details).

**Parameters** None

**Returns**
```json
{
  "dram_addr": "0x00046370",
  "cart_addr": "0x1007F220",
  "rd_len": "0x0000007F",
  "wr_len": "0x0000007F",
  "status": "0x00000000"
}
```

**How it works** Reads the PI registers `PI_DRAM_ADDR` (0xA4600000), `PI_CART_ADDR` (0xA4600004), `PI_RD_LEN` (0xA4600008), `PI_WR_LEN` (0xA460000C), and `PI_STATUS` (0xA4600010). This reveals the last ROM→RDRAM or RDRAM→ROM transfer.

---

### `n64_trace_pi_dma`

Enable or disable automatic PI DMA tracing on breakpoint hit.

**Parameters**
| Name | Type | Description |
|------|------|-------------|
| `enable` | `bool` | True to enable, False to disable |

**Returns** `{"ok": true}`

**How it works** When enabled, the daemon captures the PI DMA registers on every breakpoint hit and logs the transfer as a trace event with source, destination, and size.

---

## RSP / SP

### `n64_get_rsp_task`

Read the current RSP task header from SP DMEM.

**Parameters** None

**Returns**
```json
{
  "type": "0x00000001",
  "flags": "0x00000000",
  "ucode_boot": "0x80000000",
  "ucode": "0x80000000",
  "ucode_data": "0x80000000",
  "data_ptr": "0x80300000",
  "data_size": "0x00001000"
}
```

**How it works** Reads the `osSpTask` structure at SP DMEM address `0xA4000FC0`. This 64-byte structure contains pointers to the RSP microcode, microcode data, and task data, plus the task type and flags.

---

### `n64_trace_rsp_tasks`

Enable or disable RSP task submission tracing.

**Parameters**
| Name | Type | Description |
|------|------|-------------|
| `enable` | `bool` | True to enable, False to disable |

**Returns** `{"ok": true}`

**How it works** Sets a write breakpoint on the SP task register. When the CPU writes a new task header, the daemon captures it and logs the task type and ucode pointers.

---

### `n64_read_sp_memory`

Read bytes from SP memory (DMEM or IMEM).

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `offset` | `string` | — | Hex offset. `0x000-0xFFF` = DMEM, `0x1000-0x1FFF` = IMEM |
| `size` | `int` | `64` | Number of bytes (max 8 KB) |

**Returns**
```json
{
  "offset": "0xFC0",
  "size": 64,
  "hex": "0100000000000000..."
}
```

**How it works** Reads from SP memory address space via the core's debugger. DMEM (`0xA4000000-0xA4000FFF`) contains the RSP task header and microcode data. IMEM (`0xA4001000-0xA4001FFF`) contains the RSP microcode instructions.

---

### `n64_read_sp_registers`

Read SP control registers.

**Parameters** None

**Returns**
```json
{
  "status": "0x00000001",
  "dma_full": "0x00000000",
  "dma_busy": "0x00000000",
  "pc": "0x00000000"
}
```

**How it works** Reads the SP registers via the core's debugger: `SP_STATUS` (0xA4040000), `SP_DMA_FULL` (0xA4040004), `SP_DMA_BUSY` (0xA4040008), `SP_PC` (0xA4080000). Useful for checking if the RSP is idle (`SP_PC = 0` with RSP-HLE) or busy running microcode.

---

### `n64_check_rsp_health`

Check RSP health: status, ucode hash/type, task state.

**Parameters** None

**Returns**
```json
{
  "sp_pc": "0x00000000",
  "sp_status": "0x00000001",
  "ucode_hash": "0x0D968558",
  "ucode_type": "idle_or_hle",
  "task_active": false,
  "task_type": 0,
  "rsp_hle": true
}
```

**How it works** Combines `n64_read_sp_registers` with IMEM content analysis. Reads 1 KB of IMEM, computes CRC32, and compares against known ucode hashes to classify: `custom_gfx`, `f3dex2`, `f3dex`, `f3d`, `audio`, `jpeg`, or `idle_or_hle`. Detects RSP-HLE when `SP_PC = 0` (the HLE plugin doesn't set SP_PC).

---

## Framebuffer & Video

### `n64_read_framebuffer`

Read the current N64 framebuffer from RDRAM via VI registers.

**Parameters** None

**Returns**
```json
{
  "width": 320,
  "height": 240,
  "bpp": 4,
  "hex": "479E9E9E..."
}
```

`bpp`: 2 = RGBA5551, 4 = RGBA8888.
The `hex` string contains raw pixel data (width × height × bpp bytes).

**How it works** Reads the VI registers `VI_ORIGIN` (0xA4400004) for the framebuffer address, `VI_WIDTH` (0xA4400008) for the scanline width, and `VI_STATUS` (0xA4400000) for the bit depth. Then reads that many bytes from RDRAM. With dummy gfx, pixels are zero (no RDP processing). With Rice video + RSP-HLE, pixels are rendered.

---

### `n64_set_frame_capture_interval`

Enable or disable automatic framebuffer capture.

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `interval` | `int` | `0` | Capture every N frames (0 = disable) |

**Returns** `{"ok": true}`

**How it works** Sets a VI breakpoint that fires every N frames. On each fire, captures the framebuffer (dimensions + pixel data) into an in-memory buffer (max 100 captures). Read captures via `n64_get_frame_captures`.

---

### `n64_get_frame_count`

Get the current VI frame counter.

**Parameters** None

**Returns** `{"frame_count": 12345}`

---

### `n64_get_frame_captures`

Get list of auto-captured framebuffers.

**Parameters** None

**Returns**
```json
[
  {"frame": 10, "width": 320, "height": 240, "bpp": 4, "size": 307200},
  {"frame": 15, "width": 320, "height": 240, "bpp": 4, "size": 307200}
]
```

---

### `n64_clear_frame_captures`

Clear the auto-captured framebuffer list.

**Parameters** None

**Returns** `{"ok": true}`

---

### `n64_wait_for_frame`

Wait until the emulator reaches a specific frame count.

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `target` | `int` | — | Target frame number to wait for |
| `timeout_ms` | `int` | `5000` | Maximum wait time in milliseconds |

**Returns** `{"ok": true, "frame": N}` on success, or `{"ok": false, "reason": "timeout", "frame": N}` on timeout.

**Requires** Emulator must be running (call `n64_resume` first). The daemon polls the frame counter in a loop until the target is reached.

---

## Asset Discovery

### `n64_discover_assets`

Scan ROM, RDRAM, and RSP state to produce a structured asset manifest.

**Parameters** None

**Returns** A structured JSON manifest with:
- `rom_header`: Name, CRC, country, entry point
- `rom_regions`: Classified regions of the ROM file (code, audio, texture, display_list, etc.)
- `rdram_regions`: Classified regions of RDRAM at runtime
- `boot_flow`: Reset vector, PIF jump target, OS type
- `rsp_task`: Current RSP task if active

**How it works** Reads the first 4 KB of the ROM header, then scans every 4 KB block of ROM and RDRAM, classifying by content type: high-entropy = texture/compressed, MIPS instructions = code, repeating patterns = audio, F3DEX2 signatures = display lists. Also inspects the boot flow and RSP state.

---

### `n64_export_manifest`

Scan assets and export the full manifest to a JSON file on disk.

**Parameters**
| Name | Type | Description |
|------|------|-------------|
| `path` | `string` | Output file path |

**Returns** `{"ok": true, "path": "...", "rom_regions": N, "rdram_regions": N}`

---

## Input Injection

### `n64_set_controller`

Inject controller state into the running emulator.

**Parameters**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `channel` | `int` | `0` | Controller port (0-3) |
| `buttons` | `string` | `""` | Space-separated button names or hex value |
| `x` | `int` | `0` | Analog stick X (-128 to 127) |
| `y` | `int` | `0` | Analog stick Y (-128 to 127) |
| `sticky` | `bool` | `false` | Persist state across frames |

**Returns** `{"ok": true}`

**Supported button names** `A`, `B`, `START`, `Z`, `L`, `R`, `U_DPAD`, `D_DPAD`, `L_DPAD`, `R_DPAD`, `U_C`, `D_C`, `L_C`, `R_C`

**How it works** Parses button names into a bitmask, clamps analog values, and calls the input inject plugin's `SetControllerState` via the daemon. In sticky mode, the state persists until explicitly changed. In one-shot mode, the state is sent for one frame then cleared. Requires `--input mupen64plus-input-inject.dll` at daemon startup.

---

## Tool Index

| # | Tool | Category | Page |
|---|------|----------|------|
| 1 | `n64_start_daemon` | Lifecycle | 2 |
| 2 | `n64_stop_daemon` | Lifecycle | 2 |
| 3 | `n64_get_status` | Emulation | 3 |
| 4 | `n64_load_rom` | Emulation | 3 |
| 5 | `n64_close_rom` | Emulation | 3 |
| 6 | `n64_pause` | Emulation | 3 |
| 7 | `n64_resume` | Emulation | 3 |
| 8 | `n64_step_instruction` | Emulation | 4 |
| 9 | `n64_step_frame` | Emulation | 4 |
| 10 | `n64_get_pc` | CPU | 4 |
| 11 | `n64_get_registers` | CPU | 4 |
| 12 | `n64_read_memory` | Memory | 5 |
| 13 | `n64_write_memory` | Memory | 5 |
| 14 | `n64_dump_rdram` | Memory | 5 |
| 15 | `n64_translate_address` | Memory | 5 |
| 16 | `n64_translate_address_static` | Memory | 6 |
| 17 | `n64_add_breakpoint` | Breakpoints | 6 |
| 18 | `n64_remove_breakpoint` | Breakpoints | 6 |
| 19 | `n64_list_breakpoints` | Breakpoints | 7 |
| 20 | `n64_scan_functions` | Breakpoints | 7 |
| 21 | `n64_mark_game_state` | Tracing | 7 |
| 22 | `n64_get_trace_events` | Tracing | 8 |
| 23 | `n64_clear_trace_events` | Tracing | 8 |
| 24 | `n64_trace_rom_reads` | Tracing | 8 |
| 25 | `n64_wait_for_breakpoint` | Tracing | 8 |
| 26 | `n64_export_trace` | Tracing | 8 |
| 27 | `n64_track_struct` | Tracing | 8 |
| 28 | `n64_decode_display_list` | Tracing | 9 |
| 29 | `n64_trace_callchain` | Tracing | 9 |
| 30 | `n64_trace_scheduler` | Tracing | 9 |
| 31 | `n64_detect_os` | Tracing | 10 |
| 32 | `n64_capture_pi_dma` | PI DMA | 10 |
| 33 | `n64_trace_pi_dma` | PI DMA | 10 |
| 34 | `n64_get_rsp_task` | RSP/SP | 11 |
| 35 | `n64_trace_rsp_tasks` | RSP/SP | 11 |
| 36 | `n64_read_sp_memory` | RSP/SP | 11 |
| 37 | `n64_read_sp_registers` | RSP/SP | 11 |
| 38 | `n64_check_rsp_health` | RSP/SP | 12 |
| 39 | `n64_read_framebuffer` | Framebuffer | 12 |
| 40 | `n64_set_frame_capture_interval` | Framebuffer | 12 |
| 41 | `n64_get_frame_count` | Framebuffer | 13 |
| 42 | `n64_get_frame_captures` | Framebuffer | 13 |
| 43 | `n64_clear_frame_captures` | Framebuffer | 13 |
| 44 | `n64_wait_for_frame` | Framebuffer | 13 |
| 45 | `n64_discover_assets` | Assets | 13 |
| 46 | `n64_export_manifest` | Assets | 14 |
| 47 | `n64_set_controller` | Input | 14 |
