# PCSX2 for zdxsv

A fork of [PCSX2](https://github.com/PCSX2/pcsx2) for playing Mobile Suit Gundam: Gundam vs. Zeta Gundam (PS2)
online on [zdxsv](https://github.com/inada-s/zdxsv).

- Players: see [ZDXSV.md](ZDXSV.md) for setup.
- Upstream base: PCSX2 `fd9d310c`. Main branch: `zdxsv-master`.
- License, BIOS requirement, and everything not listed here: same as upstream PCSX2.

## What this fork adds

One line per feature. Each feature and its tests are listed in
[docs/zdxsv/features.md](docs/zdxsv/features.md).

### Online play

- Network defaults point at the zdxsv server.
- Platform info: the emulator tells the lobby that it is an emulator, so the server can tell it from a real PS2.
- Lobby save states (opt-in, for debugging): save states made online keep working after a load.

### Input latency

- Low-latency vsync, on by default, for the Z game only: the finished frame is presented before the frame
  limiter sleeps (about -14 ms from button press to screen). Settings → Network & HDD → zdxsv.

### Rollback netcode (GGPO)

In development. On by default for lobby battles of the game (setting under Settings → Network & HDD); a battle
with a peer without GGPO is cut, as a connection failure (`docs/zdxsv/lobby.md`). Battles run over GGPO only.

- GGPO library in `3rdparty/ggpo`; its MIT notice is in `bin/docs/ThirdPartyLicenses.html`.
- Delta save states: fast enough to save the whole machine every frame.
- GGPO battles: the battle of the game runs over GGPO instead of the battle server.
- Lobby battles: peers, addresses and input delay come from the lobby and a ping test. Relay servers, a match
  report to the lobby, a P2P connectivity test and a network status OSD.
- A VM shutdown or reset during a GGPO battle ends it like a disconnect (replay saved, state dropped).
- The MTVU speedhack is turned off for a VM with GGPO or a replay: delta states copy VU1 memory, which the
  MTVU thread may still be writing. The setting itself is not changed.
- Delta states include the network adapter (DEV9): a rollback across a sent frame no longer stops the game's
  network after the battle. Frames received after the loaded state are received again.

### Replays

- A GGPO battle is saved to a protobuf `.pb` file (gdxsv's replay format plus the lobby's battle-start answers; no save
  state). Playback plays the battle start from those answers on a post-entry save state, as gdxsv's common start state
  (`docs/zdxsv/replay.md`): the hosted one at `[DEV9/Eth] ZdxsvReplayStateUrl` (default
  `https://storage.googleapis.com/zdxsv/misc/rbk-p1.p2s`, downloaded once; `ZDXSV_REPLAY_STATE=<url or path>` overrides, empty = the booted state). Older files with a frame 0 state (`start_state`, no lobby answers) are not played.
- Desync check (optional in the file): a per-frame state hash (player work + RNG) and the game RNGs at frame 0 and at
  each battle load end (for round skip). Playback logs `replay state hash differs at frame F` and a `replay state
  check` summary; `tests/zdxsv/replay_check.py` compares the hashes of one battle's files. The RNGs are checked on RNG B only (RNG A takes machine-local draws). Older files play unchecked.
- A lobby battle's `.pb` is posted (multipart, as flycast) to the replay uploader (zdxsv `infra/uploader`) at
  `[DEV9/Eth] ZdxsvReplayUploadUrl` in `PCSX2.ini` (empty = no upload; `ZDXSV_GGPO` `upload=URL` overrides it).
- An uploaded replay plays from the lobby's public API (as gdxsv lbsapi):
  `ZDXSV_REPLAY=http://<lobby api>/lbs/replay?battle_code=<code>`, or straight from a `.pb` URL.
- Playback with seek, point of view switch, a control bar, key display, a skip of the mobile suit selection, and a round jump.
  One file plays every position: `ZDXSV_REPLAY_POV=p` (0-based; default the recorder's) is picked before the start and
  the lobby answers name it as the own position. A switch (bar, hotkey, `ZDXSV_REPLAY_POV_AT=frame:p,...`) to a position
  not played yet runs its battle start from the common state first, then seeks.
- Takeover (as gdxsv): the bar's "Take over" button or the hotkey "Zdxsv Replay: Take Over / Retry" plays the shown
  position from the current frame with the host pad (input delay = `mindelay`, default 2). Hold the replay's input
  shown in the panel for 1 s, or press START to skip the matching. START while taken over retries from that frame,
  and the bar's "Replay" button goes back to the replay. Not while spectating live.
- Four-screen replay (as gdxsv): `ZDXSV_REPLAY_FOUR=1` with one `ZDXSV_REPLAY` file
  starts one more PCSX2 per other position, tiled 2x2 by position (Windows), all held on the same
  frame; the extra windows follow this one's pause, speed and seeks and close with it. Replays only, not live.
- Live spectating: the lobby picks one GGPO player per battle to stream it over UDP (lobby `live_uplink=1`);
  `ZDXSV_REPLAY=udp://<lobby host>:8201[/battle code]` watches it (`docs/zdxsv/replay.md`).
  The stream has no save state (spectators start from the hosted one) and runs whether saving (`replay=0`) or upload is on or not.
  Setting `[DEV9/Eth] ZdxsvLiveAutoNext` (off; Settings → Network & HDD) moves on to the next live battle when one ends
  (as gdxsv: the newest running battle not watched yet; the lobby picks it from the watched list the spectator sends).
  The spectator trims the frame limiter's period (a few ms per frame) to stay 30 frames behind the live edge without
  stalls (`VMManager::Internal::Throttle`, `Zdxsv::g_frame_period_trim_us`); `ZDXSV_LIVE_PACING=0` turns it off.

### Releases and tools

- The auto-updater follows the GitHub releases of this fork.
- A `zdxsv-X.Y.Z` tag builds and publishes a release.
- Scripts for local builds and for running several instances side by side.
- The test options (rollback test, traces, self-tests) work in every build, so a released build can be a peer
  in the rig tests.
- Everything above is on only for the one game build it was made for (serial `SLPS-25419`, ELF CRC
  `435D8236`); any other disc runs as in stock PCSX2. [docs/zdxsv/options.md](docs/zdxsv/options.md), Game check.
- Options for tests, traces and game investigation.

## Documentation

| Document | Content |
|---|---|
| [ZDXSV.md](ZDXSV.md) | Player setup. Shipped in the release zip. |
| [docs/zdxsv/features.md](docs/zdxsv/features.md) | Every feature with how to turn it on and its tests. |
| [docs/zdxsv/options.md](docs/zdxsv/options.md) | Reference of every option: name, default, use. |
| [docs/zdxsv/lobby.md](docs/zdxsv/lobby.md) | Lobby messages, ping test, relay servers, how a lobby battle starts. |
| [docs/zdxsv/rollback.md](docs/zdxsv/rollback.md) | GGPO library, delta save states, the GGPO session, sync checks. |
| [docs/zdxsv/replay.md](docs/zdxsv/replay.md) | Replay file format, playback and its controls. |
| [docs/zdxsv/tools.md](docs/zdxsv/tools.md) | Releases, CI, local build and run scripts, PINE commands. |
| [tests/zdxsv/README.md](tests/zdxsv/README.md) | Rig test scripts: setup and machine settings. |
| [AGENTS.md](AGENTS.md) | Rules for work in this fork. |
