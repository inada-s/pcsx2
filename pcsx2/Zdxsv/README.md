# pcsx2/Zdxsv

zdxsv code of the emulator core. Rules: `AGENTS.md`, Fork Rules. Features and
options: `docs/zdxsv/`.

## Glossary

| Term | Meaning |
|---|---|
| Z game | Mobile Suit Gundam: Gundam vs. Zeta Gundam, serial `SLPS-25419`, the one game build the hooks are written for |
| frame | One vsync. A GGPO frame counts from the session start, a vsync frame (`g_FrameCount`) from boot |
| delta state | Per-frame VM save/load for rollback: EE RAM pages copy-on-write, the rest copied whole, no GS thread state |
| rollback, rerun | GGPO loads an older frame's delta state and runs the frames after it again with corrected inputs; such a frame is a rerun |
| synctest | GGPO mode on one machine: every `check` frames it rolls back, reruns with the same inputs and compares checksums |
| battle position | The player slot 0..3 in a battle. GGPO player = battle position + 1 |
| own, remote | The battle position this machine plays; the other positions |
| net RPC | The game's network call from the EE to the IOP. `fno` = its function number (send, recv, poll, connect, ...) |
| battle sock | The game's socket 0, its connection to the battle server; with GGPO its send, recv and poll are answered on the EE side |
| net HLE | That emulation of the battle sock over GGPO: no packet reaches the IOP or the battle server |
| msg, kind | One McsMessage of the game: byte 0 = length, byte 1 = kind << 4 \| sender |
| key msg | Kind 2: the msg that carries a player's input; the first one on the battle sock arms GGPO |
| K3 | Kind 3 msg, the round handshake. A barrier: the n-th K3 of every remote reaches the game once all peers' n-th is in the synced input |
| (A, B) | The two 16-bit words of the game's input record of one position |
| lockstep step | The game's per-frame routine that reads every position's (A, B) from its ring |
| zd, zds | zd: the hook at the lockstep step's ring read that feeds it the GGPO input (`OnStepCopy`). zds: the msg sync around it (K3 barrier, echo of own msgs) |
| ps, load step | Play start: the battle load step waits until every peer finished loading (`OnLoadStep`) |
| player work | The game's per-player struct, `0x2200` bytes each from `0x8395d8`; synctest `hash=pw` hashes it masked |
| H line | `H frame h0 h1 h2 h3 rng` in `ZDXSV_NET_TRACE` (`ZDXSV_PW_HASH`): per frame, a hash of each player's x, y, z and the game RNG |
| arm | GGPO takes over the battle: from the first key msg of a battle |
| delay | GGPO frame delay of the local input |
| platform info | `key=value` lines the client sends to the lobby: GGPO port (`ggpo=`), UDP addresses, NAT type |
| battle info | The lobby's notice before a battle: players and their GGPO addresses |
| ping test | UDP round trips to every peer on the GGPO port before arming; picks the delay, a peer that never answers cuts the battle |
| relay | A relay server of the battle (from the battle info) that forwards GGPO packets between peers that cannot reach each other |
| cut | A lobby battle that cannot run over GGPO: the battle sock stays silent and the game's timeout returns to the lobby (`LobbyCutCall`) |
| `.pb` replay | Replay file (`replay.proto`): frame 0 state + every input of a battle |
| replay key | Full state (`.p2s`) + HLE state saved every `ZDXSV_REPLAY_KEY` frames while a replay plays; a seek loads the newest key at or before the target |
| rbk | Local rollback test: N clients started from a saved state after entry (`ZDXSV_RBK`, `tests/zdxsv/rbk.sh`) |

## Files

| File | Contents |
|---|---|
| `Ggpo.cpp`, `Ggpo.h` | GGPO rollback session, the game's battle socket emulated over GGPO, arming from the lobby, replay recording and playback, debug traces and the local rollback test harness |
| `DeltaState.cpp`, `DeltaState.h` | Fast per-frame save and load of the VM for rollback (copy-on-write EE RAM pages) |
| `DeltaFreeze.cpp`, `DeltaFreeze.h` | The non-EE-RAM part of a delta state: CPU, IOP, VU, SPU2, DEV9, pad; section timing and offset names for reports |
| `Lobby.cpp`, `Lobby.h` | Lobby side of a GGPO battle: platform info, battle info notice, STUN, ping test, relay, match report |
| `Proto.h` | Minimal protobuf wire codec: the lobby's Ping / Pong, replay files |
| `replay.proto` | Replay file schema (gdxsv's BattleLogFile + PCSX2 fields); documentation, not compiled |
| `LobbyConnection.cpp` | A DEV9 TCP connection to the lobby: platform info message, GGPO lobby setup, lobby filter |
| `ServerHosts.cpp` | The game's server hosts, looked up as the zdxsv server's hostname by the internal DNS server |
| `InputLatency.cpp`, `InputLatency.h` | Debug: pad input latency measurement |
| `RecHooks.cpp`, `RecHooks.h` | EE recompiler: emits the hook calls (net RPC, recv, step copy, load step) before an instruction |
| `RecProbe.cpp` | Debug: EE recompiler probes and store watches (`ZDXSV_EE_PROBE`, `ZDXSV_EE_WATCH`) |
| `Overlays.cpp` | On-screen GGPO network status, replay keys and replay control bar |
| `CpuHooks.h` | Hook header: recompilers, counters, MTGS, VMManager |
| `InputHooks.h` | Hook header: pad code |
| `SaveStateHooks.h` | Hook header: save state code |
| `Dev9Hooks.h` | Hook header: DEV9 network code |
| `MediaHooks.h` | Hook header: GS present, SPU2 output |
| `UiHooks.h` | Hook header: hotkeys, replay control bar |
| `TestOptions.h` | `TestEnv()`: environment options meant for tests and diagnostics; every build reads them |
