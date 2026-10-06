# pcsx2/Zdxsv

zdxsv code of the emulator core. Rules: `AGENTS.md`, Fork Rules. Features and
options: `docs/zdxsv/`.

## Files

| File | Contents |
|---|---|
| `Ggpo.cpp`, `Ggpo.h` | GGPO rollback session, the game's battle socket emulated over GGPO, arming from the lobby, replay recording and playback, debug traces and the local rollback test harness |
| `DeltaState.cpp`, `DeltaState.h` | Fast per-frame save and load of the VM for rollback (copy-on-write EE RAM pages) |
| `Lobby.cpp`, `Lobby.h` | Lobby side of a GGPO battle: platform info, battle info notice, STUN, ping test, relay, match report |
| `InputLatency.cpp`, `InputLatency.h` | Debug: pad input latency measurement |
| `CpuHooks.h` | Hook header: recompilers, counters, MTGS, VMManager |
| `InputHooks.h` | Hook header: pad code |
| `SaveStateHooks.h` | Hook header: save state code |
| `Dev9Hooks.h` | Hook header: DEV9 network code |
| `MediaHooks.h` | Hook header: GS present, SPU2 output |
| `UiHooks.h` | Hook header: hotkeys, replay control bar |

Upstream files include only the hook headers, except `ImGuiOverlays.cpp` and `TCP_Session_In.cpp`,
which still hold zdxsv logic (overlays, lobby hook) and include `Ggpo.h` until that logic moves here.
