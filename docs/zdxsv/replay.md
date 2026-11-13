# Replays

A GGPO battle can be saved to a file and played back later.
Option names and defaults are in [options.md](options.md).

## Saving

`replay=DIR` in `ZDXSV_GGPO` saves every `net=1` battle to `DIR`; lobby
battles save to the replay folder without it (settings `ZdxsvSaveReplay`,
`ZdxsvReplayPath`; a folder that cannot be created falls back to
`<data dir>/replays`).

- The file name is `<battle_code>.pb`, else `rbk-<start time>-p<position>.pb`.
- Only frames GGPO confirmed are written, so every peer's file of one battle
  holds the same inputs.
- The file is written when the session stops, also on a VM shutdown or reset.
- A lobby battle's file is posted (multipart field `file`) to the
  lobby's upload API (zdxsv `/lbs/replay_upload`) at
  `ZdxsvReplayUploadUrl` when `ZdxsvReplayUpload` is on (default),
  with field `used_ms`: JSON, one array per round of the file's
  `round_data` `used_ms` (e.g. `[[3,7,0,12],[3,9,0,12]]`), which the
  lobby keeps for the replay search.

## File format

A protobuf `BattleLogFile`: [`pcsx2/Zdxsv/replay.proto`](../../pcsx2/Zdxsv/replay.proto).
Its field numbers are the ones the replay server reads, plus the PCSX2 fields.
The file holds no save state: the lobby's battle-start answers, the
battle-socket state at frame 0, the synced inputs of every position, and
the round results. Optional per-frame state hashes and the game RNGs at
each load end let playback detect a desync; a file without them plays
unchecked. `tests/zdxsv/replay_check.py` compares the files of one battle.
The battle-load step's frame is machine-local: when playback reaches it after
the file's play-start release, it releases N frames later and draws RNG B
N frames later. Those frames are logged as `own load late by N`, not as a
desync.
The game's MS asset slot cache holds the MSes a machine loaded before and
sets a hashed player-work byte at each battle load. Every GGPO session and
every common start empties it at frame 0 (the game reloads each battle MS),
so recorder and playback start from the same cache. A file recorded by a
build that did not empty it can differ in state hashes from a round start
on, while positions and RNGs match.

## Playing

`ZDXSV_REPLAY=<file.pb>` plays a replay; `http(s)://` downloads a `.pb`, or
the first battle of the lobby's public API
`http://<lobby api>/lbs/replay?battle_code=<code>`. A download
that starts with the gzip magic (the lobby's bucket serves them gzip encoded) is
decompressed first.

Common start: playback loads a save state of the game
at the post-entry point (logged in, before the lobby's battle start; any
user's), from `ZdxsvReplayStateUrl` (downloaded once into the cache
folder, one file per URL: a new state needs a new URL; a cached copy that
does not load is downloaded again once). The game then plays the battle start from the recorded lobby
answers, menus turbo, with the chosen point of view as the own position.
From GGPO frame 0 every frame gets the recorded inputs through the same
battle-socket emulation as a live battle, without GGPO.

- At the end (with rounds only on: the end of the last round) a replay
  started from the replay window with Return on goes back to it at once.
  Otherwise the emulator pauses: a seek plays on, Play starts again from
  the first round, resuming any other way ends the replay.
- While a replay plays, state saves and loads and disc changes are refused
  from every entry point (hotkeys, menus, Big Picture, PINE), with the OSD
  "Not available during a replay."; the replay's own states (seek keys,
  takeover) still load.
- A replay of a `ZDXSV_RBK` battle needs the recording's `ZDXSV_EE_CLAMP`.
- A VM reset plays the replay again from the start.
- A file that cannot be read or played (download failed, corrupted, an
  older format) or a common state that cannot be loaded: an error OSD
  message; the game boots to its title screen.

## Replay window

Tools → zdxsv Replays:

- **Local**: the `.pb` files in the replay folder (date, battle code,
  players, rounds, length); Browse... plays any file.
- **Remote**: the lobby's `/lbs/replay` search by battle code, player name or
  pilot name (a part of the name matches), user ID, player count, mobile suit
  (used by a player matching the name filters) and rated
  battles only, newest first (or oldest first), 100 per page.
  Plays the row's `replay_url`. Add to Favorites keeps the row.
