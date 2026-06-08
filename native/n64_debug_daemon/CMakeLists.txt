cmake_minimum_required(VERSION 3.16)
project(n64-debug-daemon VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The daemon loads libmupen64plus dynamically (LoadLibrary/dlopen).
# We only need the headers for compilation.
find_path(MUPEN64PLUS_INCLUDE m64p_types.h
  PATHS ${MUPEN64PLUS_DIR}/include $ENV{MUPEN64PLUS_DIR}/include
  NO_DEFAULT_PATH)

if(NOT MUPEN64PLUS_INCLUDE)
  message(FATAL_ERROR "mupen64plus headers not found. Set MUPEN64PLUS_DIR or -DMUPEN64PLUS_DIR=path")
endif()

add_executable(n64-debug-daemon
  src/main.cpp
  src/mupen_core_loader.cpp
  src/emulator_session.cpp
  src/memory.cpp
  src/breakpoints.cpp
  src/tracing.cpp
  src/json_rpc_server.cpp
)

target_include_directories(n64-debug-daemon PRIVATE
  ${MUPEN64PLUS_INCLUDE}
  include
)

if(WIN32)
  target_link_libraries(n64-debug-daemon PRIVATE ws2_32)
endif()