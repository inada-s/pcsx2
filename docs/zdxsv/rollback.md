# Rollback netcode (GGPO)

How a battle runs over GGPO inside the emulator. In development. Lobby
battles run over GGPO by default (`ZDXSV_EXTENSIONS=0` turns it off with all
other extensions); a battle with a peer
without GGPO is cut, as a connection failure. Option names and defaults are
in [options.md](options.md); battles matched by the lobby are in
[lobby.md](lobby.md).

## Parts

| Part | Where | What it does |
|---|---|---|
| GGPO library | `3rdparty/ggpo` | rollback netcode: input exchange, prediction, the decision to roll back |
| Delta save states | `pcsx2/Zdxsv/DeltaState.cpp` | save and load the whole machine fast enough to do it every frame |
| GGPO session | `pcsx2/Zdxsv/Ggpo*.cpp` | runs one frame at a time, feeds GGPO, answers the battle socket of the game |

## GGPO library

[GGPO](https://github.com/pond3r/ggpo) (MIT; notice in
`bin/docs/ThirdPartyLicenses.html`), as in inada-s/flycast. Changes in this
fork:

- `ggpo_get_last_confirmed_frame` added.
- On Windows the UDP sockets ignore ICMP port unreachable
  (`SIO_UDP_CONNRESET` off): a send to a peer that is not running does not
  fail the next receive.

## Delta save states

A save copies only the EE, IOP and SPU2 RAM pages written since the last save,
found with host page write protection, plus the rest of the state. A page once
written becomes hot: it stays writable and is compared and copied at every
save and load, until 32 saves in a row find it unchanged (a protect and fault
cost far more than a page copy). A hot page unchanged since the last save
keeps its copy for the next interval and adds nothing to the saved delta. A restored IOP RAM page clears the recompiled IOP code over
the words that differ; a restored SPU2 RAM page drops the decoded sample cache
of the blocks that differ.

Supporting changes in the core:

- Saved scratch fields are skipped when two states are compared.
- Counters and the GIF path keep their bookkeeping when a delta state loads.
- SPU2 DMA IRQ flags are part of the state.
- A disc read started while another is in flight waits for it: loading a state
  during a read otherwise leaves the new read with the old read's sector.
- While GGPO runs, the EE and IOP recompilers end blocks only at branches and
  page boundaries, so a rerun times events as the first run did.
- The MTVU speedhack is off for a VM with GGPO, a replay or
  `ZDXSV_DELTA_TEST`: a delta state copies VU1 memory without waiting for the
  VU1 thread. The setting itself is not changed.
- DEV9 (network adapter) is part of a delta state when DEV9 is in save
  states. Host connections are kept on a load; frames the adapter received
  after the loaded save are received again.
- SPU2 checks a voice's volume slide flag inline (the slide step stays out of
  line): the call ran twice per voice per sample, mostly with no slide.

`ZDXSV_DELTA_TEST` saves every frame, rolls back at a fixed interval, and
checks that reruns hash alike.

## GGPO session in a running game

While a session runs, the CPU leaves its run loop at every vsync, so a frame
is the unit GGPO works in. Between two frames the emulator saves the state,
rolls back and reruns frames when GGPO asks for it, then applies the synced
inputs of the next frame. Host pad input goes through GGPO.

Rerun frames are not throttled, presented or heard: a rollback of N frames
shows and plays the live frame once. SPU2 reverb is not computed in rerun
frames (unless an SPU2 IRQ address lies in the reverb work area): it only
feeds their dropped sound. The game builds a frame's draw list in its render
callbacks, VU1 draws it in the next frame, and that picture is shown one
vblank later. The present comes before the rollback, so the first present
after it shows what VU1 drew in the last rerun frame, from the list of the
rerun frame before. So VU1 microprograms run only in the last rerun frame,
and rerun frames but the last two skip the game's render callback runners
(0x20ff30, 0x20ff90, called by the battle frame step 0x2124f0 after the
logic): their draw lists are never kicked. Skipping one more frame of either
breaks that picture (`ZDXSV_RERUN_TAIL`): without
the last frame's VU1 it lacks all VU1 geometry, without the callbacks of the
frame before it repeats the picture shown before the rollback. Those callbacks also draw from
RNG A (u16 0x6d7940, ~40 draws a battle frame), so after such a rollback
RNG A differs from a run without one. RNG A is already peer-local (sound
picks draw from it) and the players and RNG B stay in sync (rbk N=2 and N=4).

