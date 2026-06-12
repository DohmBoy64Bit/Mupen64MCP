# Cruis'n USA — Custom Midway Engine Analysis

## 1. Thread Structure (TCB) @ 0x801475D0

The game uses a **custom thread control block** (not standard libultra `OSThread`).

### Layout at 0x801475D0 (primary thread)
```
Offset  Size  Description
------  ----  -----------
+0x00   4     queue_prev / link (0x80146820)
+0x04   4     queue_next / link (0x8013AEA0)
+0x08   4     (zero)
+0x0C   4     (zero)
+0x10   4     thread_id / state (0x01)
+0x14   4     sibling_ptr (0x801475E8)
+0x18   4     sibling_ptr (0x801475F0)
+0x1C   4     (zero)
+0x20   4     stack_size (0xB0000 = 720 KB)
+0x24   4     self_ptr (0x801475D0)
+0x28   4     entry_pc (0x80000480 — PIF idle loop)
+0x2C   4     stack_base_phys (0x0007F220)
+0x30   4     time_quantum (0x1370 = 4976)
+0x34-44     saved regs / padding
+0x48   4     saved_context_ptr (0x8013F668)
+0x4C   4     (zero)
+0x50   4     state (0x0 = ready, 0x20000000 = running?)
+0x54   4     flags
+0x58   4     something (0x01)
+0x5C-A0     zeros
+0xA0   4     another_tcb_ptr (0x80147718 — audio task?)
```

### Linked nodes at 0x801475E8 and 0x801475F0

Both are small (0x50 bytes) and share the same entry/stack as the primary:
```
0x801475E8: [0x801475F0, 0, 0xB0000, 0x801475D0, 0x80000480, 0x0007F220, 0x1370]
0x801475F0: [0xB0000, 0x801475D0, 0x80000480, 0x0007F220, 0x1370]
```
This forms a ring of 3 "thread info" entries all pointing to the same idle thread.

### Thread schedule entry size
The scheduler uses 0x50-byte (80-byte) entries per thread, indexed by thread ID
(byte at TCB+0x03).

## 2. Custom Exception Handler @ 0x80124C60

The exception vector at `0x80000180` does NOT jump to libultra's dispatcher:
```
lui   k0, 0x8012
addiu k0, k0, 0x4C60   ; k0 = 0x80124C60
jr    k0
nop
```

Handler at 0x80124C60:
```
lui   k0, 0x8016
addiu k0, k0, 0x5780    ; k0 = 0x80165780 (save area)
sd    at, 0x20(k0)      ; save $at
mfc0  k1, Cause         ; read cause register
sw    k1, 0x118(k0)     ; store cause
addiu at, zero, -4
and   k1, k1, at
mtc0  k1, Cause         ; clear cause bits
sd    t0, 0x58(k0)      ; save t0-t2
sd    t1, 0x60(k0)
sd    t2, 0x68(k0)
sw    zero, 0x18(k0)    ; clear flag
mfc0  t0, SR            ; read Status register
andi  t1, t0, 0x007C    ; get interrupt mask
addiu t2, zero, 1
bne   t1, t2, <skip>    ; if mask != 1, skip dispatch
and   t1, k1, t0        ; t1 = Cause & SR
andi  t2, t1, 0x4000    ; check VI bit (bit 14)
beq   t2, zero, <skip>
addiu t1, zero, 1
lui   at, 0x8014
sw    t1, -20844(at)    ; *(0x8014AE94) = 1 (VI flag)
...
```

Key points:
- Custom save area at **0x80165780** (not standard libultra context)
- Manually dispatches interrupts by checking `Cause & SR`
- VI interrupt (bit 0x4000) sets flag at **0x8014AE94**
- No `__osDispatchThread` — the game handles its own scheduling

## 3. RSP Microcode

### Location
- **Ucode boot**: 0x8012F640 (ROM copy at offset 0x12F640, 208 bytes)
  Loads ucode from RDRAM into IMEM and jumps to 0x1080
- **Ucode**: 0x8012F710 (ROM copy at offset 0x12F710, ~4 KB)
- **Ucode data**: at DMEM offset 0x160 (dispatching tables)

### Boot flow
```
1. Set at = 0xFC0 (OSTask header in DMEM)
2. Read task header fields to set up DMA
3. DMA ucode from RDRAM (0x8012F710) → IMEM (0x1000)
4. Jump to 0x1080
```

### GBI Command Dispatch (at ~0x1098-0x10B8)
The ucode uses a **10-bit GBI command** (not standard 8-bit):
```
srl   t4, s3, 22         ; extract top 10 bits
andi  t4, t4, 0x3C       ; mask to 16 entries (4-byte stride)
lw    t4, 0x160(t5)      ; load handler from dispatch table at DMEM+0x160
jr    ra                 ; jump to handler
```

### Observed GBI commands from display list
```
0x02 → G_VTX (load vertices to DMEM)
0x06 → G_DL (sub-display list call)
0x07 → triangle / draw
0x08 → branch list
0x0C → triangle (with vertex clipping mask 0x7FFF)
0x0D → set other mode
```

