# Investigating a desync

A desync: the peers of one battle no longer run the same game. The order
below goes from the cheapest evidence to the most expensive. Each step names
the facts it needs; the options are in [options.md](options.md), the checks in
[rollback.md](rollback.md) (Sync checks), the tools in [tools.md](tools.md).

## 1. Confirm it

- In the emulog of each player: `suspected desync with position N at frame F`
  and, at the session end, `desync checks N mismatches M`.
- The match report: `desync_frame` (the first differing sample, -1 = none);
  the replay: `suspected_desyncs`.
- `mismatches 0` with `desync checks` above 0: no desync was seen. A report of
  a desync without one: see step 3.
- The online check samples every 16th frame: the desync began up to 16 frames
  before `F`.

## 2. Rule out a setup difference

- `ZdxsvSync: fingerprint <hex>` must be equal in every player's emulog. A
  difference cuts lobby battles before they start; in a battle without the
  lobby it is a cause on its own.
- Game patches from the lobby (`0x9953`, [lobby.md](lobby.md)): the
  `ZdxsvPatch: ... applied` and `... skipped` lines must match across the
  players' emulogs.

## 3. Play the replays

Lobby battles save a replay on every machine (`ZdxsvSaveReplay`); each file
holds the synced inputs and that player's own state hashes
([replay.md](replay.md)).

- Play each player's file with the build that recorded it.
  `replay state hash differs at frame F` in the log: that machine's run is not
  reproducible from its inputs. Go to step 6.
- Every file plays clean but the files differ from each other: the inputs or
  the battle start differ. Compare the files frame by frame
  (`tests/zdxsv/replay_check.py`): the first frame whose inputs differ is the
  one to explain (a late input, a rollback that did not rerun it).

## 4. Find the first differing frame and value

Reproduce with traces on every peer (rig setups: [tests/zdxsv/README.md](../../tests/zdxsv/README.md)):

1. `ZDXSV_NET_TRACE=<file>` and `ZDXSV_PW_HASH=1` on every peer.
2. `tools/zdxsv/pwcheck.py <trace0> <trace1> [...]`: per player and RNG B
   the first differing frame and the runs of differing frames. `--battle CODE`
   for a trace of several battles.
3. Inputs and battle socket: `tools/zdxsv/zdcheck.py`, `tools/zdxsv/recvcheck.py`
   on the same traces.

## 5. Find the field and its writer

1. `ZDXSV_PW_DUMP=<file>` on every peer, rerun.
2. `pwdiff.py --from F-5 --to F+5 <dump0> <dump1> [...]`: the player work
   words that differ, their first frames and the values per machine.
3. `ZDXSV_EE_WATCH=<address>:<length>` on that word: every store into it with
   its pc, ra and GGPO frame. The first store whose value differs between the
   peers names the game code; its inputs are the next thing to compare.

## 6. A run that does not reproduce from its own inputs

The machine's state depends on something besides the inputs: the rollback,
the CPU core or a setting outside the fingerprint.

- Rollback: the synctest (`ZDXSV_GGPO` without `net=1`; `DIFF` lines name the
  player work offsets) and the delta state self-test (`ZDXSV_DELTA_TEST`).
  Either failing: the state save or load misses some state.
- CPU core (two builds, x86_64 and ARM64, or two recompiler settings): `ZDXSV_CYCLE_LOG`
  on the same replay in both; at the first split, `ZDXSV_EVENT_LOG` on both and
  `diff`: the first differing line is where the timing splits
  ([arm64.md](arm64.md), Comparing with an x86_64 build).
- Op results of a core: `ZDXSV_EE_OPTEST`, `ZDXSV_VU1_OPTEST` and their
  compare scripts.

## 7. Prove the fix

- The failing replay or rig run plays clean with the fix and still fails
  without it (the control).
- The online check: `ZDXSV_DESYNC_TEST=1` on one peer must report a mismatch
  on every check; without it, `mismatches 0`.