The single draw list (`SingleDrawList.cpp`, always on) removes the frame between the list and VU1. The
game keeps two draw-list slots chosen by a per-frame flag (u32 0x7ae1c0); its
main loop sends the list of the frame before (`slot[flag ^ 1]`) ahead of the
logic. The patch moves the send after the logic and makes it, and the VIF1
DMA end handler, take `slot[flag]`: VU1 draws a list in the frame that built
it, and the picture is shown at the next present, one frame earlier. The vsync
handler's GIF packet, which sets the draw buffer of context 2 for the next list,
is swapped to match (the network menus and the login page draw in context 2). Then
no rerun frame runs the render callbacks or VU1: GS memory is not rolled back,
so the first present after a rollback shows the frame drawn before it (the
predicted one), never an older picture. The framebuffer
swap (DISPFB between FBP 0x000 and 0x08c every vsync) is unchanged. The players
and RNG B stay in sync in N=4 battles and between peers with and without it.
Not checked yet: replays.

Rerun keep: the render callbacks also update state that a later draw reads.
The per-player bone matrix slots (4 x 0x65d0 bytes from 0x8afda0, written
only by the callbacks) hold the pose of the frame before; rolled back and
not advanced by the skipped callbacks, they show a mech's arms and weapon in
an old pose after a rollback. So during play (tick state 6) a rollback does
not roll them back: they keep the predicted frame's poses. Player work does
not depend on them (synctest: player work and RNG agree with them kept).
Except one part: each player's draw first copies part of its work into its
slot (handler 0x1e4340: obj+0x20 to slot+0x220, the angles at obj+0x50-0x58
as radians to slot+0x200-0x208, n 0x40-byte blocks from obj+0x7c0 to
slot+0x300 + i * 0x240; the pose's w is set to 1). The next frame reads that
part, so it follows the rerun game state: the load does not keep it, and it
is copied natively for each player the object draw walker calls in a rerun
frame (see below), else for every player after each rerun frame. A player
the walker culls (0x2ac630) is not drawn and its copy stays from an older
frame, which the draw after the cull reads. Kept, or copied for culled
players too, some pictures after a rollback differ from a full rerun.
Parts attached to a player (handler 0x1f6520, e.g. object 0xb130d0 on the
player at 0x839330) draw their matrix into a further block of the player's
slot (0x2a25c0: slot+0x300 + 0x240 * desc[4]), which the player's next draw
reads. That matrix is built in the part's draw and is in no game state, so
rerun frames that skip the callbacks still run the object draw walker
(0x2ac4e0, from the first callback runner) with only these handlers;
`ZDXSV_RERUN_PARTS=0` skips them. Without them runs of pictures after
rollbacks differ from a full rerun (up to ~4000 pixels).
Memory the game logic reads must not be kept: the object memory at
0xafd800-0xafdfff and 0xb13000-0xb6afff holds game objects and effects, and
an address is reused by another object type, so keeping it desyncs.
Beam saber trail objects (handler 0x167810) also keep their trail history:
fields +0x0-0x9f and +0x150-0x1cf, only for an object at the same address
with the same handler before and after the load (objects found through the
game object list, head at gp - 0x5b80). Without them a saber trail jumps
after a rollback. Synctest: player work agrees with them kept.
Objects with handler 0x167f50 keep +0xb0-0x1ef the same way: without them
the effect is off in the first picture after each rollback. Synctest: player
work agrees with them kept.
`ZDXSV_RERUN_KEEP` turns it off or sets other ranges, kept in any game phase
(tests); `ZDXSV_RERUN_EE_DRAW=1` turns it off.

### Synctest

`ZDXSV_GGPO` without `net=1`. Every `check` frames GGPO loads the state
`check` frames back, reruns with the same inputs and compares state hashes.
On a mismatch `DIFF` lines name the differing player work offsets.

### Battle (`net=1`)

The game runs with its own input delay 0; GGPO adds the input delay.

- The session starts when the game sends its first key message on the battle
  socket. From then on the emulator answers that socket's send, receive and
  poll itself; they never reach the network adapter.
- Every peer runs its own battle position. GGPO player = battle position + 1.
- A frame's synced input holds each player's input words (the game's own,
  built from the pad: buttons, START, left stick as the d-pad, right stick)
  and the round handshake messages the game sent.
- Without the lobby, the peer at position p listens on UDP `port` + p.
  `relay=R` routes each pair through its own `udprelay.py` (latency, jitter,
  loss).
- Each peer arms at its own first key message and waits up to 10 s (as
  flycast) for all to be synchronized; then the session closes as
  `start timeout` (other start errors: `start failed`).
- The sync handshake carries the battle code (as flycast; rbk: none). A
  peer with another code is refused: the peer that gets the refusal closes
  as `session mismatch` and answers the other's requests for 1 s more; the
  other closes as `session mismatch` too, or as `start timeout` when GGPO
  sent it no refusal in that time.
- A GGPO network thread waits on the sockets (`select`) and reads each packet
  as it arrives: packets this peer relays for others go out at once, a quality
  report (ping) is answered and a quality reply timed at once, the rest
  wait for the frame loop. So a relaying peer's rollbacks and limiter sleep do
  not delay the peers it relays for.
- With `GGPO_NETWORK_DELAY` set, the network thread also sends delayed
  packets when they are due (every 1 ms), not at the next frame.
- Time sync: when GGPO reports this peer ahead, it waits one frame
  (1/60 s, `ggpo_idle` while waiting) every 10 frames until the suggested
  frames are used, as flycast. That frame's limiter period grows by the wait, so
  the peer really falls back. The count is `Sync N` on the network OSD and
  `sync N (max X ms)` in the `net sends` log line (X: the longest wait, without
  the time `ggpo_idle` spent running a rollback).
- The session ends `ZDXSV_NET_TAIL` frames after the battle end message, or
  when a peer disconnects. The countdown starts once every input of the frame
  that sent the message is confirmed: a message sent in a predicted frame that
  a rollback undoes does not end the session.
- At that confirmed end each peer disconnects itself from the others
  (`ggpo_disconnect_player` on its own player, as flycast): the tail frames
  no longer wait for a peer whose tail is shorter or that already closed, and
  a disconnect after the confirmed end is a `net battle end`, not
  `disconnected`. A peer can confirm the end, and leave, up to the input
  delay before this peer has run the end message frame. So a disconnect
  before the end ends the session only once this peer has run its last input's
  frame without a confirmed end message. Otherwise it is a `net battle end`,
  with the replay ending at the end message frame. The tail frames run on zero inputs for the peers, so the
  replay and the pw hashes stop at the frame confirmed at the end. GGPO's
  `disconnect_without_rollback` is on: a frame is confirmed once this peer has
  its inputs, not once every peer reported them, which with 3 or more players
  would add one one-way trip of prediction. A peer that stops sending
  mid-battle keeps its received inputs and gets zero inputs after them, with no
  rollback.
- After a `net battle end` or `disconnected` the battle socket answers as a
  closed connection until the game closes it: sends are dropped, nothing to
  recv, the poll fails. The battle server closes the connection on the
  game's end msg (kind 0xf); without a close the game resends it once a
  second and gives up only after ~100 s. After a disconnect the game shows
  its own connection error at once instead of after its countdown.
- A VM shutdown or reset ends the session at once, as a disconnect: the
  replay is saved, the old VM's states are dropped.
- An input send splits the pending inputs over packets of at most 8192 bits
  of inputs (IPv6 minimum MTU); a pcsx2 input is 32 bytes, so a burst could
  outgrow one packet. A packet that starts past the receiver's next frame (an
  earlier split packet was lost) is skipped; the sender resends from the
  oldest unacked frame.

