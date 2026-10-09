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

### Input latency

- Low-latency vsync: the finished frame is presented before the frame limiter sleeps.

### Rollback netcode (GGPO)

- GGPO library in `3rdparty/ggpo`.
- Delta save states: the whole machine saved and loaded every frame.
- Lobby battles over GGPO (in development, on by default): ping test, relay servers, match report,
  connectivity and HTTPS latency tests, network status OSD.
- Load barriers: battle start and MS select wait for every peer's load (every peer needs a build with both).

### Replays

- GGPO battles saved as gdxsv-style `.pb` files, optionally uploaded to a replay server.
- Playback from a file, a URL or a battle code: seek, point of view, control bar, key display, mobile suit
  selection skip, round jump, takeover, four-screen view.
- Live spectating of lobby battles.

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
