cmake_minimum_required(VERSION 3.16)

project(aes67-srt
  VERSION 0.1.0
  DESCRIPTION "AES67 over SRT transport appliance"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Options
#
# Every hardware / platform specific piece defaults to AUTO so the project
# configures, builds and tests on a development machine (e.g. macOS) with no
# ALSA, no libsrt and no aes67-daemon installed.
#
# NOTE: these three-state options are STRING cache entries, not BOOL options.
# `option(WITH_SRT "..." AUTO)` looks equivalent and is not: the BOOL cache
# entry cannot hold "AUTO", so CMake rewrites it to OFF, and from the second
# configure onwards the detection block below is skipped entirely. That bug is
# inherited from the sibling gateway and cost an afternoon here.
# ---------------------------------------------------------------------------
set(WITH_SRT "AUTO" CACHE STRING "Build the SRT transport: ON, OFF or AUTO")
set(WITH_ALSA "AUTO" CACHE STRING "Build the RAVENNA/ALSA backend: ON, OFF or AUTO")
set_property(CACHE WITH_SRT PROPERTY STRINGS ON OFF AUTO)
set_property(CACHE WITH_ALSA PROPERTY STRINGS ON OFF AUTO)

option(WITH_TESTS "Build unit tests" ON)
option(WITH_WEBUI "Build the web UI (requires npm)" OFF)
option(FETCH_DEPS "Download missing dependencies with FetchContent" ON)

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

# --- nlohmann/json (config file, REST payloads) ----------------------------
# CMP0135: without it, a downloaded archive keeps its own timestamps and CMake
# warns that a changed URL might not trigger a rebuild.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  if(NOT FETCH_DEPS)
    message(FATAL_ERROR "nlohmann_json not found and FETCH_DEPS=OFF")
  endif()
  message(STATUS "nlohmann_json not found, fetching v3.12.0")
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- cpp-httplib (the aes67-daemon REST client) ----------------------------
# The daemon is reached over HTTP, and this is the second dependency the
# project takes: MIT, header-only, and the one the sibling `aes67-sip` has
# already proved against this exact daemon (the spec says this module mirrors
# `aes67-sip`'s `src/aes67/`).  Pinned to the same v0.56.0 the sibling runs.
#
# Fetched by URL rather than by git tag, matching the pattern above: a release
# archive needs no git at configure time and no shallow clone to keep in sync.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

find_package(httplib QUIET)
if(NOT httplib_FOUND)
  if(NOT FETCH_DEPS)
    message(FATAL_ERROR "cpp-httplib not found and FETCH_DEPS=OFF")
  endif()
  message(STATUS "cpp-httplib not found, fetching v0.56.0")
  # Compile the header into its own target and install nothing: this project
  # consumes it, it does not redistribute it.
  set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.56.0.tar.gz)
  FetchContent_MakeAvailable(httplib)
endif()

find_package(Threads REQUIRED)

# --- Detection only --------------------------------------------------------
#
# libsrt and ALSA are detected and reported, but nothing consumes them yet:
# ticket 05 builds the skeleton, and the transport and audio modules land in
# tickets 07 and 08.  Detecting them now is what makes the AUTO options real
# rather than aspirational.
set(SRT_AVAILABLE FALSE)
if(NOT WITH_SRT STREQUAL "OFF")
  # HINTS: Homebrew on Apple Silicon installs under /opt/homebrew, which CMake
  # does not always have on its default search path, and Intel/Homebrew uses
  # /usr/local. Nothing else here needs a hint.
  find_path(SRT_INCLUDE_DIR srt/srt.h
    HINTS /opt/homebrew/include /usr/local/include)
  find_library(SRT_LIBRARY NAMES srt
    HINTS /opt/homebrew/lib /usr/local/lib)
  if(SRT_INCLUDE_DIR AND SRT_LIBRARY)
    set(SRT_AVAILABLE TRUE)
    message(STATUS "libsrt found: ${SRT_LIBRARY}")
  elseif(WITH_SRT STREQUAL "ON")
    message(FATAL_ERROR "WITH_SRT=ON but libsrt headers/library were not found")
  else()
    message(STATUS "libsrt not found - the transport will refuse to open a link")
  endif()
endif()