Three mechanisms keep the peers on the same frame:

- **Play-start barrier**: each machine finishes loading the battle at its own
  time; the barrier holds the game until every peer is ready.
- **MS-select barrier**: the same for the load before MS select, which draws
  the game RNG. A peer without it never reports ready; the others wait at MS
  select. Replays without it (`hle0` of 9 values) play without it.
- **Round handshake lag** (`ZDXSV_K3_LAG`): a round-handshake message goes
  into the input a fixed number of GGPO frames after the game sent it, more
  than GGPO's prediction window, so it does not depend on network timing.

### Frame order and pacing (`net=1`)

With `ZdxsvLowLatencyPresent` a battle frame ends: present, input poll and send,
GGPO advance, rollback, wait at the prediction limit, limiter sleep, then the
next frame runs. The input goes out before the rollback and the sleep: the peers
get it that much earlier, and its age does not depend on the rollback's length.
The press is that much older when the frame runs. The input joins GGPO before
the advance, so GGPO's frame delay is the battle delay + 1, for the same frame.

During a battle the limiter does not pay a late frame back with shorter ones:
the next frame waits a full period from the late one. The process runs at high
priority (Windows) for the session.

### Rollback budget

The lobby input delay is max(`mindelay`, ceil(RTT / 2 / 16 ms - budget)): the
budget is the part of the one-way latency left to rollbacks.

