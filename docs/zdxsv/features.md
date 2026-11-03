# zdxsv features

Every feature this fork adds to PCSX2, one line each, with its switch (setting,
environment variable or key). Defaults and values: [options.md](options.md).
Tests, pass criteria and controls: [tests/zdxsv/README.md](../../tests/zdxsv/README.md),
under the same feature names. Details: the topic document of each section.

## Structure

zdxsv code meets the emulator only at the seams below (`pcsx2/Zdxsv/*Hooks.h`).
A change to a seam affects every feature on it; a feature on a seam that a path
skips does not run on that path.

| Seam (upstream code) | zdxsv code | Features on it |
|---|---|---|
| EE recompilers and interpreter: hook calls before guest instructions | `RecHooks`, `IntHooks`, `GgpoBattle`, `NoDev9`, `FastLogin`, `FastBoot` | net RPC hook (battle sock over GGPO, online without DEV9), fast login, fast boot, load barriers, input feed of GGPO and replays |
| DEV9 TCP sessions | `LobbyConnection`, `Lobby`, `TlsTunnel` | platform info, battle info, match report, line checks while paused, outdated notice, pause menu refusal, TLS, connection takeover after a lobby state load |
| DEV9 DNS server | `ServerHosts` | game hosts looked up as `ZDXSV_SERVER_HOST`, whether the public server was found (replay upload) |
| DEV9 receive and save state | `DeltaState` | received packets in delta states, lobby save states |
| Counters (vsync), VMManager | `Ggpo`, `Replay*`, `NoDev9`, `FastBoot` | GGPO frame loop, replay playback, VM start / reset / shutdown cleanup |
| MTGS, GS, SPU2 | `DeltaFreeze`, `SingleDrawList`, `InputLatency`, `Overlays` | rerun frame skips, single draw list, input latency, OSD |
| Pad | `Ggpo`, `ReplayPlay` | inputs to and from GGPO and replays |
| Hotkeys, ImGui | `Overlays`, `ReplayPlay` | replay hotkeys and bar, refused pause and speed changes |

### Network paths

The game's network calls (net RPCs) take one of two paths. Online with
`ZdxsvNoDev9` on (the default in release builds) the DEV9 rows above do not run, except the lobby connection filter (`LobbyConnection`), which `NoDev9` calls on its host sockets.

| Path | When | Network | Lobby features |
|---|---|---|---|
| DEV9 | `ZdxsvNoDev9` off, or no server host | DEV9 adapter: DNS server and TCP sessions | every lobby feature of the DEV9 rows |
| No DEV9 | `ZdxsvNoDev9` on with a server host | `NoDev9.cpp` answers the net RPCs on the EE; TCP on host sockets | fast login; the lobby connection filter: platform info, battle info, match report, outdated notice (read off the code; the platform info tested); TCP battle (1v1) through the battle server; GGPO battle (1v1), the GGPO hook answering the battle sock first; TLS (DNAS, lobby, battle TCP). Not: line checks while paused (the filter is fed from the EE), the DNS server rows, lobby save states |
| Battle sock over GGPO | an armed GGPO battle, on either path | `GgpoBattle.cpp`: send, recv and poll answered from GGPO inputs | — |

A lobby feature added on one path is added to the other in the same change, or
this table says it is missing.

## Online play ([lobby.md](lobby.md))

