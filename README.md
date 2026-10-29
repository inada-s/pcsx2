# PCSX2 for zdxsv

A fork of [PCSX2](https://github.com/PCSX2/pcsx2) for playing Mobile Suit Gundam: Gundam vs. Zeta Gundam (PS2)
online on [zdxsv](https://github.com/inada-s/zdxsv).

- Players: see [ZDXSV.md](ZDXSV.md) for setup.
- Upstream base: PCSX2 `fd9d310c`. Main branch: `zdxsv-master`.
- License, BIOS requirement, and everything not listed here: same as upstream PCSX2.

## What this fork adds

One line per feature; details in the documents below, how to turn each on and its tests in
[docs/zdxsv/features.md](docs/zdxsv/features.md).
Everything is on only for the one game build it was made for; any other disc runs as in stock PCSX2.

### Online play

- Network defaults that point at the zdxsv server.
- Platform info: the lobby can tell an emulator from a real PS2.
- Lobby save states (opt-in, for debugging).
- Settings page Settings → zdxsv (global or per game).
- Sync-safe settings forced while the game runs, and a sync fingerprint that cuts a battle between differing builds.

### Input latency

- Low-latency vsync: the finished frame is presented before the frame limiter sleeps.

### Rollback netcode (GGPO)

- GGPO library in `3rdparty/ggpo`.
- Delta save states: the whole machine saved and loaded every frame.
- Faster rerun frames.
- Lobby battles over GGPO (on by default): ping test, relay servers, match report, connectivity and HTTPS latency tests, network status OSD.
- Battle start and MS select load barriers.
- GGPO network pump thread: sockets read and written every 1 ms.
- Pause, speed changes, frame advance and state loads refused during a net battle.

### Replays

- GGPO battles saved as gdxsv-style `.pb` files, optionally uploaded to a replay server.
- Playback from a file, a URL or a battle code, with seek, point of view, control bar, key display, round jump, takeover and four-screen view.
- Live spectating of lobby battles.
- Replay window (Tools → zdxsv Replays).

### ARM64 (Apple M)

- EE and IOP recompilers with the x86 recompilers' timing.
- VU1 trace recompiler.
- EE FPU and VU math as on x86_64, so ARM64 and x86_64 peers stay in sync.

### Releases and tools

- Auto-updater and release workflow for this fork.
- Scripts for local builds and for running several instances side by side.
- macOS deps scripts that build on Xcode 26.6.
- Options for tests, traces and game investigation, including VU1 and EE FPU/COP2 op tests.

## Documentation

| Document | Content |
|---|---|
| [ZDXSV.md](ZDXSV.md) | Player setup. Shipped in the release zip. |
| [docs/zdxsv/features.md](docs/zdxsv/features.md) | Every feature with how to turn it on and its tests. |
| [docs/zdxsv/options.md](docs/zdxsv/options.md) | Every option and setting: name, default, use; the game check. |
| [docs/zdxsv/lobby.md](docs/zdxsv/lobby.md) | Network defaults, lobby messages, ping test, relay servers, how a lobby battle starts. |
| [docs/zdxsv/rollback.md](docs/zdxsv/rollback.md) | GGPO library, delta save states, the GGPO session, sync-safe settings and fingerprint, sync checks. |
| [docs/zdxsv/replay.md](docs/zdxsv/replay.md) | Replay files, upload, playback and its controls, live spectating. |
| [docs/zdxsv/tools.md](docs/zdxsv/tools.md) | Releases, CI, local build and run scripts, PINE commands. |
| [docs/zdxsv/arm64.md](docs/zdxsv/arm64.md) | ARM64 builds: CPU cores, math as x86, comparing with an x86_64 build. |
| [pcsx2/Zdxsv/README.md](pcsx2/Zdxsv/README.md) | Glossary and source file map. |
| [tests/zdxsv/README.md](tests/zdxsv/README.md) | Rig test scripts: setup and machine settings. |
| [AGENTS.md](AGENTS.md) | Rules for work in this fork. |
