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
| DEV9 save state | `DeltaState` | DEV9 registers in full and delta states (GGPO, replays) |
| Counters (vsync), VMManager | `Ggpo`, `Replay*`, `NoDev9`, `FastBoot`, `OnlineLimits`, `AllyHp` | GGPO frame loop, teammate HP on the HUD, replay playback and its unthrottled fast frames, VM start / reset / shutdown cleanup, online switch of the limited features |
| MTGS, GS, SPU2 | `DeltaFreeze`, `SingleDrawList`, `InputLatency`, `Overlays`, `FastBoot`, `ReplayPlay` | rerun frame skips, replay seek frames not drawn or heard, fast boot shows every 8th frame, single draw list, input latency, OSD |
| Pad | `Ggpo`, `ReplayPlay` | inputs to and from GGPO and replays |
| Hotkeys, ImGui, GS draw rect | `Overlays`, `ReplayPlay`, `GgpoOsd` | replay hotkeys, bar and key display on the game picture, refused pause and speed changes, network status panel |
| Patch loader (`PatchHooks.h`) | `GamePatches` | Z game patches built in, missing patches.zip quiet |
| PINE server (`PineHooks.h`) | `OnlineLimits` | PINE refused while online |
| Pad macro buttons (`InputHooks.h`) | `OnlineLimits` | macro buttons off while online |
| Pad input (`InputHooks.h`: `Pad::SetControllerState`) | `StickMessage` | held button + D-pad or left stick tilts the right stick |
| Qt emulation settings (`QtHooks.h`) | `pcsx2-qt/Zdxsv/QtEmulationSettings` | low-latency present checkbox |
| Qt setup wizard (`QtHooks.h`) | `pcsx2-qt/Zdxsv/QtSetupWizard` | first-run wizard texts, zdxsv sign-in page |
| Qt fonts | `pcsx2-qt/Zdxsv/QtFonts` | Noto Sans JP download before the OSD fonts load |
| Qt game window wheel (`QtHooks.h`, `DisplayWidget`) | `pcsx2-qt/Zdxsv/QtWheelVolume` | mouse wheel volume |
| VMManager, GS, MTGS, hotkeys, Qt main window (`UiHooks.h`, `QtHooks.h`) | `OnlineLimits`, `pcsx2-qt/Zdxsv/QtOnlineLimits`, `ReplayControls` | operations refused while online or during a replay, debugger window closed |
| Qt host start and exit (`CrashReportHooks.h`), log output (`Log::SetTapOutputLevel` in `common/Console`) | `pcsx2-qt/Zdxsv/CrashReports` | crash reports, the last log lines they carry |
| Qt host start (`AuthHooks.h`) | `Zdxsv/AuthSignIn` | sign-in test trigger (`ZDXSV_AUTH_SIGNIN_TEST`) |
| Default settings and settings load (`DefaultsHooks.h`: `VMManager::SetDefaultSettings`, Big Picture controller reset, Qt host start) | `InputDefaults` | no upstream hotkeys, USB keyboard on port 1 |

### Network paths

The game's network calls (net RPCs) take one of two paths. Online with
a server host (the default) the net RPCs never reach DEV9; zdxsv adds nothing to the DEV9 path.

| Path | When | Network | Lobby features |
|---|---|---|---|
| DEV9 | an empty server host, or the extensions off | DEV9 adapter, as upstream | none |
| No DEV9 | the Z game with a server host (default) | `NoDev9.cpp` answers the net RPCs on the EE; TCP on host sockets, only to port 8200 (the lobby); the battle server is answered by the GGPO hook, any other battle connect fails | fast login; the lobby connection filter: platform info, battle info, match report, outdated notice (read off the code; the platform info tested); GGPO battle (1v1), the GGPO hook answering the battle sock first; TLS (lobby); line checks while paused (the lobby socket read on the paused idle poll). |
| Battle sock over GGPO | an armed GGPO battle, on either path | `GgpoBattle.cpp`: send, recv and poll answered from GGPO inputs | — |

A lobby feature added on one path is added to the other in the same change, or
this table says it is missing.

## Online play ([lobby.md](lobby.md))