- **Switch for all online battle extensions**: `ZDXSV_EXTENSIONS=0`.
- **Network defaults for zdxsv**: adapter on, Sockets, internal DNS, game hosts as `ZDXSV_SERVER_HOST`.
- **Online without DEV9**: experimental, with the platform info, a TCP battle, a GGPO battle and TLS; `ZdxsvNoDev9`, `ZDXSV_NODEV9`.
- **Fast login**: DNAS and the login browser skipped; `ZdxsvFastLogin`, `ZdxsvLoginApiUrl`.
- **Fast boot**: boot to the lobby user_id selection with no input; `ZdxsvFastBoot`, `ZdxsvFastBootState`.
- **Platform info `0x9950` to the lobby**: `ZDXSV_PLATFORM_INFO`.
- **Battle over TCP to the battle server, as a PS2**: always.
- **Playing as a console**: seen as a real PS2; `ZDXSV_PLATFORM_INFO=0`.
- **A console behind a separate zproxy process**: `ZDXSV_PLATFORM_INFO=0`, server set by the lobby.
- **First login from a blank memory card**: always.
- **TLS to the zdxsv server**: DNAS, lobby, battle TCP, pinned keys; on when the client has pins.
- **Lobby save states**: `ZDXSV_LOBBY_STATE=1`.
- **Lobby connection kept while paused**: line checks `0x6001` answered.
- **Pause menu**: refused while connected to the zdxsv lobby.
- **Settings page**: Settings → zdxsv.
- **Recommended-settings buttons**: Low, Mid, High on Settings → zdxsv.

## Input latency ([options.md](options.md))

- **Low-latency vsync**: present before the limiter sleep; `ZdxsvLowLatencyVsync`.
- **Input latency measurement**: `ZDXSV_INPUT_LATENCY`.
- **Input to picture by pixel color**: `ZDXSV_INPUT_LATENCY` key `pixel=`.

## Boot to lobby

- **Boot-to-lobby time measurement**: a test tool.

## Rollback netcode (GGPO) ([rollback.md](rollback.md), [lobby.md](lobby.md))

Keys (`net=1`, `lobby=1`, `port=`, `delay=`, ...) are parts of `ZDXSV_GGPO`.

- **GGPO library**: `3rdparty/ggpo`.
- **GGPO pump thread**: sockets every 1 ms; `net=1`.
- **GGPO time sync spread over frames**: `net=1`.
- **Relay forwarding while GGPO is held**: `net=1`.
- **GGPO session check**: battle code in the sync handshake; `ZDXSV_NET_SESSION`.
- **Rollback budget in the lobby delay**: lobby battle without `delay=`.
- **Delta save states**: used by GGPO; `ZDXSV_DELTA_TEST`.
- **Faster rerun frames**: `ZDXSV_RERUN_*`.
- **Single draw list**: on (setting `ZdxsvSingleDrawList`); `ZDXSV_SINGLE_DRAW_LIST=0` turns it off.
- **Battle start and MS select load barriers**: GGPO session.
- **GGPO synctest in a running game**: `ZDXSV_GGPO` without `net=1`.
- **Rerun keep**: the bone matrix slots and beam saber trails are not rolled back during battle play; `ZDXSV_RERUN_KEEP`.
- **GGPO on by default for lobby battles**: `ZDXSV_EXTENSIONS=0` turns it off.
- **Battle UDP port setting**: `ZdxsvGgpoPort`, `port=`.
- **Minimum input delay setting**: `ZdxsvGgpoMinDelay`, `mindelay=`.
- **GGPO battle without servers**: `net=1` + `ZDXSV_RBK`.
- **Every pad input reaches a GGPO battle**: buttons, Start, both sticks.
- **Session ends on a confirmed battle end**.
- **Clean end, tails differ**.
- **Clean end, peer left first**.
- **VM shutdown or reset ends the session**.
- **A lobby battle that breaks after the start goes back to the lobby**.
- **Pause and speed refused in a net battle**: pause, speed, frame advance, state loads, pause menu.
- **MTVU speedhack off while GGPO or a replay runs**.
- **GGPO battle with the battle server and a fake lobby**: `net=1`.
- **GGPO battle from the battle info of the lobby**: `net=1,lobby=1`.
- **Input delay from the ping test**: lobby battle without `delay=`.
- **Ping test failure cuts the battle connection, OSD with the reason**.
- **Connection diagram during the ping test**: lobby battle without `delay=`.
- **Sync-safe settings forced while the Z game runs**: `ZDXSV_SYNC_FORCE=0` turns it off.
- **Sync fingerprint**: cuts a battle whose players' builds or settings differ.
- **Match report: timesync, inputs, fps, loss**.
- **Match report sent to the lobby**: `0x9952`.
- **Match report: who left, ping**.
- **Match report round data**: mobile suits per round.
- **Relay servers**: battle info with `relay_<k>`.
- **Peer without an address in the battle info**.
- **Path through a relaying peer**.
- **Unreachable peer cuts the battle connection**.
- **Cut player refuses GGPO**: a peer that started GGPO closes at once.
- **Dual-stack peers**: IPv4 and IPv6 candidates.
- **P2P connectivity test**: open, cone NAT, symmetric NAT; first lobby connection with `lobby=1`.
- **UPnP mapping of the GGPO port**: `ZdxsvUPnP`.
- **"UPnP Now" button on Settings → zdxsv**.
- **HTTPS latency test**: cloud regions; `ZDXSV_HTTPS_LATENCY=0` turns it off.
- **`machine_id` in the platform info**: Windows, macOS.
- **OSD message when the lobby refuses this PCSX2 as outdated**: notice `0x6003`.
- **Lobby network waits and ping test RTT**: bounded waits, candidates from packet sources, median RTT.
- **Battle info and match report as protobuf**.
- **Network status OSD**: `ZdxsvNetOsd`, `osd=0`.
- **Network OSD on disconnect**: held 10 s, shown while a peer is interrupted or gone.
- **UDP sockets ignore ICMP port unreachable on Windows**.

