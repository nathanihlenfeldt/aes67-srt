#!/usr/bin/env bash
#
# Installs what this project needs in order to build and test.
#
#   Linux (CI):  build-essential, cmake, ninja, clang-format, libasound2-dev,
#                libsamplerate
#   macOS (dev): cmake, ninja, libsamplerate
#
# libsrt and ALSA are platform pieces and this project does not need either of
# them to build: the CMake options are AUTO, so an absent one degrades to a
# warning and the unit tests still run.  That is why a Mac can be the whole
# development environment until real audio hardware is needed.
#
# libsamplerate is not platform-specific, and it is not optional in the same
# sense: it is what ADR 0003 measured continuous resampling with, so the clock
# module's resampler is committed to it.  Its tests have to run in CI rather than
# skip there, because a clock built unverified is worse than one not built.
#
set -euo pipefail

if command -v apt-get >/dev/null 2>&1; then
  sudo apt-get update -qq
  sudo apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build libasound2-dev libsamplerate0-dev
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
  echo "installed: build-essential cmake ninja-build libasound2-dev libsamplerate clang-format"
elif command -v brew >/dev/null 2>&1; then
  brew list cmake >/dev/null 2>&1 || brew install cmake
  brew list ninja >/dev/null 2>&1 || brew install ninja
  brew list srt >/dev/null 2>&1 || brew install srt
  brew list libsamplerate >/dev/null 2>&1 || brew install libsamplerate
  echo "installed: cmake ninja srt libsamplerate (ALSA is Linux-only; the build skips it)"
else
  echo "no apt-get or brew found: install cmake, a C++17 compiler, libsamplerate and," \
    "on Linux, libasound2-dev by hand" >&2
  exit 1
fi
