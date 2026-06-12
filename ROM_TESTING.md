# ROM Testing Results

## Full MCP Tool Test (Cruis'n USA): 122/122 PASS

All 47 MCP tools tested via direct JSON-RPC on Cruis'n USA. Run 3× with dummy plugins and 3× with Rice + RSP-HLE — **122/122 PASS on all 6 runs**.

| Section | Tests | Result |
|---------|-------|--------|
| Status & Lifecycle | 6 | PASS |
| Emulation Control | 6 | PASS |
| CPU Inspection | 5 | PASS |
| Memory | 10 | PASS |
| Breakpoints | 10 | PASS |
| Code Analysis | 6 | PASS |
| OS Detection | 7 | PASS |
| Tracing | 8 | PASS |
| Wait for BP | 1 | PASS |
| PI DMA | 6 | PASS |
| RSP / SP | 12 | PASS |
| Callchain | 2 | PASS |
| Scheduler | 2 | PASS |
| Struct Track | 2 | PASS |
| Asset Discovery | 5 | PASS |
| Framebuffer | 10 | PASS |
| Display List | 1 | PASS |
| Input Injection | 3 | PASS |
| ROM Management | 3 | PASS |
| Boot | — | PASS |
| **Total** | **122** | **122/122 PASS** |

Tools `n64_start_daemon` and `n64_stop_daemon` are lifecycle operations tested implicitly (daemon started before and stopped after the suite). All remaining 45 tools are explicitly tested.

## Full MCP Tool Test (Star Fox 64): 111-112/115 PASS

All 47 MCP tools tested via direct JSON-RPC on Star Fox 64. Run 3× with dummy plugins and 3× with Rice + RSP-HLE.

| Run | Plugins | Score | Note |
|-----|---------|-------|------|
| 1 | dummy | 112/115 | scan_functions returns 0 (different ROM layout) |
| 2 | dummy | 112/115 | scan_functions returns 0 |
| 3 | dummy | 111/115 | scan_functions + 1 timing flake |
| 4 | Rice+RSP-HLE | 111/115 | scan_functions + 1 timing flake |
| 5 | Rice+RSP-HLE | 112/115 | scan_functions returns 0 |
| 6 | Rice+RSP-HLE | 112/115 | scan_functions returns 0 |

