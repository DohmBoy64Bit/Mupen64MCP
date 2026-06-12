# Troubleshooting

## MinGW DLL ABI Compatibility (IMPORTANT)

This project relies on **two independent MinGW-w64 distributions**, and their runtime DLLs are **not ABI-compatible with each other**. Mixing them causes a `0xC0000005` (ACCESS_VIOLATION) crash during `load_rom` when real plugins (Rice video, RSP-HLE) are loaded.

### The Two Distributions

| Distribution | Location | Used by | GCC version |
|---|---|---|---|
| **WinLibs** (Brecht Sanders) | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs...\mingw64\bin` | `n64-debug-daemon.exe` (the daemon binary) | 15.2.0 |
| **MSYS2** | `C:\msys64\mingw64\bin` | Mupen64Plus plugins (video-rice, rsp-hle, audio-sdl, input-sdl, and the core DLL) | 16.1.0 |

Both supply their own versions of `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`, `SDL2.dll`, `libpng16-16.dll`, `libfreetype-6.dll`, and `zlib1.dll`.

### Why It Crashes

The Mupen64Plus plugins are built inside the MSYS2 environment and link against MSYS2's `libstdc++-6.dll`. When the daemon calls `LoadLibrary` on a plugin DLL, Windows resolves dependency DLLs by searching PATH in order. If WinLibs' `bin` appears before MSYS2's in PATH, Windows loads WinLibs' `libstdc++-6.dll` (GCC 15.2) instead of MSYS2's (GCC 16.1). The internal data structures (RTTI, exception frames, vtable layout) differ between these builds, causing an immediate access violation at ROM load time.

### Fix

**MSYS2 must appear before WinLibs in PATH.** The `start_daemon.bat` script sets the correct order:

```batch
set PATH=%MSYS2_MINGW%;%WINLIBS_MINGW%;%CORE_LIB%;%PATH%
```

With this order:
- Plugins get their matching MSYS2 runtime DLLs → **no crash**
- The daemon binary (linked against WinLibs) loads MSYS2's `libstdc++-6.dll`, which is ABI-compatible enough at the C ABI level for the daemon's needs

### Verification

Use Sysinternals `ListDLLs` to check which `libstdc++-6.dll` is loaded:

```
C:\> listdlls -d libstdc++-6 n64-debug-daemon.exe
```

If the path starts with `C:\msys64\...` the order is correct. If it shows `AppData\Local\Microsoft\WinGet\Packages\...`, WinLibs is resolving first and real plugins will crash.

### Troubleshooting Checklist

1. Check that MSYS2 bin is **before** WinLibs bin in PATH
2. Verify the plugin DLLs exist at their expected paths
3. Test with Star Fox 64 first — if it loads, PATH is correct and any remaining issue is ROM-specific
4. If all plugins crash, the MSYS2 runtime may have been updated — rebuild plugins from the **MSYS2 MINGW64 shell** (not WinLibs or any other MinGW distribution). Plugins must be built with the same toolchain that provides their runtime `libstdc++-6.dll`.
5. If just the video plugin crashes with a particular ROM, it may be a microcode compatibility issue

## Known Limitations

### General
- One TCP connection per request — `wait_for_breakpoint` uses client-side poll loop
- Only interpreter mode produces reliable debugger callbacks
- `Start-Process` in PowerShell does not inherit shell PATH; always set PATH explicitly when launching programmatically

### Input Injection
- Requires `--input path/to/mupen64plus-input-inject.dll` at daemon startup (passing `--input dummy` disables injection)

### Core DLL
- `--core <path>` is required — the daemon default (`libmupen64plus.dll`) does not match the actual DLL name (`mupen64plus.dll`)

### Rice Video Plugin
- Framebuffer writeback works with Star Fox 64 (standard F3D ucode) but produces black pixels with Cruis'n USA (custom F3DEX-based microcode). This is a plugin limitation — framebuffer dimensions and format are correctly reported.

### Function Scanner
- `scan_functions` returns 0 results if the emulator PC is still in SP boot range (`0xA4000000`). Resume and re-pause before scanning.
