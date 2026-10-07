#!/bin/bash
# Builds and runs lobbytest.cpp (unit test of the waits in pcsx2/Zdxsv/Lobby.cpp) with MinGW g++
# (Windows only: winsock). Needs no PCSX2 build, game or server; ~10 s build, ~6 s run.
#   OUT=<dir> bash tests/zdxsv/lobbytest.sh
# Env: CXX (default g++; MSYS2 mingw64 on PATH). Exit 0 = every check passed.
set -u
cd "$(dirname "$0")/../.." || exit 2
OUT=${OUT:?OUT=<dir> required}
mkdir -p "$OUT" || exit 2
# --allow-multiple-definition: common/Pcsx2Defs.h redefines __forceinline without inline, so the
# MinGW headers' inline helpers are emitted in both objects
"${CXX:-g++}" -std=c++20 -O1 -w -I pcsx2 -I . tests/zdxsv/lobbytest.cpp pcsx2/Zdxsv/Lobby.cpp \
	-lws2_32 -Wl,--allow-multiple-definition -o "$OUT/lobbytest.exe" || { echo "FAIL build"; exit 1; }
"$OUT/lobbytest.exe"
