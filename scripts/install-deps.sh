#!/usr/bin/env bash
#
# Installs what this project needs in order to build and test.
#
#   Linux (CI):  build-essential, cmake, ninja, clang-format, libasound2-dev
#   macOS (dev): cmake and ninja
#
# libsrt and ALSA are Linux-only, and this project does not need either of them to
# build: the CMake options are AUTO, so an absent platform piece degrades to a
# warning and the unit tests still run.  That is why a Mac can be the whole
# development environment until real audio hardware is needed.
#
set -euo pipefail

if command -v apt-get >/dev/null 2>&1; then
  sudo apt-get update -qq
  sudo apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build libasound2-dev
  # The transport needs libsrt. Package names differ across releases, and the
  # tests skip themselves when it is absent, so this tries and reports rather
  # than failing the whole install.
  sudo apt-get install -y --no-install-recommends libsrt-openssl-dev ||
    sudo apt-get install -y --no-install-recommends libsrt-dev ||
    echo "    libsrt headers not available from apt: the transport tests will skip"
  # The formatting gate targets clang-format 18 (ubuntu-24.04).  Where that
  # package does not exist, take whatever the distribution ships and let
  # check.sh decide whether to enforce it.
  sudo apt-get install -y --no-install-recommends clang-format-18 ||
    sudo apt-get install -y --no-install-recommends clang-format
  echo "installed: build-essential cmake ninja-build libasound2-dev libsrt clang-format"
elif command -v brew >/dev/null 2>&1; then
  brew list cmake >/dev/null 2>&1 || brew install cmake
  brew list ninja >/dev/null 2>&1 || brew install ninja
  brew list srt >/dev/null 2>&1 || brew install srt
  echo "installed: cmake ninja srt (ALSA is Linux-only; the build skips it)"
else
  echo "no apt-get or brew found: install cmake, a C++17 compiler and, on Linux," \
    "libasound2-dev by hand" >&2
  exit 1
fi
