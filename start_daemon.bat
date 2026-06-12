@echo off
setlocal

set WINLIBS_MINGW=C:\Users\SeanS\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin
set MSYS2_MINGW=C:\msys64\mingw64\bin
set CORE_LIB=D:\Mupen64MCP\build\mupen64plus\lib
set DAEMON=D:\Mupen64MCP\native\n64_debug_daemon\build\n64-debug-daemon.exe
set CORE=D:\Mupen64MCP\build\mupen64plus\lib\mupen64plus.dll

REM PATH ORDER MATTERS: MSYS2 must come before WinLibs.
REM The Rice video plugin and other Mupen64Plus plugins are built
REM with MSYS2's MINGW64 toolchain and depend on its libstdc++-6.dll,
REM libgcc_s_seh-1.dll, SDL2.dll, etc. If WinLibs appears first in PATH,
REM Windows will resolve these dependencies from the wrong MinGW distro,
REM causing a 0xC0000005 (ACCESS_VIOLATION) crash at ROM load time.
set PATH=%MSYS2_MINGW%;%WINLIBS_MINGW%;%CORE_LIB%;%PATH%

echo Starting n64-debug-daemon...
"%DAEMON%" --core "%CORE%" --port 9876
