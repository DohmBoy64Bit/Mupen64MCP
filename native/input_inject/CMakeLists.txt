cmake_minimum_required(VERSION 3.16)
project(mupen64plus-input-inject LANGUAGES C)

find_path(MUPEN64PLUS_INCLUDE m64p_types.h
  PATHS ${MUPEN64PLUS_DIR}/include $ENV{MUPEN64PLUS_DIR}/include
  NO_DEFAULT_PATH)

if(NOT MUPEN64PLUS_INCLUDE)
  message(FATAL_ERROR "mupen64plus headers not found. Set MUPEN64PLUS_DIR or -DMUPEN64PLUS_DIR=path")
endif()

add_library(mupen64plus-input-inject SHARED
  plugin.c
)

target_include_directories(mupen64plus-input-inject PRIVATE
  ${MUPEN64PLUS_INCLUDE}
)

if(WIN32)
  set_target_properties(mupen64plus-input-inject PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
  )
endif()