- **Favorites**: the kept rows (`ZdxsvReplayFavorites`); Play asks the lobby
  API for the battle's `replay_url`.
- **Live**: the lobby's `/lbs/live` (running battles first, refreshed every 10 s
  while the tab is shown). Plays `live:<API URL>#<code>`.
- Remote, Live and Favorites ask the lobby API of setting `ZdxsvLobbyApiUrl`
  (ini only, `options.md`).
- **Point of view**: the recorder's or a position (`ZDXSV_REPLAY_POV`).
- **Play** (or a double click) starts a second pcsx2 process, the replay
  child, that boots the game from the game list (it must hold the Z game)
  with the pick. The running game is left as it is (it can stay in the
  lobby). Refused during a net battle.
- **Replay child**: a game window only (`-nogui`), command line
  `-zdxsvreplay <pov>:<parent pid>:<source>`. It reads the parent's settings
  and saves nothing over them: its settings copy, cache, logs (its
  `emulog.txt`), save states and memory card folder are
  `<data dir>/replay-child/`; memory cards, DEV9, fast boot and login,
  replay saving and upload, crash reports, achievements, Discord and the
  update check are off; no play time is added; controller input reaches it
  only while its window is active (keyboard and mouse come with the focus).
  It exits when the replay does not start (the reason is in its log), when it
  ends, when its window closes or within a second of its parent's exit.
  One at a time: Play again ends the running child first.

## Controls

| Action | Pad (host pad 1) | Hotkey (default) | Control bar |
|---|---|---|---|
| Play or pause | Circle | | play / pause button |
| Seek back / forward 1 s; paused: one frame | Left / Right (held 0.5 s: repeats) | "Step Back" / "Step Forward" (unbound) | ◀ / ▶ |
| Seek back / forward 10 s | | PageUp / PageDown | |
| Seek to a frame | | | timeline: click or drag, seeks on release |
| Speed down / up (33, 50, 100, 200, 300 %) | Down / Up | | ▼ / ▲ around the speed |
| Toggle key display | | End | |
| Previous / next round | | Shift+PageUp / Shift+PageDown | step buttons around the round number |
| Take over / retry | (taken over: START retries) | "Take Over / Retry" | "Take over", "Retry", "Replay" |

Hotkeys are under Settings > Hotkeys, group "Zdxsv Replay". The pad's
controls are read before the game; the left stick works as the
D-pad. While taken over the pad plays the game.

- **Rounds only** (setting
  `ZdxsvReplaySkipMs`): a round is shown from its briefing (the load end
  before the file's play start) to the load of the rematch screen (the
  load end after the game's first tick state 6 -> 7 after that play start).
  Frames between rounds (the mobile suit selections, the rematch screens)
  run unthrottled and are not drawn, and after the last round the replay
  ends. A file without play starts skips the first mobile suit selection
  only (from frame 0 to the briefing).
- **Step** (paused): runs one frame (back: a seek to the frame before; while
  stepping, the last 60 frames are kept in memory, so it loads one of them
  instead of a key) and
  pauses again. The audio output stays paused and the step's sound is dropped;
  play resumes it.
- **Seek**: a key (full state + battle-socket state) is kept every
  `ZDXSV_REPLAY_KEY` played frames, per position. A seek loads the newest
  key at or before the target and runs to it unthrottled (the limiter mode
  stays, so the vsync mode does not change); the frames it
  runs are not drawn or heard but the last two, and the picture stays until
  a drawn one is ready (no present of a frame not drawn, nor right after a
  state load). Forward seeks run every frame. A seek stays
  in the round shown (rounds only on). A seek or a round jump from a pause
  pauses again at its target. Key files are deleted on a VM shutdown or
  reset.
- **Speed**: the emulator's speed stays 100 %. Above it, of
  every 2 or 3 frames one is shown, heard and throttled and the others run
  silent and unthrottled; below it, each frame is held one or two frame
  times. Not while spectating live.
- **Point of view**: one file plays every position; the point of view is
  picked before the start (replay window, `ZDXSV_REPLAY_POV`). A position
  not played yet runs its own battle start from the common state
  (`ZDXSV_REPLAY_POV_AT`, tests).
- **Control bar**: play or pause, speed, ◀ ▶, round steps, round results,
  takeover, time in the round, frame, and a timeline of the round shown.
  Shown for 3 s after the mouse moves over the bottom quarter or a pad
  control changes.
- **Key display** (on by default): the
  last 14 input changes of the shown position, each with the frames it was
  held; placed on the game picture (left edge, from y 130 of 480, 12 of 480
  a line, shadowed text without a background), so it follows the window
  size and the aspect ratio.
- **Round jump**: with rounds only on, round N is the N-th shown round and a
  jump goes to its briefing. Otherwise a round starts when the game's tick
  state leaves the battle load (load 0 = mobile suit selection, 1 =
  briefing, 1 + N = round N). A jump to a round not reached yet runs
  unthrottled to it. Previous round goes to the round before the one shown.
- **Round results**: the bar shows each round's W / L / D for the shown
  position's team; playback logs a round whose result differs from the file.
  Hovering them lists each round's mobile suits (the file's `used_ms`, else
  the ones at the round end): the shown team's, then the other team's.
