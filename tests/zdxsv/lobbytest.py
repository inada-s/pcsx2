"""Builds and runs lobbytest.cpp (unit test of the waits in pcsx2/Zdxsv/Lobby.cpp) with MinGW g++
(Windows only: winsock). Needs no PCSX2 build, game or server; ~10 s build, ~6 s run.
  OUT=<dir> python tests/zdxsv/lobbytest.py
Env: CXX (default g++; MSYS2 mingw64 on PATH). Exit 0 = every check passed.
"""
import os
import subprocess
import sys

import zrig


def main():
    zrig.need("OUT")
    out = os.environ["OUT"]
    os.makedirs(out, exist_ok=True)
    os.chdir(os.path.join(zrig.HERE, "..", ".."))
    exe = os.path.join(out, "lobbytest.exe")
    # --allow-multiple-definition: common/Pcsx2Defs.h redefines __forceinline without inline, so the
    # MinGW headers' inline helpers are emitted in both objects
    if subprocess.call([zrig.env("CXX", "g++"), "-std=c++20", "-O1", "-w", "-I", "pcsx2", "-I", ".",
                        "-I", "3rdparty/rapidjson/include", "tests/zdxsv/lobbytest.cpp", "pcsx2/Zdxsv/Lobby.cpp",
                        "pcsx2/Zdxsv/LobbyBattleInfo.cpp", "pcsx2/Zdxsv/LobbyPing.cpp", "-lws2_32",
                        "-Wl,--allow-multiple-definition", "-o", exe]):
        print("FAIL build")
        return 1
    return subprocess.call([exe])


if __name__ == "__main__":
    sys.exit(main())
