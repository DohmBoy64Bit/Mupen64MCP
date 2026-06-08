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
) -> dict[str, Any]:
    """Start the native n64-debug-daemon subprocess and connect to it.

    core_path: path to mupen64plus.dll
    rom_path:  optional path to a .z64 ROM to load immediately
    port:      TCP port for JSON-RPC (default 9876)
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
def n64_status() -> dict[str, Any]:
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


# ── breakpoints ────────────────────────────────────────────────


@mcp.tool()
def n64_add_exec_breakpoint(address: str) -> dict[str, Any]:
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
    """List all active breakpoints."""
    return _client().call("list_breakpoints")


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
def n64_trace_rom_reads(enable: bool) -> dict[str, Any]:
    """Enable or disable ROM-read tracing at the emulator level.

    When enabled, every PI DMA / cartridge read is logged as a trace event.
    """
    return _client().call("trace_rom_reads", {"enable": enable})


# ── entry point ────────────────────────────────────────────────


def main() -> None:
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
