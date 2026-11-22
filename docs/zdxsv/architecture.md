# Architecture

How the zdxsv parts fit together, for a reader new to the code. Where zdxsv
code meets the emulator: [features.md](features.md) (Structure). Details of each
part: the document linked from each row.

## Processes and threads

```
 PCSX2 process (player)
 +-----------------------------------------------------------------------+
 | Qt UI thread: settings, setup wizard, replay window, updater          |
 |    |                                                                  |
 |    | Host::RunOnCPUThread                                             |
 |    v                                                                  |
 | CPU thread: EE / IOP / VU, zdxsv hooks, GGPO frame loop,              |
 |   No DEV9 net RPCs, lobby protocol, replay record and playback        |
 |    |  ^          |  ^               |  ^                              |
 |    |  |          |  | _net_mutex    |  | queues, locks, atomics       |
 |    v  |          v  |               v  |                              |
 | MTGS / GS     GGPO net thread    worker threads: TLS tunnel,          |
 | thread        (UDP sockets)      live up/down, ping test, connection  |
 |                                  test, sign-in, HTTP posts, seek zip  |
 +-----------------------------------------------------------------------+
        | -zdxsvreplay pov:parent pid:source  (command line)
        v
 PCSX2 replay child process (-nogui), one per replay window;
 four-screen: one child per position, kept on one frame through
 a file-backed shared memory group (SpectateSync)
```

| Thread | Runs | State it touches | Doc |
|---|---|---|---|
| CPU thread | the VM; every hook in `*Hooks.h`; at each vsync of a battle the GGPO frame loop (below); No DEV9 answers to the game's net RPCs; lobby messages; replay record and playback | all VM state; the GGPO session and battle state (`GgpoShared.h`); delta states; the replay being recorded or played | [rollback.md](rollback.md), [lobby.md](lobby.md) |
| MTGS / GS | GS rendering, present, ImGui OSD | GS state and GS memory (never rolled back); other threads reach it only through `MTGS::RunOnGSThread` | [rollback.md](rollback.md) |
| Qt UI | windows and dialogs | settings; starts replay children; asks the CPU thread to act through `Host::RunOnCPUThread` | [replay.md](replay.md) |
| GGPO net | `select` on the GGPO UDP sockets, delayed sends | the receive and send queues, under `Udp::_net_mutex`; the CPU thread drains the receive queue in `ggpo_idle` | [rollback.md](rollback.md) |
| TLS tunnel | one per lobby connection: a loopback socket the game's TCP goes to, TLS to the server | its two sockets | [lobby.md](lobby.md) |
| Server name resolve | once at VM start (No DEV9) | an atomic server address | [lobby.md](lobby.md) |
| Ping test, relay ping | before a lobby battle | its own UDP sockets; the ping view under its lock | [lobby.md](lobby.md) |
| Connection test | P2P test of the GGPO port (STUN, UPnP) | its own socket; the result through a callback | [lobby.md](lobby.md) |
| Sign-in | loopback HTTP listener for the browser's return, then a POST | fast login state under `s_fast.mutex`; pauses the VM through the CPU thread | [options.md](options.md) |
| Fast login, replay upload, HTTPS latency | one HTTP request each | a copy of their inputs; results under a lock | [lobby.md](lobby.md), [replay.md](replay.md) |
| Live uplink / downlink | stream a running battle to or from the lobby API | the record queue of their `State`; the CPU thread appends or reads | [replay.md](replay.md) |
| Live auto-next | polls the lobby API for the next battle to watch | `LiveWait` | [replay.md](replay.md) |
| Seek key zip | writes a playback seek key to the cache folder | the `PlayKey` it zips | [replay.md](replay.md) |
| Four-screen watch | keeps the replay children of a group on one frame | the shared memory group | [replay.md](replay.md) |

## A battle frame

One vsync of a GGPO battle on the CPU thread (`Ggpo.cpp`, `GgpoSession.cpp`):

```
 vsync
   |
   +- present the frame drawn last
   +- poll the pad, add the local input, send it to the peers
   +- ggpo_advance_frame: save the delta state of the next frame
   +- ggpo_idle: take received inputs; on a misprediction, a rollback:
   |     load the delta state of the first wrong frame,
   |     rerun up to now (not throttled, shown or heard),
   |     save a delta state per rerun frame
   +- wait while at the prediction limit
   +- limiter sleep
   +- ggpo_synchronize_input: the synced inputs of the next frame
   +- run the EE to the next vsync; the game reads the synced inputs
      through its battle socket (answered by GgpoBattle.cpp)
```

