# Replays

A GGPO battle can be saved to a file and played back later. Option names and
defaults are in [options.md](options.md).

## Saving

`replay=DIR` in `ZDXSV_GGPO` saves every `net=1` battle to `DIR`. Lobby battles
save to `<data dir>/replays` without it. `replay=0` turns saving off.

- The file name is `<battle_code>.pb`. Without a battle code it is
  `rbk-<start time>-p<position>.pb`.
- The full save state is taken at GGPO frame 0 and zipped on a thread during
  the battle. The file is written when the session stops, also when the VM
  is shut down or reset during the battle.
- Only frames GGPO confirmed are written, so the files that the peers save of
  one battle hold the same inputs.

Log line: `ZdxsvGgpo: replay saved <path> frames=N ...`.

## File format

A protobuf `BattleLogFile`, schema in
[`pcsx2/Zdxsv/replay.proto`](../../pcsx2/Zdxsv/replay.proto). It keeps the field
numbers of gdxsv's replay file (inada-s/gdxsv `gdxsv/proto/gdxsv.proto`) and
adds the fields a PCSX2 replay needs from 40 on.

A gdxsv replay starts from a common save state of the game sitting in the
lobby before any battle (flycast slot 99, one per disc, shared by every
replay); the recorded battle messages then play the battle start. A PCSX2
replay starts from the save state of one battle position at GGPO frame 0
instead (`start_state`), so one file plays the point of view of the player
who saved it.

| Field | Meaning |
|---|---|
| `battle_code`, `battle_info` | the battle code, and the lobby's battle info (`key=value` lines) |
| `log_file_version` | format version (20261008) |
| `game_disk` | `zdxsv-ps2` |
| `users` | user id, name and battle position per player |
| `start_at`, `end_at` | unix seconds |
| `close_reason` | why the session ended |
| `players`, `position` | player count, and the battle position of the recording player |
| `input_delay` | GGPO input delay of the battle |
| `zds_ps`, `net_rx0`, `hle0` | battle-socket state at frame 0 |
| `input_size`, `frames`, `inputs` | the synced inputs of all players: `frames` x `players` x `input_size` bytes, frame-major, by battle position |
| `start_state` | the save state at GGPO frame 0 (a `.p2s` zip) |
| `lobby_answers` | the lobby's battle-start answers the game got (0x6911..0x6917: player count, side, players, rule, battle code, battle server), each as received (header + body) |

`tests/zdxsv/replay_check.py` reads it without a protobuf library;
`protoc --decode=zdxsv.BattleLogFile pcsx2/Zdxsv/replay.proto < file.pb` prints it.

## Playing

`ZDXSV_REPLAY=<file.pb>` plays a replay. Boot the game; any save state of it
works. The first frame loads the frame 0 state of the replay and its
battle-socket state. Then every frame gets the recorded inputs of all players
through the same battle-socket emulation as a live GGPO battle, without GGPO.

