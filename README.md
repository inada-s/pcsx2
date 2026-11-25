# PCSX2 for zdxsv

A fork of [PCSX2](https://github.com/PCSX2/pcsx2) for playing Mobile Suit Gundam: Gundam vs. Zeta Gundam (PS2)
online on [zdxsv](https://github.com/inada-s/zdxsv).

- Players: see the [setup guide](https://pcsx2.zdxsv.net/) (Japanese).
- Upstream base: PCSX2 `fd9d310c`. Main branch: `zdxsv-master`.
- License, BIOS requirement, and everything not listed here: same as upstream PCSX2.

## What this fork adds

One line per feature; details in the documents below, how to turn each on in
[docs/zdxsv/features.md](docs/zdxsv/features.md), tests in [tests/zdxsv/README.md](tests/zdxsv/README.md).
Everything is on only for the one game build it was made for; any other disc runs as in stock PCSX2.

### Online play

- Network defaults that point at the zdxsv server.
- Platform info: the lobby can tell an emulator from a real PS2.
- Fast login through the zdxsv login API (DNAS skipped), and fast boot: from game start to the lobby ID selection with no input.
- Settings page Settings → zdxsv (global or per game).
- Crash reports to the zdxsv developers, on by default, with a setting to turn them off.
- Sync-safe settings forced while the game runs.

### Input latency

- Low-latency present: the finished frame is presented before the frame limiter sleeps.
- Single draw list (Z game): a frame's draw list drawn in that frame, one frame less input to picture.

### Rollback netcode (GGPO)

- GGPO library in `3rdparty/ggpo`.
- Delta save states: the whole machine saved and loaded every frame.
- Faster rerun frames.
- Lobby battles over GGPO (on by default): ping test, relay servers, match report, connectivity and HTTPS latency tests, network status OSD.
- Battle start and MS select load barriers.
- Round-end barrier.
- GGPO network thread: packets read, relayed and pings answered as they arrive.
- Pause, speed changes, frame advance and state loads refused during a net battle.

### Replays

- GGPO battles saved as `.pb` files, optionally uploaded to a replay server.
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
| [guide/](guide/) | Player setup guide in Japanese with screenshots, published to [pcsx2.zdxsv.net](https://pcsx2.zdxsv.net/). |
| [docs/zdxsv/architecture.md](docs/zdxsv/architecture.md) | Start here: processes and threads and the state each touches, a battle frame, trust boundaries, kinds of failure and what the player sees. |
| [docs/zdxsv/features.md](docs/zdxsv/features.md) | Structure: where zdxsv code meets the emulator, the network paths. Every feature, one line each. |
| [docs/zdxsv/options.md](docs/zdxsv/options.md) | Every option and setting: name, default, use; the game check. |
| [docs/zdxsv/lobby.md](docs/zdxsv/lobby.md) | Network defaults, lobby messages, ping test, relay servers, how a lobby battle starts. |
| [docs/zdxsv/rollback.md](docs/zdxsv/rollback.md) | GGPO library, delta save states, the GGPO session, sync-safe settings, sync checks. |
| [docs/zdxsv/replay.md](docs/zdxsv/replay.md) | Replay files, upload, playback and its controls, live spectating. |
| [docs/zdxsv/desync.md](docs/zdxsv/desync.md) | How to investigate a desync, step by step. |
| [docs/zdxsv/tools.md](docs/zdxsv/tools.md) | Releases, CI, local build and run scripts, PINE commands. |
| [docs/zdxsv/maintenance.md](docs/zdxsv/maintenance.md) | Procedures: merging upstream PCSX2, cutting a release, rolling one back. |
| [docs/zdxsv/crash-reports.md](docs/zdxsv/crash-reports.md) | Crash reports: the switch, what is sent, behavior per platform, symbols. |
| [docs/zdxsv/arm64.md](docs/zdxsv/arm64.md) | ARM64 builds: CPU cores, math as x86, comparing with an x86_64 build. |
| [docs/zdxsv/upstream.md](docs/zdxsv/upstream.md) | Changes to upstream PCSX2 code: the fork point, added and changed files, every changed spot. |
| [pcsx2/Zdxsv/README.md](pcsx2/Zdxsv/README.md) | Glossary and source file map. |
| [tests/zdxsv/README.md](tests/zdxsv/README.md) | Rig test scripts: setup, settings, the test of each feature. |
| [AGENTS.md](AGENTS.md) | Rules for work in this fork. |
