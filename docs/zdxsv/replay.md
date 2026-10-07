# Replays

A GGPO battle can be saved to a file and played back later. Option names and
defaults are in [options.md](options.md).

## Saving

`replay=DIR` in `ZDXSV_GGPO` saves every `net=1` battle to `DIR`. Lobby battles
save to `<data dir>/replays` without it. `replay=0` turns saving off.

- The file name is `<battle_code>.zdxr`. Without a battle code it is
  `rbk-<start time>-p<position>.zdxr`.
- The full save state is taken at GGPO frame 0 and zipped on a thread during
  the battle. The file is written when the session stops, also when the VM
  is shut down or reset during the battle.
- Only frames GGPO confirmed are written, so the files that the peers save of
  one battle hold the same inputs.

Log line: `ZdxsvGgpo: replay saved <path> frames=N ...`.

## Upload

A lobby battle's replay is also posted to the lobby's replay server, when the
battle info names one (`replay_upload=<url>`, zdxsv `ZDXSV_LOBBY_REPLAY_ADDR`)
and the setting `ZdxsvUploadReplay` is on (default; Settings → Network & HDD).

- `POST <url>?battle_code=<code>&user_id=<own user id>`, body = the `.zdxr`
  file. The server keeps the first upload of a battle and answers 409 to the
  other players: one file holds every player's input.
- On a thread of its own after the file is written; nothing waits for it.
- The server serves stored files at `GET <url>/<battle_code>.zdxr`.

Log lines: `ZdxsvGgpo: replay upload <url>: ok|already there, N bytes, T ms`,
or `ZdxsvGgpo: replay upload <url> failed: status S`.

## File format

1. The line `ZDXSV-REPLAY 1`.
2. `key=value` lines, then an empty line.
3. The save state at GGPO frame 0: a `.p2s` zip of `state_size` bytes.
4. The synced inputs of all players: `frames` x `players` x `input_size`
   bytes, frame-major, by battle position.

| Key | Meaning |
|---|---|
| `battle_code`, `user_id` | from the battle info of the lobby |
| `players`, `position` | player count, and the battle position of the recording player |
| `delay` | GGPO input delay of the battle |
| `start_at`, `end_at` | unix seconds |
| `frames` | number of frames in the file |
| `close` | why the session ended |
| `user_<p>`, `name_<p>` | user id and name per position |
| `zds_ps`, `rx0`, `hle0` | battle-socket state at frame 0 |
| `input_size`, `state_size` | sizes in bytes |

## Playing

`ZDXSV_REPLAY=<file.zdxr>` plays a replay. Boot the game; any save state of it
works. The first frame loads the frame 0 state of the replay and its
battle-socket state. Then every frame gets the recorded inputs of all players
through the same battle-socket emulation as a live GGPO battle, without GGPO.

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

`ZDXSV_REPLAY=a.zdxr;b.zdxr` loads the files that different players saved of
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
