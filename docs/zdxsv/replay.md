# Replays

A GGPO battle can be saved to a file and played back later, as in gdxsv.
Option names and defaults are in [options.md](options.md).

## Saving

`replay=DIR` in `ZDXSV_GGPO` saves every `net=1` battle to `DIR`; lobby
battles save to `<data dir>/replays` without it.

- The file name is `<battle_code>.pb`, else `rbk-<start time>-p<position>.pb`.
- Only frames GGPO confirmed are written, so every peer's file of one battle
  holds the same inputs.
- The file is written when the session stops, also on a VM shutdown or reset.
- A lobby battle's file is posted (multipart, as flycast) to the replay
  uploader (zdxsv `infra/uploader`) at `ZdxsvReplayUploadUrl`.

## File format

A protobuf `BattleLogFile`: [`pcsx2/Zdxsv/replay.proto`](../../pcsx2/Zdxsv/replay.proto).
It keeps the field numbers of gdxsv's replay file and adds the PCSX2 fields.
The file holds no save state: the lobby's battle-start answers, the
battle-socket state at frame 0, the synced inputs of every position, and
the round results. Optional per-frame state hashes and the game RNGs at
each load end let playback detect a desync; a file without them plays
unchecked. `tests/zdxsv/replay_check.py` compares the files of one battle.

## Playing

`ZDXSV_REPLAY=<file.pb>` plays a replay; `http(s)://` downloads a `.pb`, or
the first battle of the lobby's public API
`http://<lobby api>/lbs/replay?battle_code=<code>` (as gdxsv lbsapi).

Common start (as gdxsv's slot 99): playback loads a save state of the game
at the post-entry point (logged in, before the lobby's battle start; any
user's), from `ZdxsvReplayStateUrl` (downloaded once into the cache
folder). The game then plays the battle start from the recorded lobby
answers, menus turbo, with the chosen point of view as the own position.
From GGPO frame 0 every frame gets the recorded inputs through the same
battle-socket emulation as a live battle, without GGPO.

- At the end the emulator pauses. A seek plays on; resuming without one
  ends the replay.
- A replay of a `ZDXSV_RBK` battle needs the recording's `ZDXSV_EE_CLAMP`.
- A VM reset plays the replay again from the start.

## Controls

| Action | Hotkey (default) | Control bar |
|---|---|---|
| Seek back / forward 10 s | PageUp / PageDown | yes |
| Seek to a frame | | timeline: click or drag, seeks on release |
| Play or pause | | yes |
| Switch point of view | Home | eye button |
| Toggle key display | End | |
| Previous / next round | Shift+PageUp / Shift+PageDown | step buttons around the round number (`R0` = the briefing) |
| Take over / retry | "Take Over / Retry" | "Take over", "Retry", "Replay" |

Hotkeys are under Settings > Hotkeys, group "Zdxsv Replay".

- **Seek**: a key (full state + battle-socket state) is kept every
  `ZDXSV_REPLAY_KEY` played frames, per position. A seek loads the newest
  key at or before the target and runs to it unthrottled. Forward seeks run
  every frame. Key files are deleted on a VM shutdown or reset.
- **Point of view**: one file plays every position. A switch loads that
  position's newest key at or before the current frame; a position not
  played yet first runs its own battle start from the common state.
- **Control bar**: play or pause, seek -10 s and +10 s, time, timeline,
  point of view, round steps, takeover. When it shows: setting
  `ZdxsvReplayBar`.
- **Key display** (as flycast `gdxsv:ReplayKeyDisplay`): the last 14 input
  changes of the shown position, each with the frames it was held.
- **Skip mobile suit selection** (as flycast
  `gdxsv:ReplaySkipMsSelection`): runs unthrottled from frame 0 to the
  briefing. A seek or a switch ends it.
- **Round jump**: a round starts when the game's tick state leaves the
  battle load (load 0 = mobile suit selection, 1 = briefing, 1 + N =
  round N). A jump to a round not reached yet runs unthrottled to it.
  Previous round goes to the round before the one shown.
- **Round results**: the bar shows each round's W / L / D for the shown
  position's team; playback logs a round whose result differs from the file.
- **Takeover** (as gdxsv): the shown position plays from the current frame
  with the host pad, input delay `mindelay`. Hold the replay's input shown
  in the panel for 1 s, or press START to skip the matching. START while
  taken over retries from the takeover frame and is not sent to the game;
  "Replay" goes back to the replay. Not while spectating live.
- **Four-screen** (as gdxsv, Windows, files only): `ZDXSV_REPLAY_FOUR=1`
  starts one more PCSX2 per other position, tiled 2x2, all held on the same
  frame; they follow this one's pause, speed and seeks and close with it.

## Live spectating

A lobby GGPO battle is streamed to the lobby while it runs, as gdxsv does
(zdxsv `pkg/lobby/spectator.go`).

- Uplink: the lobby marks one GGPO player per battle (`live_uplink=1` in the
  battle info). That client sends the replay header and every confirmed
  frame to the lobby's UDP address, then the close; whether saving or
  upload is on or not.
- Transfer: 1000-byte datagrams, go-back-N on the receiver's ack, both
  uplink to lobby and lobby to spectator.
- Spectator: `ZDXSV_REPLAY=udp://host:port[/battle code]` (no code = the
  newest live battle). It starts from the common state and plays as a
  replay from the uplink's point of view.
- Pacing (as flycast `gdxsv:LiveBufferFrames`): at the newest frame it waits
  until 30 more frames are there; far behind it runs unthrottled to catch
  up. In between the frame limiter's period is trimmed by a few ms per frame
  to stay 30 frames behind without stalls. Nothing from the lobby for 30 s =
  stream lost.
- Auto-next (as flycast `gdxsv:LiveAutoNext`, setting `ZdxsvLiveAutoNext`):
  at the end of a stream the spectator asks the lobby for its newest live
  battle not watched yet, resets the VM and watches it from its start.
