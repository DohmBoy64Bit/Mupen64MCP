@echo off
setlocal enabledelayedexpansion

set SRC_DIR=%~dp0src
set INC_DIR=%~dp0include
set MUPEN_DIR=%~dp0..\..\build\mupen64plus\include
set BUILD_DIR=%~dp0build
set OBJ_DIR=%BUILD_DIR%\CMakeFiles\n64-debug-daemon.dir\src

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"

echo Compiling...
g++ -std=c++17 -c "%SRC_DIR%\main.cpp" -I"%INC_DIR%" -I"%MUPEN_DIR%" -o "%OBJ_DIR%\main.cpp.obj"
if %errorlevel% neq 0 exit /b %errorlevel%

g++ -std=c++17 -c "%SRC_DIR%\mupen_core_loader.cpp" -I"%INC_DIR%" -I"%MUPEN_DIR%" -o "%OBJ_DIR%\mupen_core_loader.cpp.obj"
if %errorlevel% neq 0 exit /b %errorlevel%

g++ -std=c++17 -c "%SRC_DIR%\emulator_session.cpp" -I"%INC_DIR%" -I"%MUPEN_DIR%" -o "%OBJ_DIR%\emulator_session.cpp.obj"
if %errorlevel% neq 0 exit /b %errorlevel%

g++ -std=c++17 -c "%SRC_DIR%\memory.cpp" -I"%INC_DIR%" -I"%MUPEN_DIR%" -o "%OBJ_DIR%\memory.cpp.obj"
if %errorlevel% neq 0 exit /b %errorlevel%

g++ -std=c++17 -c "%SRC_DIR%\breakpoints.cpp" -I"%INC_DIR%" -I"%MUPEN_DIR%" -o "%OBJ_DIR%\breakpoints.cpp.obj"
if %errorlevel% neq 0 exit /b %errorlevel%

g++ -std=c++17 -c "%SRC_DIR%\tracing.cpp" -I"%INC_DIR%" -I"%MUPEN_DIR%" -o "%OBJ_DIR%\tracing.cpp.obj"
if %errorlevel% neq 0 exit /b %errorlevel%

g++ -std=c++17 -c "%SRC_DIR%\json_rpc_server.cpp" -I"%INC_DIR%" -I"%MUPEN_DIR%" -o "%OBJ_DIR%\json_rpc_server.cpp.obj"
if %errorlevel% neq 0 exit /b %errorlevel%

echo Linking...
g++ -std=c++17 "%OBJ_DIR%\*.cpp.obj" -o "%BUILD_DIR%\n64-debug-daemon.exe" -lws2_32
if %errorlevel% neq 0 exit /b %errorlevel%

echo.
echo Building input_inject plugin DLL...
set INPUT_INJECT_DIR=%~dp0..\input_inject
set INPUT_INJECT_BUILD=%BUILD_DIR%

gcc -c "%INPUT_INJECT_DIR%\plugin.c" -I"%INPUT_INJECT_DIR%" -I"%MUPEN_DIR%" -o "%OBJ_DIR%\input_inject.obj"
if %errorlevel% neq 0 exit /b %errorlevel%

gcc "%OBJ_DIR%\input_inject.obj" -shared -o "%INPUT_INJECT_BUILD%\mupen64plus-input-inject.dll" -Wl,--out-implib,"%INPUT_INJECT_BUILD%\libmupen64plus-input-inject.a"
if %errorlevel% neq 0 exit /b %errorlevel%

echo Build successful: %BUILD_DIR%\n64-debug-daemon.exe
echo Input inject plugin: %INPUT_INJECT_BUILD%\mupen64plus-input-inject.dll
