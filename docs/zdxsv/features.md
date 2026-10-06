# zdxsv features and their tests

Every feature this fork adds to PCSX2, with the tests that cover it.
Details of each feature are in the topic documents of this directory, and
every option is in [options.md](options.md).
The rules for keeping this file up to date are in [AGENTS.md](../../AGENTS.md).

- A test is a rig test: a script that runs one or more emulators with the game
  and checks the result. The game image, BIOS and save states are not in the
  repository.
- **Control** is the variant of a test that must fail. It shows the test can
  detect the problem.
- "none" means the feature has no automated test. "none listed" means the test
  index this file was made from names no test for it.
- This file does not record results. A result belongs to the commit it ran on.

## Where the test scripts are

| Location | Scripts |
|---|---|
| Repository root | `run.py` |
| `tools/zdxsv/` | `pwcheck.py`, `udprelay.py` |
| `tests/zdxsv/` | all other scripts named below |

Setup and the machine settings the scripts need (game image, work root,
zdxsv checkout, save states): `tests/zdxsv/README.md`.

## Online play

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Network defaults for zdxsv (adapter on, Sockets, internal DNS, game hosts to the zdxsv server) | always | unit: `DEV9Config.*` in `tests/ctest/core/dev9_config_tests.cpp` | none |
| Platform info `0x9950` to the lobby | always on a zdxsv lobby; `ZDXSV_PLATFORM_INFO=0` turns it off | `m4z.sh` with samples of the lobby `/api/stat`: the lobby counts each client by platform | none |
| Battle over TCP to the battle server, as a PS2 | always | `m4.sh`: 4 emulators, fake lobby, one battle. Variant `MASHP=1 SHOW=1`: time-up with side 1 ahead, all 4 report a loss | none |
| First login from a blank memory card | always | `launch.ps1 -Memcard <blank>`, then `drive.py fresh`, `register`, `newlogin` | none |
| Lobby save states (DEV9 kept in save states, connections taken over after a load) | `ZDXSV_LOBBY_STATE=1` | no test of its own; `m4.sh` starts its clients from lobby save states | none |

## Input latency

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Low-latency vsync (present before the limiter sleep) | on by default; `ZdxsvLowLatencyVsync` in `PCSX2.ini` | `latency.ps1 -LowLatency 1`: press-to-present time on the main menu | `latency.ps1 -LowLatency 0`: the time is longer |
| Input latency measurement | `ZDXSV_INPUT_LATENCY` | it is the tool of the test above; `m4lat.sh` uses it in a battle | none |

## Rollback netcode (GGPO)

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| GGPO library (`3rdparty/ggpo`) | linked always | unit: `GGPO.*` in `tests/ctest/core/ggpo_tests.cpp` | `GGPO.SyncTestCatchesStateOutsideSave` |
| Delta save states | used by GGPO; self-test `ZDXSV_DELTA_TEST` | `deltatest.ps1`: every rerun frame hashes like its first run | `break=ee`, `blocks=linked`: mismatches must be reported |
| GGPO synctest in a running game | `ZDXSV_GGPO` without `net=1` | none listed | `control=input` exists, no listed script uses it |
| GGPO on by default for lobby battles | setting `ZdxsvGgpo` (on) | `CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto GGPO_DEFAULT=1 m4z.sh`: client 1 has no `ZDXSV_GGPO`, logs the default options, GGPO battle with 0 mismatches | the same with `ZdxsvGgpo = false` in client 1's ini: the battle stays on the battle server, the checks fail |
| GGPO battle without servers | `ZDXSV_GGPO=net=1` + `ZDXSV_RBK=i/N` | `rbk.sh 2 1` and `rbk.sh 4 1`: random input, per-frame player work hashes agree on all peers (`pwcheck.py`). The start states come from `rbkprep.sh`. Network variant: `LAT=30 JITTER=15 LOSS=0.05`. Also `run.py rbk_test_random` | `PS= rbk.sh 4 1`: without the play-start barrier the peers desync |
| VM shutdown or reset ends the session | always | `rbk.sh 2 1` with `replay=DIR` and `ENV0=ZDXSV_VM_TEST=1500:shutdown` (then `:reset`): position 0 logs `vm shutdown: session 1` and saves its replay, no `.replay-*.p2s` is left, the peer logs `peer disconnected`. Replay: `rplay.sh` with `PCSX2_ENV=ZDXSV_VM_TEST=1400:shutdown`: no `zdxsv-replay*` file left in the cache | the build without the `VMManager.cpp` calls: shutdown leaves `.replay-*.p2s` and no replay; after a reset the old session goes on (hundreds of delta loads into the reset VM) to the battle end |
| GGPO battle with the battle server and a fake lobby | `ZDXSV_GGPO=net=1` | `m4relay.sh` with `DELAY`, `JITTER`, `LOSS`: the 4 battle results agree and every relay forwarded packets | none |
| GGPO battle from the battle info of the lobby | `ZDXSV_GGPO=net=1,lobby=1,port=P` | fake lobby: `m4ggpo.sh`, 4 results agree and each client has 3 lobby peers. Real lobby: `EMU=1 GGPO=7101 m4z.sh`, and 1v1 with `CLIENTS="1 3"` | `GGPO_CLIENTS="1 2 3" m4ggpo.sh`: one client without GGPO, the battle stays on the battle server |
| Input delay from the ping test | lobby battle without `delay=` | 1v1 `m4z.sh` with `GDELAY=auto`: log `lobby delay D`. `GMIN=3`: delay 3 | none |
| Ping test failure cuts the battle connection | lobby battle | 1v1 `m4z.sh` with `BAD_SESSION=3`: both clients cut and return to the lobby | none |
| Match report `0x9952` | lobby battle | `REPORT=1` on the 1v1 or the cut case: the lobby logs 2 reports with the battle code and the expected result | none |
| Relay servers | battle info with `relay_<k>` | `RELAY=1 GGPO_LAT=100 GDELAY=auto m4ggpo.sh`: every path `relay 0`, delay 2. Real lobby: the same options on 1v1 `m4z.sh` | the same without `RELAY`: every path `direct`, delay 7 |
| Dual-stack peers (IPv4 and IPv6 candidates) | lobby battle | none listed | none |
| P2P connectivity test (open, cone NAT, symmetric NAT) | first lobby connection with `lobby=1` | none listed | none |
| Network status OSD | `net=1`; `osd=0` hides it | none listed | none |
| UDP sockets ignore ICMP port unreachable on Windows | always in GGPO | none listed | none |