- **Switch for all online battle extensions**: `ZDXSV_EXTENSIONS=0`.
- **Online without DEV9**: with the platform info, a TCP battle, a GGPO battle and TLS; every connection menu button connects with no network setting, also from a blank card; ネット終了 closes the lobby connection and returns to the main menu; on with a server host (`ZDXSV_SERVER_HOST`).
- **Fast login**: DNAS and the login browser skipped; the game waits at DNAS (VM paused) for the login API's answer; no answer or a server error keeps it held (OSD, retry 2-30 s backoff) until it answers or the player resumes (then, like no token or a refused login, back to the network menu: no DNAS). The game's reconnect after a failed battle connect (its login browser) logs in the same way once its earlier file load ends; a failed login leaves the game in its browser; `ZdxsvFastLogin`, `ZdxsvLoginApiUrl`.
- **Sign-in**: Settings → zdxsv → Account, or the fast login at DNAS with no token (VM paused), signs in through the server (Discord) and saves the token the fast login sends; `ZdxsvAuthUrl`. Signing in makes no zdxsv account: the first login, or one with an empty slot picked, makes it; Carry Over from PS2 links a PS2 account by its carry-over code. Account picks one of the Discord account's accounts (`ZdxsvAccount`); the server sets the limits.
- **Fast boot**: boot to the lobby user_id selection with no input; `ZdxsvFastBoot`, `ZdxsvFastBootState`, `ZdxsvFastBootLobby` (on through ID 1 into the lobby).
- **Platform info `0x9950` to the lobby**: `ZDXSV_PLATFORM_INFO`.
- **Battle over TCP to the battle server, as a PS2**: always.
- **Playing as a console**: seen as a real PS2; `ZDXSV_PLATFORM_INFO=0`.
- **A console behind a separate zproxy process**: `ZDXSV_PLATFORM_INFO=0`, server set by the lobby.
- **First login from a blank memory card**: always.
- **TLS to the zdxsv server**: lobby only, pinned keys; on when the client has pins.
- **Lobby connection kept while paused**: line checks `0x6001` answered.
- **Pause menu**: refused while connected to the zdxsv lobby.
- **PCSX2 features limited while online** (zdxsv lobby or net battle): PINE refused (`ZDXSV_PINE_ONLINE`); macro buttons (turbo included) not loaded; input recording, manual hardware renderer fixes, texture and draw data dumping off; `ZDXSV_SYNC_FORCE=0` turns the settings part off.
- **Operations refused while online** (OSD "Not available while online."): state saves and loads, disc changes, memory card swaps and setting changes, GS dumps, video capture, the software renderer toggle, upscale hotkeys, GS settings that recreate the renderer or change the upscale (held until offline), resets and VM shutdowns (quitting the app still works; in a net battle it asks first); the debugger window closes and does not open; debugger breakpoints and memory checks are cleared when the game goes online.
- **Settings page**: Settings → zdxsv.
- **First-run wizard**: zdxsv texts; a question before leaving the controller page on the keyboard; a zdxsv sign-in page in place of RetroAchievements; always.
- **Recommended-settings buttons**: Low, Mid, High on Settings → zdxsv.

## Input latency ([options.md](options.md))

- **VSync on by default**: `[EmuCore/GS] VsyncEnable`.
- **Low-latency present**: present before the limiter sleep; always on, no setting; `ZDXSV_LOW_LATENCY_PRESENT=0` turns it off.
- **Net battle pacing**: input sent before the limiter sleep, no limiter catch-up after a late frame, high process priority; `ZDXSV_EXTENSIONS=0`. See [rollback.md](rollback.md).
- **Input latency measurement**: `ZDXSV_INPUT_LATENCY`.
- **Input to picture by pixel color**: `ZDXSV_INPUT_LATENCY` key `pixel=`.

## Boot to lobby

- **Boot-to-lobby time measurement**: a test tool.

## Rollback netcode (GGPO) ([rollback.md](rollback.md), [lobby.md](lobby.md))

Keys (`net=1`, `lobby=1`, `port=`, `delay=`, ...) are parts of `ZDXSV_GGPO`.

- **GGPO library**: `3rdparty/ggpo`.
- **GGPO pings answered on the network thread**: `net=1`.
- **GGPO time sync spread over frames**: `net=1`.
- **Relay forwarding while GGPO is held**: `net=1`.
- **GGPO session check**: battle code in the sync handshake; `ZDXSV_NET_SESSION`.
- **Rollback budget in the lobby delay**: lobby battle without `delay=`.
- **Delta save states**: used by GGPO; `ZDXSV_DELTA_TEST`.
- **Faster rerun frames**: `ZDXSV_RERUN_*`.
- **Single draw list**: always on, no setting; `ZDXSV_SINGLE_DRAW_LIST=0` turns it off.
- **Battle start and MS select load barriers**: GGPO session.
- **In-game button settings in GGPO battles**: each player's own input record takes its action bits from that player's button settings in guest RAM; GGPO session.
- **GGPO synctest in a running game**: `ZDXSV_GGPO` without `net=1`.
- **Rerun keep**: the bone matrix slots and beam saber trails are not rolled back during battle play, and parts attached to a player are drawn in every rerun frame; `ZDXSV_RERUN_KEEP`, `ZDXSV_RERUN_PARTS`.
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
- **Z game patches built in**: the widescreen patch (Graphics → Display → Apply Widescreen Patches); no pnach file (patches, cheats, patches.zip) is read for the Z game; `ZDXSV_EXTENSIONS=0` turns it off.
- **No patches.zip, no warning**: releases ship no patches.zip; a missing one is skipped without the "Failed to open patches.zip" OSD (other games get no bundled patches); `ZDXSV_EXTENSIONS=0` turns it off.
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
- **Frame time OSD** (off by default): `ZdxsvFrameOsd`.
- **Teammate HP on the HUD** in the own max HP slot: `ZdxsvAllyHp`.
- **Team messages without a right stick**: a held button + D-pad or left stick also tilts the right stick, before GGPO and replays take the input: `ZdxsvStickMessageButton`.
- **Network OSD on disconnect**: held 10 s, shown while a peer is interrupted or gone.
- **UDP sockets ignore ICMP port unreachable on Windows**.

