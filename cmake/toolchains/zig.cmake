# Cross-compiles with `zig cc`. libsqlite3 must come from a target sysroot.
# Example, static aarch64 musl binary:
#   cmake -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/zig.cmake \
#         -DZIG_TARGET=aarch64-linux-musl -DCMAKE_FIND_ROOT_PATH=/path/to/sysroot -DACHD_STATIC=ON
# Other targets: arm-linux-musleabihf, x86_64-linux-musl, mipsel-linux-musl.

set(CMAKE_SYSTEM_NAME Linux)
# Nested projects (check_ipo_supported) see only the environment, not -D values.
if(NOT ZIG_TARGET)
  set(ZIG_TARGET "$ENV{_ACHD_ZIG_TARGET}")
endif()
if(NOT ZIG_EXE)
  set(ZIG_EXE "$ENV{_ACHD_ZIG_EXE}")
endif()
if(NOT ZIG_TARGET)
  message(FATAL_ERROR "Set ZIG_TARGET, e.g. -DZIG_TARGET=aarch64-linux-musl")
endif()
set(ENV{_ACHD_ZIG_TARGET} "${ZIG_TARGET}")
string(REGEX MATCH "^[^-]+" CMAKE_SYSTEM_PROCESSOR "${ZIG_TARGET}")

if(NOT ZIG_EXE)
  find_program(ZIG_EXE zig REQUIRED)
endif()
set(ENV{_ACHD_ZIG_EXE} "${ZIG_EXE}")
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ZIG_TARGET ZIG_EXE)

# CMake wants a single executable per tool, so wrap `zig <tool>`.
set(_wrappers ${CMAKE_BINARY_DIR}/zig-wrappers)
foreach(_tool cc ar ranlib)
  set(_args "")
  if(_tool STREQUAL "cc")
    set(_args "-target ${ZIG_TARGET}")
  endif()
  file(WRITE ${_wrappers}/zig-${_tool} "#!/bin/sh\nexec \"${ZIG_EXE}\" ${_tool} ${_args} \"$@\"\n")
  file(CHMOD ${_wrappers}/zig-${_tool} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
endforeach()

set(CMAKE_C_COMPILER ${_wrappers}/zig-cc)
set(CMAKE_AR ${_wrappers}/zig-ar CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB ${_wrappers}/zig-ranlib CACHE FILEPATH "" FORCE)
# Used instead of CMAKE_AR/RANLIB when LTO is on.
set(CMAKE_C_COMPILER_AR ${_wrappers}/zig-ar CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_RANLIB ${_wrappers}/zig-ranlib CACHE FILEPATH "" FORCE)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
