"""n64-viewer — Enhanced live N64 emulation dashboard.

Features:
- ROM-agnostic status display (works with any ROM)
- Frame capture preview (auto-captured frames displayed as canvas)
- CPU registers (all 32 GPRs in a grid)
- Scene detection (PC-range based, generic)
- Memory hex viewer (simple hex+ASCII)
- Breakpoint management (add/remove/list)
- OS detection display
- PI DMA / RSP status
- Event log with filtering
- Input injection with analog stick visualization
- Auto-refresh every 250ms

Usage: python n64_viewer.py [--port 9876]
"""
import sys, time, struct, argparse, math
sys.path.insert(0, "D:/Mupen64MCP/mcp/python")
from n64_debug_mcp.daemon_client import DaemonClient, DaemonConfig

try:
    import tkinter as tk
    from tkinter import ttk
except ImportError:
    print("tkinter not available.")
    sys.exit(1)

try:
    from PIL import Image, ImageTk
    _PIL_AVAILABLE = True
except ImportError:
    _PIL_AVAILABLE = False
    print("PIL not available. Images will be text-only.")

# ── helpers ────────────────────────────────────────────────────

def pch(v):
    if isinstance(v, str) and v.startswith("0x"):
        return int(v, 16)
    return int(v) if v else 0

BUTTON_VALS = {
    "A": 128, "B": 64, "Z": 32, "START": 16,
    "R": 4096, "L": 8192, "R_C": 256, "L_C": 512,
    "D_C": 1024, "U_C": 2048,
    "U_DPAD": 8, "D_DPAD": 4, "L_DPAD": 2, "R_DPAD": 1,
}

# Generic PC-based scene detection
SCENE_RANGES = [
    (0x80000000, 0x80001000, "Boot Vector", "#4444AA"),
    (0x80000400, 0x80002000, "IPL3 / Boot", "#AA44AA"),
    (0x80002000, 0x80020000, "Game Init", "#AA44AA"),
    (0x80020000, 0x80080000, "Game Code", "#33AA33"),
    (0x80080000, 0x80100000, "Game Code", "#33AA33"),
    (0x80100000, 0x80180000, "Game Code", "#33AA33"),
    (0x80180000, 0x80200000, "Game Code", "#33AA33"),
    (0x80200000, 0x80400000, "Game Data", "#AA8800"),
    (0xA4000000, 0xA4001000, "RSP Boot", "#AA4444"),
    (0xA4001000, 0xA4002000, "RSP Ucode", "#AA4444"),
]

def classify_scene(pc):
    pc = pch(pc)
    for lo, hi, name, color in SCENE_RANGES:
        if lo <= pc < hi:
            return (name, color)
    return ("Unknown", "#888888")

# ── main window ────────────────────────────────────────────────

