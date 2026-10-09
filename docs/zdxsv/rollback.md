# Rollback netcode (GGPO)

How a battle runs over GGPO inside the emulator. In development. Lobby
battles run over GGPO by default (setting `ZdxsvGgpo`); a battle with a peer
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

A save copies only the EE RAM pages written since the last save, found with
host page write protection, plus the rest of the state.

Supporting changes in the core:

- Saved scratch fields are skipped when two states are compared.
- Counters and the GIF path keep their bookkeeping when a delta state loads.
- SPU2 DMA IRQ flags are part of the state.
- While GGPO runs, the EE and IOP recompilers end blocks only at branches and
  page boundaries, so a rerun times events as the first run did.
- The MTVU speedhack is off for a VM with GGPO, a replay or
  `ZDXSV_DELTA_TEST`: a delta state copies VU1 memory without waiting for the
  VU1 thread. The setting itself is not changed.
- DEV9 (network adapter) is part of a delta state when DEV9 is in save
  states. Host connections are kept on a load; frames the adapter received
  after the loaded save are received again.

`ZDXSV_DELTA_TEST` saves every frame, rolls back at a fixed interval, and
checks that reruns hash alike.

## GGPO session in a running game

While a session runs, the CPU leaves its run loop at every vsync, so a frame
is the unit GGPO works in. Between two frames the emulator saves the state,
rolls back and reruns frames when GGPO asks for it, then applies the synced
inputs of the next frame. Host pad input goes through GGPO.

Rerun frames are not throttled, presented or heard: a rollback of N frames
shows and plays the live frame once.

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
- The session ends `ZDXSV_NET_TAIL` frames after the battle end message, or
  when a peer disconnects.
- A VM shutdown or reset ends the session at once, as a disconnect: the
  replay is saved, the old VM's states are dropped.

Two mechanisms keep the peers on the same frame:

- **Play-start barrier**: each machine finishes loading the battle at its own
  time; the barrier holds the game until every peer is ready.
- **Round handshake lag** (`ZDXSV_K3_LAG`): a round-handshake message goes
  into the input a fixed number of GGPO frames after the game sent it, more
  than GGPO's prediction window, so it does not depend on network timing.

### Rollback test mode

`ZDXSV_RBK=i/N` runs a battle without any server, from a save state taken
after battle entry, as position i of N. The emulator answers the lobby calls
itself, runs turbo until the battle, and exits when the session ends.

## Sync checks

The peers run different views of one battle, so their whole state cannot be
compared. The check compares, per frame, each player's coordinates and the
game RNG:

- `ZDXSV_PW_HASH=1` writes the hashes to the net trace. In sync, the
  coordinates and RNG B agree on all peers; RNG A also takes draws only one
  machine makes, so `pwcheck.py` reports it without judging it. Not judged:
  frames before the play start (local load timing), and from each game end to
  the next play start (the peers enter the end phase on different frames).
- `ZDXSV_PW_DUMP` writes the player work itself, to find the field behind a
  mismatch.
- `ZDXSV_NET_TRACE` writes the battle socket traffic and the inputs per frame.
