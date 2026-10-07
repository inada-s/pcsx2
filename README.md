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

- Low-latency vsync, on by default: the finished frame is presented before the frame limiter sleeps
  (about -14 ms from button press to screen).

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

- A GGPO battle is saved to a `.zdxr` file.
- Playback with seek, point of view switch, a control bar, key display, a skip of the mobile suit selection, and a round jump.

### Releases and tools

- The auto-updater follows the GitHub releases of this fork.
- A `zdxsv-X.Y.Z` tag builds and publishes a release.
- Scripts for local builds and for running several instances side by side.
- Test options that can be abused (inputs, game memory, other players' battles, the updater URL, file paths)
  work only in a test build (`build-local.sh --test-options`, CMake `-DZDXSV_TEST_OPTIONS=ON`); releases leave
  it off. List: [docs/zdxsv/options.md](docs/zdxsv/options.md), Test build.
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
