cmake_minimum_required(VERSION 3.20)

project(achivmentsd VERSION 0.1.0 LANGUAGES C)

option(ACHD_SYSTEM_DEPS "Use installed libubox/uclient/ustream-ssl/mbedtls/json-c instead of fetching them" OFF)
# libsqlite3 always comes from the system (or the cross sysroot).
option(ACHD_STATIC "Link a fully static executable" OFF)
option(ACHD_BUILD_TESTS "Build unit and integration tests" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE MinSizeRel)
endif()

# Whole-program LTO for our code and the vendored libraries alike; set before
# the dependencies are declared so their targets inherit it.
include(CheckIPOSupported)
check_ipo_supported(RESULT ACHD_LTO OUTPUT ACHD_LTO_ERROR)
if(ACHD_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
else()
  message(STATUS "LTO unavailable: ${ACHD_LTO_ERROR}")
endif()

include(cmake/Deps.cmake)
find_package(SQLite3 REQUIRED)
# CMake 4.3 renamed the imported target and deprecated the old name.
if(TARGET SQLite3::SQLite3)
  set(ACHD_SQLITE SQLite3::SQLite3)
else()
  set(ACHD_SQLITE SQLite::SQLite3)
endif()

# Replays from a queue built while the clock was implausible get no backdate.
string(TIMESTAMP ACHD_BUILD_EPOCH "%s" UTC)

add_library(achd_core STATIC
  src/awards.c
  src/config.c
  src/creds.c
  src/db.c
  src/http_server.c
  src/msg.c
  src/netstate.c
  src/params.c
  src/refresh.c
  src/router.c
  src/upstream.c
  src/util.c
)
target_include_directories(achd_core PUBLIC src)
target_compile_definitions(achd_core PUBLIC
  _GNU_SOURCE
  ACHD_VERSION="${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
  ACHD_BUILD_EPOCH=${ACHD_BUILD_EPOCH}LL
)
if(ACHD_DLOPEN_SSL)
  target_compile_definitions(achd_core PRIVATE ACHD_DLOPEN_SSL)
endif()
target_compile_options(achd_core PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(achd_core PUBLIC achd::uclient achd::ustream-ssl achd::ubox ${ACHD_SQLITE})

add_executable(achivmentsd src/main.c)
target_compile_options(achivmentsd PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(achivmentsd PRIVATE achd_core)

if(ACHD_STATIC)
  target_link_options(achivmentsd PRIVATE -static)
endif()

include(GNUInstallDirs)
install(TARGETS achivmentsd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

if(ACHD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