Order and pacing: [rollback.md](rollback.md) (Frame order and pacing). Guest
code skipped in rerun frames and why: [rollback.md](rollback.md) (GGPO session
in a running game). Delta states: [rollback.md](rollback.md) (Delta save states).

## Trust boundaries

Data from outside the process. The rule for all of it (`AGENTS.md`): check
sizes, counts and ranges; bound every wait.

| Input | Who controls it | Enters at |
|---|---|---|
| Lobby TCP messages, incl. battle info `0x9951` and game patches `0x9953` (written in EE RAM) | the lobby server | `TlsTunnel.cpp`, `LobbyConnection.cpp`, `Lobby.cpp`, `LobbyBattleInfo.cpp`, `ServerPatches.cpp` |
| Lobby HTTP API answers (JSON) | the lobby API server | `FastLogin.cpp`, `AuthSignIn.cpp`, `ReplayList.cpp`, `ReplayLoad.cpp`, `LobbyLive.cpp` |
| Peer UDP: GGPO packets, ping and relay packets, STUN answers | any host that can reach the port | `3rdparty/ggpo` (`network/udp.cpp`), `GgpoSession.cpp`, `GgpoBattle.cpp`, `LobbyPing.cpp`, `Lobby.cpp` |
| Replay files and replay URLs | the file's author; the URL from the lobby, Favorites or Browse... | `ReplayLoad.cpp`, `replay.proto` |
| Live streams | the lobby API server | `LobbyLive.cpp`, `ReplayLive.cpp` |
| Save state files | the file's author | upstream loader; `DeltaState.cpp` and `NoDev9.cpp` entries; refused while online |
| Update feed and its assets | the GitHub releases of this fork | `pcsx2-qt/AutoUpdaterDialog.cpp` |
| Font download | the font URL | `pcsx2-qt/Zdxsv/QtFonts.cpp` |
| Browser return to the sign-in listener | local processes (loopback only) | `AuthSignIn.cpp` |
| PINE | local processes (loopback only); refused while online | `PINE.cpp`, `OnlineLimits.cpp` |
| Replay child command line, four-screen shared memory | the parent process; local processes | `pcsx2-qt/Zdxsv/QtReplayChild.cpp`, `SpectateSync.cpp` |
| Environment variables | the user who starts PCSX2; tests and diagnostics only | `TestOptions.h` (`TestEnv`), listed in [options.md](options.md) |

## Kinds of failure

| Failure | What the player sees | Where |
|---|---|---|
| Login server not answering | OSD: not answering, retrying; resuming the game goes back to the network menu | `FastLogin.cpp` |
| Login refused or failed | OSD `zdxsv login failed: ...` | `FastLogin.cpp` |
| Client too old for the lobby | OSD: the lobby refused this PCSX2 as outdated | `LobbyConnection.cpp` |
| Battle info incomplete; opponent without an address | OSD `No battle: ...`; back to the lobby | `GgpoLobby.cpp` |
| Opponent not answering the ping test; GGPO port in use | OSD `No battle: ...` (router and firewall, or port in use); back to the lobby | `GgpoLobby.cpp`, [lobby.md](lobby.md) (Connection cut) |
| GGPO start timeout or session mismatch | the game's connection error | [rollback.md](rollback.md) (Battle) |
| Opponent interrupted or gone during a battle | the network status panel names them (also with the OSD off); after a disconnect, the game's connection error at once | [lobby.md](lobby.md) (Network status OSD), [rollback.md](rollback.md) (Battle) |
| Load barrier held past its limit; rollback state load failed | the battle ends as a connection failure | [rollback.md](rollback.md) |
| Desync (the peers' states differ) | nothing at once: the battle goes on; logged, in the match report and the replay | [rollback.md](rollback.md) (Online desync check), [desync.md](desync.md) |
| Operation not allowed online or in a replay (save state, pause, speed, ...) | OSD naming the refused operation | `OnlineLimits.cpp`, `FeatureLimits.cpp` |
| Replay cannot start | OSD with the reason; a replay child exits | `ReplayLoad.cpp` |
| Sync-safe settings overridden | OSD naming the settings | [rollback.md](rollback.md) (Sync-safe settings) |
| Crash | a crash report, if on | [crash-reports.md](crash-reports.md) |