Compared to standard F3DEX2:
| Function | F3DEX2 | This ucode |
|----------|--------|------------|
| G_VTX | 0x02 | 0x02 |
| G_DL | 0x06/0x03 | 0x06 |
| G_TRI2 | 0x08 | 0x08 |
| G_BRANCH_Z | 0x0B | 0x0C |
| G_SETOTHERMODE | 0x0D/0x0E | 0x0D |

Assignment differs — likely derived from **F3DGBI** (GoldenEye variant) or a
Midway-customized F3DEX2.

## 4. Display List

Current visible display list at 0x803D31E0 (phys 0x003D31E0):
```
0x803D31E0:  07 000000 00000000   → draw
0x803D31E8:  02 000440 00000140   → G_VTX 4 vertices (0x140 = 20 bytes each = 5-word vertex?)
0x803D31F0:  02 000580 00000140   → G_VTX 4 vertices
0x803D31F8:  02 0006C0 00000140
0x803D3200:  02 000800 00000140
0x803D3208:  08 000000 00000140   → branch/call
0x803D3210:  0C 007FFF 06C00440   → triangle (0x7FFF = all vertices active)
0x803D3218:  0C 007FFF 08000580
0x803D3220:  08 000000 00000140   → branch/call
0x803D3228:  0D 000000 04400580   → set other mode
0x803D3230:  08 000000 00000280
0x803D3238:  06 000000 003DF230   → G_DL to 0x003DF230 (sub-DL)
0x803D3240:  07 000000 00000000   → draw
```

Vertex size: 0x140 = 320 bytes for 16 vertices = 20 bytes per vertex
(standard F3D uses 16 bytes per vertex, F3DEX2 uses 16/24/32 — this 20-byte
vertex is non-standard)

## 5. VI / Framebuffer

### Current mode (NTSC 320×240)
```
VI_STATUS   = 0x3116 (NTSC, 16-bit, gamma on)
VI_ORIGIN   = 0x0018B380 (single framebuffer!)
VI_WIDTH    = 0x140 = 320
VI_V_SYNC   = 0x20D = 525 (NTSC)
VI_H_START  = 0x006C02EC
VI_V_START  = 0x002501FF
```

### Single-buffer rendering
Cruis'n USA uses **one fixed framebuffer** at physical 0x0018B380.
VI_ORIGIN does NOT alternate. The game renders directly to the active buffer.

### VI interrupt handled differently
- Custom handler sets flag at 0x8014AE94 on VI interrupt
- No standard `__osViSwapBuffer` call — swap would need to be detected
  by watching VI_ORIGIN register writes or the custom flag

### For RT64/N64Recomp
Flip detection needs to:
1. Hook writes to VI_ORIGIN (0xA4400004)
2. Or monitor the custom flag at 0x8014AE94
3. Or detect writes to the framebuffer address range

## 6. Saved Context Area @ 0x80165780

```
+0x00-0x1F: 8 saved registers (zero currently)
+0x20:      sentinel (0xFFFFFFFF) + saved PC (0x80000000)
+0x28-0x5F: more save slots
+0x60:      flags/control (0x0000FF01 = interrupt-in-progress)
```

## 7. Audio Task @ 0x80147718

A second task structure lives at 0x80147718, containing audio-related pointers:
```
+0x00:  queue link (0x8013AEA0 ring)
+0x10:  type (0x01 = audio task?)
+0x28:  ucode_boot (0x8012F640 — shares with gfx!)
+0x30:  ucode (0x8012F710 — also shared?)
+0x38:  ucode_data (0x8013BEC0)
+0x40:  data_ptr (0x801462A0)
+0x44:  data_size (0x400)
```

The audio and graphics may share the same ucode but with different data
sections.

## 8. PI DMA Activity

The PI DMA registers at the time of measurement show:
```
DRAM_ADDR: 0x000017F0 (low RDRAM — boot area)
CART_ADDR: 0x10080590 (ROM at offset 0x80590)
RD_LEN:    0x7F (128 bytes)
```

This suggests the game loads data from ROM via PI DMA in small chunks
(not the standard libultra `osPiRawStartDma` pattern). Small DMA transfers
(128 bytes) are typical for streaming audio or loading individual game objects.

## Summary of gaps for standard tooling

| Area | Standard expects | Cruis'n USA reality |
|------|-----------------|---------------------|
| Thread struct | `OSThread` (0x48 bytes) | Custom TCB (0x50+ bytes) |
| Exception handler | libultra dispatcher | Custom handler at 0x80124C60 |
| Scheduler | `__osDispatchThread` | Manual interrupt dispatch |
| GBI | F3DEX2 (8-bit commands) | Custom F3DEX2 variant (10-bit dispatch, 20-byte vertices) |
| Framebuffer | Double-buffered via `__osViSwapBuffer` | Single buffer, no swap |
| VI swap hook | `__osViSwapBuffer` address | Write to VI_ORIGIN or flag at 0x8014AE94 |
| Boot | IPL3 → libultra init | `rom_code_direct` → custom init |
| PI DMA | `osPiRawStartDma` | Custom small-transfer DMA |
| RSP ucode | Standard F3D/F3DEX2 | Custom F3D variant |
| Audio ucode | Standard audio ucode | Shares graphics ucode? |
