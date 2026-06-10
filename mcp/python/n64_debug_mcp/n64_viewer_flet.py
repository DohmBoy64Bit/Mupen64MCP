"""Flet-based Steam-style N64 Live Viewer Dashboard.

Connects to n64-debug-daemon via JSON-RPC over TCP.
Auto-connects on startup; shows startup screen if daemon not found.
"""

import json
import socket
import time
import threading
import os
import subprocess
import glob
from pathlib import Path
from typing import Any, Optional

try:
    import flet as ft
except ImportError:
    print("Flet not installed. Run: pip install flet")
    raise

# ── Steam-like color palette ─────────────────────────────────
BG_MAIN = "#0d1117"
BG_CARD = "#161b22"
BG_CARD_HOVER = "#1c2128"
BORDER_CARD = "#30363d"
ACCENT = "#1a9fff"
ACCENT_HOVER = "#66c0f4"
TEXT_PRIMARY = "#c9d1d9"
TEXT_SECONDARY = "#8b949e"
STATUS_GREEN = "#238636"
STATUS_RED = "#da3633"
STATUS_YELLOW = "#d29922"
STATUS_BLUE = "#1a9fff"

# ── Daemon client ────────────────────────────────────────────
class DaemonClient:
    def __init__(self, host="127.0.0.1", port=9876):
        self.host = host
        self.port = port
        self._seq = 0

    def _call(self, method: str, params: dict | None = None, timeout: float = 10.0) -> Any:
        self._seq += 1
        req = json.dumps({"jsonrpc": "2.0", "id": self._seq, "method": method, "params": params or {}}, separators=(',', ':'))
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        try:
            sock.connect((self.host, self.port))
            sock.sendall((req + "\n").encode())
            buf = b""
            while True:
                c = sock.recv(32768)
                if not c:
                    break
                buf += c
                try:
                    resp = json.loads(buf.decode("utf-8", errors="replace"))
                    if "error" in resp:
                        return {"_error": resp["error"]["message"]}
                    return resp.get("result", {})
                except (json.JSONDecodeError, UnicodeDecodeError):
                    continue
        except Exception as e:
            return {"_error": str(e)}
        finally:
            sock.close()

    def call(self, method: str, params: dict | None = None, timeout: float = 10.0) -> Any:
        return self._call(method, params, timeout)

    def ping(self, timeout: float = 2.0) -> bool:
        r = self._call("ping", timeout=timeout)
        return isinstance(r, dict) and r.get("pong", False)

# ── Helpers ──────────────────────────────────────────────────
def find_roms():
    roms_dir = Path(__file__).parent.parent.parent / "roms"
    if not roms_dir.exists():
        roms_dir = Path("roms")
    files = sorted(roms_dir.glob("*.z64")) + sorted(roms_dir.glob("*.n64")) + sorted(roms_dir.glob("*.v64"))
    return [(f.name, str(f)) for f in files]

