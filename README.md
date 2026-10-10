# PCSX2 for zdxsv

A fork of [PCSX2](https://github.com/PCSX2/pcsx2) for playing Mobile Suit Gundam: Gundam vs. Zeta Gundam (PS2)
online on [zdxsv](https://github.com/inada-s/zdxsv).

- Players: see [ZDXSV.md](ZDXSV.md) for setup.
- Upstream base: PCSX2 `fd9d310c`. Main branch: `zdxsv-master`.
- License, BIOS requirement, and everything not listed here: same as upstream PCSX2.

## What this fork adds

One line per feature. How to turn each on and its tests: [docs/zdxsv/features.md](docs/zdxsv/features.md).
Everything is on only for the one game build it was made for; any other disc runs as in stock PCSX2.

### Online play

- Network defaults that point at the zdxsv server.
- Platform info: the lobby can tell an emulator from a real PS2.
- Lobby save states (opt-in, for debugging): save states made online keep working after a load.
- Settings page Settings → zdxsv (global or per game).
- Sync-safe settings while the game runs, as in RetroAchievements hardcore mode: cheats off; recompilers,
  EE/VU rounding and clamping, game fixes (GameDB only), extra memory, GS hardware download mode at
  their defaults; speedhacks, emulation speed and frame rate at their defaults during battles, replays
  and live spectating. An OSD message names the overridden settings; the settings page greys them out.
- Sync fingerprint (build + those settings, not the BIOS) in the platform info: a lobby battle whose
  players' fingerprints differ is cut, and the players return to the lobby.

### Input latency

- Low-latency vsync: the finished frame is presented before the frame limiter sleeps.
  In a GGPO session the rollback also runs before the sleep, so it does not delay the next present
  (`ZDXSV_PRESENT_FIRST=0`: the old order, docs/zdxsv/options.md).

### Rollback netcode (GGPO)

- GGPO library in `3rdparty/ggpo`.
- Delta save states: the whole machine saved and loaded every frame.
- Faster rerun frames: no reverb, VU1 only in the last, the game's render callbacks skipped but in the last two
  (controls `ZDXSV_RERUN_REVERB`, `ZDXSV_RERUN_VU1`, `ZDXSV_RERUN_EE_DRAW`, `ZDXSV_RERUN_TAIL`,
  docs/zdxsv/options.md; A/B them with `tests/zdxsv/benchab.sh`, check the picture after rollbacks
  with `tests/zdxsv/rerunpic.sh`).
- GS snapshots (`ZDXSV_SNAP`, screenshots) are taken of shown frames only, never of a rollback rerun frame.
- Lobby battles over GGPO (in development, on by default): ping test, relay servers, match report,
  connectivity and HTTPS latency tests, network status OSD.
- Load barriers: battle start and MS select wait for every peer's load (every peer needs a build with both).
- GGPO network pump thread: sockets are read and written every 1 ms, not once per frame, as flycast
  (`ZDXSV_NET_PUMP=0`: off, docs/zdxsv/options.md).

### Replays

- GGPO battles saved as gdxsv-style `.pb` files, optionally uploaded to a replay server.
- Playback from a file, a URL or a battle code: seek, point of view, control bar, key display, mobile suit
  selection skip, round jump, takeover, four-screen view.
- Live spectating of lobby battles.
- Replay window (Tools → zdxsv Replays): local replay files, the lobby's replay search and its live battles; Play
  boots the game from the game list with the chosen replay and point of view.

### Releases and tools

- Auto-updater and release workflow for this fork.
- Scripts for local builds and for running several instances side by side.
- Options for tests, traces and game investigation; every build reads them.

## Documentation

| Document | Content |
|---|---|
| [ZDXSV.md](ZDXSV.md) | Player setup. Shipped in the release zip. |
| [docs/zdxsv/features.md](docs/zdxsv/features.md) | Every feature with how to turn it on and its tests. |
| [docs/zdxsv/options.md](docs/zdxsv/options.md) | Every option and setting: name, default, use; the game check. |
| [docs/zdxsv/lobby.md](docs/zdxsv/lobby.md) | Network defaults, lobby messages, ping test, relay servers, how a lobby battle starts. |
| [docs/zdxsv/rollback.md](docs/zdxsv/rollback.md) | GGPO library, delta save states, the GGPO session, sync checks. |
| [docs/zdxsv/replay.md](docs/zdxsv/replay.md) | Replay files, upload, playback and its controls, live spectating. |
| [docs/zdxsv/tools.md](docs/zdxsv/tools.md) | Releases, CI, local build and run scripts, PINE commands. |
| [pcsx2/Zdxsv/README.md](pcsx2/Zdxsv/README.md) | Glossary and source file map. |
| [tests/zdxsv/README.md](tests/zdxsv/README.md) | Rig test scripts: setup and machine settings. |
| [AGENTS.md](AGENTS.md) | Rules for work in this fork. |