- **Takeover**: in a battle only, the shown position plays from
  the current frame with the host pad, input delay `mindelay`. A 1 s
  countdown (a filling circle) runs paused; the replay's input at the
  frame is shown as a guide (matched green, missing yellow, extra white)
  and the countdown does not wait for it. START while taken over retries
  from the takeover frame and is not sent to the game; the end of the
  battle retries too. "Replay" goes back to the replay. Not while
  spectating live.
- **Four-screen** (Windows, files only): `ZDXSV_REPLAY_FOUR=1`
  starts one more PCSX2 per other position, tiled 2x2, all held on the same
  frame; they follow this one's pause, speed and seeks and close with it.

## Live spectating

A lobby GGPO battle is streamed to the lobby while it runs
(zdxsv `pkg/lobby/spectator.go`).

- Uplink: the lobby marks one GGPO player per battle (`live_uplink=1` in the
  battle info). That client streams the replay header, every confirmed
  frame and the close to the lobby, whether saving or upload is on or not.
  A frame is the inputs plus the frame's u32 state hash (`frame_bytes` =
  players x 32 + 4); frames without it still play.
- Round results: at each confirmed round end the uplink adds the round's
  `round_data` entry (winner, MS per position) to the stream after the frames
  before it. The spectator shows them as a file's `round_data` and logs a
  round its own playback ends with another winner.
- Transfer: the lobby HTTP API (`ZdxsvLobbyApiUrl`), `/lbs/live_stream`. The
  uplink opens one POST at the battle start and streams the body (chunked)
  until the close; a spectator opens one GET and gets the same bytes from
  their start until the upload ends. The lobby does not parse them. Both the
  uplink and the lobby send 3 times a second (an empty batch: a keep-alive
  record), on their own threads.
- Live stream: `ZLV1`, then records: u8 type, u32 LE payload length, payload.
  Types: 0 keep-alive (empty), 1 header (a `BattleLogFile` without inputs),
  2 inputs (u32 LE `frame_bytes`, then frames, in order), 3 round (one
  `round_data` entry), 4 end (the close reason, UTF-8). Others are skipped.
  A stream that ends without an end record: stream lost.
- Spectator: `ZDXSV_REPLAY=live:<API URL>[#battle code]` (no code = the
  newest live battle). It starts from the common state and plays as a
  replay from the uplink's point of view, checking the state hashes as a
  replay file's.
- Pacing: at the newest frame it waits
  until 60 more frames are there; far behind it runs unthrottled to catch
  up. In between the frame limiter's period is trimmed by a few ms per frame
  to stay 60 frames behind without stalls. Nothing from the lobby for 30 s =
  stream lost.
- The wait at the newest frame holds the frame: the CPU does
  not run, the VM loop does. Pause, shutdown and closing the window answer
  at once while waiting; the screen and the replay bar keep redrawing.
- Auto-next (setting `ZdxsvLiveAutoNext`):
  at the end of a stream the spectator asks the lobby for its newest live
  battle not watched yet, resets the VM and watches it from its start.
  A live stream that does not play (no battle, open failed, not a replay
  stream) is skipped: the spectator keeps waiting and does not
  count it as a move. While it waits (VM paused, the lobby asked every 5 s) an
  OSD shows the time waited, battles watched, battles skipped and the last
  skip's cause. Resuming the VM stops the wait.
