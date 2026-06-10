import subprocess, time, sys, os, signal

# Set PATH for MINGW DLLs
env = os.environ.copy()
mingw_bin = r"C:\msys64\mingw64\bin"
if mingw_bin not in env.get('PATH', ''):
    env['PATH'] = mingw_bin + os.pathsep + env.get('PATH', '')

# Config
PORT = 9876
CORE = r"D:\Mupen64MCP\build\mupen64plus\lib\mupen64plus.dll"
ROM = r"D:\Mupen64MCP\roms\starfox64.z64"
GFX = r"D:\Mupen64MCP\plugins\mupen64plus-video-rice.dll"
AUDIO = "dummy"
INPUT = r"D:\Mupen64MCP\native\n64_debug_daemon\build\mupen64plus-input-inject.dll"
RSP = r"D:\Mupen64MCP\plugins\mupen64plus-rsp-hle.dll"
DATA = r"D:\Mupen64MCP\data"
CONFIG = r"D:\Mupen64MCP\config"
VIEWER = r"D:\Mupen64MCP\mcp\python\n64_debug_mcp\n64_viewer.py"
PYTHON = r"C:\Users\SeanS\AppData\Local\Programs\Python\Python311\python.exe"
DAEMON = r"D:\Mupen64MCP\native\n64_debug_daemon\build\n64-debug-daemon.exe"

def main():
    # Kill any existing daemon
    try:
        subprocess.run(["taskkill", "/F", "/IM", "n64-debug-daemon.exe"], 
                      capture_output=True, check=False)
        time.sleep(1)
    except Exception:
        pass

    # Launch daemon
    print("=" * 60)
    print("Starting n64-viewer with Star Fox 64")
    print("=" * 60)
    print(f"\nROM: {ROM}")
    print(f"Video: Rice (real rendering)")
    print(f"RSP: HLE")
    print(f"Port: {PORT}")
    print()

    daemon_args = [
        DAEMON,
        "--core", CORE,
        "--rom", ROM,
        "--gfx", GFX,
        "--audio", AUDIO,
        "--input", INPUT,
        "--rsp", RSP,
        "--datadir", DATA,
        "--configdir", CONFIG,
        "--port", str(PORT),
    ]

    print("[1/3] Starting daemon...")
    daemon_proc = subprocess.Popen(daemon_args, stdout=subprocess.PIPE, 
                                    stderr=subprocess.PIPE, env=env)
    print(f"       Daemon PID: {daemon_proc.pid}")
    time.sleep(5)

    # Launch viewer
    print("[2/3] Starting viewer GUI...")
    viewer_proc = subprocess.Popen([PYTHON, VIEWER, "--port", str(PORT)], env=env)
    print(f"       Viewer PID: {viewer_proc.pid}")
    print()
    print("[3/3] Running! Close the viewer window to stop.")
    print()
    print("Controls:")
    print("  - Pause / Resume / Step: Top buttons")
    print("  - Input: START, A, B, Z, DPAD")
    print("  - Tabs: Framebuffer, Registers, Memory, OS Info, Events, Breakpoints")
    print()

    # Wait for viewer
    try:
        viewer_proc.wait()
    except KeyboardInterrupt:
        print("\nInterrupted by user.")
    finally:
        print("\nShutting down...")
        viewer_proc.terminate()
        daemon_proc.terminate()
        try:
            daemon_proc.wait(timeout=5)
        except:
            daemon_proc.kill()
        print("Done!")

if __name__ == "__main__":
    main()