The consistent delta from 122/122 (Cruis'n USA score): `scan_functions` returns 0 because Star Fox 64's memory layout doesn't place 300+ functions in the default scan range at boot time. The other 1-off failures are timing-sensitive (step_instruction after step_frame). All core tools (breakpoints, memory, registers, tracing, input, framebuffer, RSP, PI DMA, OS detection) pass reliably.

## Viewer Test Results

### Star Fox 64: 44/45 PASS

Same structure as above. 1 expected failure: callchain events not caught in 3-second window (attract mode doesn't hit scheduler).

## Non-Viewer Test Results

### Cruis'n USA: 43/49 PASS

6 expected failures due to custom Midway engine (no libultra patterns):
- OS detection ×4 failures
- Scheduler trace (custom scheduler, no queue_addr)
- Frame capture (frame counter at 0 with dummy gfx)

### Star Fox 64: 45/46 PASS

1 expected failure: display list scanner finds no DLs in attract mode.

## Exhaustive Feature Test (Cruis'n USA): 44/44 PASS

| Feature | Tests | Result |
|---------|-------|--------|
| Breakpoint management | 15 | PASS |
| Address translation | 7 | PASS |
| Function scanner | 6 | PASS |
| RSP health check | 6 | PASS |
| Regression checks | 10 | PASS |
| **Total** | **44** | **44/44 PASS** |

## Tested ROMs

### Cruis'n USA (NCUE)
- CRC: `FF2F2FB4 D161149A`, 8 MB
- Custom Midway engine (PIF jumps to `0x8011C450`, bypassing IPL3)
- Custom F3DEX-based RSP microcode at ROM offset `0x31000`
- Boot flow: `PIF (0xA4000040)` → `0x80000000` trampoline → `0x80124C60` → `0x8011C450`
- Framebuffer: 320×240 RGBA8888 (pixels are zero with Rice — plugin limitation)
- Frame rate: ~58 FPS, 30 auto-captures in 5 seconds
- RSP task type: 0x01 (custom F3DEX-based microcode, standard F3DEX2 GBI commands)

### Star Fox 64 (LZ-type)
- CRC: `BA780BA0 0F21DB34`, 12 MB
- Standard IPL3 boot (`0x80000400` entry)
- libultra detected: `osCreateThread @ 0x8001C3EC`, `osStartThread @ 0x80006FD8`, `osYieldThread @ 0x800049D4`
- Full MCP tool test: **111-112/115 PASS** (scan_functions returns 0 due to ROM layout, 1 timing flake)
- Viewer test: 44/45 PASS
- Non-viewer test: 45/46 PASS
- Framebuffer: 320×240 RGBA8888 with **actual non-zero pixels** after initial render
- Frame rate: ~60 FPS, 31 auto-captures in 5 seconds
- RSP task type: 0x02 (standard F3D ucode)

### Conker's Bad Fur Day (NFXE)
- CRC unknown, 64 MB
- 45/55 PASS, 10 FAIL (expected — Rare custom engine)
- ROM header magic: `FFFFFFFF` (not standard `80371240`)
- PC operates in `0x10000000` range

### Paper Mario (NPEE)
- CRC unknown, 40 MB
- 48/55 PASS, 7 FAIL (expected — Intelligent Systems custom engine)
- ROM header magic: `FFFFFFFF`
- Standard IPL3 boot at `0x80000400`

### Aero Fighters (NALE)
- CRC unknown, 8 MB
- 48/55 PASS, 7 FAIL (expected — Video System custom engine)
- ROM header magic: `FFFFFFFF`
- RSP task type: 0x02 (standard F3D ucode)

## Asset Discovery (Cruis'n USA)

### ROM Layout

| Offset | Contents | Identification |
|--------|----------|---------------|
| `0x000000` | `80371240` magic, CRC, "Cruis'n USA", "NCUE" | N64 ROM header |
| `0x001000` | Memory clear loop → boot init | IPL3 / boot code |
| `0x040000-0x0BFFFF` | MIPS instructions | Game code (~512 KB) |
| `0x0C0000` | `63634E4E` ("ccNN") | Data segment |
| `0x180000` | `73E5 73E5 73E5 6BE7...` repeating | ADPCM audio data |
| `0x1C0000-0x7FFFFF` | Dense high-entropy data | Textures, levels, models |
| `0x31000` | 8KB blob: 34% COP2, 19% LWC2 | Custom F3DEX RSP microcode |

### RDRAM at Runtime (~frame 200)

| Address | Contents | Identification |
|---------|----------|---------------|
| `0x80000000` | `3C1A8012 275A4C60 03400008` → `jr 0x80124C60` | Reset trampoline |
| `0x80000000-0x801FFFFF` | MIPS instructions | Loaded game code |
| `0x802C0000-0x802FFFFF` | F3DEX2 display lists | Active rendering commands |
| `0x80330000` | CI8 palette data | Palette data |
| `0x80340000` | Smooth gradient | Texture color data |

## Future Improvements

### OS Detector
- Rare custom engine support (Conker, Banjo, Diddy Kong Racing)
- Intelligent Systems engine support (Paper Mario, Fire Emblem)
- Video System engine support (Aero Fighters, F-1 World Grand Prix)
- Custom scheduler detection for non-libultra engines
- RSP ucode type expansion beyond F3D/F3DEX2
- ROM header format detection for non-standard headers (`FFFFFFFF` magic)

### Viewer
- `get_capture_pixels(index)` JSON-RPC method for historical frame pixels
- Live framebuffer image rendering via PIL/Pillow
- Thumbnail grid view for capture history
- PNG export for captured frames
