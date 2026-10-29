# Releases, CI and tools

## Releases and updates

- The auto-updater checks the GitHub releases of this fork (`zdxsv-X.Y.Z`
  tags) instead of pcsx2.net.
- Pushing a tag `zdxsv-X.Y.Z` runs `.github/workflows/zdxsv_release.yml`. It
  builds Windows x64 and publishes two files with the same content:
  `pcsx2-zdxsv-windows-x64.7z` for the updater and `.zip` for a first install.
- A tag with a suffix, such as `zdxsv-0.0.1-rc1`, becomes a prerelease. The
  updater skips prereleases.
- The release zip holds `portable.txt`, `ZDXSV.md` and `ZDXSV.ja.md`.

## CI

- One Windows build (CMake + clang-cl) runs on pushes to and pull requests
  into `zdxsv-master` and `ai/zdxsv`. It also runs the unit tests. A draft
  pull request is not built; the build starts when it is marked ready for
  review.
- The Linux and macOS workflows only run by hand (`workflow_dispatch`).

## Local build and run

| Script | What it does |
|---|---|
| `build-local.sh` | Builds PCSX2 on Windows from Git Bash, as the Windows CI's MSBuild path; the first run builds `deps/`. `--run` launches the build. |
| `.github/workflows/scripts/macos/build-dependencies.sh` | Builds the macOS deps, as upstream but without `qt110-xcode264.patch`: Qt 6.11.1 already contains it, and applying it again breaks arm64 Qt builds on Xcode 26.6. |
| `run.py` | Launches several local instances side by side, each with its own data directory under `work/`. Modes: `rom`, `state`, `rbk_test`, `rbk_test_random`. Settings: environment variables listed at the top of the script. |
| `tools/zdxsv/udprelay.py` | UDP relay that adds latency, jitter and loss between two GGPO peers. |
| `tools/zdxsv/pwcheck.py` | Sync check across the net traces of peers, or of a replay and its live battle ([rollback.md](rollback.md), Sync checks). `--battle CODE`: one battle of a multi-battle trace. |
| `tools/zdxsv/zdcheck.py` | Checks the `zd=1` lines (`Z`) of a `ZDXSV_NET_TRACE` file. |
| `tools/zdxsv/recvcheck.py` | Checks the receive slots (`R` lines) of a `ZDXSV_NET_TRACE` file. |
| `tools/zdxsv/ramcount.py` | Finds EE RAM values that count down or up linearly across `ZDXSV_RAM_DUMP` dumps. |
| `tools/zdxsv/ramvals.py` | Lists EE RAM u16 slots by the values they take across `ZDXSV_RAM_DUMP` dumps. |
| `tests/zdxsv/vu1_optest_cmp.py` | `vu1_optest_cmp.py <a.bin> <b.bin> <emulog of a> [cases]`: compares two `ZDXSV_VU1_OPTEST` files (e.g. x86_64 vs ARM64): per op, the cases whose outputs differ, then the first such cases with inputs and both outputs. |
| `tests/zdxsv/ee_optest_cmp.py` | `ee_optest_cmp.py <a.bin> <b.bin> <emulog of a> [cases]`: compares two `ZDXSV_EE_OPTEST` files (record size per op: `V*` = COP2): per op, the cases whose outputs differ and which output (Fd/ACC, FCR31; VF3 lanes, status, MAC, clip, Q), then the first such cases with inputs and both outputs. |

`run.py rbk_test` runs a GGPO battle between the instances without a server
and ends with the sync check. `LAT=ms JITTER=ms LOSS=0..1` send each pair of
instances through `udprelay.py`. `SYNC_CHECK=0` turns the check off.

## PINE commands for test rigs

| Opcode | Arguments | Action |
|---|---|---|
| `0x30` | pad u8, bind u8, value u8 | set a pad input |
| `0x31` | name length u16, file name `*.png` (no folder) | queue a GS screenshot into the snapshots folder; returns u32 length + the full path. `pine.py snap` moves it to the path asked for |
| `0x32` | none; returns u32 | read the frame count |

## Unit tests

| File | Covers |
|---|---|
| `tests/ctest/core/ggpo_tests.cpp` | GGPO synctest |
| `tests/ctest/core/dev9_config_tests.cpp` | network defaults |
