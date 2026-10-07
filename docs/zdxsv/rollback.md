# Rollback netcode (GGPO)

How a battle runs over GGPO inside the emulator. In development. Lobby battles
of the game (serial SLPS-25419) run over GGPO by default: setting `ZdxsvGgpo`,
read when the game starts; `ZDXSV_GGPO` replaces it. A battle with a peer
without GGPO still goes through the battle server. The release will run
battles over GGPO only. Option names and
defaults are in [options.md](options.md). Battles matched by the lobby are in
[lobby.md](lobby.md).

## Parts

| Part | Where | What it does |
|---|---|---|
| GGPO library | `3rdparty/ggpo` | rollback netcode: input exchange, prediction, the decision to roll back |
| Delta save states | `pcsx2/Zdxsv/DeltaState.cpp` | save and load the whole machine fast enough to do it every frame |
| GGPO session | `pcsx2/Zdxsv/Ggpo.cpp` | runs one frame at a time, feeds GGPO, answers the battle socket of the game |

## GGPO library

[GGPO](https://github.com/pond3r/ggpo) (MIT), imported from inada-s/flycast.
Changes made in this fork:

- `ggpo_get_last_confirmed_frame` added.
- On Windows the UDP sockets ignore ICMP port unreachable (`SIO_UDP_CONNRESET`
  off). A send to a peer that has not started yet or has exited no longer
  fails the next receive with error 10054.

## Delta save states

A save copies only the EE RAM pages written since the last save, found with
page write protection, plus the rest of the state. That is fast enough to save
every frame. A page is a host page (`__pagesize`: 4 KiB on x86, 16 KiB on
arm64), the unit vtlb protects and reports.

Supporting changes in the core:

- Saved scratch fields are skipped when two states are compared.
- Counters and the GIF path keep their bookkeeping when a delta state loads.
- SPU2 DMA IRQ flags are part of the state.
- While GGPO runs, the EE and IOP recompilers end blocks only at branches and
  page boundaries. A rerun after a rollback then times events the same way as
  the first run.
- The MTVU speedhack is off for a VM with GGPO options, a replay or
  `ZDXSV_DELTA_TEST` (`GgpoOnVmInitialize`, kept off on settings reloads by
  `VMManager::LoadCoreSettings`): a delta save or load copies VU1 memory
  without waiting for the VU1 thread.
- DEV9 (network adapter registers and buffers) is part of a delta state when
  DEV9 is in save states (GGPO, replay, `ZDXSV_LOBBY_STATE=1`). Without it, a
  rollback across a sent frame left DEV9's TX descriptor one ahead of the IOP
  driver's, and after the battle the IOP spun on `BD_TX was not ready` and
  sent no result. Host connections are kept on a delta load; the frames the
  adapter received after the loaded save are received again
  (`ZdxsvDelta: load received N frames again`). A frame sent again after a
  rollback reaches TCP_Session as an old sequence number.

The self-test `ZDXSV_DELTA_TEST` saves every frame, rolls back at a fixed
interval, and checks that each rerun frame hashes like its first run.

## GGPO session in a running game

While a session runs, the CPU leaves its run loop at every vsync, so a frame
is the unit GGPO works in. Between two frames the emulator saves the state,
rolls back and reruns frames when GGPO asks for it, then applies the synced
inputs of the next frame. Host pad input is held back and goes through GGPO.

Rerun frames are not throttled, not presented and not heard: the vsync packet
of a rerun frame tells the GS thread to skip the present (as for a duplicate
frame), and SPU2 drops the samples of rerun frames (`Zdxsv/MediaHooks.h`). A
rollback of N frames shows and plays the live frame once. The session report
logs `output: presented frames .. | audio samples played .. dropped (rerun) ..`.

### Synctest

`ZDXSV_GGPO` without `net=1`. Every frame is saved. Every `check` frames GGPO
loads the state `check` frames back, reruns the frames with the same inputs
and compares the state hashes (`hash=`: by default the player work of all 4
players and the game RNG). Results go to the log; on a mismatch `DIFF` lines
name the differing player work offsets.

### Battle (`net=1`)

The game runs with its own input delay 0 and GGPO adds the input delay: 2 in
local tests, picked from the ping test in lobby battles.

- The session starts when the game sends its first key message on the battle
  socket. From then on the emulator answers the send, receive and poll calls
  of that socket itself; they never reach the network adapter.
- Every peer runs its own battle position. GGPO player = battle position + 1.
- The synced input of a frame holds the pad of each player and the round
  handshake messages the game sent.
- Without the lobby, the peer at position p listens on UDP `port` + p on
  `host`. With `relay=R` each pair of peers can go through its own
  `udprelay.py`, which adds latency, jitter and loss.
- The session ends `ZDXSV_NET_TAIL` frames after the battle end message, or
  when a peer disconnects.
- A VM shutdown or reset during the session closes it at once: the peers see
  a disconnect, the replay is saved, the delta states and the per-battle state
  of the old VM are dropped. Without this a reset VM kept the session, and its
  rollbacks loaded the old VM's states. Log line:
  `ZdxsvGgpo: vm shutdown|vm reset: session 1, ...`.

Two mechanisms keep the peers on the same frame:

- **Play-start barrier** (`ZDXSV_ZDS_PS=1`): each machine finishes loading the
  battle at its own time. The barrier holds the game until every peer is
  ready, so all start the battle on the same GGPO frame.
- **Round handshake lag** (`ZDXSV_K3_LAG`, default 8): a round-handshake
  message goes into the input a fixed number of GGPO frames after the game
  sent it. The number is above the 6 prediction frames of GGPO, so the frame
  is final by then and does not depend on network timing.

### Rollback test mode

`ZDXSV_RBK=i/N` runs a battle without any server. The emulator starts from a
save state taken after battle entry, as position i of N. It answers the lobby
calls itself, runs turbo until the battle, and exits when the GGPO session
ends. With `ZDXSV_RAND_INPUT` the pad input is seeded random.

## Sync checks

The peers run different views of one battle, so their whole state cannot be
compared. The check compares, per frame, each player's coordinates and the
game RNG instead:

- `ZDXSV_PW_HASH=1` writes a hash of each player's x, y, z (player work
  `+0x2a8`) and the 2 game RNG words (`0x6d7940`, `0x6d793c`) per GGPO frame to
  the net trace. In sync, the coordinates and RNG B (`0x6d793c`) agree across
  the peers. RNG A (`0x6d7940`) also takes draws that only one machine makes
  (a sound pick), so `pwcheck.py` reports it but does not judge it. Frames
  before the play start (`PS` line; without one, the end of the battle load)
  are not judged: the scene steps there follow local load timing.
- `ZDXSV_PW_DUMP` writes the work RAM itself, to find the field behind a
  mismatch.
- `ZDXSV_NET_TRACE` writes the battle socket traffic and the inputs per frame.

The tests that use them are listed in [features.md](features.md).