## Replays

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Save a GGPO battle as a replay | `replay=DIR` in `ZDXSV_GGPO`; default for lobby battles | `rbk.sh 2 1` with `replay=DIR`, then `replay_check.py` on the file of every peer: same inputs on the common frames. Real lobby: 1v1 `m4z.sh`, then `replay_check.py` | a copy with 1 input byte changed fails at that frame |
| Play a replay | `ZDXSV_REPLAY=file.zdxr` | `rplay.sh` against the trace of the live battle: player work equal on every frame | a copy with changed buttons fails from the changed frame |
| Seek (keys every 600 frames, hotkeys) | PageUp, PageDown; `ZDXSV_REPLAY_SEEK` in tests | `rplay.sh` with `ZDXSV_REPLAY_SEEK=at:to,...`: the predicted key is loaded and player work equals live after the seeks. Hotkeys: `WINDOW=1 KEYS=...` | `ZDXSV_REPLAY_KEY_NOHLE=1` with the same seeks fails |
| Point of view switch | `ZDXSV_REPLAY=a.zdxr;b.zdxr`, Home | `rplay.sh` with `ZDXSV_REPLAY_POV_AT=frame:P`: after the switch the trace equals the live trace of the other player | without `POV_AT` it differs from that trace |
| Control bar | shown during a replay; `ZDXSV_REPLAY_BAR` | `rplay.sh` with `WINDOW=1 KEYS="...bar:NAME..."` (real mouse through `pcsx2ctl.ps1`): the logged seek frame is the one computed from the click position | none |
| Key display | End; `ZDXSV_REPLAY_KEY_DISPLAY=1` | `keycheck.py`: every logged key history equals the inputs in the file up to that frame | a copy with 1 input bit flipped fails for the histories that reach that frame |
| Skip mobile suit selection | on by default; `ZDXSV_REPLAY_SKIP_MS=0` turns it off | `rplay.sh` with `SKIP_MS=1`: the logged briefing frame equals the one in the live trace | `SKIP_MS=0`: the screenshot at 25 s shows the selection screen |
| Round jump | Shift+PageUp, Shift+PageDown, bar step buttons; `ZDXSV_REPLAY_ROUND_AT` in tests | `rplay.sh` with `SKIP_MS=1 PCSX2_ENV=ZDXSV_REPLAY_ROUND_AT=1000:2,17000:1,6000:3`: every logged load end is 1 frame after a live trace `L` line leaving state 8 (same vsync), the rounds start at the predicted loads, player work equals live. Keys and bar: `WINDOW=1 KEYS="10:Shift+PageDown;40:bar:show,w600,bar:nextround"` | none |

## Releases and tools

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Self-update from the GitHub releases of this fork | on in release builds; `ZDXSV_UPDATE_URL` for tests | none listed | none |
| Release workflow (a `zdxsv-X.Y.Z` tag builds the release) | push a tag | none | none |
| Windows CI build | every push and PR | the workflow itself; it also runs the unit tests | none |
| PINE opcodes `0x30` to `0x32` (pad input, screenshot, frame count) | PINE enabled | no test of its own; the rig scripts drive the emulator through them (`pine.py`) | none |
| EE probe and watch (`ZDXSV_EE_PROBE`, `ZDXSV_EE_WATCH`) | environment variables | `probelint.py`: refuses a bad or stale probe list | none |
| `run.py` (several local instances side by side) | `python run.py rom`, `state`, `rbk_test` | `rbk_test` ends with the sync check of `pwcheck.py` | none |
