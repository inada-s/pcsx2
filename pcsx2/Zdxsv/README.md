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
