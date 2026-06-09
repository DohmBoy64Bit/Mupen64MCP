"""n64-viewer — Live N64 emulation dashboard with scene detection, game state,
speed gauge, track view, event feed, and input injection buttons.

Usage: python n64_viewer.py [--port 9876]
"""
import sys, time, struct, argparse
sys.path.insert(0, "D:/Mupen64MCP/mcp/python")
from n64_debug_mcp.daemon_client import DaemonClient, DaemonConfig

try:
    import tkinter as tk
    from tkinter import ttk
except ImportError:
    print("tkinter not available.")
    sys.exit(1)

# ── helpers ────────────────────────────────────────────────────

def pch(v):
    if isinstance(v, str) and v.startswith("0x"):
        return int(v, 16)
    return int(v) if v else 0

def classify_scene(pc, speed):
    pc = pch(pc)
    if 0x80124000 <= pc <= 0x80125FFF:
        return ("Attract Mode", "#3366CC")
    if 0x80102000 <= pc <= 0x80103FFF:
        if speed and speed > 10:
            return ("Menu (moving)", "#CC8800")
        return ("Menu / Title", "#CC8800")
    if 0x80115000 <= pc <= 0x80116FFF:
        return ("Active Code", "#33AA33")
    if 0x80100000 <= pc <= 0x80103FFF:
        return ("Boot / Init", "#AA44AA")
    if speed and speed > 10:
        return ("Racing", "#33CC33")
    return (f"Unknown", "#888888")

# 0x8013A000 layout: two consecutive car states (each 4 floats = 16 bytes)
CAR_STATE_FIELDS = [
    ("pos_x",      0,  "X position"),
    ("pos_z",      4,  "Z position (track progress)"),
    ("steer_y",    8,  "Steering / Y velocity"),
    ("speed",     12,  "Speed"),
    ("pos_x_n",   16,  "X (next frame)"),
    ("pos_z_n",   20,  "Z (next frame)"),
    ("steer_y_n", 24,  "Steering (next frame)"),
    ("unk_1",     28,  "Unknown (normalization?)"),
]

BUTTON_VALS = {
    "START": 16, "A": 128, "B": 64, "Z": 32, "R": 4096, "L": 8192,
    "U": 8, "D": 4, "L_DPAD": 2, "R_DPAD": 1,
}

# ── main window ────────────────────────────────────────────────

