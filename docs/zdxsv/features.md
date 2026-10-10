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
- "none" means the feature has no automated test.
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
| Network defaults for zdxsv (adapter on, Sockets, internal DNS, game hosts looked up as `zdxsv.net`) | always | unit: `DEV9Config.*` in `tests/ctest/core/dev9_config_tests.cpp` | none |
| Platform info `0x9950` to the lobby | always on a zdxsv lobby; `ZDXSV_PLATFORM_INFO=0` turns it off | `m4z.sh` with samples of the lobby `/api/stat`: the lobby counts each client by platform | none |
| Battle over TCP to the battle server, as a PS2 | always | `m4.sh`: 4 emulators, fake lobby, one battle. Variant `MASHP=1 SHOW=1`: time-up with side 1 ahead, all 4 report a loss | none |
| Playing as a console (seen as a real PS2): TCP battle through the battle server | `ZDXSV_PLATFORM_INFO=0` | `m4z.sh` (default: 4 consoles, real lobby, fresh boot and login from the memory cards): every client logs `platform info off` and no `bridge`; the clients not behind zproxy (p2, p4) join the battle server over TCP; 4 results with the same battle code and frame count | none |
| A console behind a separate zproxy process | `ZDXSV_PLATFORM_INFO=0`, server set to zproxy's address by the lobby | the same `m4z.sh` run, `ZP="1 3"` (default): zproxy runs with `-upnp=false`, registers with the lobby, the PS2 connects to it and it joins the battle server over UDP (P2P on); no UPnP line in its log | none |
| First login from a blank memory card | always | `launch.ps1 -Memcard <blank>`, then `drive.py fresh`, `register`, `newlogin` | none |
| Lobby save states (DEV9 kept in save states, connections taken over after a load) | `ZDXSV_LOBBY_STATE=1` | no test of its own; `m4.sh` starts its clients from lobby save states | none |
| Settings page Settings → zdxsv (environment variable > setting > default; an overridden setting greyed out) | always | none (checked by hand) | none |

## Input latency

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Low-latency vsync (present before the limiter sleep) | setting `ZdxsvLowLatencyVsync` | `latency.ps1 -LowLatency 1`: press-to-present time on the main menu | `latency.ps1 -LowLatency 0`, or `ZDXSV_GAME_CRC=0` (not the Z game): the time is longer |
| Input latency measurement | `ZDXSV_INPUT_LATENCY` | it is the tool of the test above; `m4lat.sh` uses it in a battle (`M4=m4ggpo.sh`: over GGPO, frame pacing split by rollback) | none |