## Replays ([replay.md](replay.md))

- **Save a GGPO battle as a replay**: `ZdxsvSaveReplay`, `ZdxsvReplayPath`, `replay=`.
- **Upload a lobby battle's replay**: `ZdxsvReplayUpload`, `upload=`.
- **Play a replay**: `ZDXSV_REPLAY=file.pb`.
- **Replay that does not start**: error OSD.
- **Play a replay by URL or battle code**: `ZDXSV_REPLAY=http(s)://...`.
- **Common state**: a cached copy that does not load is downloaded again once.
- **Seek**: PageUp, PageDown, pad Left / Right, bar; `ZDXSV_REPLAY_SEEK`.
- **Frame step**: pad Left / Right or bar arrows while paused.
- **Playback speed**: pad Up / Down, bar arrows.
- **Point of view**: picked before the start; `ZDXSV_REPLAY_POV`, `ZDXSV_REPLAY_POV_AT`.
- **Control bar**: shown after the mouse moves or a pad control; no setting.
- **Key display**: End; `ZdxsvReplayKeyDisplay`.
- **Operations refused during a replay**: state saves and loads, disc changes.
- **Live spectating**: lobby `live_uplink=1`; `ZDXSV_REPLAY=live:...`.
- **Live round results**.
- **Live speed trim**: `ZDXSV_LIVE_PACING=0` turns it off.
- **Live wait without blocking**.
- **Replay window**: Tools → zdxsv Replays; `ZdxsvLobbyApiUrl`.
- **Return to the game after a replay**: `ZdxsvReplayReturn`.
- **Live auto-next**: `ZdxsvLiveAutoNext`, `ZDXSV_LIVE_NEXT`.
- **Live auto-next skip**: with auto-next.
- **Rounds only** (skip mobile suit selection, rematch screens): `ZdxsvReplaySkipMs`.
- **Round jump**: Shift+PageUp, Shift+PageDown, bar step buttons.
- **Round mobile suits**: hover the round results on the bar.
- **Takeover**: bar "Take over" / "Retry" / "Replay", hotkey "Take Over / Retry".
- **Four-screen replay**: `ZDXSV_REPLAY_FOUR=1`.

## Game window

- **Mouse wheel volume**: the wheel over the game screen changes the volume in 5% steps, OSD with the new volume; not while a menu takes the mouse or the wheel is bound to an input; `ZdxsvWheelVolume`.
- **Keyboard and hotkeys**: the host keyboard is a USB keyboard on USB port 1 (in-game chat; Settings > Controllers > USB Port 1, None turns it off); no upstream PCSX2 hotkeys are bound by default, the zdxsv replay ones stay (bind any under Settings > Hotkeys). Settings files from before get both once on start (`[Zdxsv] InputDefaultsVersion`).

## ARM64 builds ([arm64.md](arm64.md))

- **EE and IOP recompilers with the x86 timing**: `EnableEE`, `EnableIOP`.
- **VU1 trace recompiler**: `PCSX2_VU1_JIT=0` turns it off.
- **FPU and VU math as x86**.
- **EE FPU interpreter as the recompiler**: all builds.

## Releases and tools ([tools.md](tools.md))

- **Self-update from the GitHub releases of this fork**: release builds; `ZDXSV_UPDATE_URL`.
- **Crash reports** ([crash-reports.md](crash-reports.md)): `ZdxsvCrashReports`, on by default in a build with `ZDXSV_SENTRY_DSN`.
- **Release workflow**: a `zdxsv-X.Y.Z` tag.
- **Windows CI build**: every push and PR.
- **PINE opcodes `0x30` to `0x32`**: pad input, screenshot, frame count.
- **EE probe, watch and rerun profile**: `ZDXSV_EE_PROBE`, `ZDXSV_EE_WATCH`, `ZDXSV_EE_PROFILE`.
- **`run.py`**: several local instances side by side.