class N64Viewer:
    def __init__(self, port=9876, host="127.0.0.1"):
        self.port = port
        self.host = host
        self.root = tk.Tk()
        self.root.title("N64 Viewer")
        self.root.geometry("680x720")

        cfg = DaemonConfig(core_path="dummy", port=port, host=host)
        self.dc = DaemonClient(cfg)
        try:
            self.dc._connect()
        except Exception as e:
            self._show_error(f"Cannot connect: {e}")
            return

        self.running = True
        self._last_positions = []
        self._build_ui()
        self._poll()

    def _show_error(self, msg):
        tk.Label(self.root, text=msg, fg="red", font=("", 12)).pack(expand=True)

    def _safe_call(self, method, params=None):
        try:
            return self.dc.call(method, params)
        except Exception:
            return None

    # ── ui construction ──────────────────────────────────────

    def _build_ui(self):
        # ── status bar ──
        sf = ttk.LabelFrame(self.root, text="Emulator Status", padding=4)
        sf.pack(fill="x", padx=4, pady=2)

        top = ttk.Frame(sf)
        top.pack(fill="x")
        self.lbl_scene = ttk.Label(top, text="Scene: --", font=("", 10, "bold"),
                                   foreground="white", background="#888888", padding=(4, 0))
        self.lbl_scene.pack(side="right", padx=4)

        self.lbl_frame = ttk.Label(sf, text="Frame: --")
        self.lbl_frame.pack(anchor="w")
        self.lbl_pc = ttk.Label(sf, text="PC: --")
        self.lbl_pc.pack(anchor="w")
        self.lbl_state = ttk.Label(sf, text="State: --")
        self.lbl_state.pack(anchor="w")
        self.lbl_fb = ttk.Label(sf, text="FB: --")
        self.lbl_fb.pack(anchor="w")

        # ── speed + steering ──
        gf = ttk.LabelFrame(self.root, text="Speed & Steering", padding=4)
        gf.pack(fill="x", padx=4, pady=2)

        self.lbl_speed = ttk.Label(gf, text="Speed: --")
        self.lbl_speed.pack(anchor="w")
        self.speed_canvas = tk.Canvas(gf, width=640, height=16, bg="#222", highlightthickness=0)
        self.speed_canvas.pack(fill="x", padx=2, pady=2)

        self.lbl_steer = ttk.Label(gf, text="Steer: --")
        self.lbl_steer.pack(anchor="w")
        self.steer_canvas = tk.Canvas(gf, width=640, height=16, bg="#222", highlightthickness=0)
        self.steer_canvas.pack(fill="x", padx=2, pady=2)

        # ── track position ──
        tf = ttk.LabelFrame(self.root, text="Track Position (top-down)", padding=4)
        tf.pack(fill="x", padx=4, pady=2)

        self.track_canvas = tk.Canvas(tf, width=640, height=140, bg="#111", highlightthickness=0)
        self.track_canvas.pack()
        self.lbl_pos = ttk.Label(tf, text="X: --  Z: --")
        self.lbl_pos.pack(anchor="w")

        # ── game data table ──
        df = ttk.LabelFrame(self.root, text="Game Data (0x8013A000)", padding=4)
        df.pack(fill="x", padx=4, pady=2)
        self.data_text = tk.Text(df, height=7, font=("Consolas", 9), wrap="none")
        self.data_text.pack(fill="x")

        # ── event feed ──
        ef = ttk.LabelFrame(self.root, text="Events", padding=4)
        ef.pack(fill="both", expand=True, padx=4, pady=2)
        self.event_text = tk.Text(ef, height=6, font=("Consolas", 9), wrap="none",
                                  state="disabled", bg="#0a0a0a", fg="#ccc")
        self.event_text.pack(fill="both", expand=True)

        # ── controls ──
        cf = ttk.Frame(self.root)
        cf.pack(fill="x", padx=4, pady=2)

        ttk.Button(cf, text="Pause", command=self._cmd("pause")).pack(side="left", padx=2)
        ttk.Button(cf, text="Resume", command=self._cmd("resume")).pack(side="left", padx=2)
        ttk.Separator(cf, orient="vertical").pack(side="left", fill="y", padx=6)
        for name in ("START", "A", "B", "Z", "A+R", "START+A"):
            ttk.Button(cf, text=name, command=self._inject_cmd(name)).pack(side="left", padx=1)

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

    # ── polling ─────────────────────────────────────────────

    def _poll(self):
        if not self.running:
            return
        s = self._safe_call("status")
        if not s:
            self.root.after(1000, self._poll)
            return

        frame = s.get("frame", "?")
        pc = s.get("pc", "0x0")
        paused = s.get("paused", "?")

        self.lbl_frame.config(text=f"Frame: {frame}")
        self.lbl_pc.config(text=f"PC: {pc}")
        state_str = "PAUSED" if paused else "RUNNING"
        self.lbl_state.config(text=f"State: {state_str}",
                              foreground="red" if paused else "green")

        # ── scene ──
        raw = self._safe_call("read_mem", {"address": "0x8013A00C", "size": 4})
        speed_now = None
        if raw:
            h = raw.get("hex", "")
            if h and len(h) >= 8:
                speed_now = struct.unpack(">f", struct.pack(">I", int(h[:8], 16)))[0]
        scene, color = classify_scene(pc, speed_now)
        self.lbl_scene.config(text=scene, background=color)

        # ── speed bar ──
        if speed_now is not None:
            self.lbl_speed.config(text=f"Speed: {speed_now:.2f}")
        self.speed_canvas.delete("all")
        bar_w = min(int((speed_now or 0) / 1000 * 620), 620)
        self.speed_canvas.create_rectangle(0, 0, bar_w, 16, fill="#33CC33", outline="")
        self.speed_canvas.create_text(bar_w + 4, 8, anchor="w",
                                      text=f"{speed_now:.0f}" if speed_now else "0",
                                      fill="#ccc", font=("Consolas", 8))

        # ── game data (0x8013A000) ──
        r = self._safe_call("read_mem", {"address": "0x8013A000", "size": 64})
        floats = {}
        if r:
            h = r.get("hex", "")
            for name, offset, desc in CAR_STATE_FIELDS:
                if offset * 2 + 8 <= len(h):
                    w = int(h[offset*2:offset*2+8], 16)
                    floats[name] = struct.unpack(">f", struct.pack(">I", w))[0]
                else:
                    floats[name] = 0.0

        self.data_text.delete("1.0", "end")
        if floats:
            rows = []
            for name, offset, desc in CAR_STATE_FIELDS:
                val = floats.get(name, 0)
                pair = floats.get(name.replace("_n", "_next") if "_n" in name else "", None)
                rows.append(f"  {name:<12s} {val:>8.2f}  ({desc})")
            self.data_text.insert("1.0", "\n".join(rows))

            # ── steer ──
            steer = floats.get("steer_y", 0)
            self.lbl_steer.config(text=f"Steer: {steer:.2f}")
            self.steer_canvas.delete("all")
            cx = int((steer + 1.0) / 2.0 * 620)
            cx = max(10, min(630, cx))
            self.steer_canvas.create_line(10, 8, 630, 8, fill="#555")
            self.steer_canvas.create_oval(cx - 4, 4, cx + 4, 12, fill="#FF8800", outline="")

            # ── track position ──
            px = floats.get("pos_x", 0)
            pz = floats.get("pos_z", 0)
            self.lbl_pos.config(text=f"X: {px:.2f}  Z: {pz:.2f}")
            self._last_positions.append((px, pz))
            if len(self._last_positions) > 30:
                self._last_positions.pop(0)

            # Normalize positions for display
            self.track_canvas.delete("all")
            if self._last_positions:
                xs = [p[0] for p in self._last_positions]
                zs = [p[1] for p in self._last_positions]
                min_x, max_x = min(xs), max(xs)
                min_z, max_z = min(zs), max(zs)
                rx = max(max_x - min_x, 100)
                rz = max(max_z - min_z, 100)
                pts = []
                for x, z in self._last_positions:
                    sx = 40 + int((x - min_x) / rx * 560)
                    sy = 110 - int((z - min_z) / rz * 100)
                    pts.append((sx, sy))
                # trail
                for i in range(1, len(pts)):
                    alpha = int(80 * i / len(pts))
                    color = f"#{alpha:02x}{alpha:02x}{alpha:02x}"
                    self.track_canvas.create_line(pts[i-1][0], pts[i-1][1],
                                                  pts[i][0], pts[i][1],
                                                  fill="#88ff88", width=2)
                # current position dot
                if pts:
                    self.track_canvas.create_oval(pts[-1][0]-4, pts[-1][1]-4,
                                                  pts[-1][0]+4, pts[-1][1]+4,
                                                  fill="#ff4444", outline="")

        # ── events ──
        try:
            evts = self._safe_call("get_trace_events", {"count": 30})
            if evts:
                self.event_text.config(state="normal")
                self.event_text.delete("1.0", "end")
                for e in evts[-20:]:
                    f = e.get("frame", "?")
                    t = e.get("type", "?")
                    epc = e.get("pc", "")
                    extra = ""
                    if e.get("data"):
                        extra = " " + " ".join(f"{d['key']}={d['value']}" for d in e["data"][:2])
                    self.event_text.insert("end",
                        f"f{f:>6} {t:<12s} {epc}{extra}\n")
                self.event_text.config(state="disabled")
        except Exception:
            pass

        # ── fb status ──
        fb = self._safe_call("read_framebuffer")
        if fb:
            w = fb.get("width", 0)
            h = fb.get("height", 0)
            sz = fb.get("size", 0)
            self.lbl_fb.config(text=f"FB: {w}x{h} {sz}B")

        self.root.after(500, self._poll)

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
