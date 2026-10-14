# ARM64 builds

An ARM64 build (Apple M) computes and times the game as an x86_64 build does,
so ARM64 and x86_64 peers stay in sync. Everything here is ARM64-only code
(`ARCH_ARM64`) unless it says otherwise; x86_64 builds are unchanged by it.
Its options: [options.md](options.md#arm64-cores).

## CPU cores

| Core | ARM64 | Turned off by |
|---|---|---|
| EE | recompiler `pcsx2/arm64/iR5900.cpp` (experimental) | `EmuCore/CPU/Recompiler` `EnableEE` off: EE interpreter, with its own timing |
| IOP | recompiler `pcsx2/arm64/iR3000A.cpp` (experimental) | `EnableIOP` off |
| VU1 | trace recompiler `pcsx2/arm64/VU1Trace.inl` (included by `pcsx2/VUops.cpp`) on upstream's VU1 interpreter | `PCSX2_VU1_JIT=0` |
| VU0 | interpreter | - |

The sync fingerprint does not refuse an ARM64 build for its interpreter cores.

## EE recompiler

Blocks are compiled into native code and calls of the interpreter's handlers,
with the x86 recompiler's timing, so both hosts count the same EE cycles.

- Block layout as x86: a block ends at a branch + delay slot, at a backward
  target inside the block, at SYSCALL/BREAK/ERET, at a 4 KB page, before a
  compiled block. Branches jump to the next compiled block without returning,
  past its prologue (same frame); a constant target's lookup entry is resolved
  at compile time.
- Each block end commits `cycle += max(blockcycles >> 3, 1)`, fraction dropped.
- Both paths of every branch (also BLTZAL..BGEZALL, BC0x/BC1x/BC2x) commit and
  test events; ERET/EI/traps force an event test; a split block tests events
  only after more than 6 instructions.
- Events (counters, interrupts, IOP and VU runs) are tested only once the cycle
  target is reached (the EE interpreter tests them at every branch).
- LB..LW test events after an EE counter read (0x10000000-0x10001fff) only at
  a block-constant address.
- A FlushCache SYSCALL with a constant v1 adds 5650 cycles instead of the call.
- MFC0 Count, MFPC, MTC0 Status/Count, MTPS/MTPC commit the block's cycles
  first, as `iCOP0.cpp`.
- Timeout loop skip (`recSkipTimeoutLoop`), only in a block that branches to its
  own start.
- Wait loops as x86 (`s_nBlockFF`, `Speedhacks.WaitLoop`): a block branching to
  its own start with only loads, ALU ops on loaded or constant values,
  mfc/cfc, cache and sync skips to the next event on that branch (e.g. a GS
  CSR VSINT poll).
- Native ops:
  - Integer ALU, shift, HI/LO move, MOVZ/MOVN, computed as x86:
    ADD/SUB/ADDI/DADD/DSUB/DADDI write their result on overflow.
  - COP1 ADD.S/SUB.S/MUL.S/DIV.S/MOV.S/MFC1/MTC1, as the interpreter
    (`FPU.cpp`, see [EE FPU](#ee-fpu)).
  - COP2 macro FMAC ops (VADD..VMSUBA but VADDi, VOPMULA/VOPMSUB) with the VU1
    recompiler's FMAC code on VU0, when VU0 runs no micro program (else the
    handler, which finishes it first).
- A state loaded while the pre-ELF interpreter loop runs (a replay's common
  state during boot) switches to compiled blocks (`Interpreter.cpp`); a state
  load drops the interpreter's uncommitted cycles (`SaveState.cpp`).
- An exit request (VM stop) made on the CPU thread between two Execute calls is
  kept for the next event test, as x86's `eeRecExitRequested`.
- The Zdxsv EE hooks run through `Zdxsv/IntHooks.cpp`.

## IOP recompiler

- x86 block layout; cycles added at the block end (MULT/DIV penalties);
  timeslice and event tests at every jump or branch; nop loop fast-forward;
  blocks cleared only on a write to a block start; IRX import HLE.
- Integer ALU, shift and HI/LO move ops native; others call the interpreter's
  handlers.

## VU1 recompiler

Same results as the interpreter.

- A trace = a run of instruction pairs up to a branch delay slot. Its stall
  and pipeline-flush decisions are worked out at compile time per pipeline
  state at entry. Pairs with E/D/T bits or XGKICK run through the interpreter.
- Native:
  - Upper ADD/SUB/MUL/MADD/MSUB (all A/bc/i/q forms but ADDi; not Fd = 0),
    OPMULA/OPMSUB, with MAC/status flags, when VU0 overflow clamping is on.
  - Upper MAX/MINI/FTOI/ITOF/ABS/CLIP.
  - Lower MOVE/LQ/SQ/LQI/LQD/SQI/SQD, integer ALU, ILW/ISW(R),
    MFIR/MTIR/MR32/MFP, flag ops, branches and the branch countdown.
  - With clamping: DIV/SQRT/RSQRT and the EFU ops but EATAN/EEXP/ESIN.
  - The FDIV/EFU pipe entries and result writes.
- Other ops (EATAN/EEXP/ESIN, the remaining FMAC forms) call the interpreter
  handlers.
- TPC, cycle and code, and the FMAC pipe positions and count (tracked at
  compile time from the read position at entry), are stored only before a
  handler call that may read them and at trace exit.
- An FMAC pipe entry that retires inside the trace before any handler or
  XGKICK call (which read the pipe) stores only its MAC/status/clip result;
  only entries still pending there are written in full. Retire flag updates
  are picked at compile time.
- Native FMAC ops with x86 NaN rules: when no lane of the result is NaN, no
  operand or product was NaN, so only FTZ is applied; a NaN result reruns the
  op with the full x86 NaN rules out of line after the trace.
- Within a trace ACC stays in a register across native FMAC ops, and operand
  lanes a native x86 FMAC op wrote in the trace (FTZ done) skip DAZ.
- With the VU1 FPCR at DAZ/FTZ (FZ) and round toward zero (the default),
  native x86 FMAC ops emit no DAZ/FTZ code: FZ flushes denormal operands and
  results in hardware. FZ tests tininess before rounding, x86 after it; toward
  zero both give the same result.
- Within a trace VI[STATUS], VI[MAC] and the flags of entries that retire in
  it live in registers; they are stored before handler calls, native flag ops
  (FSAND.., FMAND..) and at exit. An FMAC entry whose MAC/status the next
  in-trace retire replaces before any flag op reads them computes only its
  sticky status bits (OR-accumulated, folded in at the next store).
- Up to 16 traces per address (pipe state and code differ): programs that the
  game uploads in turn to the same micro memory keep their code.
- A trace re-reads its code only after a VU1 micro memory write
  (`InterpVU1::Clear`) or reset.
- A trace keeps its pipeline state at exit (taken after its first run) and the
  traces that ran after it: back-to-back traces skip the state build and
  lookup, and a trace's exit jumps straight into such a successor when
  `InterpVU1::Execute` would run it next (branch, E bit, VPU_STAT, cycle
  budget), as microVU links its blocks. The jump keeps the stack frame and
  callee-saved registers of the first trace (no restore and save in between).
  A successor found again from C++ gets a direct branch patched into that
  jump; replacing a successor entry first restores the indirect jump.

## Math as x86

The ARM64 interpreters and recompilers follow what x86 SSE code computes, in
the default clamp mode (other clamp modes keep upstream's code).

### EE FPU

- All builds: the EE FPU interpreter computes as the EE recompiler: add/sub cut
  the smaller operand's mantissa, DIV.S uses the divide rounding mode, SQRT.S
  rounds to nearest.
- As `pcsx2/x86/iFPU.cpp` (interpreter `pcsx2/FPU.cpp` and native
  `pcsx2/arm64/iR5900.cpp`):
  - ADD/SUB/MUL/DIV/RSQRT/MADD/MSUB (and the ACC forms): operands unclamped,
    denormals as 0; a NaN result is +Fmax, an infinity +/-Fmax; O/U left alone;
    DIV.S clears D/I first.
  - ABS clamps to +Fmax and flushes denormals; NEG clamps NaN/Inf, keeps
    denormals.
  - CFC1 masks FCR31 and reads FCR0 for 0..15.
  - SQRT/RSQRT set I on any sign bit (also -0); RSQRT x/0 takes the sign of Fs
    and sets I for 0/0.
  - MAX/MIN clamp NaN/Inf, count denormals as 0, on +0 vs -0 return Ft (Fs when
    Fd = Ft), keep O/U.

### VU (interpreter `pcsx2/VUops.cpp`, VU1 recompiler, native COP2 macro ops)

- DIV/SQRT/RSQRT and EFU ops as microVU: unclamped inputs, its op order, flags
  and NaN; of two NaN operands the first, as SSE. FDIV D/I also set DS/IS.
- FMAC operand clamp: the operands microVU clamps per op (SUB, MUL, MULA,
  MADD/MADDA broadcast, COP2 MADDw/MSUB; `clampType` in `microVU_Upper.inl`)
  turn a NaN of either sign into +max.
- FMAC NaN/Inf, ADD/SUB/MUL/MADD/MSUB (all forms, OPMULA/OPMSUB): unclamped
  operands keep Inf/NaN (DAZ only); a NaN operand passes quieted (Fs, or ACC in
  MADDA/MSUB*, first); an invalid op gives `0xffc00000`; results are not
  clamped; MAC gets S and Z only (no O/U). SUBi/MULi `.xyzw` clamp I as x86.
- VU0 status flag in COP2 macro mode (`VU0.cpp` too): FMAC ops keep the D/I
  flags of the last VDIV/VSQRT/VRSQRT; CTC2 to the status flag writes only the
  sticky bits (6-11).

## Comparing with an x86_64 build

- Op results: `ZDXSV_VU1_OPTEST` (`pcsx2/Zdxsv/VuOpTest.cpp`) and
  `ZDXSV_EE_OPTEST` (`pcsx2/Zdxsv/EeOpTest.cpp`) on both builds, then
  `vu1_optest_cmp.py` / `ee_optest_cmp.py` ([tools.md](tools.md)).
- Timing: `ZDXSV_CYCLE_LOG` in the same replay on both builds; at a split,
  `ZDXSV_EVENT_LOG` on both and `diff` for the first differing event test.
- Recompiler self-checks: `PCSX2_VU1_JIT=check`, `PCSX2_VU1_JIT_SELFTEST`,
  `PCSX2_ARM64_EE_NATIVE=0`.
- Profiles of JIT code: `PCSX2_EE_JIT_MAP`, `PCSX2_VU1_JIT_MAP`.
