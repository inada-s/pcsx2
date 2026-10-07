# pcsx2/Zdxsv

zdxsv code of the emulator core. Rules: `AGENTS.md`, Fork Rules. Features and
options: `docs/zdxsv/`.

## Files

| File | Contents |
|---|---|
| `Ggpo.cpp`, `Ggpo.h` | GGPO rollback session, the game's battle socket emulated over GGPO, arming from the lobby, replay recording and playback, debug traces and the local rollback test harness |
| `DeltaState.cpp`, `DeltaState.h` | Fast per-frame save and load of the VM for rollback (copy-on-write EE RAM pages) |
| `Lobby.cpp`, `Lobby.h` | Lobby side of a GGPO battle: platform info, battle info notice, STUN, ping test, relay, match report |
| `LobbyConnection.cpp` | A DEV9 TCP connection to the lobby: platform info message, GGPO lobby setup, lobby filter |
| `InputLatency.cpp`, `InputLatency.h` | Debug: pad input latency measurement |
| `RecHooks.cpp`, `RecHooks.h` | EE recompiler: emits the hook calls (net RPC, recv, step copy, load step) before an instruction |
| `RecProbe.cpp` | Debug: EE recompiler probes and store watches (`ZDXSV_EE_PROBE`, `ZDXSV_EE_WATCH`) |
| `CpuHooks.h` | Hook header: recompilers, counters, MTGS, VMManager |
| `InputHooks.h` | Hook header: pad code |
| `SaveStateHooks.h` | Hook header: save state code |
| `Dev9Hooks.h` | Hook header: DEV9 network code |
| `MediaHooks.h` | Hook header: GS present, SPU2 output |
| `UiHooks.h` | Hook header: hotkeys, replay control bar |
| `TestOptions.h` | `TestEnv()`: environment options meant for tests and diagnostics; every build reads them |

Upstream files include only the hook headers, except `ImGuiOverlays.cpp`, which still holds zdxsv
logic (overlays) and includes `Ggpo.h` until that logic moves here, and `TCP_Session_In.cpp`, which
calls the lobby filter (`Zdxsv/Lobby.h`) directly.
