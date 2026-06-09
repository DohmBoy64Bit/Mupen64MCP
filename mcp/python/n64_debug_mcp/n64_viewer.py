"""n64-viewer — Live N64 emulation status dashboard.

Connects to a running n64-debug-daemon. Shows live status, VI registers,
game data from RDRAM, and attempts framebuffer display.

Usage: python n64_viewer.py [--port 9876]
"""
import sys, time, struct, threading, argparse
sys.path.insert(0, "D:/Mupen64MCP/mcp/python")
from n64_debug_mcp.daemon_client import DaemonClient, DaemonConfig

try:
    import tkinter as tk
    from tkinter import ttk
except ImportError:
    print("tkinter not available. Install python-tk (e.g. python3-tk on Linux)")
    sys.exit(1)

def pch(v):
    if isinstance(v, str) and v.startswith("0x"):
        return int(v, 16)
    return int(v) if v else 0

class N64Viewer:
    def __init__(self, port=9876, host="127.0.0.1"):
        self.port = port
        self.host = host
        self.root = tk.Tk()
        self.root.title("N64 Viewer")
        self.root.geometry("520x520")

        cfg = DaemonConfig(core_path="dummy", port=port, host=host)
        self.dc = DaemonClient(cfg)
        try:
            self.dc._connect()
        except Exception as e:
            self._show_error(f"Cannot connect: {e}")
            return

        self.running = True
        self._build_ui()
        self._poll()

    def _show_error(self, msg):
        tk.Label(self.root, text=msg, fg="red", font=("", 12)).pack(expand=True)

    def _build_ui(self):
        sf = ttk.LabelFrame(self.root, text="Emulator Status", padding=4)
        sf.pack(fill="x", padx=4, pady=2)
        self.lbl_frame = ttk.Label(sf, text="Frame: --")
        self.lbl_frame.pack(anchor="w")
        self.lbl_pc = ttk.Label(sf, text="PC: --")
        self.lbl_pc.pack(anchor="w")
        self.lbl_state = ttk.Label(sf, text="State: --")
        self.lbl_state.pack(anchor="w")

        vf = ttk.LabelFrame(self.root, text="Video Interface", padding=4)
        vf.pack(fill="x", padx=4, pady=2)
        self.lbl_fb = ttk.Label(vf, text="FB: --")
        self.lbl_fb.pack(anchor="w")

        df = ttk.LabelFrame(self.root, text="Game Data", padding=4)
        df.pack(fill="both", expand=True, padx=4, pady=2)
        self.lbl_data = tk.Text(df, height=10, font=("Consolas", 9))
        self.lbl_data.pack(fill="both", expand=True)

        cf = ttk.Frame(self.root)
        cf.pack(fill="x", padx=4, pady=2)
        ttk.Button(cf, text="Pause", command=self._cmd("pause")).pack(side="left", padx=2)
        ttk.Button(cf, text="Resume", command=self._cmd("resume")).pack(side="left", padx=2)

    def _cmd(self, method):
        return lambda: self._safe_call(method)

    def _safe_call(self, method):
        try:
            self.dc.call(method)
        except Exception:
            pass

    def _poll(self):
        if not self.running:
            return
        try:
            s = self.dc.call("status")
        except Exception:
            self.root.after(2000, self._poll)
            return

        frame = s.get("frame", "?")
        pc = s.get("pc", "0x0")
        paused = s.get("paused", "?")
        self.lbl_frame.config(text=f"Frame: {frame}")
        self.lbl_pc.config(text=f"PC: {pc}")
        state_str = "PAUSED" if paused else "RUNNING"
        self.lbl_state.config(text=f"State: {state_str}", foreground="red" if paused else "green")

        # VI / framebuffer
        try:
            fb = self.dc.call("read_framebuffer")
            w = fb.get("width", 0)
            h = fb.get("height", 0)
            sz = fb.get("size", 0)
            pix = fb.get("pixels", "")
            nz = sum(1 for i in range(0, min(len(pix), 400), 4) if pix[i:i+4] != "0000")
            self.lbl_fb.config(text=f"FB: {w}x{h} {sz}B, non-zero pixels: {nz}/100")
        except Exception as e:
            self.lbl_fb.config(text=f"FB: error ({e})")

        # Game data
        lines = []
        for addr, label in [("0x8013A000", "Position"), ("0x80138000", "GameState")]:
            try:
                r = self.dc.call("read_mem", {"address": addr, "size": 16})
                h = r.get("hex", "")
                floats = []
                for i in range(0, min(len(h), 16), 8):
                    w_val = int(h[i:i+8], 16)
                    if w_val:
                        floats.append(f"{struct.unpack('>f', struct.pack('>I', w_val))[0]:.2f}")
                if floats:
                    lines.append(f"{label} ({addr}): {', '.join(floats[:4])}")
            except Exception:
                pass
        self.lbl_data.delete("1.0", "end")
        self.lbl_data.insert("1.0", "\n".join(lines) if lines else "(no data)")

        self.root.after(500, self._poll)

    def run(self):
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.mainloop()

    def _on_close(self):
        self.running = False
        self.root.destroy()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=9876)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()
    N64Viewer(port=args.port, host=args.host).run()

if __name__ == "__main__":
    main()