## Replays ([replay.md](replay.md))

- **Save a GGPO battle as a replay**: `ZdxsvSaveReplay`, `ZdxsvReplayPath`, `replay=`.
- **Upload a lobby battle's replay**: `ZdxsvReplayUpload`, `upload=`.
- **Play a replay**: `ZDXSV_REPLAY=file.pb`.
- **Replay that does not start**: error OSD.
- **Play a replay by URL or battle code**: `ZDXSV_REPLAY=http(s)://...`.
- **Common state**: a cached copy that does not load is downloaded again once.
- **Seek**: PageUp, PageDown; `ZDXSV_REPLAY_SEEK`.
- **Point of view**: Home; `ZDXSV_REPLAY_POV`, `ZDXSV_REPLAY_POV_AT`.
- **Control bar**: `ZdxsvReplayBar`.
- **Key display**: End; `ZdxsvReplayKeyDisplay`.
- **Live spectating**: lobby `live_uplink=1`; `ZDXSV_REPLAY=udp://...`.
- **Live round results**.
- **Live speed trim**: `ZDXSV_LIVE_PACING=0` turns it off.
- **Live wait without blocking**.
- **Replay window**: Tools → zdxsv Replays; `ZdxsvLobbyApiUrl`.
- **Return to the game after a replay**: `ZdxsvReplayReturn`.
- **Live auto-next**: `ZdxsvLiveAutoNext`, `ZDXSV_LIVE_NEXT`.
- **Live auto-next skip**: with auto-next.
- **Skip mobile suit selection**: `ZdxsvReplaySkipMs`.
- **Round jump**: Shift+PageUp, Shift+PageDown, bar step buttons.
- **Round mobile suits**: hover the round results on the bar.
- **Takeover**: bar "Take over" / "Retry" / "Replay", hotkey "Take Over / Retry".
- **Four-screen replay**: `ZDXSV_REPLAY_FOUR=1`.

## ARM64 builds ([arm64.md](arm64.md))

- **EE and IOP recompilers with the x86 timing**: `EnableEE`, `EnableIOP`.
- **VU1 trace recompiler**: `PCSX2_VU1_JIT=0` turns it off.
- **FPU and VU math as x86**.
- **EE FPU interpreter as the recompiler**: all builds.

## Releases and tools ([tools.md](tools.md))

- **Self-update from the GitHub releases of this fork**: release builds; `ZDXSV_UPDATE_URL`.
- **Release workflow**: a `zdxsv-X.Y.Z` tag.
- **Windows CI build**: every push and PR.
- **PINE opcodes `0x30` to `0x32`**: pad input, screenshot, frame count.
- **EE probe, watch and rerun profile**: `ZDXSV_EE_PROBE`, `ZDXSV_EE_WATCH`, `ZDXSV_EE_PROFILE`.
- **`run.py`**: several local instances side by side.
