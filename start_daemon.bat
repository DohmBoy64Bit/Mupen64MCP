@echo off
setlocal

set WINLIBS_MINGW=C:\Users\SeanS\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin
set MSYS2_MINGW=C:\msys64\mingw64\bin
set CORE_LIB=D:\Mupen64MCP\build\mupen64plus\lib
set DAEMON=D:\Mupen64MCP\native\n64_debug_daemon\build\n64-debug-daemon.exe
set CORE=D:\Mupen64MCP\build\mupen64plus\lib\mupen64plus.dll

set PATH=%WINLIBS_MINGW%;%MSYS2_MINGW%;%CORE_LIB%;%PATH%

echo Starting n64-debug-daemon...
"%DAEMON%" --core "%CORE%" --port 9876