## Rollback netcode (GGPO)

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| GGPO library (`3rdparty/ggpo`) | linked always | unit: `GGPO.*` in `tests/ctest/core/ggpo_tests.cpp` | `GGPO.SyncTestCatchesStateOutsideSave` |
| GGPO pump thread (sockets every 1 ms) | `net=1` | `TRACE=0 rbk.sh 2 1` x2: `pump thread started` per peer, logged OSD pings 0-5 ms | `PCSX2_ENV=ZDXSV_NET_PUMP=0`: no such line, pings 2-23 ms |
| Delta save states | used by GGPO; self-test `ZDXSV_DELTA_TEST` | `deltatest.ps1` exit 0 (`result PASS`): the 2nd rerun of each window hashes like the 1st (same code cache) | `break=ee`: `result FAIL`, exit 1 |
| GGPO synctest in a running game | `ZDXSV_GGPO` without `net=1` | `deltatest.ps1 -Var ZDXSV_GGPO -Spec start=9000,frames=3000,mask=fcff -Route arcade -End 12300`: 0 of 3000 frames mismatch in an arcade battle (`hash=pw`) | `control=input`: mismatches |
| GGPO on by default for lobby battles | setting `ZdxsvGgpo` (on) | `CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto GGPO_DEFAULT=1 m4z.sh`: client 1 has no `ZDXSV_GGPO`, logs the default options, GGPO battle with 0 mismatches | the same with `ZdxsvGgpo = false` in client 1's ini: client 3 cuts the battle connection, the checks fail |
| Minimum input delay setting | setting `ZdxsvGgpoMinDelay`; `mindelay=` overrides it | `ZdxsvGgpoMinDelay = 4` in the `[DEV9/Eth]` section of both clients' ini, `CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto GGPO_DEFAULT=1 GMIN=3 m4z.sh`: client 1 `lobby delay 4 ... min 4`, client 3 (`mindelay=3`) `lobby delay 3 ... min 3`, 0 mismatches | without the setting: `lobby delay 2 ... min 2` |
| GGPO battle without servers | `ZDXSV_GGPO=net=1` + `ZDXSV_RBK=i/N` | `rbk.sh 2 1` and `rbk.sh 4 1`: random input, per frame, the x, y, z of every player and game RNG B agree on all peers (`pwcheck.py`). The start states come from `rbkprep.sh`. Network variant: `LAT=30 JITTER=15 LOSS=0.05`. Also `run.py rbk_test_random` | `PS= rbk.sh 4 1`: without the play-start barrier the peers desync |
| Every pad input reaches a GGPO battle: buttons, Start, both sticks (axis <= 0x40 / >= 0xc0 = a direction) | always (`net=1`) | `rbk.sh 2` + PINE presses on p1 (`pine.py` opcode 0x30, binds 9, 18-25): NET_TRACE `Z` lines of position 0, the synced (A, B) equals the game's own record (Start = A 0x8000 / B 0x8000, left stick = d-pad B bits, right stick = the same bits in A) | none |
| VM shutdown or reset ends the session | always | `rbk.sh 2 1` with `replay=DIR` and `ENV0=ZDXSV_VM_TEST=1500:shutdown` (then `:reset`): position 0 logs `vm shutdown: session 1` and saves its replay, no `.replay-*.p2s` is left, the peer logs `peer disconnected`. Replay: `rplay.sh` with `PCSX2_ENV=ZDXSV_VM_TEST=1400:shutdown`: no `zdxsv-replay*` file left in the cache | the build without the `VMManager.cpp` calls: shutdown leaves `.replay-*.p2s` and no replay; after a reset the old session goes on (hundreds of delta loads into the reset VM) to the battle end |
| MTVU speedhack off while GGPO or a replay runs | always (GGPO options, `ZDXSV_REPLAY`, `ZDXSV_DELTA_TEST`) | `rbk.sh 2 1` with `MTVU=1` (`vuThread = true` in the ini): every peer logs `MTVU speedhack off for this VM`, no `MTVU speedhack is enabled` warning (one per state save or load with MTVU on), 0 mismatches | the build without it: MTVU stays on, `MTVU speedhack is enabled` warnings |
| GGPO battle with the battle server and a fake lobby | `ZDXSV_GGPO=net=1` | `m4relay.sh` with `DELAY`, `JITTER`, `LOSS`: the 4 battle results agree and every relay forwarded packets | none |
| GGPO battle from the battle info of the lobby | `ZDXSV_GGPO=net=1,lobby=1,port=P` | fake lobby: `m4ggpo.sh`, 4 results agree and each client has 3 lobby peers. Real lobby: `EMU=1 GGPO=7101 m4z.sh`, and 1v1 with `CLIENTS="1 3"` | `CLIENTS="1 3" GGPO_CLIENTS=1 m4z.sh`: client 3 without GGPO, client 1 cuts the battle connection |
| Input delay from the ping test | lobby battle without `delay=` | 1v1 `m4z.sh` with `GDELAY=auto`: log `lobby delay D`. `GMIN=3`: delay 3 | none |
| Ping test failure cuts the battle connection | lobby battle | 1v1 `m4z.sh` with `BAD_SESSION=3`: both clients cut and return to the lobby | none |
| Match report `0x9952` | lobby battle | `REPORT=1` on the 1v1 or the cut case: the lobby logs 2 reports with the battle code and the expected result | none |
| Relay servers | battle info with `relay_<k>` | `RELAY=1 GGPO_LAT=100 GDELAY=auto m4ggpo.sh`: every path `relay 0`, delay 2. Real lobby: the same options on 1v1 `m4z.sh` | the same without `RELAY`: every path `direct`, delay 7 |
| Dual-stack peers (IPv4 and IPv6 candidates) | lobby battle | unit: `GGPO.UdpDualStack`: the GGPO socket sends and receives over IPv6 (skipped on a host without IPv6). The choice between the candidates has no test | none |
| P2P connectivity test (open, cone NAT, symmetric NAT) | first lobby connection with `lobby=1` | 1v1 `m4z.sh` with `NAT=open`: one `udp test: nat=open` line per client | none |
| HTTPS latency test (cloud regions, as flycast's ping test) | when the game goes online; `ZDXSV_HTTPS_LATENCY=0` turns it off | 1v1 `m4z.sh` with `HTTPS=1`: one test per client, its platform info in `lobby.log` has `asia-northeast1` | `HTTPS_OFF=3`: that client's platform info has no region |
| Bounded lobby network waits | lobby battle | `lobbytest.sh`: unit test of `Zdxsv/Lobby.cpp` with a fake STUN | none |
| Network status OSD | `net=1`; setting `ZdxsvNetOsd`, `osd=0` hides it | `EMU=1 GGPO=7101 OSD=1 m4z.sh`: the logged OSD text of each client has its delay and every opponent's user id, name, pilot name and ping; names and pilot names equal the lobby db (`osdname.py`) | none |
| UDP sockets ignore ICMP port unreachable on Windows | always in GGPO | unit: `GGPO.UdpReadsPastPortUnreachable`: after a send to a closed port, the next poll still reads the datagram behind the error | the same test with the `SIO_UDP_CONNRESET` call removed from `udp.cpp`: 0 datagrams read |

## Replays

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Save a GGPO battle as a replay | `replay=DIR` in `ZDXSV_GGPO`; default for lobby battles | `rbk.sh 2 1` with `replay=DIR`, then `replay_check.py` on the file of every peer: same inputs on the common frames. Real lobby: 1v1 `m4z.sh`, then `replay_check.py` | a copy with 1 input byte changed fails at that frame |
| Play a replay | `ZDXSV_REPLAY=file.pb` | `rplay.sh` against the trace of the live battle: player work equal on every frame | a copy with changed buttons fails from the changed frame |
| Play a replay by URL or battle code | `ZDXSV_REPLAY=http(s)://.../x.pb` or the lobby's `/lbs/replay?battle_code=C` | `UPLOAD=1 REPLAY_API=4 PWTRACE=1 CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto m4z.sh`: the API's `replay_url` = the stored url, player work + rng = the players'. Without a lobby: `API=1 FILE=<the battle's .pb> rplay.sh <its traces>` | `API=1 FILE=<another battle's .pb> rplay.sh` with the same traces fails |
| Seek (keys every 600 frames, hotkeys) | PageUp, PageDown; `ZDXSV_REPLAY_SEEK` in tests | `rplay.sh` with `ZDXSV_REPLAY_SEEK=at:to,...`: the predicted key is loaded and player work equals live after the seeks. Hotkeys: `WINDOW=1 KEYS=...` | `ZDXSV_REPLAY_KEY_NOHLE=1` with the same seeks fails |
| Point of view (pick, switch) | `ZDXSV_REPLAY_POV=P`, Home, `ZDXSV_REPLAY_POV_AT=frame:P` | `POV=1 rplay.sh` on the position-0 file: `net armed ..., position 1` + pwcheck; `PCSX2_ENV=ZDXSV_REPLAY_POV_AT=2500:1,3500:0`: new position's battle start, seek, state hashes equal | without `POV` the game arms as the recorder (position 0) |
| Control bar | shown during a replay; setting `ZdxsvReplayBar`, `ZDXSV_REPLAY_BAR` | `rplay.sh` with `WINDOW=1 KEYS="...bar:NAME..."` (real mouse through `pcsx2ctl.ps1`): the logged seek frame is the one computed from the click position | none |
| Key display | End; setting `ZdxsvReplayKeyDisplay`, `ZDXSV_REPLAY_KEY_DISPLAY=1` | `keycheck.py`: every logged key history equals the inputs in the file up to that frame | a copy with 1 input bit flipped fails for the histories that reach that frame |
| Live spectating | lobby `live_uplink=1` (uplink), `ZDXSV_REPLAY=udp://host:8201[/code]` (spectator) | `m4z.sh` `LIVE=2 PWTRACE=1`: the spectator closes at the uplink's saved frame count, player work + rng = the players' | a receiver that ignores the lobby's close push (it has no frame_bytes) stalls 30 frames before the end |
| Live speed trim | on while spectating; `ZDXSV_LIVE_PACING=0` turns it off | `LIVE=2 LIVE_SPEED=1.02 CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto m4z.sh`, the spectator's `live: pace` log lines after the battle load (read, no scripted check): gap 28-32, at most 1 more wait | the same with `ZDXSV_LIVE_PACING=0`: the gap runs down to the newest frame |
| Replay window (local, remote, live lists) | Tools → zdxsv Replays; setting `ZdxsvLobbyApiUrl` | `replayui.ps1 -DataPath <instance> -Tab Local/Remote/Live -Row <code>` (UI Automation; Remote/Live against a local zdxsv with `ZDXSV_LOBBY_API_ADDR`, Live during `m4z.sh`): the instance emulog logs `replay <source>` and the common start of the picked point of view; the lobby logs `live subscribe` | the same `.pb` through `ZDXSV_REPLAY` logs the same lines and state hashes |
| Live auto-next | setting `ZdxsvLiveAutoNext`; `ZDXSV_LIVE_NEXT=N` in tests | `BATTLES=2 LIVE=2 LIVE_NEXT=1 PWTRACE=1 CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto m4z.sh`: the spectator waits at the end of battle 1, moves on to battle 2; both streams close at the uplink's saved frame count, battle 2 player work + rng = the players' | `LIVE_NEXT=0`: the spectator exits after battle 1 (not run) |
| Skip mobile suit selection | on by default (setting `ZdxsvReplaySkipMs`); `ZDXSV_REPLAY_SKIP_MS=0` turns it off | `rplay.sh` with `SKIP_MS=1`: the logged briefing frame equals the one in the live trace | `SKIP_MS=0`: the screenshot at 25 s shows the selection screen |
| Round jump | Shift+PageUp, Shift+PageDown, bar step buttons; `ZDXSV_REPLAY_ROUND_AT` in tests | `rplay.sh` with `SKIP_MS=1 PCSX2_ENV=ZDXSV_REPLAY_ROUND_AT=1000:2,17000:1,6000:3`: every logged load end is 1 frame after a live trace `L` line leaving state 8 (same vsync), the rounds start at the predicted loads, player work equals live. Keys and bar: `WINDOW=1 KEYS="10:Shift+PageDown;40:bar:show,w600,bar:nextround"` | none |
| Takeover | bar "Take over" / "Retry" / "Replay" buttons, hotkey "Take Over / Retry", START = retry or skip the matching; `ZDXSV_REPLAY_TAKEOVER`, `ZDXSV_REPLAY_TAKEOVER_RETRY` in tests | `rplay.sh` on a 2-player lobby battle `.pb` with `SKIP_MS=1 PCSX2_ENV="ZDXSV_REPLAY_TAKEOVER=6000:replay ZDXSV_REPLAY_TAKEOVER_RETRY=9000"`: the own position packed by the takeover (pad + kind-3 msgs, delay 2) plays to the end, retry goes back to 6000, player work + rng = the players' | `ZDXSV_REPLAY_TAKEOVER=6000:rand`: FAIL. Input matching panel with a real pad: not run |
| Four-screen replay | `ZDXSV_REPLAY_FOUR=1` (one file) | `FOUR=1 rplay.sh`: guest trace pwcheck + host `spread` lines <= 4 frames | `PCSX2_ENV=ZDXSV_REPLAY_SYNC=0`: the spread FAILs |

## Releases and tools

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Self-update from the GitHub releases of this fork | on in release builds; `ZDXSV_UPDATE_URL` for tests | `updatecheck.py --exe <build at a zdxsv-X.Y.Z tag> --inis <data dir>/PCSX2/inis`: a local release list offers `zdxsv-999.0.0`, the startup check fetches it once and logs `Update needed.` | `--offer zdxsv-0.0.0`: `No update needed.` An untagged build fetches nothing |
| Release workflow (a `zdxsv-X.Y.Z` tag builds the release) | push a tag | none | none |
| Windows CI build | every push and PR | the workflow itself; it also runs the unit tests | none |
| PINE opcodes `0x30` to `0x32` (pad input, screenshot, frame count) | PINE enabled | no test of its own; the rig scripts drive the emulator through them (`pine.py`) | none |
| EE probe, watch and rerun profile (`ZDXSV_EE_PROBE`, `ZDXSV_EE_WATCH`, `ZDXSV_EE_PROFILE`) | environment variables | `probelint.py`: refuses a bad or stale probe list | none |
| `run.py` (several local instances side by side) | `python run.py rom`, `state`, `rbk_test` | `rbk_test` ends with the sync check of `pwcheck.py` | none |