set(ALSA_AVAILABLE FALSE)
if(NOT WITH_ALSA STREQUAL "OFF")
  find_path(ALSA_INCLUDE_DIR alsa/asoundlib.h)
  find_library(ALSA_LIBRARY NAMES asound)
  if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
    set(ALSA_AVAILABLE TRUE)
    message(STATUS "ALSA found: ${ALSA_LIBRARY}")
  elseif(WITH_ALSA STREQUAL "ON")
    message(FATAL_ERROR "WITH_ALSA=ON but libasound was not found")
  else()
    message(STATUS "ALSA not found - reserved, no module uses it yet")
  endif()
endif()

# ---------------------------------------------------------------------------
# Core library
#
# One library holding everything but main(), so the tests link exactly the code
# the binary runs.  Module directories appear in this list as their tickets land.
# ---------------------------------------------------------------------------
set(AES67_SRT_SOURCES
  src/aes67/daemon_client.cpp
  src/aes67/fake_daemon_client.cpp
  src/app.cpp
  src/audio/null_backend.cpp
  src/config.cpp
  src/log.cpp
  src/transport/link.cpp
  src/util.cpp
  src/version.cpp
  src/wire/frame.cpp)

add_library(aes67-srt-core STATIC ${AES67_SRT_SOURCES})
target_include_directories(aes67-srt-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(aes67-srt-core PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(aes67-srt-core PUBLIC
  AES67_SRT_VERSION="${PROJECT_VERSION}"
  AES67_SRT_WITH_SRT=$<BOOL:${SRT_AVAILABLE}>
  AES67_SRT_WITH_ALSA=$<BOOL:${ALSA_AVAILABLE}>)
target_link_libraries(aes67-srt-core
  PUBLIC nlohmann_json::nlohmann_json httplib::httplib Threads::Threads)

# libsrt is linked only when it was found.  The transport compiles either way and
# reports why it cannot open a link, so a development machine without it still
# builds, tests and reports honestly rather than shipping a stub that pretends.
if(SRT_AVAILABLE)
  target_include_directories(aes67-srt-core PUBLIC "${SRT_INCLUDE_DIR}")
  target_link_libraries(aes67-srt-core PUBLIC "${SRT_LIBRARY}")
  message(STATUS "transport: libsrt linked from ${SRT_LIBRARY}")
else()
  message(STATUS "transport: built without libsrt - Link::open will refuse")
endif()

add_executable(aes67-srt src/main.cpp)
target_compile_options(aes67-srt PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(aes67-srt PRIVATE aes67-srt-core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
if(WITH_TESTS)
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Web UI
#
# Off by default, and there is nothing to build yet: the control surface is
# tickets 13 and 14.  Turning it on early is a hard error rather than a silent
# no-op, so an absent bundle is never mistaken for a built one.
# ---------------------------------------------------------------------------
if(WITH_WEBUI)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/webui/package.json")
    message(FATAL_ERROR
      "WITH_WEBUI=ON but webui/ does not exist yet - the control surface is "
      "tickets 13 and 14 (see docs/spec/0001-aes67-srt.md)")
  endif()
  find_program(NPM_EXECUTABLE npm REQUIRED)
  add_custom_target(webui
    COMMAND ${NPM_EXECUTABLE} install
    COMMAND ${NPM_EXECUTABLE} run build
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/webui
    COMMENT "Building web UI")
endif()

# ---------------------------------------------------------------------------
# Installation
#
# Used by `cmake --install`.  The one-command installer (ticket 15) installs the
# same files itself, so keep the two in sync.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS aes67-srt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/config/aes67-srt.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/aes67-srt
        RENAME aes67-srt.conf.sample)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/config/aes67-srt.dev.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/aes67-srt)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/systemd/aes67-srt.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/aes67-srt
        OPTIONAL)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check.sh
              ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install-deps.sh
        DESTINATION ${CMAKE_INSTALL_DATADIR}/aes67-srt/scripts
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/README.md
              ${CMAKE_CURRENT_SOURCE_DIR}/docs/ROADMAP.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/aes67-srt)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "aes67-srt ${PROJECT_VERSION} configuration")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  libsrt     : ${SRT_AVAILABLE}")
message(STATUS "  ALSA       : ${ALSA_AVAILABLE} (reserved, no module uses it yet)")
message(STATUS "  tests      : ${WITH_TESTS}")
message(STATUS "  web UI     : ${WITH_WEBUI} (not implemented yet)")
message(STATUS "")