class N64Viewer:
    def __init__(self, port=9876, host="127.0.0.1"):
        self.port = port
        self.host = host
        self.root = tk.Tk()
        self.root.title("N64 Viewer - Live Emulation Dashboard")
        self.root.geometry("1000x850")
        self.root.configure(bg="#1a1a2e")

        cfg = DaemonConfig(core_path="dummy", port=port, host=host)
        self.dc = DaemonClient(cfg)
        try:
            self.dc._connect()
        except Exception as e:
            self._show_error(f"Cannot connect to daemon at {host}:{port}: {e}")
            return

        self.running = True
        self._last_positions = []
        self._frame_captures = []
        self._os_info = None
        self._registers = {}
        self._build_ui()
        self._poll()

    def _show_error(self, msg):
        lbl = tk.Label(self.root, text=msg, fg="red", bg="#1a1a2e",
                       font=("Consolas", 12))
        lbl.pack(expand=True)

    def _safe_call(self, method, params=None):
        try:
            return self.dc.call(method, params)
        except Exception:
            return None

    # ── ui construction ──────────────────────────────────────

    def _build_ui(self):
        # Main frame with padding
        main = tk.Frame(self.root, bg="#1a1a2e")
        main.pack(fill="both", expand=True, padx=4, pady=4)

        # ── TOP ROW: Status + Frame + Controls ──
        top_frame = tk.Frame(main, bg="#16213e")
        top_frame.pack(fill="x", pady=2)

        # Status panel
        status_panel = tk.Frame(top_frame, bg="#16213e")
        status_panel.pack(side="left", fill="y", padx=4)

        self.lbl_scene = tk.Label(status_panel, text="SCENE: --", font=("Consolas", 11, "bold"),
                                  fg="white", bg="#888888", padx=6, pady=2)
        self.lbl_scene.pack(anchor="w", pady=1)

        self.lbl_frame = tk.Label(status_panel, text="Frame: --", font=("Consolas", 10),
                                  fg="#00ff00", bg="#16213e")
        self.lbl_frame.pack(anchor="w")

        self.lbl_pc = tk.Label(status_panel, text="PC: --", font=("Consolas", 10),
                               fg="#00ccff", bg="#16213e")
        self.lbl_pc.pack(anchor="w")

        self.lbl_state = tk.Label(status_panel, text="State: --", font=("Consolas", 10),
                                  fg="white", bg="#16213e")
        self.lbl_state.pack(anchor="w")

        self.lbl_fb = tk.Label(status_panel, text="FB: --", font=("Consolas", 9),
                               fg="#aaaaaa", bg="#16213e")
        self.lbl_fb.pack(anchor="w")

        # Controls panel
        ctrl_panel = tk.Frame(top_frame, bg="#16213e")
        ctrl_panel.pack(side="right", fill="y", padx=4)

        tk.Label(ctrl_panel, text="Controls", font=("Consolas", 10, "bold"),
                 fg="white", bg="#16213e").pack()

        btn_frame = tk.Frame(ctrl_panel, bg="#16213e")
        btn_frame.pack()

        for name, method in [("Pause", "pause"), ("Resume", "resume"),
                              ("Step", "step_instruction")]:
            tk.Button(btn_frame, text=name, command=self._cmd(method),
                      font=("Consolas", 9), bg="#0f3460", fg="white",
                      activebackground="#1a5276", width=8).pack(side="left", padx=2)

        # Input injection panel
        inject_frame = tk.Frame(ctrl_panel, bg="#16213e")
        inject_frame.pack(pady=2)

        tk.Label(inject_frame, text="Input:", font=("Consolas", 9),
                 fg="white", bg="#16213e").pack(anchor="w")

        btn_row1 = tk.Frame(inject_frame, bg="#16213e")
        btn_row1.pack()
        for name in ("START", "A", "B", "Z"):
            tk.Button(btn_row1, text=name, command=self._inject_cmd(name),
                      font=("Consolas", 8), bg="#e94560", fg="white",
                      width=4).pack(side="left", padx=1)

        btn_row2 = tk.Frame(inject_frame, bg="#16213e")
        btn_row2.pack(pady=1)
        for name in ("R", "L", "U_DPAD", "D_DPAD"):
            tk.Button(btn_row2, text=name, command=self._inject_cmd(name),
                      font=("Consolas", 8), bg="#533483", fg="white",
                      width=4).pack(side="left", padx=1)

        # Analog stick canvas
        self.stick_canvas = tk.Canvas(inject_frame, width=80, height=80, bg="#111",
                                      highlightthickness=1, highlightbackground="#333")
        self.stick_canvas.pack(pady=2)
        self.stick_canvas.create_oval(10, 10, 70, 70, outline="#555", width=1)
        self.stick_canvas.create_line(40, 10, 40, 70, fill="#333")
        self.stick_canvas.create_line(10, 40, 70, 40, fill="#333")
        self.stick_dot = self.stick_canvas.create_oval(37, 37, 43, 43, fill="#00ff00", outline="")

        # ── MIDDLE ROW: Notebook with tabs ──
        self.notebook = ttk.Notebook(main)
        self.notebook.pack(fill="both", expand=True, pady=2)

        # Tab 1: Framebuffer
        self.tab_fb = tk.Frame(self.notebook, bg="#1a1a2e")
        self.notebook.add(self.tab_fb, text="Framebuffer")
        self._build_fb_tab()

        # Tab 2: Registers
        self.tab_regs = tk.Frame(self.notebook, bg="#1a1a2e")
        self.notebook.add(self.tab_regs, text="Registers")
        self._build_regs_tab()

        # Tab 3: Memory
        self.tab_mem = tk.Frame(self.notebook, bg="#1a1a2e")
        self.notebook.add(self.tab_mem, text="Memory")
        self._build_mem_tab()

        # Tab 4: OS Info
        self.tab_os = tk.Frame(self.notebook, bg="#1a1a2e")
        self.notebook.add(self.tab_os, text="OS Info")
        self._build_os_tab()

        # Tab 5: Events
        self.tab_events = tk.Frame(self.notebook, bg="#1a1a2e")
        self.notebook.add(self.tab_events, text="Events")
        self._build_events_tab()

        # Tab 6: Breakpoints
        self.tab_bp = tk.Frame(self.notebook, bg="#1a1a2e")
        self.notebook.add(self.tab_bp, text="Breakpoints")
        self._build_bp_tab()

        # ── BOTTOM: Status bar ──
        self.status_bar = tk.Label(main, text="Connected", font=("Consolas", 9),
                                    fg="#00ff00", bg="#0f3460", anchor="w", padx=4)
        self.status_bar.pack(fill="x", pady=2)

    def _build_fb_tab(self):
        # Main horizontal split: image on left, text on right
        main_frame = tk.Frame(self.tab_fb, bg="#1a1a2e")
        main_frame.pack(fill="both", expand=True, padx=4, pady=4)

        # Left: Image view
        left_frame = tk.Frame(main_frame, bg="#1a1a2e")
        left_frame.pack(side="left", fill="both", expand=True)

        # Image canvas (scaled 2x: 320x240 -> 640x480)
        self.fb_canvas = tk.Canvas(left_frame, width=640, height=480, bg="#000",
                                   highlightthickness=1, highlightbackground="#333")
        self.fb_canvas.pack(pady=4)

        # Image controls
        ctrl = tk.Frame(left_frame, bg="#1a1a2e")
        ctrl.pack()
        tk.Label(ctrl, text="View:", font=("Consolas", 9),
                 fg="white", bg="#1a1a2e").pack(side="left")
        self.fb_view_mode = tk.StringVar(value="image")
        tk.Radiobutton(ctrl, text="Image", variable=self.fb_view_mode, value="image",
                       font=("Consolas", 9), fg="white", bg="#1a1a2e",
                       selectcolor="#1a1a2e", command=self._refresh_fb).pack(side="left")
        tk.Radiobutton(ctrl, text="Text", variable=self.fb_view_mode, value="text",
                       font=("Consolas", 9), fg="white", bg="#1a1a2e",
                       selectcolor="#1a1a2e", command=self._refresh_fb).pack(side="left")
        tk.Button(ctrl, text="Refresh", command=self._refresh_fb,
                  font=("Consolas", 9), bg="#0f3460", fg="white").pack(side="left", padx=4)

        # Right: Text panel
        right_frame = tk.Frame(main_frame, bg="#1a1a2e", width=300)
        right_frame.pack(side="right", fill="y", padx=4)
        right_frame.pack_propagate(False)

        # Capture controls
        ctrl2 = tk.Frame(right_frame, bg="#1a1a2e")
        ctrl2.pack(fill="x", pady=2)
        tk.Label(ctrl2, text="Capture Interval:", font=("Consolas", 9),
                 fg="white", bg="#1a1a2e").pack(side="left")
        self.capture_interval = tk.Spinbox(ctrl2, from_=0, to=60, width=4,
                                           font=("Consolas", 9))
        self.capture_interval.pack(side="left", padx=2)
        tk.Button(ctrl2, text="Set", command=self._set_capture_interval,
                  font=("Consolas", 9), bg="#0f3460", fg="white").pack(side="left", padx=2)

        # Capture list
        self.capture_list = tk.Listbox(right_frame, height=10, font=("Consolas", 9),
                                        bg="#0a0a0a", fg="#ccc")
        self.capture_list.pack(fill="both", expand=True, pady=2)
        self.capture_list.bind("<<ListboxSelect>>", self._on_capture_select)

        # Frame info text
        self.fb_info_text = tk.Text(right_frame, height=4, font=("Consolas", 9),
                                     bg="#0a0a0a", fg="#ccc", wrap="word")
        self.fb_info_text.pack(fill="x", pady=2)
        self.fb_info_text.insert("1.0", "Select a frame or click Refresh to load the current framebuffer.")
        self.fb_info_text.config(state="disabled")

    def _build_regs_tab(self):
        self.reg_labels = {}
        frame = tk.Frame(self.tab_regs, bg="#1a1a2e")
        frame.pack(fill="both", expand=True, padx=4, pady=4)

        # 32 GPRs in 4 columns of 8
        for col in range(4):
            col_frame = tk.Frame(frame, bg="#1a1a2e")
            col_frame.pack(side="left", fill="y", padx=2)
            for row in range(8):
                idx = col * 8 + row
                reg_name = f"${idx:02d}"
                lbl = tk.Label(col_frame, text=f"{reg_name}: 0x00000000",
                               font=("Consolas", 9), fg="#ccc", bg="#1a1a2e",
                               anchor="w", width=24)
                lbl.pack(anchor="w")
                self.reg_labels[idx] = lbl

        # PC, HI, LO
        self.reg_pc = tk.Label(frame, text="PC: 0x00000000", font=("Consolas", 10, "bold"),
                               fg="#00ccff", bg="#1a1a2e")
        self.reg_pc.pack(side="bottom", anchor="w", pady=4)

    def _build_mem_tab(self):
        # Address entry
        addr_frame = tk.Frame(self.tab_mem, bg="#1a1a2e")
        addr_frame.pack(fill="x", padx=4, pady=2)
        tk.Label(addr_frame, text="Address:", font=("Consolas", 9),
                 fg="white", bg="#1a1a2e").pack(side="left")
        self.mem_addr = tk.Entry(addr_frame, font=("Consolas", 9), width=12,
                                  bg="#0a0a0a", fg="#ccc", insertbackground="white")
        self.mem_addr.insert(0, "0x80000000")
        self.mem_addr.pack(side="left", padx=2)
        tk.Label(addr_frame, text="Size:", font=("Consolas", 9),
                 fg="white", bg="#1a1a2e").pack(side="left")
        self.mem_size = tk.Spinbox(addr_frame, from_=16, to=256, width=5,
                                    font=("Consolas", 9))
        self.mem_size.delete(0, "end")
        self.mem_size.insert(0, "64")
        self.mem_size.pack(side="left", padx=2)
        tk.Button(addr_frame, text="Read", command=self._read_mem,
                  font=("Consolas", 9), bg="#0f3460", fg="white").pack(side="left", padx=2)

        # Hex viewer
        self.mem_text = tk.Text(self.tab_mem, height=20, font=("Consolas", 9),
                                 wrap="none", bg="#0a0a0a", fg="#ccc",
                                 insertbackground="white")
        self.mem_text.pack(fill="both", expand=True, padx=4, pady=2)
        scrollbar = tk.Scrollbar(self.mem_text, command=self.mem_text.yview)
        scrollbar.pack(side="right", fill="y")
        self.mem_text.config(yscrollcommand=scrollbar.set)

    def _build_os_tab(self):
        self.os_text = tk.Text(self.tab_os, height=20, font=("Consolas", 10),
                                wrap="word", bg="#0a0a0a", fg="#ccc",
                                insertbackground="white")
        self.os_text.pack(fill="both", expand=True, padx=4, pady=4)
        self.os_text.insert("1.0", "Click 'Detect OS' to analyze the ROM.\n")
        self.os_text.config(state="disabled")

        tk.Button(self.tab_os, text="Detect OS", command=self._detect_os,
                  font=("Consolas", 9), bg="#0f3460", fg="white").pack(pady=2)

    def _build_events_tab(self):
        self.event_text = tk.Text(self.tab_events, height=20, font=("Consolas", 9),
                                   wrap="none", bg="#0a0a0a", fg="#ccc",
                                   insertbackground="white")
        self.event_text.pack(fill="both", expand=True, padx=4, pady=2)
        self.event_text.config(state="disabled")

        # Filter
        filter_frame = tk.Frame(self.tab_events, bg="#1a1a2e")
        filter_frame.pack(fill="x", padx=4)
        tk.Label(filter_frame, text="Filter:", font=("Consolas", 9),
                 fg="white", bg="#1a1a2e").pack(side="left")
        self.event_filter = tk.Entry(filter_frame, font=("Consolas", 9), width=15,
                                      bg="#0a0a0a", fg="#ccc", insertbackground="white")
        self.event_filter.pack(side="left", padx=2)
        tk.Button(filter_frame, text="Apply", command=self._refresh_events,
                  font=("Consolas", 9), bg="#0f3460", fg="white").pack(side="left", padx=2)

    def _build_bp_tab(self):
        # BP list
        self.bp_list = tk.Listbox(self.tab_bp, height=8, font=("Consolas", 9),
                                   bg="#0a0a0a", fg="#ccc")
        self.bp_list.pack(fill="x", padx=4, pady=2)

        # Add BP
        add_frame = tk.Frame(self.tab_bp, bg="#1a1a2e")
        add_frame.pack(fill="x", padx=4)
        tk.Label(add_frame, text="Addr:", font=("Consolas", 9),
                 fg="white", bg="#1a1a2e").pack(side="left")
        self.bp_addr = tk.Entry(add_frame, font=("Consolas", 9), width=12,
                                 bg="#0a0a0a", fg="#ccc", insertbackground="white")
        self.bp_addr.pack(side="left", padx=2)
        tk.Button(add_frame, text="Add", command=self._add_bp,
                  font=("Consolas", 9), bg="#0f3460", fg="white").pack(side="left", padx=2)
        tk.Button(add_frame, text="Remove", command=self._remove_bp,
                  font=("Consolas", 9), bg="#e94560", fg="white").pack(side="left", padx=2)
        tk.Button(add_frame, text="Refresh", command=self._refresh_bp,
                  font=("Consolas", 9), bg="#0f3460", fg="white").pack(side="left", padx=2)

    # ── commands ─────────────────────────────────────────────

    def _cmd(self, method):
        return lambda: self._safe_call(method)

    def _inject_cmd(self, name):
        parts = name.split("+")
        val = sum(BUTTON_VALS[p] for p in parts if p in BUTTON_VALS)
        def do():
            self._safe_call("set_controller_state", {
                "channel": 0, "buttons": val, "x": 0, "y": 0, "sticky": False,
            })
        return do

    def _set_capture_interval(self):
        try:
            interval = int(self.capture_interval.get())
            self._safe_call("set_frame_capture_interval", {"interval": interval})
        except ValueError:
            pass

    def _refresh_fb(self):
        self._update_framebuffer()

    def _read_mem(self):
        addr = self.mem_addr.get()
        try:
            size = int(self.mem_size.get())
        except ValueError:
            size = 64
        r = self._safe_call("read_mem", {"address": addr, "size": size})
        self.mem_text.delete("1.0", "end")
        if r and r.get("hex"):
            h = r["hex"]
            lines = []
            for i in range(0, len(h), 32):
                chunk = h[i:i+32]
                hex_part = " ".join(chunk[j:j+4] for j in range(0, len(chunk), 4))
                ascii_part = ""
                for j in range(0, len(chunk), 2):
                    byte_val = int(chunk[j:j+2], 16)
                    ascii_part += chr(byte_val) if 32 <= byte_val < 127 else "."
                lines.append(f"{i//2:04X}: {hex_part:<40s} {ascii_part}")
            self.mem_text.insert("1.0", "\n".join(lines))

    def _detect_os(self):
        r = self._safe_call("detect_os", timeout=10)
        self.os_text.config(state="normal")
        self.os_text.delete("1.0", "end")
        if r:
            self._os_info = r
            lines = [
                f"OS Type: {r.get('os_type', 'unknown')}",
                f"Boot Type: {r.get('boot_type', 'unknown')}",
                f"Boot Address: {r.get('boot_addr', 'unknown')}",
                f"Has Dispatch: {r.get('has_dispatch', False)}",
                f"Functions Found: {r.get('function_count', 0)}",
                "",
                "Detected Functions:",
            ]
            for fn in r.get('functions', []):
                lines.append(f"  {fn.get('name', 'unknown')} @ {fn.get('address', 'unknown')} ({fn.get('location', 'unknown')})")
            lines.append("")
            lines.append(f"Ucode Type: {r.get('ucode_type', 'unknown')}")
            lines.append(f"Current Context: {r.get('context', {})}")
            self.os_text.insert("1.0", "\n".join(lines))
        else:
            self.os_text.insert("1.0", "OS detection failed.\n")
        self.os_text.config(state="disabled")

    def _add_bp(self):
        addr = self.bp_addr.get()
        if addr:
            self._safe_call("add_exec_breakpoint", {"address": addr})
            self._refresh_bp()

    def _remove_bp(self):
        sel = self.bp_list.curselection()
        if sel:
            idx = sel[0]
            item = self.bp_list.get(idx)
            # Parse index from item
            if "[" in item and "]" in item:
                bp_idx = int(item.split("[")[1].split("]")[0])
                self._safe_call("remove_breakpoint", {"index": bp_idx})
                self._refresh_bp()

    def _refresh_bp(self):
        r = self._safe_call("list_breakpoints")
        self.bp_list.delete(0, "end")
        if r:
            for bp in r:
                addr = bp.get("address", "unknown")
                idx = bp.get("index", -1)
                self.bp_list.insert("end", f"[{idx}] 0x{addr}")

    def _refresh_events(self):
        r = self._safe_call("get_trace_events", {"count": 50})
        self.event_text.config(state="normal")
        self.event_text.delete("1.0", "end")
        if r:
            filt = self.event_filter.get().lower()
            for e in r:
                etype = e.get("type", "?")
                if filt and filt not in etype.lower():
                    continue
                f = e.get("frame", "?")
                epc = e.get("pc", "")
                extra = ""
                if e.get("data"):
                    extra = " " + " ".join(f"{d['key']}={d['value']}" for d in e["data"][:2])
                self.event_text.insert("end", f"f{f:>6} {etype:<12s} {epc}{extra}\n")
        self.event_text.config(state="disabled")

    def _update_framebuffer(self):
        # Get latest captures
        r = self._safe_call("get_frame_captures")
        if r and isinstance(r, list):
            self._frame_captures = r
            self.capture_list.delete(0, "end")
            for cap in r:
                self.capture_list.insert("end",
                    f"Frame {cap.get('frame', '?')}: {cap.get('width', 0)}x{cap.get('height', 0)} {cap.get('size', 0)}B")
            # Show latest with actual pixel data
            if r:
                cap = r[-1]
                r2 = self._safe_call("read_framebuffer")
                if r2 and r2.get("pixels"):
                    self._show_capture(cap, r2.get("pixels"))
                else:
                    self._show_capture(cap)

    def _on_capture_select(self, event):
        sel = self.capture_list.curselection()
        if sel and self._frame_captures:
            idx = sel[0]
            if idx < len(self._frame_captures):
                cap = self._frame_captures[idx]
                # Fetch full pixel data for the latest capture
                if idx == len(self._frame_captures) - 1:
                    r = self._safe_call("read_framebuffer")
                    if r and r.get("pixels"):
                        self._show_capture(cap, r.get("pixels"))
                    else:
                        self._show_capture(cap)
                else:
                    self._show_capture(cap)

    def _render_framebuffer_image(self, cap, pixels_hex):
        """Convert hex pixel data to a tkinter PhotoImage and display it."""
        if not _PIL_AVAILABLE:
            return False
        try:
            w = cap.get('width', 0)
            h = cap.get('height', 0)
            bpp = cap.get('bpp', 4)
            if w == 0 or h == 0 or not pixels_hex:
                return False

            # Convert hex to bytes
            pixels_bytes = bytes.fromhex(pixels_hex)

            # Create PIL image
            if bpp == 4:
                # RGBA8888
                img = Image.frombytes('RGBA', (w, h), pixels_bytes)
            elif bpp == 2:
                # RGBA5551: need to convert
                img = self._convert_rgba5551_to_rgba8888(pixels_bytes, w, h)
            else:
                return False

            # Scale 2x for better visibility
            img = img.resize((w * 2, h * 2), Image.NEAREST)

            # Convert to tkinter format
            photo = ImageTk.PhotoImage(img)

            # Store reference to prevent garbage collection
            self._current_fb_image = photo

            # Draw on canvas
            self.fb_canvas.delete("all")
            self.fb_canvas.create_image(0, 0, anchor="nw", image=photo)
            self.fb_canvas.create_rectangle(0, 0, w*2, h*2, outline="#333", width=1)

            return True
        except Exception as e:
            print(f"Image render error: {e}")
            return False

    def _convert_rgba5551_to_rgba8888(self, data, width, height):
        """Convert RGBA5551 (2 bytes/pixel) to RGBA8888 PIL Image."""
        import array
        out = array.array('B', [0] * (width * height * 4))
        for i in range(width * height):
            pixel = (data[i * 2] << 8) | data[i * 2 + 1]
            r = (pixel >> 11) & 0x1F
            g = (pixel >> 6) & 0x1F
            b = (pixel >> 1) & 0x1F
            a = pixel & 0x1
            # Scale 5-bit to 8-bit
            out[i * 4 + 0] = (r << 3) | (r >> 2)
            out[i * 4 + 1] = (g << 3) | (g >> 2)
            out[i * 4 + 2] = (b << 3) | (b >> 2)
            out[i * 4 + 3] = 255 if a else 0
        return Image.frombytes('RGBA', (width, height), out.tobytes())

    def _show_capture(self, cap, pixels_hex=None):
        w = cap.get('width', 0)
        h = cap.get('height', 0)
        sz = cap.get('size', 0)
        frame = cap.get('frame', '?')
        bpp = cap.get('bpp', 4)

        self.fb_canvas.delete("all")
        view_mode = self.fb_view_mode.get()

        if view_mode == "image" and _PIL_AVAILABLE and pixels_hex:
            # Try to render image
            if self._render_framebuffer_image(cap, pixels_hex):
                # Update info text
                self.fb_info_text.config(state="normal")
                self.fb_info_text.delete("1.0", "end")
                self.fb_info_text.insert("1.0", f"Frame {frame}\n{w}x{h} {sz}B\nFormat: {'RGBA8888' if bpp == 4 else 'RGBA5551'}\nScaled: {w*2}x{h*2}")
                self.fb_info_text.config(state="disabled")
                return

        # Fallback to text display
        self.fb_canvas.create_text(320, 240, text=f"Frame {frame}\n{w}x{h}\n{sz} bytes\nFormat: {'RGBA8888' if bpp == 4 else 'RGBA5551'}",
                                   font=("Consolas", 16, "bold"), fill="#00ff00", justify="center")
        self.fb_canvas.create_rectangle(5, 5, 635, 475, outline="#333", width=2)

        # Update info text
        self.fb_info_text.config(state="normal")
        self.fb_info_text.delete("1.0", "end")
        if not _PIL_AVAILABLE:
            self.fb_info_text.insert("1.0", "PIL not available. Install Pillow for image rendering.\n")
        self.fb_info_text.insert("end", f"Frame {frame}\n{w}x{h} {sz}B\nFormat: {'RGBA8888' if bpp == 4 else 'RGBA5551'}")
        self.fb_info_text.config(state="disabled")

    # ── polling ─────────────────────────────────────────────

    def _poll(self):
        if not self.running:
            return

        s = self._safe_call("status")
        if not s:
            self.status_bar.config(text="Disconnected", fg="#ff0000")
            self.root.after(1000, self._poll)
            return

        self.status_bar.config(text="Connected", fg="#00ff00")

        frame = s.get("frame", "?")
        pc = s.get("pc", "0x0")
        paused = s.get("paused", True)

        self.lbl_frame.config(text=f"Frame: {frame}")
        self.lbl_pc.config(text=f"PC: {pc}")
        state_str = "PAUSED" if paused else "RUNNING"
        self.lbl_state.config(text=f"State: {state_str}")
        self.lbl_state.config(foreground="red" if paused else "green")

        # Scene
        scene, color = classify_scene(pc)
        self.lbl_scene.config(text=f"SCENE: {scene}", bg=color)

        # Registers
        regs = self._safe_call("get_registers")
        if regs:
            gpr = regs.get("gpr", [])
            for i in range(32):
                val_str = gpr[i] if i < len(gpr) else "0x0"
                try:
                    val = int(val_str, 16) if isinstance(val_str, str) else val_str
                except ValueError:
                    val = 0
                self.reg_labels[i].config(text=f"${i:02d}: 0x{val:08X}")
            self.reg_pc.config(text=f"PC: {pc}")

        # FB status
        fb = self._safe_call("read_framebuffer")
        if fb:
            w = fb.get("width", 0)
            h = fb.get("height", 0)
            sz = fb.get("size", 0)
            self.lbl_fb.config(text=f"FB: {w}x{h} {sz}B")

        # Update captures every 2 seconds
        if int(frame) % 10 < 2:
            self._update_framebuffer()

        # Update events
        self._refresh_events()

        # Analog stick
        self.stick_canvas.coords(self.stick_dot, 37, 37, 43, 43)

        self.root.after(250, self._poll)

    def run(self):
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.mainloop()

    def _on_close(self):
        self.running = False
        self.root.destroy()

def main():
    parser = argparse.ArgumentParser(description="N64 Emulation Dashboard")
    parser.add_argument("--port", type=int, default=9876)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()
    N64Viewer(port=args.port, host=args.host).run()

if __name__ == "__main__":
    main()
