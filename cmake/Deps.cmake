# Provides achd::ubox (with blobmsg_json), achd::ustream-ssl and achd::uclient.
#
# Vendored mode builds only the libubox/uclient/ustream-ssl sources the daemon
# uses: their own CMake files want pkg-config json-c, Lua and installed
# libubox headers, none of which exist when cross-compiling from scratch.

if(ACHD_SYSTEM_DEPS)
  find_path(UBOX_INCLUDE_DIR libubox/uloop.h REQUIRED)
  find_library(UBOX_LIB ubox REQUIRED)
  find_library(BLOBMSG_JSON_LIB blobmsg_json REQUIRED)
  find_path(JSONC_INCLUDE_DIR json.h PATH_SUFFIXES json-c REQUIRED)
  find_library(JSONC_LIB json-c REQUIRED)
  find_library(USTREAM_SSL_LIB ustream-ssl REQUIRED)
  find_library(UCLIENT_LIB uclient REQUIRED)

  add_library(achd::ubox INTERFACE IMPORTED)
  target_include_directories(achd::ubox INTERFACE ${UBOX_INCLUDE_DIR} ${JSONC_INCLUDE_DIR})
  target_link_libraries(achd::ubox INTERFACE ${BLOBMSG_JSON_LIB} ${UBOX_LIB} ${JSONC_LIB})
  add_library(achd::ustream-ssl INTERFACE IMPORTED)
  target_link_libraries(achd::ustream-ssl INTERFACE ${USTREAM_SSL_LIB} achd::ubox)
  add_library(achd::uclient INTERFACE IMPORTED)
  target_link_libraries(achd::uclient INTERFACE ${UCLIENT_LIB} achd::ubox ${CMAKE_DL_LIBS})
  # System ustream-ssl only loads through uclient_new_ssl_context(); the
  # vendored build links its ops table directly.
  set(ACHD_DLOPEN_SSL ON)
  return()
endif()

include(FetchContent)
# Extracted files get extraction-time mtimes so a pin bump rebuilds everything.
cmake_policy(SET CMP0135 NEW)
set(FETCHCONTENT_QUIET ON)

# SOURCE_SUBDIR points at a directory that does not exist so FetchContent only
# downloads; the targets are defined below.
FetchContent_Declare(libubox
  URL https://github.com/openwrt/libubox/archive/da2d205a5cf3e877c45a2eec488218cbaf452e7d.tar.gz
  URL_HASH SHA256=537d8a547467084d34e693169af985ec536ec0a6998b116c8fc0539be9553568
  SOURCE_SUBDIR _none)
FetchContent_Declare(uclient
  URL https://github.com/openwrt/uclient/archive/daad21fa2c17cae54d37149bf27bb3dd78e50a3f.tar.gz
  URL_HASH SHA256=bc2cb7822a7f0f1747789796da942b0314ab95ef4927ed58a77f86cc6e2961e2
  PATCH_COMMAND git apply --unsafe-paths ${CMAKE_CURRENT_LIST_DIR}/patches/uclient-content-length.patch
  SOURCE_SUBDIR _none)
FetchContent_Declare(ustream_ssl
  URL https://github.com/openwrt/ustream-ssl/archive/cea28c5bc43ae80c3531c98f4e4dc67b7dcf0ebb.tar.gz
  URL_HASH SHA256=3ac8e7b4e27f95713f14a0b99d59040c03d9c9bbe850e016b5f0f5781181fd9b
  SOURCE_SUBDIR _none)
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2
  URL_HASH SHA256=ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110)
FetchContent_Declare(jsonc
  URL https://s3.amazonaws.com/json-c_releases/releases/json-c-0.18.tar.gz
  URL_HASH SHA256=876ab046479166b869afc6896d288183bbc0e5843f141200c677b3e8dfb11724)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_APPS OFF CACHE BOOL "" FORCE)
set(DISABLE_WERROR ON CACHE BOOL "" FORCE)
set(DISABLE_EXTRA_LIBS ON CACHE BOOL "" FORCE)
set(DISABLE_THREAD_LOCAL_STORAGE ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(libubox uclient ustream_ssl mbedtls jsonc)

# Consumers include <libubox/...>, so stage every header under that prefix.
set(ACHD_DEP_INCLUDE ${CMAKE_BINARY_DIR}/dep-include)
file(GLOB _ubox_headers ${libubox_SOURCE_DIR}/*.h)
file(COPY ${_ubox_headers}
          ${ustream_ssl_SOURCE_DIR}/ustream-ssl.h
          ${uclient_SOURCE_DIR}/uclient.h
          ${uclient_SOURCE_DIR}/uclient-utils.h
     DESTINATION ${ACHD_DEP_INCLUDE}/libubox)

set(_dep_warnings -w)

add_library(achd_ubox STATIC
  ${libubox_SOURCE_DIR}/avl.c
  ${libubox_SOURCE_DIR}/avl-cmp.c
  ${libubox_SOURCE_DIR}/base64.c
  ${libubox_SOURCE_DIR}/blob.c
  ${libubox_SOURCE_DIR}/blobmsg.c
  ${libubox_SOURCE_DIR}/blobmsg_json.c
  ${libubox_SOURCE_DIR}/kvlist.c
  ${libubox_SOURCE_DIR}/md5.c
  ${libubox_SOURCE_DIR}/runqueue.c
  ${libubox_SOURCE_DIR}/safe_list.c
  ${libubox_SOURCE_DIR}/udebug.c
  ${libubox_SOURCE_DIR}/udebug-remote.c
  ${libubox_SOURCE_DIR}/ulog.c
  ${libubox_SOURCE_DIR}/uloop.c
  ${libubox_SOURCE_DIR}/usock.c
  ${libubox_SOURCE_DIR}/ustream.c
  ${libubox_SOURCE_DIR}/ustream-fd.c
  ${libubox_SOURCE_DIR}/utils.c)
target_include_directories(achd_ubox PUBLIC ${ACHD_DEP_INCLUDE} ${jsonc_SOURCE_DIR} ${jsonc_BINARY_DIR})
target_compile_options(achd_ubox PRIVATE ${_dep_warnings})
target_link_libraries(achd_ubox PUBLIC json-c)
add_library(achd::ubox ALIAS achd_ubox)

add_library(achd_ustream_ssl STATIC
  ${ustream_ssl_SOURCE_DIR}/ustream-ssl.c
  ${ustream_ssl_SOURCE_DIR}/ustream-mbedtls.c)
target_compile_definitions(achd_ustream_ssl PRIVATE HAVE_MBEDTLS)
target_include_directories(achd_ustream_ssl PRIVATE ${ustream_ssl_SOURCE_DIR})
target_compile_options(achd_ustream_ssl PRIVATE ${_dep_warnings})
target_link_libraries(achd_ustream_ssl PUBLIC achd_ubox mbedtls mbedx509 mbedcrypto)
add_library(achd::ustream-ssl ALIAS achd_ustream_ssl)

add_library(achd_uclient STATIC
  ${uclient_SOURCE_DIR}/uclient.c
  ${uclient_SOURCE_DIR}/uclient-http.c
  ${uclient_SOURCE_DIR}/uclient-utils.c)
target_compile_options(achd_uclient PRIVATE ${_dep_warnings})
target_link_libraries(achd_uclient PUBLIC achd_ubox ${CMAKE_DL_LIBS})
add_library(achd::uclient ALIAS achd_uclient)

set(ACHD_DLOPEN_SSL OFF)