- Each battle frame (frame loop to frame loop, limiter sleep left out) gives
  its slack: the frame period (59.94 Hz) less the work besides rollback
  reruns, in frames of its own emulation (net waits left out): the rollback
  depth that fits. A histogram in steps of 0.05 frames gives the median.
- After a net battle of at least 1800 frames the learned budget moves halfway
  to clamp(median - 1, 0, 1.5) and is saved (`ZdxsvRollbackBudget`).
- The delay uses 0 for now, so it is ceil(RTT / 32 ms) as before: rollbacks
  still cost too much on PS2, and a deeper rollback would
  drop frames. The budget gets used once reruns are cheaper; the switch is
  `kBudgetUsed` in `GgpoBudget.cpp` (`ZDXSV_ROLLBACK_BUDGET=1` for tests).
- The lobby log line `lobby delay` and the match report (`rollback_budget`,
  `rollback_budget_used`) carry the learned and the used budget.
### Rollback test mode

`ZDXSV_RBK=i/N` runs a battle without any server, from a save state taken
after battle entry, as position i of N. The emulator answers the lobby calls
itself, runs turbo until the battle, and exits when the session ends.

## Sync-safe settings and fingerprint

- While the game runs (`Zdxsv/SyncSettings.cpp`), as in RetroAchievements
  hardcore mode: cheats off; recompilers, EE/VU rounding and clamping, game
  fixes (GameDB only), extra memory and GS hardware download mode at their
  defaults. During battles, replays and live spectating also speedhacks,
  emulation speed, frame rate and sync to host refresh rate (off: it runs
  the game up to 5 % off nominal speed). An OSD message names the overridden
  settings; the settings page greys them out.
- Sync fingerprint: a hash of the build, those settings and the recompiler
  settings (not the BIOS), sent in the platform info and logged as
  `ZdxsvSync: fingerprint <hex>`. A lobby battle whose players' fingerprints
  differ is cut, and the players return to the lobby.

## Sync checks

The peers run different views of one battle, so their whole state cannot be
compared. The check compares, per frame, each player's coordinates and the
game RNG:

- `ZDXSV_PW_HASH=1` writes the hashes to the net trace. In sync, the
  coordinates and RNG B agree on all peers; RNG A also takes draws only one
  machine makes, so `pwcheck.py` reports it without judging it. Not judged:
  frames before the play start (local load timing), and from each game end to
  the next play start (the peers enter the end phase on different frames).
  A replay whose play start comes after its live battle's: those frames are
  printed as `own load late`, not judged.
- `ZDXSV_PW_DUMP` writes the player work itself, to find the field behind a
  mismatch.
- `ZDXSV_NET_TRACE` writes the battle socket traffic and the inputs per frame.
