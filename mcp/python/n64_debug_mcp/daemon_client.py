"""TCP JSON-RPC client for n64-debug-daemon."""
from __future__ import annotations

import json
import socket
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class DaemonConfig:
    core_path: str | Path
    rom_path: str | Path | None = None
    data_dir: str | Path | None = None
    config_dir: str | Path | None = None
    gfx: str = "dummy"
    audio: str = "dummy"
    input: str = "dummy"
    rsp: str = "dummy"
    port: int = 9876
    host: str = "127.0.0.1"
    allow_write: bool = False

    def to_args(self) -> list[str]:
        args = [
            "--core", str(self.core_path),
            "--port", str(self.port),
            "--gfx", self.gfx,
            "--audio", self.audio,
            "--input", self.input,
            "--rsp", self.rsp,
        ]
        if self.rom_path:
            args.extend(["--rom", str(self.rom_path)])
        if self.data_dir:
            args.extend(["--datadir", str(self.data_dir)])
        if self.config_dir:
            args.extend(["--configdir", str(self.config_dir)])
        if self.allow_write:
            args.append("--allow-write-memory")
        return args


class DaemonClient:
    """Manages the n64-debug-daemon subprocess and JSON-RPC calls."""

    def __init__(self, config: DaemonConfig) -> None:
        self.config = config
        self._process: subprocess.Popen | None = None
        self._seq = 0

    # ── lifecycle ──────────────────────────────────────────────

    def start(self, timeout: float = 5.0) -> None:
        exe = self._find_daemon()
        args = self.config.to_args()
        self._process = subprocess.Popen(
            [str(exe), *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                self._connect()
                self.call("ping")
                return
            except (ConnectionRefusedError, OSError, TimeoutError):
                time.sleep(0.2)
        raise RuntimeError("Daemon did not start within timeout")

    def stop(self) -> None:
        self._close_socket()
        if self._process:
            try:
                self.call("shutdown")
            except Exception:
                pass
            self._process.terminate()
            self._process.wait(timeout=5)
            self._process = None

    def __enter__(self) -> "DaemonClient":
        self.start()
        return self

    def __exit__(self, *args: Any) -> None:
        self.stop()

    # ── JSON-RPC ───────────────────────────────────────────────

    def call(self, method: str, params: dict[str, Any] | None = None) -> Any:
        self._seq += 1
        req = json.dumps({"jsonrpc": "2.0", "id": self._seq, "method": method, "params": params or {}},
                         separators=(',', ':'))

        # Daemon uses one-request-per-connection, open/close per call
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        try:
            sock.connect((self.config.host, self.config.port))
            sock.sendall((req + "\n").encode())
            response = self._recv_response(sock)
            if "error" in response:
                raise DaemonError(response["error"]["message"], response["error"].get("code"))
            return response.get("result")
        finally:
            sock.close()

    # ── helpers ────────────────────────────────────────────────

    def _find_daemon(self) -> Path:
        # Search relative to cwd, then script dir, then typical project roots
        exe_name = "n64-debug-daemon.exe"
        search_dirs = [
            Path.cwd(),
            Path(__file__).parent.parent.parent.parent,  # project root
            Path(__file__).parent.parent,
        ]
        candidates = [
            d / "native/n64_debug_daemon/build" / exe_name
            for d in search_dirs
        ]
        candidates += [Path(exe_name), Path(f"./native/n64_debug_daemon/build/{exe_name}")]
        for c in candidates:
            resolved = c.resolve()
            if resolved.exists():
                return resolved
        return Path(exe_name)  # hope it's on PATH

    def _connect(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect((self.config.host, self.config.port))
        sock.close()

    def _ensure_connected(self) -> None:
        pass  # connection is created per call

    def _close_socket(self) -> None:
        pass  # connection is closed per call

    def _recv_response(self, sock: socket.socket) -> dict[str, Any]:
        buf = bytearray()
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError("Daemon disconnected")
            buf.extend(chunk)
            try:
                return json.loads(buf.decode())
            except (json.JSONDecodeError, UnicodeDecodeError):
                if len(buf) > 1024 * 1024:
                    raise RuntimeError("Response too large (>1MB)")
                # partial data, keep reading


class DaemonError(Exception):
    def __init__(self, message: str, code: int | None = None):
        self.code = code
        super().__init__(message)
