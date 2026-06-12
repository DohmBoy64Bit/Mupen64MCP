"""n64-debug-mcp — MCP server wrapping n64-debug-daemon.

Exposes AI-friendly tools for inspecting and controlling a running N64 ROM
via Mupen64Plus's debugger API.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Any

from mcp.server.fastmcp import FastMCP

from n64_debug_mcp.daemon_client import DaemonClient, DaemonConfig

mcp = FastMCP("n64-debug-mcp")

# ── global state ───────────────────────────────────────────────
_daemon: DaemonClient | None = None


def _client() -> DaemonClient:
    if _daemon is None:
        raise RuntimeError("Daemon not started — call n64_start_daemon first")
    return _daemon


def _resolve_path(p: str) -> str:
    return str(Path(p).resolve()) if p else p


# ── lifecycle tools ────────────────────────────────────────────


@mcp.tool()
def n64_start_daemon(
    core_path: str,
    rom_path: str = "",
    data_dir: str = "",
    config_dir: str = "",
    port: int = 9876,
    allow_write: bool = False,
    input_path: str = "",
) -> dict[str, Any]:
    """Start the native n64-debug-daemon subprocess and connect to it.

    core_path: path to mupen64plus.dll
    rom_path:  optional path to a .z64 ROM to load immediately
    port:      TCP port for JSON-RPC (default 9876)
    input_path: path to input plugin DLL (default: built-in dummy)
    """
    global _daemon
    if _daemon is not None:
        return {"error": "Daemon already running — call n64_stop_daemon first"}

    cfg = DaemonConfig(
        core_path=_resolve_path(core_path),
        rom_path=_resolve_path(rom_path) if rom_path else None,
        data_dir=_resolve_path(data_dir) if data_dir else None,
        config_dir=_resolve_path(config_dir) if config_dir else None,
        port=port,
        allow_write=allow_write,
        input=_resolve_path(input_path) if input_path else "dummy",
    )
    _daemon = DaemonClient(cfg)
    _daemon.start()
    return _daemon.call("status")


@mcp.tool()
def n64_stop_daemon() -> dict[str, Any]:
    """Shut down the native daemon and release resources."""
    global _daemon
    if _daemon is None:
        return {"ok": False, "error": "Not running"}
    _daemon.stop()
    _daemon = None
    return {"ok": True}


# ── emulation control ──────────────────────────────────────────


@mcp.tool()
def n64_get_status() -> dict[str, Any]:
    """Return current daemon and emulator state.

    Includes: core_loaded, rom_loaded, running, paused, frame, pc.
    """
    return _client().call("status")


@mcp.tool()
def n64_load_rom(path: str) -> dict[str, Any]:
    """Load a .z64 ROM file and start emulation.

    path: absolute or relative path to the ROM.
    """
    return _client().call("load_rom", {"path": _resolve_path(path)})


@mcp.tool()
def n64_close_rom() -> dict[str, Any]:
    """Close the currently loaded ROM and stop emulation."""
    return _client().call("close_rom")


@mcp.tool()
def n64_pause() -> dict[str, Any]:
    """Pause emulation so registers and memory can be inspected."""
    return _client().call("pause")


@mcp.tool()
def n64_resume() -> dict[str, Any]:
    """Resume emulation from a paused state."""
    return _client().call("resume")


@mcp.tool()
def n64_step_instruction() -> dict[str, Any]:
    """Execute a single instruction and return the new PC."""
    return _client().call("step_instruction")


@mcp.tool()
def n64_step_frame() -> dict[str, Any]:
    """Execute until the next vertical blanking interval (one frame)."""
    return _client().call("step_frame")


# ── CPU inspection ─────────────────────────────────────────────


@mcp.tool()
def n64_get_pc() -> str:
    """Return the current program counter (PC) as a hex string."""
    return _client().call("get_pc")


@mcp.tool()
def n64_get_registers() -> dict[str, Any]:
    """Return all 32 GPRs and the PC.

    Returns: {"gpr": [...], "pc": "0x..."}
    """
    return _client().call("get_registers")


# ── memory ─────────────────────────────────────────────────────


@mcp.tool()
def n64_read_memory(address: str, size: int = 64) -> dict[str, Any]:
    """Read bytes from the emulated N64 address space.

    address: hex virtual or physical address, e.g. "0x80340000"
    size:    number of bytes to read (max 1 MB)
    Returns: {"address": "...", "size": N, "hex": "..."}
    """
    return _client().call("read_mem", {"address": address, "size": size})


@mcp.tool()
def n64_write_memory(address: str, data: str) -> dict[str, Any]:
    """Write bytes to emulated N64 memory (disabled by default).

    address: hex virtual address to write to
    data:    hex-encoded byte string, e.g. "AABBCCDD"
    """
    return _client().call("write_mem", {"address": address, "data": data})


@mcp.tool()
def n64_dump_rdram(size: int = 0x800000) -> dict[str, Any]:
    """Dump a range of RDRAM starting at 0x80000000.

    size: number of bytes (default 8 MB = full RDRAM).
    Returns first 4 KB as hex in the response.
    """
    return _client().call("dump_rdram", {"size": size})


@mcp.tool()
def n64_translate_address(vaddr: str) -> dict[str, Any]:
    """Translate a virtual address to a physical address.

    Uses the core's debugger TLB translation.  For KSEG0 addresses
    (0x80000000-0x9FFFFFFF) this usually returns the identity mapping.
    """
    return _client().call("translate_address", {"vaddr": vaddr})


@mcp.tool()
def n64_translate_address_static(
    file_offset: str = "",
    kseg0: str = "",
) -> dict[str, Any]:
    """Translate between ROM file offset and RDRAM KSEG0/KSEG1 addresses.

    Provide either file_offset or kseg0 to get the other forms.
    Uses the standard mapping: file offset 0x1000 → RDRAM 0x80100000.
    """
    params = {}
    if file_offset:
        params["file_offset"] = file_offset
    if kseg0:
        params["kseg0"] = kseg0
    return _client().call("translate_address_static", params)


# ── breakpoints ────────────────────────────────────────────────


@mcp.tool()
def n64_add_breakpoint(address: str) -> dict[str, Any]:
    """Set an execution breakpoint at a virtual address.

    address: hex virtual address to break on, e.g. "0x802A1234"
    Returns: {"index": N}
    """
    return _client().call("add_exec_breakpoint", {"address": address})


@mcp.tool()
def n64_remove_breakpoint(index: int) -> dict[str, Any]:
    """Remove a breakpoint by index."""
    return _client().call("remove_breakpoint", {"index": index})


@mcp.tool()
def n64_list_breakpoints() -> list[dict[str, Any]]:
    """List all active breakpoints with addresses, flags, and enabled status."""
    return _client().call("list_breakpoints")


@mcp.tool()
def n64_scan_functions(
    start_addr: str = "0x80100000",
    end_addr: str = "0x80800000",
) -> list[dict[str, Any]]:
    """Scan RDRAM for MIPS function prologues and return all entry points.

    Finds ADDIU sp,sp,-N + SW ra,N(sp) patterns and returns each function's
    address, stack size, and approximate code size. Default scan range covers
    the full 8MB RDRAM.
    """
    return _client().call("scan_functions", {
        "start_addr": start_addr,
        "end_addr": end_addr,
    })


# ── tracing ────────────────────────────────────────────────────


@mcp.tool()
def n64_mark_game_state(label: str, notes: str = "") -> dict[str, Any]:
    """Tag the current game state so subsequent trace events are labelled.

    label: short name like "title_screen" or "track_00_loading"
    notes: optional description
    """
    return _client().call("mark_game_state", {"label": label, "notes": notes})


@mcp.tool()
def n64_get_trace_events(count: int = 100) -> list[dict[str, Any]]:
    """Return recent trace events (step, breakpoint hits, etc.)."""
    return _client().call("get_trace_events", {"count": count})


@mcp.tool()
def n64_clear_trace_events() -> dict[str, Any]:
    """Clear the trace event buffer."""
    return _client().call("clear_events")


@mcp.tool()
def n64_trace_rom_reads(enable: bool) -> dict[str, Any]:
    """Enable or disable ROM-read tracing at the emulator level.

    When enabled, every PI DMA / cartridge read is logged as a trace event.
    """
    return _client().call("trace_rom_reads", {"enable": enable})


@mcp.tool()
def n64_wait_for_breakpoint(timeout: float = 10.0, poll_interval: float = 0.05) -> dict[str, Any]:
    """Block until a breakpoint fires (emulator becomes paused), up to `timeout` seconds.

    Polls status in a loop.  Use after resume() to wait for the CPU to hit a BP.
    Returns the final status dict, or an error if timeout is reached.
    """
    import time
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = _client().call("status")
        if status.get("paused"):
            return status
        time.sleep(poll_interval)
    return {"error": f"Timeout ({timeout}s) waiting for breakpoint",
            "last_status": _client().call("status")}


@mcp.tool()
def n64_export_trace(path: str, count: int = 0) -> dict[str, Any]:
    """Export trace events to a JSON file.

    path:   file path for the output JSON (e.g. "trace_export.json")
    count:  number of recent events to export (0 = all events)
    """
    events = _client().call("get_trace_events", {"count": count})
    import json as _json
    with open(path, "w") as f:
        _json.dump(events, f, indent=2)
    return {"ok": True, "path": path, "events_written": len(events)}


# ── PI DMA ───────────────────────────────────────────────────


@mcp.tool()
def n64_capture_pi_dma() -> dict[str, Any]:
    """Read the current PI DMA registers (DRAM_ADDR, CART_ADDR, RD_LEN,
    WR_LEN, STATUS).

    Use this to inspect the last PI DMA transfer (ROM → RDRAM or
    RDRAM → ROM) and determine source/destination/size.
    """
    return _client().call("capture_pi_dma")


@mcp.tool()
def n64_trace_pi_dma(enable: bool) -> dict[str, Any]:
    """Enable or disable automatic PI DMA tracing.

    When enabled, every PI DMA completion is logged as a trace
    event with source (cart address), destination (DRAM address),
    and transfer size.
    """
    return _client().call("enable_pi_dma_trace", {"enable": enable})


# ── RSP / SP ───────────────────────────────────────────────────


@mcp.tool()
def n64_get_rsp_task() -> dict[str, Any]:
    """Read the current RSP task header from SP DMEM (osSpTask at 0xA4000FC0).

    Returns: type, flags, ucode ptrs, data ptrs, and sizes.
    Use this to inspect what microcode the RSP is about to execute.
    """
    return _client().call("get_rsp_task")


@mcp.tool()
def n64_trace_rsp_tasks(enable: bool) -> dict[str, Any]:
    """Enable or disable RSP task submission tracing.

    When enabled, every RSP task submission is logged as a trace event.
    """
    return _client().call("trace_rsp_tasks", {"enable": enable})


@mcp.tool()
def n64_read_sp_memory(offset: str, size: int = 64) -> dict[str, Any]:
    """Read bytes from SP memory (DMEM or IMEM).

    offset: hex offset within SP memory space (0x000-0xFFF = DMEM,
            0x1000-0x1FFF = IMEM), e.g. "0xFC0" for task header
    size:   number of bytes to read (max 8 KB)
    Returns: {"offset": "...", "size": N, "hex": "..."}
    """
    return _client().call("read_sp_mem", {"offset": offset, "size": size})


@mcp.tool()
def n64_read_sp_registers() -> dict[str, Any]:
    """Read SP control registers (MEM_ADDR, DRAM_ADDR, RD_LEN, WR_LEN,
    STATUS, DMA_FULL, DMA_BUSY, PC).

    Useful for checking RSP/DMA status and the current SP program counter.
    """
    return _client().call("read_sp_regs")


@mcp.tool()
def n64_check_rsp_health() -> dict[str, Any]:
    """Check RSP health: status, ucode hash/type, task state.

    Returns: rsp_hle, sp_pc, sp_status, ucode_hash, ucode_type,
             task_active, task_type. Use to diagnose RSP-HLE conflicts
             and determine if the loaded ucode is GFX or audio.
    """
    return _client().call("rsp_health_check")


# ── display list decoder ─────────────────────────────────────

_RDP_CMDS: dict[int, tuple[str, str]] = {
    0xE6: ("G_SETCIMG", "img_addr=0x{w2:08X}"),
    0xE5: ("G_SETZIMG", "img_addr=0x{w2:08X}"),
    0xFD: ("G_SETTIMG", "fmt={fmt} siz={siz} width={width} addr=0x{addr:08X}"),
    0xFC: ("G_SETCOMBINE", "m1=0x{w1:08X} m2=0x{w2:08X}"),
    0xFA: ("G_SETENVCOLOR", "rgba=0x{w2:08X}"),
    0xF9: ("G_SETPRIMCOLOR", "rgba=0x{w2:08X} min_lvl={min_lvl}"),
    0xF8: ("G_SETBLENDCOLOR", "rgba=0x{w2:08X}"),
    0xF7: ("G_SETFOGCOLOR", "rgba=0x{w2:08X}"),
    0xF6: ("G_SETFILLCOLOR", "rgba=0x{w2:08X}"),
    0xF4: ("G_FILLRECT", "x1={x1} y1={y1} x2={x2} y2={y2}"),
    0xF2: ("G_SETTILESIZE", "uls={uls} ult={ult} lrs={lrs} lrt={lrt}"),
    0xF3: ("G_LOADBLOCK", "uls={uls} ult={ult} lrs={lrs} dxt={dxt}"),
    0xF1: ("G_LOADTILE", "uls={uls} ult={ult} lrs={lrs} lrt={lrt}"),
    0xF5: ("G_SETTILE", "fmt={fmt} siz={siz} line={line} tmem={tmem} tile={tile} palette={pal} ct={ct} mt={mt} mask_s={msk_s} shift_s={shf_s} mask_t={msk_t} shift_t={shf_t}"),
    0xE8: ("G_RDPLOADSYNC", ""),
    0xE9: ("G_RDPPIPESYNC", ""),
    0xEA: ("G_RDPTILESYNC", ""),
    0xEB: ("G_RDPFULLSYNC", ""),
    0xEC: ("G_SETKEYGB", "wg={wg} wb={wb} center=0x{center:04X} scale=0x{scale:04X}"),
    0xED: ("G_SETKEYR", "wr={wr} center=0x{center:04X} scale=0x{scale:04X}"),
    0xEE: ("G_SETCONVERT", "k=0x{w1:04X}{w2:08X}"),
    0xEF: ("G_SETSCISSOR", "x0={x0} y0={y0} x1={x1} y1={y1}"),
    0xF0: ("G_SETPRIMDEPTH", "z={z} dz={dz}"),
    0xE7: ("G_RDPSETOTHERMODE", "mode0=0x{w1:08X} mode1=0x{w2:08X}"),
    0xE4: ("G_TEXRECT", "x0={x0} y0={y0} x1={x1} y1={y1}"),
    0xE3: ("G_TEXRECTFLIP", "x0={x0} y0={y0} x1={x1} y1={y1}"),
    0xE1: ("G_RDPHALF_1", "val=0x{w2:08X}"),
    0xE2: ("G_RDPHALF_2", "val=0x{w2:08X}"),
}

_RSP_CMDS: dict[int, tuple[str, str]] = {
    0xBA: ("G_VTX", "n={n} v0={v0} vaddr=0x{w2:08X}"),
    0xBF: ("G_TRI1", "i0={i2} i1={i1} i2={i0} flag={flag}"),
    0xBE: ("G_TRI2", "i0={i3} i1={i4} i2={i5} i3={i0} i4={i1} i5={i2}"),
    0xDF: ("G_ENDDL", ""),
    0xDE: ("G_DL", "addr=0x{w2:08X} push={push}"),
    0xBB: ("G_MODIFYVTX", "idx={idx} ofs={ofs} val=0x{w2:08X}"),
    0xDA: ("G_MOVEMEM", "idx={idx} ofs={ofs} len={datalen} addr=0x{addr:08X}"),
    0xDB: ("G_MOVEWORD", "idx={idx} ofs={ofs} data=0x{w2:08X}"),
    0xB4: ("G_MTX", "addr=0x{w2:08X} params=0x{w1>>16:04X}"),
    0x01: ("G_MTX", "addr=0x{w2:08X} params=0x{w1>>16:04X}"),
    0x06: ("G_DMA_IO", "flags=0x{w1>>16:04X} addr=0x{w2:08X} size={size}"),
    0x04: ("G_VTX", "n={n} v0={v0} addr=0x{w2:08X}"),
    0x07: ("G_TRI1", "i0={i2} i1={i1} i2={i0}"),
    0x08: ("G_TRI2", "i0={i3} i1={i4} i2={i5} i3={i0} i4={i1} i5={i2}"),
    0xB3: ("G_SPECIAL1", "s1=0x{w1:08X} s2=0x{w2:08X}"),
    0xB7: ("G_SPECIAL2", "s1=0x{w1:08X} s2=0x{w2:08X}"),
}


def _dl_fields(op: int, w1: int, w2: int, tmpl: str) -> dict[str, int]:
    """Extract named bitfields from display list words for template formatting."""
    d: dict[str, int] = {}
    d["w1"] = w1
    d["w2"] = w2
    d["fmt"] = (w1 >> 8) & 7
    d["siz"] = (w1 >> 10) & 3
    d["width"] = (w1 >> 12) & 0xFFF
    d["addr"] = w2
    d["min_lvl"] = (w1 >> 8) & 0xFF
    d["x1"] = (w1 >> 12) & 0xFFF
    d["y1"] = (w1 >> 0) & 0xFFF
    d["x2"] = (w2 >> 12) & 0xFFF
    d["y2"] = (w2 >> 0) & 0xFFF
    d["uls"] = (w1 >> 12) & 0xFFF
    d["ult"] = (w1 >> 0) & 0xFFF
    d["lrs"] = (w2 >> 12) & 0xFFF
    d["lrt"] = (w2 >> 0) & 0xFFF
    d["dxt"] = (w2 >> 0) & 0xFFF
    d["line"] = (w1 >> 12) & 0x1FF
    d["tmem"] = (w1 >> 0) & 0x1FF
    d["tile"] = (w2 >> 24) & 7
    d["pal"] = (w2 >> 20) & 0xF
    d["ct"] = (w2 >> 19) & 1
    d["mt"] = (w2 >> 18) & 1
    d["msk_s"] = (w2 >> 14) & 0xF
    d["shf_s"] = (w2 >> 10) & 0xF
    d["msk_t"] = (w2 >> 4) & 0xF
    d["shf_t"] = (w2 >> 0) & 0xF
    d["wg"] = (w1 >> 12) & 0xFF
    d["wb"] = (w1 >> 0) & 0xFF
    d["center"] = (w2 >> 16) & 0xFFFF
    d["scale"] = w2 & 0xFFFF
    d["x0"] = (w1 >> 12) & 0xFFF
    d["y0"] = (w1 >> 0) & 0xFFF
    d["z"] = (w1 >> 16) & 0xFFFF
    d["dz"] = w1 & 0xFFFF
    d["n"] = (w1 >> 20) & 0xFF
    d["v0"] = (w1 >> 16) & 0xF
    d["i0"] = (w2 >> 0) & 0xFF
    d["i1"] = (w2 >> 8) & 0xFF
    d["i2"] = (w2 >> 16) & 0xFF
    d["i3"] = (w1 >> 0) & 0xFF
    d["i4"] = (w1 >> 8) & 0xFF
    d["i5"] = (w1 >> 16) & 0xFF
    d["flag"] = (w1 >> 0) & 0xFF
    d["push"] = (w1 >> 16) & 1
    d["idx"] = (w1 >> 16) & 0xFF
    d["ofs"] = w1 & 0xFFFF
    d["datalen"] = (w2 >> 16) & 0xFF
    d["size"] = (w1 >> 8) & 0xFFFF
    return d


def _decode_dl_entry(w1: int, w2: int) -> tuple[str, str, str]:
    """Decode one display list command (two 32-bit words). Handles RDP then RSP."""
    op = (w1 >> 24) & 0xFF
    d = _dl_fields(op, w1, w2)

    if op in _RDP_CMDS:
        name, tmpl = _RDP_CMDS[op]
        desc = tmpl.format(**d)
        return ("RDP", name, desc)

    if op in _RSP_CMDS:
        name, tmpl = _RSP_CMDS[op]
        desc = tmpl.format(**d)
        return ("RSP", name, desc)

    return ("???", f"UNK_0x{op:02X}", f"0x{w1:08X} 0x{w2:08X}")


# ── callchain tracing ───────────────────────────────────────


@mcp.tool()
def n64_trace_callchain(addresses: str, enable: bool = True) -> dict[str, Any]:
    """Set execution breakpoints at multiple function addresses and capture
    call/register context on each hit (RA, A0-A3). Auto-resumes after capture.

    Use this to trace function call sequences and arguments without
    manually single-stepping. Read captured events via n64_get_trace_events.

    addresses: comma-separated hex addresses, e.g. "0x8011C450,0x80124C60"
    enable:    true to start tracing, false to stop
    Returns: {"ok": true, "bps_set": N} on enable
    """
    if not enable:
        return _client().call("trace_callchain_stop")
    return _client().call("trace_callchain", {"addresses": addresses})


# ── OS detection ────────────────────────────────────────────


@mcp.tool()
def n64_detect_os() -> dict[str, Any]:
    """Detect the OS type, boot flow, RSP microcode, and thread function
    addresses for the currently loaded ROM.

    Returns:
      rom:        header info (name, CRC, entry, country, clockrate)
      boot:       boot type (ipl3/rom_code_direct/custom) + first instructions
      os:         OS type (libultra/custom) + detected function addresses
      rsp:        microcode type + boot bytes + DMEM state

    Use the reported function addresses with n64_trace_scheduler and
    n64_trace_callchain.
    """
    return _client().call("detect_os")


# ── scheduler tracing ───────────────────────────────────────


@mcp.tool()
def n64_trace_scheduler(ctx_switch_addr: str, queue_addr: str = "",
                        enable: bool = True) -> dict[str, Any]:
    """Trace the game's RTOS scheduler by monitoring the run queue
    and context-switch function.

    Captures:
      - sched_ctx_switch: thread switch RA, A0 (old TCB), A1 (new TCB)
      - sched_queue_write: queue head pointer changes

    Auto-resumes after each capture. Read events via n64_get_trace_events.
    Use n64_detect_os to find the right addresses for your game.

    ctx_switch_addr: hex address of context-switch function (or 0 to skip)
    queue_addr:      hex address of run queue head (or "" to skip)
    enable:          true to start, false to stop
    """
    if not enable:
        return _client().call("trace_scheduler_stop")
    params = {"ctx_switch_addr": ctx_switch_addr}
    if queue_addr:
        params["queue_addr"] = queue_addr
    return _client().call("trace_scheduler", params)


# ── struct tracking ──────────────────────────────────────────


@mcp.tool()
def n64_track_struct(address: str, size: int = 16, enable: bool = True) -> dict[str, Any]:
    """Watch writes to a memory region and log every change with offset/val/prev/pc.

    Sets a write breakpoint on the address range. Each write is captured
    transparently (auto-resume) and logged as a trace event. Use
    n64_get_trace_events to read the captured writes.

    address: hex address of the struct to track (e.g. "0x80147620" for OSTask)
    size:    number of bytes to watch (default 16, max 4096)
    enable:  true to start tracking, false to stop
    Returns: {"ok": true, "bp_index": N} on enable
    """
    if not enable:
        return _client().call("track_struct_stop")
    return _client().call("track_struct", {"address": address, "size": size})


# ── asset discovery ──────────────────────────────────────────


@mcp.tool()
def n64_discover_assets() -> dict[str, Any]:
    """Scan ROM, RDRAM, and RSP state to produce a structured asset manifest.

    Reads the ROM header, scans all ROM and RDRAM regions (classifying
    by content type: code, audio, display_list, texture, etc.), captures
    the boot flow and active RSP task, and returns everything as a
    structured JSON manifest. Use this to understand the runtime layout
    of the loaded game.
    """
    return _client().call("scan_assets")


@mcp.tool()
def n64_export_manifest(path: str) -> dict[str, Any]:
    """Scan assets and export the full manifest to a JSON file.

    path: file path for the output JSON (e.g. "asset_manifest.json")
    """
    manifest = _client().call("scan_assets")
    import json as _json
    with open(path, "w") as f:
        _json.dump(manifest, f, indent=2)
    return {"ok": True, "path": path, "rom_regions": len(manifest.get("rom_regions", [])),
            "rdram_regions": len(manifest.get("rdram_regions", []))}


@mcp.tool()
def n64_read_framebuffer() -> dict[str, Any]:
    """Read the current N64 framebuffer from RDRAM via VI registers.

    Returns width, height, bytes-per-pixel (2=RGBA5551, 4=RGBA8888),
    and the raw pixel data as hex. Use n64_viewer.py to display.
    """
    return _client().call("read_framebuffer")


@mcp.tool()
def n64_set_frame_capture_interval(interval: int = 0) -> dict[str, Any]:
    """Enable or disable automatic framebuffer capture every N frames.

    interval: capture every N frames (0 = disable auto-capture)
    """
    return _client().call("set_frame_capture_interval", {"interval": interval})


@mcp.tool()
def n64_get_frame_count() -> dict[str, Any]:
    """Get the current VI frame counter.
    """
    return _client().call("frame_count")


@mcp.tool()
def n64_get_frame_captures() -> dict[str, Any]:
    """Get list of auto-captured framebuffers (frame, width, height, bpp, size).
    """
    return _client().call("get_frame_captures")


@mcp.tool()
def n64_clear_frame_captures() -> dict[str, Any]:
    """Clear the auto-captured framebuffer list.
    """
    return _client().call("clear_frame_captures")


@mcp.tool()
def n64_wait_for_frame(target: int, timeout_ms: int = 5000) -> dict[str, Any]:
    """Wait until the emulator reaches a specific frame count.

    target:     target frame number to wait for
    timeout_ms: max time to wait in milliseconds (default 5000)
    """
    return _client().call("wait_for_frame", {"target": target, "timeout_ms": timeout_ms})


@mcp.tool()
def n64_decode_display_list(address: str, size: int = 256) -> dict[str, Any]:
    """Read a display list from memory and decode each GBI command.

    address: hex virtual address of the display list (e.g. "0x802C0000")
    size:    number of bytes to decode (must be multiple of 8, default 256)
    Returns: list of decoded commands with offset, domain, name, and args.
    """
    if size % 8 != 0:
        size = (size + 7) & ~7
    raw = _client().call("read_mem", {"address": address, "size": size})
    hex_str: str = raw.get("hex", "")
    if not hex_str or hex_str == "0" * len(hex_str):
        return {"address": address, "commands": [], "error": "All zeros — no display list found"}

    # Big-endian: each 32-bit word is 8 hex chars
    words = [int(hex_str[i:i+8], 16) for i in range(0, len(hex_str), 8)]
    cmds = []
    for i in range(0, len(words) - 1, 2):
        w1, w2 = words[i], words[i + 1]
        domain, name, desc = _decode_dl_entry(w1, w2)
        offset = i * 4
        cmds.append({
            "offset": f"0x{offset:X}",
            "domain": domain,
            "name": name,
            "args": desc,
            "raw": f"0x{w1:08X} 0x{w2:08X}",
        })
        if name == "G_ENDDL":
            break

    return {"address": address, "command_count": len(cmds), "commands": cmds}


# ── input injection ─────────────────────────────────────────────


# N64 button values from the BUTTONS bitfield (m64p_plugin.h)
BUTTON_VALUES: dict[str, int] = {
    "R_DPAD": 1 << 0,
    "L_DPAD": 1 << 1,
    "D_DPAD": 1 << 2,
    "U_DPAD": 1 << 3,
    "START":  1 << 4,
    "Z":      1 << 5,
    "B":      1 << 6,
    "A":      1 << 7,
    "R_C":    1 << 8,
    "L_C":    1 << 9,
    "D_C":    1 << 10,
    "U_C":    1 << 11,
    "R":      1 << 12,
    "L":      1 << 13,
}

@mcp.tool()
def n64_set_controller(
    channel: int = 0,
    buttons: str = "",
    x: int = 0,
    y: int = 0,
    sticky: bool = False,
) -> dict[str, Any]:
    """Inject controller state into the running emulator.

    channel: controller port (0-3, default 0)
    buttons: space-separated button names, e.g. "A START Z"
             Names: A B START Z L R U_DPAD D_DPAD L_DPAD R_DPAD
                    U_C D_C L_C R_C
             Or a raw hex value like "0x00A0"
    x: analog stick X (-128..127, default 0)
    y: analog stick Y (-128..127, default 0)
    sticky: if True, state persists across frames until explicitly
            changed or cleared (default False — one-shot)
    """
    if buttons.startswith("0x"):
        raw = int(buttons, 16)
    else:
        raw = 0
        for name in buttons.upper().split():
            name = name.strip()
            if name in BUTTON_VALUES:
                raw |= BUTTON_VALUES[name]
    x_clamped = max(-128, min(127, x))
    y_clamped = max(-128, min(127, y))
    return _client().call("set_controller_state", {
        "channel": channel, "buttons": raw, "x": x_clamped, "y": y_clamped, "sticky": sticky,
    })


# ── entry point ────────────────────────────────────────────────


def main() -> None:
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
