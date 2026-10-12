# PCSX2 for zdxsv

A fork of [PCSX2](https://github.com/PCSX2/pcsx2) for playing Mobile Suit Gundam: Gundam vs. Zeta Gundam (PS2)
online on [zdxsv](https://github.com/inada-s/zdxsv).

- Players: see [ZDXSV.md](ZDXSV.md) for setup.
- Upstream base: PCSX2 `fd9d310c`. Main branch: `zdxsv-master`.
- License, BIOS requirement, and everything not listed here: same as upstream PCSX2.

## What this fork adds

One line per feature. How to turn each on and its tests: [docs/zdxsv/features.md](docs/zdxsv/features.md).
Everything is on only for the one game build it was made for; any other disc runs as in stock PCSX2.

### Online play

- Network defaults that point at the zdxsv server.
- Platform info: the lobby can tell an emulator from a real PS2.
- Lobby save states (opt-in, for debugging): save states made online keep working after a load.
- Settings page Settings → zdxsv (global or per game).
- Sync-safe settings while the game runs, as in RetroAchievements hardcore mode: cheats off; recompilers,
  EE/VU rounding and clamping, game fixes (GameDB only), extra memory, GS hardware download mode at
  their defaults; speedhacks, emulation speed and frame rate at their defaults during battles, replays
  and live spectating. An OSD message names the overridden settings; the settings page greys them out.
- Sync fingerprint (build, those settings and the recompiler settings, not the BIOS; an ARM64 build
  is not refused for its interpreter cores) in the platform info, logged as
  `ZdxsvSync: fingerprint <hex>`: a lobby battle whose
  players' fingerprints differ is cut, and the players return to the lobby.
- EE FPU interpreter computes as the EE recompiler: add/sub cut the smaller operand's mantissa, DIV.S
  uses the divide rounding mode, SQRT.S rounds to nearest (an interpreter peer, e.g. ARM64, stays in sync longer).

### Input latency

- Low-latency vsync: the finished frame is presented before the frame limiter sleeps.
  In a GGPO session the rollback also runs before the sleep, so it does not delay the next present
  (`ZDXSV_PRESENT_FIRST=0`: the old order, docs/zdxsv/options.md).

### Rollback netcode (GGPO)

- GGPO library in `3rdparty/ggpo`.
- Delta save states: the whole machine saved and loaded every frame.
- Faster rerun frames: no reverb, VU1 only in the last, the game's render callbacks skipped but in the last two
  (controls `ZDXSV_RERUN_REVERB`, `ZDXSV_RERUN_VU1`, `ZDXSV_RERUN_EE_DRAW`, `ZDXSV_RERUN_TAIL`,
  docs/zdxsv/options.md; A/B them with `tests/zdxsv/benchab.sh`, check the picture after rollbacks
  with `tests/zdxsv/rerunpic.sh`).
- GS snapshots (`ZDXSV_SNAP`, screenshots) are taken of shown frames only, never of a rollback rerun frame.
- Lobby battles over GGPO (in development, on by default): ping test, relay servers, match report,
  connectivity and HTTPS latency tests, network status OSD.
- Load barriers: battle start and MS select wait for every peer's load (every peer needs a build with both).
- GGPO network pump thread: sockets are read and written every 1 ms, not once per frame, as flycast
  (`ZDXSV_NET_PUMP=0`: off, docs/zdxsv/options.md).

### Replays

- GGPO battles saved as gdxsv-style `.pb` files, optionally uploaded to a replay server.
- Playback from a file, a URL or a battle code: seek, point of view, control bar, key display, mobile suit
  selection skip, round jump, takeover, four-screen view.
- Live spectating of lobby battles.
- Replay window (Tools → zdxsv Replays): local replay files, the lobby's replay search and its live battles; Play
  boots the game from the game list with the chosen replay and point of view.

### Releases and tools

- Auto-updater and release workflow for this fork.
- Scripts for local builds and for running several instances side by side.
- macOS deps scripts: no `qt110-xcode264.patch` (Qt 6.11.1 already has it; Apple `patch` reverted it,
  which broke arm64 Qt builds on Xcode 26.6).
- ARM64 VU1 recompiler (`pcsx2/arm64/VU1Trace.inl`, included by `pcsx2/VUops.cpp`; the VU1
  interpreter itself is upstream's): runs of pairs up to a branch delay slot become native traces
  whose stall and pipeline-flush decisions are worked out at compile time per pipeline state at
  entry; pairs with E/D/T bits or XGKICK run through the interpreter. Upper ADD/SUB/MUL/MADD/MSUB
  (all A/bc/i/q forms but ADDi; not Fd = 0) and OPMULA/OPMSUB are NEON code with MAC/status flags when VU0 overflow
  clamping is on; upper MAX/MINI/FTOI/ITOF/ABS/CLIP, lower MOVE/LQ/SQ/LQI/LQD/SQI/SQD, integer
  ALU, ILW/ISW(R), MFIR/MTIR/MR32/MFP, flag ops, branches and (with clamping) DIV/SQRT/RSQRT and EFU ops but EATAN/EEXP/ESIN are native too; other ops (EATAN/EEXP/ESIN,
  the remaining FMAC forms) call the interpreter handlers; the FDIV/EFU pipe entries are native. TPC, cycle and code
  are stored only before a handler call that may read them and at trace exit; so are the FMAC
  pipe positions and count (tracked at compile time from the read position at entry). A trace re-reads its code only after a
  VU1 micro memory write (`InterpVU1::Clear`) or reset. A trace keeps its pipeline state at exit
  (taken after its first run) and the traces that ran after it, so back-to-back traces skip the
  state build and lookup; with traces on, a trace's exit jumps straight into such a successor
  when `InterpVU1::Execute` would run it next (branch, E bit, VPU_STAT, cycle budget), as
  microVU links its blocks. The branch countdown and FDIV/EFU result writes are native. Same results as the interpreter. Env `PCSX2_VU1_JIT`: `0` = interpreter only, `check` = every trace is also run by the
  interpreter from the same start and the VU1 registers and memory compared, and every kept exit
  state compared with the real one (log `VU1 trace check`), unset = traces.
  Env `PCSX2_VU1_JIT_MAP=<file>`: start address and kind of each emitted trace section (and the
  handler it calls), to attribute `sample` addresses in VU1 JIT code to per-step work.
  Env `PCSX2_VU1_JIT_SELFTEST=<n>`: at the first trace compile, each native DIV/SQRT/RSQRT, EFU and OPMULA/OPMSUB
  op and its handler run on the same <n> generated inputs; log `VU1 JIT selftest: <op> <n> checked,
  <k> differ` (and the first inputs that differ; outputs Q, P, status, MAC, ACC, Fd). VU1 is restored afterwards.
- VU1 op test (`pcsx2/Zdxsv/VuOpTest.cpp`, all platforms): env `ZDXSV_VU1_OPTEST=<file>[,<n>[,finite]]`
  runs 49 VU1 ops (FMAC, FDIV, EFU, conversions, CLIP) as short micro programs through the VU1 core
  in use (x86 microVU, ARM64 traces or interpreter) on <n> generated inputs at the first VU1 micro
  program start, and writes inputs and outputs to <file>. `tests/zdxsv/vu1_optest_cmp.py` compares
  the files of two builds (x86_64 vs ARM64) op by op. Details: [options](docs/zdxsv/options.md).
- EE FPU op test (`pcsx2/Zdxsv/EeOpTest.cpp`, all platforms): env `ZDXSV_EE_OPTEST=<file>[,<n>[,finite]]`
  runs 24 COP1 ops (arithmetic, multiply-add, SQRT/RSQRT, MAX/MIN, compares, conversions) as a guest
  loop through the EE core in use on <n> generated inputs at vsync 300, and writes inputs and outputs
  (Fd or ACC, FCR31) to <file>. `tests/zdxsv/ee_optest_cmp.py` compares the files of two builds.
- ARM64 VU interpreter (`pcsx2/VUops.cpp`, `ARCH_ARM64`) and VU1 recompiler: DIV/SQRT/RSQRT and the
  EFU ops compute as x86 microVU does (unclamped inputs, its op order, flags and NaN), so ARM64 and
  x86_64 peers get the same Q, P and D/I flags; FDIV D/I also set DS/IS. x86_64 builds unchanged.
- ARM64 EE FPU (`pcsx2/FPU.cpp`, `ARCH_ARM64`): CFC1, SQRT.S, RSQRT.S, MAX.S, MIN.S compute as the x86
  recompiler (`pcsx2/x86/iFPU.cpp`) does: CFC1 masks FCR31 and reads FCR0 for 0..15, SQRT/RSQRT set I on
  any sign bit (also -0), RSQRT x/0 takes the sign of Fs and sets I for 0/0, MAX/MIN clamp NaN/Inf, count
  denormals as 0 and on +0 vs -0 return Ft (Fs when Fd = Ft), and keep O/U. x86_64 builds unchanged.
- ARM64 EE recompiler (`pcsx2/arm64/iR5900.cpp`, experimental): blocks compiled into calls of the
  interpreter's handlers, with the x86 recompiler's timing, so ARM64 and x86_64 peers count the same
  EE cycles. On when the EE recompiler setting is on (`EmuCore/CPU/Recompiler` `EnableEE`, the
  default); off = EE interpreter (its own timing, as upstream). VU stays the interpreter on ARM64.
  Timing as the x86 recompiler: same block layout (end at a branch + delay slot, at a backward target
  inside the block, at SYSCALL/BREAK/ERET, at a 4 KB page, before a compiled block); each block end
  commits `cycle += max(blockcycles >> 3, 1)` with the fraction dropped; both paths of every branch
  (also BLTZAL..BGEZALL, BC0x/BC1x/BC2x) commit and test events; ERET/EI/traps force an event test;
  a split block tests events only after more than 6 instructions; LB..LW test events after an EE
  counter read (0x10000000-0x10001fff) only at a block-constant address; a FlushCache SYSCALL with a
  constant v1 adds 5650 cycles instead of the call; timeout loop skip (`recSkipTimeoutLoop`, only in a block that branches to its own start).
  Integer ALU, shift, HI/LO move and MOVZ/MOVN ops are emitted as native code, computed as the x86
  recompiler does: ADD/SUB/ADDI/DADD/DSUB/DADDI write their result on overflow (the interpreter skips
  the write). COP1 ADD.S/SUB.S/MUL.S/DIV.S/MOV.S/MFC1/MTC1 are native as the interpreter (FPU.cpp:
  fpuDouble inputs, cut add/sub operand, overflow/underflow clamps and flags, DIV.S in the divide
  rounding mode). COP2 macro FMAC ops (VADD..VMSUBA, not VADDi; VOPMULA/VOPMSUB) are native with the VU1 JIT's FMAC
  code on VU0 when VU0 runs no micro program (else the handler, which finishes it first).
  Branches jump to the next compiled block without returning.
  `ZDXSV_CYCLE_LOG=n` (docs/zdxsv/options.md) logs the EE/IOP cycle counters in a replay to compare
  two builds. Env `PCSX2_ARM64_EE_NATIVE=0`: every op calls its interpreter handler (A/B check).
  Events (counters, interrupts, IOP and VU runs) are tested only once the cycle target is reached, as
  the x86 recompiler does (the EE interpreter tests them at every branch).
  Wait loops as the x86 recompiler (`s_nBlockFF`, `Speedhacks.WaitLoop`): a block branching to its own
  start with only loads, ALU ops on loaded or constant values, mfc/cfc, cache and sync skips to the next
  event on that branch (e.g. a GS CSR VSINT poll), so the EE cycles differ from the EE interpreter there.
  Env `PCSX2_EE_JIT_MAP=<file>`: host address of each compiled instruction (pc, name, code word).
  MFC0 Count, MFPC, MTC0 Status/Count, MTPS/MTPC commit the block's cycles first, as `iCOP0.cpp`. A
  state loaded while the pre-ELF interpreter loop runs (a replay's common state during boot) switches
  to compiled blocks (`Interpreter.cpp`), and a state load drops the interpreter's uncommitted cycles
  (`SaveState.cpp`): before, the whole lobby phase of a replay ran on the interpreter.
  `ZDXSV_EVENT_LOG=<file>[,<lines>[,<from>]]`: one line per EE/IOP event test from the replay's common state
  load; `diff` an ARM64 and an x86 file for the first timing split. With `<from>`, after that many event
  lines also one `B ee <cycle> <pc>` line per EE recompiler block entry (both recompilers), to find the block.
  An exit request (VM stop) made on the CPU thread between two Execute calls is kept for the next
  event test, as x86's `eeRecExitRequested` (`Interpreter.cpp`): it jumped into the returned Execute
  frame, so ARM64 crashed (SIGBUS, pc 0xff) at every VM stop after a settings reload (replay end, rbk exit).
- ARM64 IOP recompiler (`pcsx2/arm64/iR3000A.cpp`, experimental) with the x86 IOP recompiler's timing, so
  ARM64 and x86 hosts count the same IOP cycles: x86 block layout, cycles added at the block end (MULT/DIV
  penalties), timeslice and event tests at every jump or branch, nop loop fast-forward, block clears only
  on a write to a block start, IRX import HLE. Integer ALU, shift and HI/LO move ops native, others call
  the interpreter's handlers. On when `EmuCore/CPU/Recompiler` `EnableIOP` is on (the default); compare
  with an x86 build via `ZDXSV_CYCLE_LOG`.
- Options for tests, traces and game investigation; every build reads them.

## Documentation

| Document | Content |
|---|---|
| [ZDXSV.md](ZDXSV.md) | Player setup. Shipped in the release zip. |
| [docs/zdxsv/features.md](docs/zdxsv/features.md) | Every feature with how to turn it on and its tests. |
| [docs/zdxsv/options.md](docs/zdxsv/options.md) | Every option and setting: name, default, use; the game check. |
| [docs/zdxsv/lobby.md](docs/zdxsv/lobby.md) | Network defaults, lobby messages, ping test, relay servers, how a lobby battle starts. |
| [docs/zdxsv/rollback.md](docs/zdxsv/rollback.md) | GGPO library, delta save states, the GGPO session, sync checks. |
| [docs/zdxsv/replay.md](docs/zdxsv/replay.md) | Replay files, upload, playback and its controls, live spectating. |
| [docs/zdxsv/tools.md](docs/zdxsv/tools.md) | Releases, CI, local build and run scripts, PINE commands. |
| [pcsx2/Zdxsv/README.md](pcsx2/Zdxsv/README.md) | Glossary and source file map. |
| [tests/zdxsv/README.md](tests/zdxsv/README.md) | Rig test scripts: setup and machine settings. |
| [AGENTS.md](AGENTS.md) | Rules for work in this fork. |