# ── Main App ─────────────────────────────────────────────────
def main(page: ft.Page):
    page.title = "N64 Viewer"
    page.theme_mode = ft.ThemeMode.DARK
    page.bgcolor = BG_MAIN
    page.padding = 0
    page.spacing = 0
    page.window_width = 1200
    page.window_height = 800

    dc = DaemonClient()
    daemon_proc: Optional[subprocess.Popen] = None
    poll_thread: Optional[threading.Thread] = None
    poll_stop = threading.Event()
    connected = False
    current_rom_name = "Unknown"
    frame_count = 0
    last_frame = 0
    last_frame_time = time.time()
    fps = 0
    selected_tab = "fb"

    # ── State ───────────────────────────────────────────────
    status_data = {"paused": True, "pc": "0x00000000", "frame_count": 0, "running": False}
    registers_data = {}
    os_info_data = {}
    events_data = []
    breakpoints_data = []
    captures_data = []
    mem_address = "0x80000000"
    mem_size = 256

    # ── UI References ───────────────────────────────────────
    # Top bar
    rom_text = ft.Text(current_rom_name, size=14, color=TEXT_PRIMARY, weight=ft.FontWeight.BOLD)
    fps_text = ft.Text("● 0 FPS", size=12, color=TEXT_SECONDARY)
    status_dot = ft.Container(width=8, height=8, bgcolor=STATUS_RED, border_radius=4)

    # Status card
    frame_text = ft.Text("0", size=28, color=TEXT_PRIMARY, weight=ft.FontWeight.BOLD, font_family="Consolas")
    pc_text = ft.Text("PC: 0x00000000", size=12, color=TEXT_SECONDARY, font_family="Consolas")
    state_text = ft.Text("STOPPED", size=12, color=STATUS_RED, weight=ft.FontWeight.BOLD)

    # Controls
    btn_resume = ft.ElevatedButton("▶ Resume", bgcolor=STATUS_GREEN, color="white", on_click=lambda e: send_resume())
    btn_pause = ft.ElevatedButton("⏸ Pause", bgcolor=STATUS_YELLOW, color="white", on_click=lambda e: send_pause())
    btn_step = ft.ElevatedButton("⏭ Step", bgcolor=ACCENT, color="white", on_click=lambda e: send_step())

    # Input buttons
    btn_a = ft.ElevatedButton("A", width=40, height=35, bgcolor="#ef4444", color="white", on_click=lambda e: send_input("0x0080"))
    btn_b = ft.ElevatedButton("B", width=40, height=35, bgcolor="#eab308", color="white", on_click=lambda e: send_input("0x0040"))
    btn_z = ft.ElevatedButton("Z", width=40, height=35, bgcolor="#a855f7", color="white", on_click=lambda e: send_input("0x0020"))
    btn_start = ft.ElevatedButton("START", width=70, height=35, bgcolor="#06b6d4", color="white", on_click=lambda e: send_input("0x0010"))
    btn_r = ft.ElevatedButton("R", width=40, height=35, bgcolor=BG_CARD, color=TEXT_PRIMARY, on_click=lambda e: send_input("0x1000"))
    btn_l = ft.ElevatedButton("L", width=40, height=35, bgcolor=BG_CARD, color=TEXT_PRIMARY, on_click=lambda e: send_input("0x2000"))
    btn_reset = ft.ElevatedButton("Reset", width=80, height=30, bgcolor=BORDER_CARD, color=TEXT_PRIMARY, on_click=lambda e: reset_stick())

    # Analog stick
    stick_x = 0
    stick_y = 0
    stick_dot = ft.Container(width=12, height=12, bgcolor=ACCENT, border_radius=6, left=58, top=58)
    stick_label = ft.Text("X: 0  Y: 0", size=10, color=TEXT_SECONDARY, font_family="Consolas")
    stick_gesture = None

    # Sidebar
    nav_items = []
    nav_refs = {}

    # Main content areas (one per tab)
    fb_image = ft.Image(src="", width=320, height=240, fit=ft.BoxFit.CONTAIN, border_radius=4)
    fb_info = ft.Text("No framebuffer data", size=12, color=TEXT_SECONDARY)
    fb_capture_list = ft.ListView(height=200, spacing=2, auto_scroll=True)
    fb_interval = ft.TextField(value="5", width=50, text_align=ft.TextAlign.CENTER, bgcolor=BG_CARD, border_color=BORDER_CARD, color=TEXT_PRIMARY, text_size=12)

    reg_grid = ft.GridView(runs_count=4, spacing=4, run_spacing=4, padding=8, height=400)

    mem_text = ft.TextField(multiline=True, read_only=True, min_lines=15, bgcolor=BG_CARD, border_color=BORDER_CARD, color=TEXT_PRIMARY, text_size=11)
    mem_addr_field = ft.TextField(value="0x80000000", width=120, bgcolor=BG_CARD, border_color=BORDER_CARD, color=TEXT_PRIMARY, text_size=12)
    mem_size_field = ft.TextField(value="256", width=60, bgcolor=BG_CARD, border_color=BORDER_CARD, color=TEXT_PRIMARY, text_size=12)

    os_text = ft.TextField(multiline=True, read_only=True, min_lines=15, bgcolor=BG_CARD, border_color=BORDER_CARD, color=TEXT_PRIMARY, text_size=12)

    events_list = ft.ListView(height=300, spacing=2, auto_scroll=True)
    btn_trace_callchain = ft.ElevatedButton("Callchain: OFF", width=140, height=30, bgcolor=BG_CARD, color=TEXT_SECONDARY, on_click=lambda e: toggle_trace("callchain"))
    btn_trace_scheduler = ft.ElevatedButton("Scheduler: OFF", width=140, height=30, bgcolor=BG_CARD, color=TEXT_SECONDARY, on_click=lambda e: toggle_trace("scheduler"))
    btn_trace_rom = ft.ElevatedButton("ROM Reads: OFF", width=140, height=30, bgcolor=BG_CARD, color=TEXT_SECONDARY, on_click=lambda e: toggle_trace("rom_reads"))
    btn_trace_struct = ft.ElevatedButton("Struct: OFF", width=140, height=30, bgcolor=BG_CARD, color=TEXT_SECONDARY, on_click=lambda e: toggle_trace("struct"))
    btn_clear_events = ft.ElevatedButton("Clear", width=80, height=30, bgcolor=BORDER_CARD, color=TEXT_PRIMARY, on_click=lambda e: clear_events())

    bp_list = ft.ListView(height=300, spacing=2, auto_scroll=True)
    bp_address = ft.TextField(value="0x80000000", width=150, bgcolor=BG_CARD, border_color=BORDER_CARD, color=TEXT_PRIMARY, text_size=12)
    btn_add_bp = ft.ElevatedButton("Add", width=60, height=30, bgcolor=ACCENT, color="white", on_click=lambda e: add_bp())

    # ── Content panels ─────────────────────────────────────
    def build_fb_tab():
        return ft.Column([
            ft.Row([ft.Text("Framebuffer", size=16, color=TEXT_PRIMARY, weight=ft.FontWeight.BOLD)], alignment=ft.MainAxisAlignment.CENTER),
            ft.Row([fb_image], alignment=ft.MainAxisAlignment.CENTER),
            ft.Row([fb_info], alignment=ft.MainAxisAlignment.CENTER),
            ft.Row([
                ft.Text("Capture Interval:", size=12, color=TEXT_SECONDARY),
                fb_interval,
                ft.ElevatedButton("Set", width=50, height=30, bgcolor=ACCENT, color="white", on_click=lambda e: set_capture_interval()),
                ft.ElevatedButton("Refresh", width=70, height=30, bgcolor=BG_CARD, color=TEXT_PRIMARY, on_click=lambda e: refresh_fb()),
            ], alignment=ft.MainAxisAlignment.CENTER, spacing=8),
            ft.Divider(height=1, color=BORDER_CARD),
            ft.Text("Recent Captures", size=14, color=TEXT_PRIMARY, weight=ft.FontWeight.BOLD),
            fb_capture_list,
        ], scroll=ft.ScrollMode.AUTO, expand=True)

    def build_reg_tab():
        return ft.Column([
            ft.Text("CPU Registers", size=16, color=TEXT_PRIMARY, weight=ft.FontWeight.BOLD),
            ft.Text("32 General Purpose Registers", size=11, color=TEXT_SECONDARY),
            ft.Container(content=reg_grid, border=ft.BorderSide(1, BORDER_CARD), border_radius=4, padding=4, bgcolor=BG_CARD),
        ], scroll=ft.ScrollMode.AUTO, expand=True)

    def build_mem_tab():
        return ft.Column([
            ft.Text("Memory Viewer", size=16, color=TEXT_PRIMARY, weight=ft.FontWeight.BOLD),
            ft.Row([
                ft.Text("Address:", size=12, color=TEXT_SECONDARY),
                mem_addr_field,
                ft.Text("Size:", size=12, color=TEXT_SECONDARY),
                mem_size_field,
                ft.ElevatedButton("Read", width=60, height=30, bgcolor=ACCENT, color="white", on_click=lambda e: read_memory()),
            ], spacing=8),
            ft.Container(content=mem_text, border=ft.BorderSide(1, BORDER_CARD), border_radius=4, padding=8, bgcolor=BG_CARD, expand=True),
        ], scroll=ft.ScrollMode.AUTO, expand=True)

    def build_os_tab():
        return ft.Column([
            ft.Text("OS Information", size=16, color=TEXT_PRIMARY, weight=ft.FontWeight.BOLD),
            ft.Row([
                ft.ElevatedButton("🔍 Detect OS", width=120, height=35, bgcolor=ACCENT, color="white", on_click=lambda e: detect_os()),
            ], spacing=8),
            ft.Container(content=os_text, border=ft.BorderSide(1, BORDER_CARD), border_radius=4, padding=8, bgcolor=BG_CARD, expand=True),
        ], scroll=ft.ScrollMode.AUTO, expand=True)

    def build_events_tab():
        return ft.Column([
            ft.Text("Trace Events", size=16, color=TEXT_PRIMARY, weight=ft.FontWeight.BOLD),
            ft.Row([
                btn_trace_callchain, btn_trace_scheduler, btn_trace_rom, btn_trace_struct,
                ft.Container(width=20),
                btn_clear_events,
            ], spacing=8, wrap=True),
            ft.Container(content=events_list, border=ft.BorderSide(1, BORDER_CARD), border_radius=4, padding=8, bgcolor=BG_CARD, expand=True),
        ], scroll=ft.ScrollMode.AUTO, expand=True)

    def build_bp_tab():
        return ft.Column([
            ft.Text("Breakpoints", size=16, color=TEXT_PRIMARY, weight=ft.FontWeight.BOLD),
            ft.Row([
                ft.Text("Address:", size=12, color=TEXT_SECONDARY),
                bp_address,
                btn_add_bp,
                ft.ElevatedButton("Remove All", width=90, height=30, bgcolor=STATUS_RED, color="white", on_click=lambda e: remove_all_bp()),
            ], spacing=8),
            ft.Container(content=bp_list, border=ft.BorderSide(1, BORDER_CARD), border_radius=4, padding=8, bgcolor=BG_CARD, expand=True),
        ], scroll=ft.ScrollMode.AUTO, expand=True)

    content_area = ft.Container(content=build_fb_tab(), expand=True, padding=12)

    # ── Sidebar Builder ────────────────────────────────────
    def make_nav(icon, label, key):
        def on_click(e):
            select_tab(key)
        c = ft.Container(
            content=ft.Row([
                ft.Text(icon, size=18),
                ft.Text(label, size=13, color=TEXT_PRIMARY),
            ], spacing=8),
            padding=ft.Padding(left=16, top=10, right=16, bottom=10),
            border_radius=4,
            on_click=on_click,
            bgcolor=BG_CARD if key == selected_tab else None,
            border=ft.Border(left=ft.BorderSide(3, ACCENT if key == selected_tab else "transparent"))
        )
        nav_refs[key] = c
        return c

    sidebar = ft.Container(
        content=ft.Column([
            ft.Container(height=12),
            make_nav("📺", "Framebuffer", "fb"),
            make_nav("📊", "Registers", "reg"),
            make_nav("💾", "Memory", "mem"),
            make_nav("ℹ️", "OS Info", "os"),
            make_nav("📡", "Events", "evt"),
            make_nav("🛑", "Breakpoints", "bp"),
            ft.Divider(height=1, color=BORDER_CARD),
            make_nav("⚙️", "Settings", "set"),
        ], spacing=2),
        width=160,
        bgcolor=BG_CARD,
        border=ft.Border(right=ft.BorderSide(1, BORDER_CARD))
    )

    # ── Top Bar ─────────────────────────────────────────────
    top_bar = ft.Container(
        content=ft.Row([
            ft.Text("🎮 N64 Viewer", size=18, color=TEXT_PRIMARY, weight=ft.FontWeight.BOLD),
            ft.Container(expand=True),
            rom_text,
            ft.Container(width=8),
            fps_text,
            ft.Container(width=8),
            status_dot,
        ], alignment=ft.MainAxisAlignment.SPACE_BETWEEN),
        height=48,
        padding=ft.Padding(left=16, top=0, right=16, bottom=0),
        border=ft.Border(bottom=ft.BorderSide(1, BORDER_CARD)),
        bgcolor=BG_CARD
    )

    # ── Status Card ───────────────────────────────────────
    status_card = ft.Container(
        content=ft.Column([
            ft.Text("Status", size=12, color=TEXT_SECONDARY, weight=ft.FontWeight.BOLD),
            frame_text,
            pc_text,
            state_text,
        ], spacing=4),
        padding=12,
        border_radius=8,
        bgcolor=BG_CARD,
        border=ft.BorderSide(1, BORDER_CARD),
        width=140
    )

    # ── Controls Card ─────────────────────────────────────
    controls_card = ft.Container(
        content=ft.Column([
            ft.Text("Controls", size=12, color=TEXT_SECONDARY, weight=ft.FontWeight.BOLD),
            ft.Row([btn_resume, btn_pause, btn_step], spacing=6),
        ], spacing=8),
        padding=12,
        border_radius=8,
        bgcolor=BG_CARD,
        border=ft.BorderSide(1, BORDER_CARD),
        expand=True
    )

    # ── Input Card ─────────────────────────────────────────
    input_card = ft.Container(
        content=ft.Column([
            ft.Text("Input", size=12, color=TEXT_SECONDARY, weight=ft.FontWeight.BOLD),
            ft.Row([btn_a, btn_b, btn_z, btn_start], spacing=4),
            ft.Row([btn_r, btn_l], spacing=4),
            ft.Row([stick_label], alignment=ft.MainAxisAlignment.CENTER),
            # Analog stick area
            ft.GestureDetector(
                content=ft.Container(
                    content=ft.Stack([
                        ft.Container(width=120, height=120, border_radius=60, border=ft.BorderSide(2, BORDER_CARD), bgcolor=BG_CARD),
                        ft.Container(width=80, height=80, border_radius=40, border=ft.BorderSide(1, BORDER_CARD), left=20, top=20),
                        ft.Container(width=4, height=4, border_radius=2, bgcolor=TEXT_SECONDARY, left=58, top=58),
                        stick_dot,
                    ]),
                    width=120,
                    height=120,
                    alignment=ft.alignment.center,
                ),
                on_pan_start=on_stick_start,
                on_pan_update=on_stick_update,
                on_pan_end=on_stick_end,
                drag_interval=16,
                mouse_cursor=ft.MouseCursor.MOVE,
            ),
            ft.Row([btn_reset], alignment=ft.MainAxisAlignment.CENTER),
        ], spacing=6, alignment=ft.MainAxisAlignment.CENTER),
        padding=12,
        border_radius=8,
        bgcolor=BG_CARD,
        border=ft.BorderSide(1, BORDER_CARD),
        width=200,
        alignment=ft.alignment.center
    )

    # ── Right panel (cards) ─────────────────────────────────
    right_panel = ft.Container(
        content=ft.Column([
            status_card,
            ft.Container(height=8),
            controls_card,
            ft.Container(height=8),
            input_card,
        ], spacing=0),
        width=220,
        padding=ft.Padding(left=8, top=8, right=8, bottom=8),
        border=ft.Border(left=ft.BorderSide(1, BORDER_CARD)),
        bgcolor=BG_MAIN
    )

    # ── Main layout ─────────────────────────────────────────
    main_layout = ft.Row([
        sidebar,
        content_area,
        right_panel,
    ], expand=True)

    body = ft.Column([top_bar, main_layout], expand=True, spacing=0)

    # ── Startup Screen ─────────────────────────────────────
    rom_dropdown = ft.Dropdown(
        options=[ft.dropdown.Option(name, name) for name, path in find_roms()],
        width=300,
        bgcolor=BG_CARD,
        border_color=BORDER_CARD,
        color=TEXT_PRIMARY,
    )
    host_field = ft.TextField(value="127.0.0.1", width=140, bgcolor=BG_CARD, border_color=BORDER_CARD, color=TEXT_PRIMARY)
    port_field = ft.TextField(value="9876", width=80, bgcolor=BG_CARD, border_color=BORDER_CARD, color=TEXT_PRIMARY)
    startup_status = ft.Text("Checking for daemon...", size=14, color=TEXT_SECONDARY)
    btn_start_daemon = ft.ElevatedButton("🚀 Start Daemon & Connect", width=220, height=45, bgcolor=ACCENT, color="white", on_click=lambda e: start_daemon_click())
    btn_connect = ft.ElevatedButton("🔗 Connect to Existing", width=200, height=40, bgcolor=BG_CARD, color=TEXT_PRIMARY, on_click=lambda e: connect_click())

    startup_screen = ft.Container(
        content=ft.Column([
            ft.Text("🎮 N64 Viewer", size=32, color=TEXT_PRIMARY, weight=ft.FontWeight.BOLD),
            ft.Container(height=24),
            ft.Container(
                content=ft.Column([
                    startup_status,
                    ft.Container(height=24),
                    ft.Row([ft.Text("ROM:", size=14, color=TEXT_SECONDARY), rom_dropdown], spacing=8),
                    ft.Container(height=12),
                    ft.Row([ft.Text("Host:", size=14, color=TEXT_SECONDARY), host_field, ft.Text("Port:", size=14, color=TEXT_SECONDARY), port_field], spacing=8),
                    ft.Container(height=24),
                    ft.Row([btn_start_daemon, btn_connect], spacing=16),
                ], alignment=ft.MainAxisAlignment.CENTER, horizontal_alignment=ft.CrossAxisAlignment.CENTER),
                padding=32,
                border_radius=16,
                bgcolor=BG_CARD,
                border=ft.BorderSide(1, BORDER_CARD),
                width=480,
            ),
        ], alignment=ft.MainAxisAlignment.CENTER, horizontal_alignment=ft.CrossAxisAlignment.CENTER),
        alignment=ft.alignment.center,
        expand=True
    )

    # ── Functions ──────────────────────────────────────────
    def select_tab(key):
        nonlocal selected_tab
        selected_tab = key
        # Update sidebar highlights
        for k, ref in nav_refs.items():
            ref.bgcolor = BG_CARD if k == key else None
            ref.border = ft.Border(left=ft.BorderSide(3, ACCENT if k == key else "transparent"))
        # Update content
        if key == "fb":
            content_area.content = build_fb_tab()
        elif key == "reg":
            content_area.content = build_reg_tab()
        elif key == "mem":
            content_area.content = build_mem_tab()
        elif key == "os":
            content_area.content = build_os_tab()
        elif key == "evt":
            content_area.content = build_events_tab()
        elif key == "bp":
            content_area.content = build_bp_tab()
        elif key == "set":
            content_area.content = ft.Column([
                ft.Text("Settings", size=16, color=TEXT_PRIMARY, weight=ft.FontWeight.BOLD),
                ft.Text("Connection", size=14, color=TEXT_PRIMARY),
                ft.Text(f"Host: {dc.host}", size=12, color=TEXT_SECONDARY),
                ft.Text(f"Port: {dc.port}", size=12, color=TEXT_SECONDARY),
                ft.Text("Polling interval: 500ms", size=12, color=TEXT_SECONDARY),
                ft.Divider(height=1, color=BORDER_CARD),
                ft.Text("Theme", size=14, color=TEXT_PRIMARY),
                ft.Text("Dark mode (Steam style)", size=12, color=TEXT_SECONDARY),
            ], scroll=ft.ScrollMode.AUTO, expand=True)
        page.update()

    def send_resume():
        dc.call("resume", timeout=5)
    def send_pause():
        dc.call("pause", timeout=5)
    def send_step():
        dc.call("step_instruction", timeout=5)
        time.sleep(0.2)
        refresh_status()
    def send_input(buttons_hex):
        dc.call("set_controller_state", {"channel": 0, "buttons": buttons_hex, "x": stick_x, "y": stick_y}, timeout=5)
    def reset_stick():
        nonlocal stick_x, stick_y
        stick_x, stick_y = 0, 0
        stick_dot.left = 58
        stick_dot.top = 58
        stick_label.value = "X: 0  Y: 0"
        send_input("0x0000")
        page.update()

    def on_stick_start(e: ft.DragStartEvent):
        pass
    def on_stick_update(e: ft.DragUpdateEvent):
        nonlocal stick_x, stick_y
        # Center of container is 60,60
        cx, cy = 60, 60
        dx = e.local_x - cx
        dy = e.local_y - cy
        # Clamp to radius 50
        dist = (dx*dx + dy*dy) ** 0.5
        max_r = 50
        if dist > max_r:
            dx = dx * max_r / dist
            dy = dy * max_r / dist
        # Map to -128..127
        stick_x = max(-128, min(127, int(dx * 128 / max_r)))
        stick_y = max(-128, min(127, int(-dy * 128 / max_r)))  # Y inverted
        stick_dot.left = cx + dx - 6
        stick_dot.top = cy + dy - 6
        stick_label.value = f"X: {stick_x}  Y: {stick_y}"
        page.update()
    def on_stick_end(e: ft.DragEndEvent):
        # Spring back to center
        nonlocal stick_x, stick_y
        stick_x, stick_y = 0, 0
        stick_dot.left = 58
        stick_dot.top = 58
        stick_label.value = "X: 0  Y: 0"
        send_input("0x0000")
        page.update()

    def refresh_status():
        nonlocal status_data, frame_count, last_frame, last_frame_time, fps
        r = dc.call("status", timeout=5)
        if isinstance(r, dict):
            status_data = r
            frame_count = r.get("frame_count", 0)
            # Calculate FPS
            now = time.time()
            delta = now - last_frame_time
            if delta > 0:
                fps = int((frame_count - last_frame) / delta)
            last_frame = frame_count
            last_frame_time = now
            # Update UI
            frame_text.value = str(frame_count)
            pc_text.value = f"PC: {r.get('pc', '0x00000000')}"
            paused = r.get("paused", True)
            running = r.get("running", False)
            if running and not paused:
                state_text.value = "RUNNING"
                state_text.color = STATUS_GREEN
            elif paused:
                state_text.value = "PAUSED"
                state_text.color = STATUS_YELLOW
            else:
                state_text.value = "STOPPED"
                state_text.color = STATUS_RED
            fps_text.value = f"● {fps} FPS"
            status_dot.bgcolor = STATUS_GREEN if running and not paused else STATUS_YELLOW if paused else STATUS_RED
        else:
            fps_text.value = "● DISCONNECTED"
            status_dot.bgcolor = STATUS_RED
        page.update()

    def refresh_registers():
        r = dc.call("get_registers", timeout=5)
        if isinstance(r, dict) and "gpr" in r:
            gpr = r["gpr"]
            reg_grid.controls.clear()
            for i, val in enumerate(gpr):
                name = f"${i:02d}"
                if i == 0: name = "$zero"
                elif i == 1: name = "$at"
                elif i == 2: name = "$v0"
                elif i == 3: name = "$v1"
                elif i == 4: name = "$a0"
                elif i == 5: name = "$a1"
                elif i == 6: name = "$a2"
                elif i == 7: name = "$a3"
                elif i == 8: name = "$t0"
                elif i == 9: name = "$t1"
                elif i == 10: name = "$t2"
                elif i == 11: name = "$t3"
                elif i == 12: name = "$t4"
                elif i == 13: name = "$t5"
                elif i == 14: name = "$t6"
                elif i == 15: name = "$t7"
                elif i == 16: name = "$s0"
                elif i == 17: name = "$s1"
                elif i == 18: name = "$s2"
                elif i == 19: name = "$s3"
                elif i == 20: name = "$s4"
                elif i == 21: name = "$s5"
                elif i == 22: name = "$s6"
                elif i == 23: name = "$s7"
                elif i == 24: name = "$t8"
                elif i == 25: name = "$t9"
                elif i == 26: name = "$k0"
                elif i == 27: name = "$k1"
                elif i == 28: name = "$gp"
                elif i == 29: name = "$sp"
                elif i == 30: name = "$fp"
                elif i == 31: name = "$ra"
                reg_grid.controls.append(
                    ft.Container(
                        content=ft.Column([
                            ft.Text(name, size=10, color=TEXT_SECONDARY, font_family="Consolas"),
                            ft.Text(val, size=12, color=TEXT_PRIMARY, font_family="Consolas", weight=ft.FontWeight.BOLD),
                        ], spacing=1),
                        padding=6,
                        border_radius=4,
                        bgcolor=BG_MAIN,
                        border=ft.BorderSide(1, BORDER_CARD),
                    )
                )
            page.update()

    def read_memory():
        addr = mem_addr_field.value
        try:
            size = int(mem_size_field.value)
        except:
            size = 256
        r = dc.call("read_mem", {"address": addr, "size": size}, timeout=5)
        if isinstance(r, dict) and "hex" in r:
            h = r["hex"]
            lines = []
            for i in range(0, len(h), 32):
                chunk = h[i:i+32]
                addr_str = f"{i//2:04X}"
                hex_parts = " ".join(chunk[j:j+4] for j in range(0, len(chunk), 4))
                ascii_str = ""
                for j in range(0, len(chunk), 2):
                    try:
                        b = int(chunk[j:j+2], 16)
                        ascii_str += chr(b) if 32 <= b < 127 else "."
                    except:
                        ascii_str += "."
                lines.append(f"{addr_str}: {hex_parts:<40s} {ascii_str}")
            mem_text.value = "\n".join(lines)
        else:
            mem_text.value = f"Error: {r}"
        page.update()

    def detect_os():
        r = dc.call("detect_os", timeout=10)
        if isinstance(r, dict):
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
                lines.append(f"  {fn.get('name', '?')} @ {fn.get('address', '?')} ({fn.get('source', '?')})")
            os_text.value = "\n".join(lines)
        else:
            os_text.value = f"Error: {r}"
        page.update()

    def toggle_trace(name):
        r = dc.call(f"trace_{name}", timeout=5)
        # Toggle button state
        if name == "callchain":
            btn = btn_trace_callchain
        elif name == "scheduler":
            btn = btn_trace_scheduler
        elif name == "rom_reads":
            btn = btn_trace_rom
        elif name == "struct":
            btn = btn_trace_struct
        else:
            return
        is_on = btn.text.endswith("ON")
        btn.text = f"{name.replace('_', ' ').title()}: {'OFF' if is_on else 'ON'}"
        btn.bgcolor = ACCENT if not is_on else BG_CARD
        btn.color = "white" if not is_on else TEXT_SECONDARY
        page.update()

    def clear_events():
        dc.call("clear_events", timeout=5)
        events_list.controls.clear()
        page.update()

    def refresh_events():
        r = dc.call("get_events", timeout=5)
        if isinstance(r, dict) and "events" in r:
            events = r["events"]
            events_list.controls.clear()
            for e in events[-20:]:
                t = e.get("type", "?")
                ts = e.get("timestamp", 0)
                summary = str(e)
                events_list.controls.append(
                    ft.Container(
                        content=ft.Text(f"[{ts:.2f}] {t}: {summary[:80]}", size=10, color=TEXT_SECONDARY, font_family="Consolas"),
                        padding=4,
                        border_radius=2,
                        bgcolor=BG_MAIN,
                    )
                )
            page.update()

    def refresh_breakpoints():
        r = dc.call("list_breakpoints", timeout=5)
        if isinstance(r, list):
            breakpoints_data = r
            bp_list.controls.clear()
            for bp in r:
                addr = bp.get("address", "?") if isinstance(bp, dict) else str(bp)
                idx = bp.get("index", 0) if isinstance(bp, dict) else 0
                bp_list.controls.append(
                    ft.Row([
                        ft.Text(f"{idx}: {addr}", size=11, color=TEXT_PRIMARY, font_family="Consolas", expand=True),
                        ft.ElevatedButton("×", width=28, height=28, bgcolor=STATUS_RED, color="white", on_click=lambda e, idx=idx: remove_bp(idx)),
                    ], spacing=4)
                )
            page.update()

    def add_bp():
        addr = bp_address.value
        dc.call("add_exec_breakpoint", {"address": addr}, timeout=5)
        refresh_breakpoints()

    def remove_bp(idx):
        dc.call("remove_breakpoint", {"index": idx}, timeout=5)
        refresh_breakpoints()

    def remove_all_bp():
        r = dc.call("list_breakpoints", timeout=5)
        if isinstance(r, list):
            for bp in r:
                idx = bp.get("index", 0) if isinstance(bp, dict) else bp
                dc.call("remove_breakpoint", {"index": idx}, timeout=5)
        refresh_breakpoints()

    def refresh_fb():
        r = dc.call("read_framebuffer", timeout=5)
        if isinstance(r, dict):
            w = r.get("width", 0)
            h = r.get("height", 0)
            fb_info.value = f"{w}×{h}  {r.get('size', 0)} bytes"
            # Try to render hex as image if available
            pixels_hex = r.get("pixels", "")
            if pixels_hex and len(pixels_hex) > 0:
                try:
                    from PIL import Image
                    import io
                    import base64
                    data = bytes.fromhex(pixels_hex)
                    # Create a simple visualization
                    img = Image.new('RGBA', (w, h), (0,0,0,0))
                    if len(data) >= w * h * 4:
                        for y in range(min(h, 240)):
                            for x in range(min(w, 320)):
                                idx = (y * w + x) * 4
                                r_val = data[idx]
                                g_val = data[idx+1]
                                b_val = data[idx+2]
                                a_val = data[idx+3]
                                img.putpixel((x, y), (r_val, g_val, b_val, a_val))
                    buf = io.BytesIO()
                    img.save(buf, format='PNG')
                    buf.seek(0)
                    b64 = base64.b64encode(buf.read()).decode()
                    fb_image.src_base64 = b64
                except Exception as ex:
                    fb_info.value = f"Image error: {ex}"
        else:
            fb_info.value = "No framebuffer data"
        page.update()

    def set_capture_interval():
        try:
            interval = int(fb_interval.value)
        except:
            interval = 5
        dc.call("set_frame_capture_interval", {"interval": interval}, timeout=5)

    def refresh_captures():
        r = dc.call("get_frame_captures", timeout=5)
        if isinstance(r, dict) and "captures" in r:
            captures = r["captures"]
            fb_capture_list.controls.clear()
            for c in captures[-10:]:
                frame = c.get("frame", 0)
                w = c.get("width", 0)
                h = c.get("height", 0)
                size = c.get("size", 0)
                fb_capture_list.controls.append(
                    ft.Container(
                        content=ft.Text(f"Frame {frame}: {w}×{h} {size}B", size=11, color=TEXT_SECONDARY, font_family="Consolas"),
                        padding=4,
                        border_radius=2,
                        bgcolor=BG_MAIN,
                    )
                )
            page.update()

    def start_daemon_click():
        nonlocal daemon_proc
        rom_name = rom_dropdown.value
        rom_path = None
        for name, path in find_roms():
            if name == rom_name:
                rom_path = path
                break
        if not rom_path:
            startup_status.value = "No ROM selected"
            startup_status.color = STATUS_RED
            page.update()
            return
        # Start daemon
        exe = r"D:\Mupen64MCP\native\n64_debug_daemon\build\n64-debug-daemon.exe"
        args = [
            "--core", r"D:\Mupen64MCP\build\mupen64plus\lib\mupen64plus.dll",
            "--rom", rom_path,
            "--gfx", "dummy",
            "--audio", "dummy",
            "--input", r"D:\Mupen64MCP\native\n64_debug_daemon\build\mupen64plus-input-inject.dll",
            "--rsp", "dummy",
            "--datadir", r"D:\Mupen64MCP\data",
            "--configdir", r"D:\Mupen64MCP\config",
            "--port", "9876",
        ]
        env = os.environ.copy()
        mingw_bin = r"C:\msys64\mingw64\bin"
        if mingw_bin not in env.get('PATH', ''):
            env['PATH'] = mingw_bin + os.pathsep + env.get('PATH', '')
        startup_status.value = "Starting daemon..."
        startup_status.color = ACCENT
        page.update()
        try:
            daemon_proc = subprocess.Popen([exe] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
            time.sleep(4)
            if dc.ping():
                startup_status.value = "Connected!"
                startup_status.color = STATUS_GREEN
                page.update()
                time.sleep(0.5)
                show_dashboard()
            else:
                startup_status.value = "Daemon started but not responding"
                startup_status.color = STATUS_RED
                page.update()
        except Exception as e:
            startup_status.value = f"Failed: {e}"
            startup_status.color = STATUS_RED
            page.update()

    def connect_click():
        nonlocal dc
        dc = DaemonClient(host=host_field.value, port=int(port_field.value))
        if dc.ping():
            startup_status.value = "Connected!"
            startup_status.color = STATUS_GREEN
            page.update()
            time.sleep(0.5)
            show_dashboard()
        else:
            startup_status.value = "No daemon found at that address"
            startup_status.color = STATUS_RED
            page.update()

    def show_dashboard():
        page.controls.clear()
        page.add(body)
        start_polling()
        page.update()
        # Initial refresh
        refresh_status()
        refresh_fb()
        refresh_captures()
        refresh_breakpoints()
        refresh_registers()

    def start_polling():
        nonlocal poll_thread
        poll_stop.clear()
        def poll():
            while not poll_stop.is_set():
                try:
                    refresh_status()
                    if selected_tab == "reg":
                        refresh_registers()
                    elif selected_tab == "evt":
                        refresh_events()
                    elif selected_tab == "bp":
                        refresh_breakpoints()
                    elif selected_tab == "fb":
                        refresh_captures()
                except Exception:
                    pass
                time.sleep(0.5)
        poll_thread = threading.Thread(target=poll, daemon=True)
        poll_thread.start()

    def stop_polling():
        poll_stop.set()
        if poll_thread:
            poll_thread.join(timeout=1)

    def shutdown():
        stop_polling()
        if daemon_proc:
            try:
                dc.call("shutdown", timeout=5)
                time.sleep(1)
                daemon_proc.terminate()
                daemon_proc.wait(timeout=5)
            except:
                pass
        page.window_close()

    # ── Init ────────────────────────────────────────────────
    page.on_window_close = lambda e: shutdown()

    # Check for daemon on startup
    if dc.ping():
        show_dashboard()
    else:
        page.add(startup_screen)

# ── Entry point ────────────────────────────────────────────
if __name__ == "__main__":
    ft.run(main, view=ft.AppView.FLET_APP)