Common start (as gdxsv): a file without `start_state`, or any file with
`ZDXSV_REPLAY_COMMON=1`, plays the battle start itself, from a save state of
the game at the post-entry point (logged in, before the lobby's battle start;
any user's). That state is hosted, as gdxsv's slot 99: `[DEV9/Eth]
ZdxsvReplayStateUrl` in `PCSX2.ini` (or `ZDXSV_REPLAY_STATE=<url or path>`)
is downloaded once into the cache folder and loaded at the first frame; with
neither set, boot from such a state yourself. The battle start is answered from `lobby_answers` as the
point of view, menus run turbo, and at GGPO frame 0 that state replaces
`start_state`. The log line `frame 0 HLE state equals|differs from the file's`
compares the battle-socket state reached with the recorded one. One file only.

- `ZDXSV_REPLAY=http(s)://...` downloads it first: a `.pb` URL, or the lobby's
  public API `http://<ZDXSV_LOBBY_API_ADDR>/lbs/replay?battle_code=<code>`
  (a JSON list, as gdxsv lbsapi), whose first battle's `replay_url` is then
  downloaded. Rig: `tests/zdxsv/m4z.sh UPLOAD=1 REPLAY_API=N`.
- The replay is shown from the side of the recording player.
- At the end the emulator pauses. A seek then plays on; resuming without one
  ends the replay.
- Files saved before the battle-socket keys existed play with an empty state
  and may drift.
- A replay of a `ZDXSV_RBK` battle needs the `ZDXSV_EE_CLAMP` of the recording
  (`tests/zdxsv/rplay.sh` reads it from the recording's `rbk env` log line).

Log lines: `ZdxsvGgpo: replay <file>: position P of N, F frames ...`,
`ZdxsvGgpo: replay end at frame ...`.

## Controls

| Action | Hotkey (default) | Control bar |
|---|---|---|
| Seek back 10 s | PageUp | yes |
| Seek forward 10 s | PageDown | yes |
| Seek to a frame | | timeline: click or drag, seeks on release |
| Play or pause | | yes |
| Switch point of view | Home | eye button |
| Toggle key display | End | |
| Previous round / next round | Shift+PageUp / Shift+PageDown | step buttons around the round number (`R0` = before round 1); the timeline marks round starts |

The hotkeys are under Settings > Hotkeys, group "Zdxsv Replay". The defaults
apply when hotkeys are reset to defaults.

### Seek

Every 600 played frames a key is kept: the full state as
`cache/zdxsv-replay-key-p<P>-<frame>.p2s`, zipped on a thread, plus the
battle-socket state. A seek loads the newest key at or before the target,
unless running on from the current frame is as close, then runs to the target
unlimited. Forward seeks past the played part run every frame.
The key files, and `cache/zdxsv-replay-p<P>.p2s` of the frame 0 state, are
deleted when the VM is shut down or reset; the reset VM plays the replay again
from the start.

### Point of view

`ZDXSV_REPLAY=a.pb;b.pb` loads the files that different players saved of
one battle. The inputs must match on the common frames. Each file adds the
frame 0 state of its position.

A switch moves to the next position that has a file, at the current frame: it
loads the newest key of that position at or before the frame and runs to it
unlimited. Keys are kept per position.

Files saved before the network adapter was kept in replay states (commit
`bbbe3e9d2`) may hang for a position.

### Control bar

The bar is at the bottom of the window: play or pause, seek -10 s and +10 s,
time and frame of the length, a timeline, the point of view button. The point
of view button is enabled with a second file.

- It is shown while paused and for 3 s after the mouse moves over the bottom
  quarter of the window.
- A seek from the bar plays on from a pause. Play at the end restarts from
  frame 0.
- While paused, the window redraws at 10 Hz so the bar sees the mouse.

Log lines: `ZdxsvGgpo: replay bar: <action>`, and `replay bar: layout` with
the x range of each element in window pixels.

### Key display

As `gdxsv:ReplayKeyDisplay` of flycast. The left edge shows the last 14 input
changes of the shown position, newest on top. Each has the number of frames it
was held (shown up to 99) and its d-pad and button glyphs, from the recorded
game input word. It follows seeks and point of view switches.

While on, it logs `ZdxsvGgpo: replay keys frame F pos P: <word>*<frames> ...`
every 600 frames.

### Skip mobile suit selection

On by default, as `gdxsv:ReplaySkipMsSelection` of flycast. The replay runs
unlimited from frame 0 to the briefing, then plays at the normal speed. The
briefing is the frame at which the tick state of the game leaves the battle
load for the second time.

- A seek or a switch during the skip ends it.
- Playing from frame 0 again jumps to the briefing.

Log lines: `ZdxsvGgpo: replay skip MS selection: briefing at frame F`, or
`cancelled by a seek`, or `replay ended first`.

### Round jump

A round starts at the frame at which the tick state of the game leaves the
battle load. Load 0 ends at the mobile suit selection, load 1 at the briefing,
load 1 + N at the start of round N. Round 0 is the briefing.

- Loads are recorded as frames are played. Frames always play in order up to
  the furthest played frame (a forward seek runs every frame between), so the
  list is complete up to that frame.
- A jump to a known round start is a seek. A jump to an unknown one seeks to
  the furthest played frame and runs unlimited until that load ends.
- Previous round goes to the round before the one shown, not to the start of
  the shown round.

Log lines: `ZdxsvGgpo: replay: load K ends at frame F`,
`replay round N: starts at frame F` (known) or `replay round N: start at frame
F` (after the run), `cancelled by a seek`, or `replay ended first`.

## Live spectating

A lobby GGPO battle is streamed to the lobby while it runs, as gdxsv does
(`SpectatorInputPush` / `Ack` / `SubscribeRequest` / `Challenge` on the
lobby's UDP socket, zdxsv `pkg/lobby/spectator.go`).

- Uplink: the lobby marks one GGPO player per battle (`live_uplink=1` in the
  battle info, the lowest `udp_rtt`). That client sends the replay header, the
  frame 0 state and every confirmed frame to the lobby's UDP address, then the
  close. It needs replay saving on (the default for lobby battles).
- Transfer: 1000-byte datagrams, go-back-N from the receiver's ack (window 32,
  resent after 200 ms without progress), both uplink to lobby and lobby to
  spectator.
- Spectator: `ZDXSV_REPLAY=udp://host:port[/battle code]` (the lobby's UDP
  port, 8201 by default; no code = the newest live battle there). Boot the
  game as for a file. It waits up to 15 s for the header, the state and a
  frame, then plays as a replay (seek, point of view of the uplink, keys).
- Pacing: at the newest frame it waits until 30 more frames are there (or the
  close); more than 300 frames behind it runs unlimited until 90 behind
  (flycast's `gdxsv:LiveBufferFrames` and its catch-up edges). Nothing from the
  lobby for 30 s = stream lost.
- Auto-next (flycast's `gdxsv:LiveAutoNext`): with `[DEV9/Eth]
  ZdxsvLiveAutoNext` on (Settings → Network & HDD), at the end of a stream the
  spectator pauses and asks the lobby every 5 s for its newest live battle (a
  subscribe without code or cookie: the challenge names it, running battles
  first). One not watched yet resets the VM and is watched from its start
  (catching up if far along). A seek from the end cancels the move. Tests:
  `ZDXSV_LIVE_NEXT=N` = N more battles, 0 = off.

Log lines: `live: uplink CODE to ADDR`, `ZdxsvGgpo: live: battle CODE, N
frames so far`, `live: N frames behind at frame F: catching up`, `caught up`,
`live: stream closed (REASON) at frame F, N waits T ms`, `live: auto-next:
waiting for a new battle at HOST (N watched)`, `moving on to CODE after T s`.
