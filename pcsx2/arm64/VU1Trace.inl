// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARM64 VU1 recompiler. Included at the end of VUops.cpp: it uses the interpreter's stall and
// pipeline functions to work out each trace's decisions at compile time.

#include "arm64/AsmHelpers.h"
#include "R5900OpcodeTables.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <memory>
#include <unordered_map>
#include <vector>
// Per-pair decode: final handlers (sub-tables resolved), register usage and the upper/lower
// register backup decisions, as _vu1Exec in VU1microInterp.cpp works them out on every step.
// An entry is re-decoded when the micro memory under it changes (the code words are compared).
struct VU1DecodedPair
{
	u64 code;
	FnPtr_VuVoid upper;
	FnPtr_VuVoid lower;
	_VURegsNum uregs;
	_VURegsNum lregs;
	u8 valid;
	u8 ibit;
	u8 discard;
	u8 vfreg;
	u8 vireg;
	u8 fmac;
};

static VU1DecodedPair s_vu1_decoded[VU1_PROGSIZE / 8];

// The handler the sub-table dispatchers (which only index by VU1.code) would call.
static FnPtr_VuVoid _vu1ResolveUpper(u32 code)
{
	const FnPtr_VuVoid f = VU1_UPPER_OPCODE[code & 0x3f];
	const u32 sub = (code >> 6) & 0x1f;
	if (f == VU1_UPPER_FD_00)
		return VU1_UPPER_FD_00_TABLE[sub];
	if (f == VU1_UPPER_FD_01)
		return VU1_UPPER_FD_01_TABLE[sub];
	if (f == VU1_UPPER_FD_10)
		return VU1_UPPER_FD_10_TABLE[sub];
	if (f == VU1_UPPER_FD_11)
		return VU1_UPPER_FD_11_TABLE[sub];
	return f;
}

static FnPtr_VuVoid _vu1ResolveLower(u32 code)
{
	FnPtr_VuVoid f = VU1_LOWER_OPCODE[code >> 25];
	if (f != VU1LowerOP)
		return f;
	f = VU1LowerOP_OPCODE[code & 0x3f];
	const u32 sub = (code >> 6) & 0x1f;
	if (f == VU1LowerOP_T3_00)
		return VU1LowerOP_T3_00_OPCODE[sub];
	if (f == VU1LowerOP_T3_01)
		return VU1LowerOP_T3_01_OPCODE[sub];
	if (f == VU1LowerOP_T3_10)
		return VU1LowerOP_T3_10_OPCODE[sub];
	if (f == VU1LowerOP_T3_11)
		return VU1LowerOP_T3_11_OPCODE[sub];
	return f;
}

static void _vu1Decode(VU1DecodedPair& d, const u32* ptr)
{
	VURegs* VU = &VU1;
	std::memset(&d, 0, sizeof(d));
	d.code = static_cast<u64>(ptr[0]) | (static_cast<u64>(ptr[1]) << 32);
	d.upper = _vu1ResolveUpper(ptr[1]);
	d.lower = _vu1ResolveLower(ptr[0]);
	VU->code = ptr[1];
	VU1regs_UPPER_OPCODE[VU->code & 0x3f](&d.uregs);
	d.ibit = (ptr[1] & 0x80000000) != 0;
	if (!d.ibit)
	{
		VU->code = ptr[0];
		VU1regs_LOWER_OPCODE[VU->code >> 25](&d.lregs);
		if (d.uregs.VFwrite)
		{
			if (d.lregs.VFwrite == d.uregs.VFwrite)
				d.discard = 1;
			if (d.lregs.VFread0 == d.uregs.VFwrite || d.lregs.VFread1 == d.uregs.VFwrite)
				d.vfreg = d.uregs.VFwrite;
		}
		if (d.uregs.VIwrite & (1 << REG_CLIP_FLAG))
		{
			if (d.lregs.VIwrite & (1 << REG_CLIP_FLAG))
				d.discard = 1;
			if (d.lregs.VIread & (1 << REG_CLIP_FLAG))
				d.vireg = REG_CLIP_FLAG;
		}
	}
	d.fmac = d.uregs.pipe == VUPIPE_FMAC || d.lregs.pipe == VUPIPE_FMAC;
	d.valid = 1;
}

// VU1 traces: runs of pairs from one entry PC up to a branch delay slot, compiled for one state
// of the FMAC/IALU/FDIV/EFU pipes at entry. _vu1Exec's stall and flush decisions depend
// only on that state (remaining cycles and registers of the pending entries) and on the code,
// so they are worked out once at compile time by running the same functions on a copy of VU1.
// At run time only their effects are applied: the new cycle and how many entries retire.
// Pairs with E/D/T bits or XGKICK end a trace; the interpreter steps them.

// Pipeline state at trace entry, the trace lookup key. Bytes keep it at 32 so the compare is inline.
struct VU1PipeSig
{
	u8 fmac[4][5]; // remaining cycles, regupper, xyzwupper, reglower, xyzwlower
	u8 ialu[4][2]; // remaining cycles, reg
	u8 fdiv; // remaining cycles + 1, 0 = off
	u8 efu;
	u8 fmaccount;
	u8 ialucount;
};
static_assert(sizeof(VU1PipeSig) == 32);

struct VU1TraceStep
{
	u64 code;
	VU1DecodedPair d;
	u32 cycle_rel; // VU1.cycle after the stall checks, relative to the cycle at trace entry
	u8 fmac_retire;
	u8 ialu_retire;
	u8 fdiv_retire;
	u8 efu_retire;
	u8 efu_dec; // the EFU stall check shortened efu.Cycle by one
};

struct VU1Trace
{
	VU1PipeSig sig;
	u32 last_start_rel; // cycle before the last step, relative to the cycle at trace entry
	std::vector<VU1TraceStep> steps;
	void (*code)() = nullptr; // native code, compiled on first run
	u32 micro_gen = 0; // s_vu1_micro_gen when the steps were last compared with VU1.Micro
	// Pipe state at trace exit. Like the stalls, it depends only on sig and the code, so it is
	// taken once, after the first run: 0 = not yet, 1 = exit_sig, 2 = not representable.
	u8 exit_known = 0;
	VU1PipeSig exit_sig;
	// Traces that ran next, looked up with exit_sig; valid while epoch == s_vu1_trace_epoch.
	struct
	{
		u32 pc;
		u32 epoch;
		VU1Trace* t;
	} succ[2] = {};
};

static constexpr u32 VU1_TRACE_VARIANTS = 16;
static constexpr u32 VU1_TRACE_MAX_STEPS = 256;

struct VU1TraceSlot
{
	std::unique_ptr<VU1Trace> variants[VU1_TRACE_VARIANTS];
	u32 next;
};

static VU1TraceSlot s_vu1_traces[VU1_PROGSIZE / 8];

// Bumped by every VU1 micro memory write (InterpVU1::Clear) and reset: a trace compares its
// steps with VU1.Micro once per generation instead of on every run.
static u32 s_vu1_micro_gen = 1;

// Bumped when a trace is freed: invalidates every VU1Trace::succ.
static u32 s_vu1_trace_epoch = 1;

// The trace that ran last, while nothing else has touched VU1 since its exit.
static VU1Trace* s_vu1_prev;

// Trace chaining: a trace's exit jumps straight into a successor from its succ when
// InterpVU1::Execute would run that one next anyway.
static struct
{
	u64 limit; // the trace's start cycle must stay below this: startcycles + cycles of Execute
	VU1Trace* last; // the trace that ran last, written by the chain jump
} s_vu1_chain;

void _vu1TraceClear()
{
	s_vu1_micro_gen++;
}

// Called by InterpVU1::Execute before an interpreter step and at entry.
void _vu1TraceForget()
{
	s_vu1_prev = nullptr;
}

static bool _vu1PipeSig(const VURegs* VU, VU1PipeSig* s)
{
	std::memset(s, 0, sizeof(*s));
	if (VU->fmaccount > 4 || VU->ialucount > 4 ||
		VU->fmacwritepos != ((VU->fmacreadpos + VU->fmaccount) & 3) ||
		VU->ialuwritepos != ((VU->ialureadpos + VU->ialucount) & 3))
		return false;

	const auto remaining = [VU](u64 sCycle, u32 Cycle) -> u64 {
		const u64 end = sCycle + Cycle;
		return end > VU->cycle ? end - VU->cycle : 0;
	};

	for (u32 i = 0, p = VU->fmacreadpos; i < VU->fmaccount; i++, p = (p + 1) & 3)
	{
		const u64 r = remaining(VU->fmac[p].sCycle, VU->fmac[p].Cycle);
		if (r > 0xff || (VU->fmac[p].regupper | VU->fmac[p].reglower | VU->fmac[p].xyzwupper | VU->fmac[p].xyzwlower) > 0xff)
			return false;
		// A ready entry never stalls: only its place in the queue matters.
		if (r == 0)
			continue;
		s->fmac[i][0] = static_cast<u8>(r);
		s->fmac[i][1] = VU->fmac[p].regupper;
		s->fmac[i][2] = VU->fmac[p].xyzwupper;
		s->fmac[i][3] = VU->fmac[p].reglower;
		s->fmac[i][4] = VU->fmac[p].xyzwlower;
	}
	for (u32 i = 0, p = VU->ialureadpos; i < VU->ialucount; i++, p = (p + 1) & 3)
	{
		const u64 r = remaining(VU->ialu[p].sCycle, VU->ialu[p].Cycle);
		if (r > 0xff || static_cast<u32>(VU->ialu[p].reg) > 0xff)
			return false;
		if (r == 0)
			continue;
		s->ialu[i][0] = static_cast<u8>(r);
		s->ialu[i][1] = static_cast<u8>(VU->ialu[p].reg);
	}
	if (VU->fdiv.enable)
	{
		const u64 r = remaining(VU->fdiv.sCycle, VU->fdiv.Cycle);
		if (r > 0xfe)
			return false;
		s->fdiv = static_cast<u8>(r + 1);
	}
	if (VU->efu.enable)
	{
		const u64 r = remaining(VU->efu.sCycle, VU->efu.Cycle);
		if (r > 0xfe)
			return false;
		s->efu = static_cast<u8>(r + 1);
	}
	s->fmaccount = VU->fmaccount;
	s->ialucount = VU->ialucount;
	return true;
}

static __fi bool _vu1TraceEnds(const u32* ptr, const VU1DecodedPair& d)
{
	return (ptr[1] & 0x58000000) || d.lregs.pipe == VUPIPE_XGKICK; // E, D, T flags, XGKICK
}

static __fi bool _vu1IsBranch(const u32* ptr, const VU1DecodedPair& d)
{
	return !d.ibit && ((ptr[0] >> 25) & 0x70) == 0x20; // B, BAL, JR, JALR, IBxx
}

static VU1DecodedPair& _vu1DecodedAt(u32 pc)
{
	const u32* ptr = reinterpret_cast<const u32*>(&VU1.Micro[pc]);
	VU1DecodedPair& d = s_vu1_decoded[pc >> 3];
	const u64 code = static_cast<u64>(ptr[0]) | (static_cast<u64>(ptr[1]) << 32);
	if (!d.valid || d.code != code)
		_vu1Decode(d, ptr);
	return d;
}

static std::unique_ptr<VU1Trace> _vu1TraceCompile(u32 pc, const VU1PipeSig& sig)
{
	alignas(16) static VURegs sim;
	std::memcpy(&sim, &VU1, sizeof(VURegs));
	const u32 saved_code = VU1.code;
	const u64 base = sim.cycle;

	auto t = std::make_unique<VU1Trace>();
	t->sig = sig;
	bool delay_slot = false;
	while (pc < VU1_PROGSIZE && t->steps.size() < VU1_TRACE_MAX_STEPS)
	{
		const u32* ptr = reinterpret_cast<const u32*>(&VU1.Micro[pc]);
		const VU1DecodedPair& d = _vu1DecodedAt(pc);
		const bool branch = _vu1IsBranch(ptr, d);
		if (_vu1TraceEnds(ptr, d) || (delay_slot && branch))
			break;

		VU1TraceStep& s = t->steps.emplace_back();
		s.code = d.code;
		s.d = d;
		t->last_start_rel = static_cast<u32>(sim.cycle - base);

		sim.cycle++;
		const u32 efu_cycle = sim.efu.Cycle;
		_vuTestUpperStalls(&sim, &s.d.uregs);
		if (!d.ibit)
			_vuTestLowerStalls(&sim, &s.d.lregs);
		s.cycle_rel = static_cast<u32>(sim.cycle - base);
		s.efu_dec = sim.efu.Cycle != efu_cycle;

		const u32 fmaccount = sim.fmaccount;
		const u32 ialucount = sim.ialucount;
		const int fdiv = sim.fdiv.enable;
		const int efu = sim.efu.enable;
		_vuTestPipes(&sim); // not VU1: no XGKICK transfer
		s.fmac_retire = static_cast<u8>(fmaccount - sim.fmaccount);
		s.ialu_retire = static_cast<u8>(ialucount - sim.ialucount);
		s.fdiv_retire = fdiv && !sim.fdiv.enable;
		s.efu_retire = efu && !sim.efu.enable;

		if (d.fmac)
			_vuClearFMAC(&sim);
		_vuAddUpperStalls(&sim, &s.d.uregs);
		_vuAddLowerStalls(&sim, &s.d.lregs);
		if (d.fmac)
			sim.fmacwritepos = (sim.fmacwritepos + 1) & 3;

		pc += 8;
		if (delay_slot)
			break;
		delay_slot = branch;
	}
	VU1.code = saved_code;
	return t;
}

// Each trace becomes native code doing _vu1Exec's work for its steps, with the per-step
// decisions (retires, register backups, handlers, stall entries) resolved at compile time.
namespace a64 = vixl::aarch64;

static ArmConstantPool s_vu1_jit_pool;
static u8* s_vu1_jit_start;
static u8* s_vu1_jit_ptr;
static u8* s_vu1_jit_end;
static constexpr u32 VU1_JIT_POOL_SIZE = 64 * 1024;
static constexpr u32 VU1_JIT_MAX_STEP_CODE = 1024;
// PCSX2_VU1_JIT: 0 = interpreter only, check = see _vu1TraceCheck, unset = traces.
enum class VU1JitMode { Off, On, Check };
static const VU1JitMode s_vu1_jit_mode = [] {
	const char* env = std::getenv("PCSX2_VU1_JIT");
	if (env && std::strcmp(env, "0") == 0)
		return VU1JitMode::Off;
	if (env && std::strcmp(env, "check") == 0)
		return VU1JitMode::Check;
	return VU1JitMode::On;
}();
// PCSX2_VU1_JIT_MAP=<file>: start address of each emitted section, what it does and the handler
// it calls, to attribute `sample` addresses in JIT code.
static FILE* const s_vu1_jit_map = [] {
	const char* path = std::getenv("PCSX2_VU1_JIT_MAP");
	return path ? std::fopen(path, "w") : nullptr;
}();
static void _vu1JitMark(const char* what, const void* fn = nullptr)
{
	if (!s_vu1_jit_map)
		return;
	static std::unordered_map<const void*, const char*> names;
	const char*& sym = names[fn];
	if (!sym)
	{
		Dl_info info;
		sym = (fn && dladdr(fn, &info) && info.dli_sname) ? info.dli_sname : "-";
	}
	std::fprintf(s_vu1_jit_map, "%p %s %s\n", armGetCurrentCodePointer(), what, sym);
}

// As _vuFDIVflush and _vuEFUflush do once the result is due.
static void _vu1JitXGKICK() { _vuXGKICKTransfer((VU1.cycle - VU1.xgkicklastcycle) - 1, false); }

template <typename T>
static s64 _vu1Off(const T& field)
{
	return reinterpret_cast<const u8*>(&field) - reinterpret_cast<const u8*>(&VU1);
}

// The JIT keeps &VU1 in x19, the cycle at trace entry in x20 and s_vu1_jit_fconst in x21.
static const a64::Register& s_vu1_jit_rvu = a64::x19;

static a64::MemOperand _vu1JitMem(s64 off)
{
	return a64::MemOperand(s_vu1_jit_rvu, off);
}

// TPC, cycle and code of the current step: native code knows them at compile time, so they are
// stored only before a call that may read them and at trace exit.
// The FMAC read/write positions and count too: the count is part of the trace key, the read
// position at entry is in w27, entries retired since then are known.
struct VU1JitPending
{
	bool tpc, cycle, code, fmac;
	u32 tpc_v, cycle_rel, code_v;
	u32 fmac_ret, fmac_cnt;
};
static VU1JitPending s_vu1_jit_pending;
// The FMAC constant registers (_vuJitEmitFconst) are loaded; calls clobber them.
static bool s_vu1_jit_fconst_live;
// Within a trace: lanes (bit 3 = x) of VF[0..31] / ACC (32) in memory that a native x86 FMAC op
// wrote (DAZ done, no denormal: operands skip DAZ), and whether v30 holds ACC (calls clobber it).
static struct
{
	u8 clean[33];
	bool acc_live;
	s8 out_reg; // the native FMAC result of the current pair: register, lanes
	u8 out_lanes;
} s_vu1_jit_vf;
// Flags in registers within a trace: w28 = VI[STATUS] (st_loaded; st_dirty: not stored yet),
// w15 = VI[MAC] not stored yet (mac_dirty), w9 + i = mac | status << 16 of the FMAC entry at
// pipe position i (bit i of live; entries that retire within the trace), w14 = the same of
// VU1.macflag/statusflag (scratch). Calls clobber w9..w15.
// sticky: v31 holds the per-lane MAC bits of FMAC entries whose flags are dead (overwritten
// before any read), OR-accumulated; only their sticky status bits are left to apply.
static struct
{
	bool st_loaded, st_dirty, mac_dirty, scratch, sticky;
	u8 live;
} s_vu1_jit_fl;
static constexpr s64 VU1_JIT_FL_SAVE = 160; // stack save area of w9..w15 around calls
static constexpr s64 VU1_JIT_STICKY_SAVE = 192; // stack save area of v31 around the XGKICK call
static void _vu1JitStatusLoad();
// VU_STAT_UPDATE of the OR of the MAC bits in v31, to the sticky bits of w28. Clobbers w0-w2.
static void _vu1JitStickyFold()
{
	if (!s_vu1_jit_fl.sticky)
		return;
	_vu1JitStatusLoad();
	// The lanes hold disjoint MAC bits: the sum is their OR.
	armAsm->Addv(a64::s29, a64::v31.V4S());
	armAsm->Fmov(a64::w0, a64::s29);
	armAsm->Orr(a64::w1, a64::w0, a64::Operand(a64::w0, a64::LSR, 1));
	armAsm->Orr(a64::w1, a64::w1, a64::Operand(a64::w1, a64::LSR, 2));
	armAsm->And(a64::w1, a64::w1, 0x11111111);
	armAsm->Mov(a64::w2, 0x1248);
	armAsm->Mul(a64::w1, a64::w1, a64::w2);
	armAsm->Ubfx(a64::w1, a64::w1, 12, 4);
	armAsm->Orr(a64::w28, a64::w28, a64::Operand(a64::w1, a64::LSL, 6));
	s_vu1_jit_fl.st_dirty = true;
	s_vu1_jit_fl.sticky = false;
}
static void _vu1JitFlagsStore()
{
	_vu1JitStickyFold();
	if (s_vu1_jit_fl.st_dirty)
		armAsm->Str(a64::w28, _vu1JitMem(_vu1Off(VU1.VI[REG_STATUS_FLAG].UL)));
	if (s_vu1_jit_fl.mac_dirty)
		armAsm->Str(a64::w15, _vu1JitMem(_vu1Off(VU1.VI[REG_MAC_FLAG].UL)));
	s_vu1_jit_fl.st_dirty = s_vu1_jit_fl.mac_dirty = false;
}
static void _vu1JitStatusLoad()
{
	if (!s_vu1_jit_fl.st_loaded)
		armAsm->Ldr(a64::w28, _vu1JitMem(_vu1Off(VU1.VI[REG_STATUS_FLAG].UL)));
	s_vu1_jit_fl.st_loaded = true;
}
static a64::Register _vu1JitFlReg(u32 i)
{
	return a64::WRegister(9 + (i & 3));
}
// Saves (load = false) or restores the live entry flag registers around a call.
static void _vu1JitFlSave(bool load)
{
	for (u32 i = 0; i < 4; i++)
	{
		if (!(s_vu1_jit_fl.live & (1u << i)))
			continue;
		const a64::MemOperand m(a64::sp, VU1_JIT_FL_SAVE + i * 4);
		if (load)
			armAsm->Ldr(_vu1JitFlReg(i), m);
		else
			armAsm->Str(_vu1JitFlReg(i), m);
	}
}

// Clobbers w0/x1. keep: the stores sit on a conditional path, the values stay pending after it.
// fmac = false: the callee does not touch the FMAC pipe.
static void _vu1JitFlush(bool keep = false, bool fmac = true)
{
	VU1JitPending& p = s_vu1_jit_pending;
	if (fmac && p.fmac)
	{
		armAsm->Add(a64::w0, a64::w27, p.fmac_ret & 3);
		armAsm->And(a64::w0, a64::w0, 3);
		armAsm->Str(a64::w0, _vu1JitMem(_vu1Off(VU1.fmacreadpos)));
		armAsm->Add(a64::w0, a64::w27, (p.fmac_ret + p.fmac_cnt) & 3);
		armAsm->And(a64::w0, a64::w0, 3);
		armAsm->Str(a64::w0, _vu1JitMem(_vu1Off(VU1.fmacwritepos)));
		armAsm->Mov(a64::w0, p.fmac_cnt);
		armAsm->Str(a64::w0, _vu1JitMem(_vu1Off(VU1.fmaccount)));
		if (!keep)
			p.fmac = false;
	}
	if (p.tpc)
	{
		armAsm->Mov(a64::w0, p.tpc_v);
		armAsm->Str(a64::w0, _vu1JitMem(_vu1Off(VU1.VI[REG_TPC].UL)));
	}
	if (p.cycle)
	{
		armAsm->Add(a64::x1, a64::x20, p.cycle_rel);
		armAsm->Str(a64::x1, _vu1JitMem(_vu1Off(VU1.cycle)));
	}
	if (p.code)
	{
		armAsm->Mov(a64::w0, p.code_v);
		armAsm->Str(a64::w0, _vu1JitMem(_vu1Off(VU1.code)));
	}
	if (!keep)
		p.tpc = p.cycle = p.code = false;
}

static void _vu1JitEmitHandler(FnPtr_VuVoid f)
{
	if (f != VU1MI_NOP)
	{
		_vu1JitFlush();
		_vu1JitFlagsStore();
		_vu1JitFlSave(false);
		armEmitCall(reinterpret_cast<const void*>(f));
		_vu1JitFlSave(true);
		s_vu1_jit_fconst_live = false;
		s_vu1_jit_vf.acc_live = false;
		s_vu1_jit_fl.st_loaded = s_vu1_jit_fl.scratch = false;
	}
}

// pos = (pos + 1) & 3, count += delta
static void _vu1JitEmitPosStep(s64 off_pos, s64 off_count, int delta)
{
	armAsm->Ldr(a64::w0, _vu1JitMem(off_pos));
	armAsm->Add(a64::w0, a64::w0, 1);
	armAsm->And(a64::w0, a64::w0, 3);
	armAsm->Str(a64::w0, _vu1JitMem(off_pos));
	armAsm->Ldr(a64::w0, _vu1JitMem(off_count));
	armAsm->Add(a64::w0, a64::w0, delta);
	armAsm->Str(a64::w0, _vu1JitMem(off_count));
}

// x23 + i = &VU1 + ((fmacreadpos at trace entry + i) & 3) * sizeof(fmacPipe)
static a64::Register _vu1JitFmacBase(u32 i)
{
	return a64::XRegister(23 + (i & 3));
}

// Retires the oldest FMAC entry (rb = its base), as _vuFMACflush does; the caller tracks the
// read position and count. flagreg: the entry's flagreg when known at compile time, else -1.
// fl: the pipe position whose flag register holds the entry's mac/status, else -1.
// dead: the entry's flags are dead (_vu1JitFlagsDead), its sticky bits are in v31.
static void _vu1JitEmitFMACretire(const a64::Register& rb, int flagreg, int fl, bool dead = false)
{
	const s64 off_fmac = _vu1Off(VU1.fmac[0]);
	const auto entry = [&rb, off_fmac](size_t field) { return a64::MemOperand(rb, off_fmac + field); };
	if (dead)
	{
		if (flagreg & (1 << REG_CLIP_FLAG))
		{
			armAsm->Ldr(a64::w2, entry(offsetof(fmacPipe, clipflag)));
			armAsm->Str(a64::w2, _vu1JitMem(_vu1Off(VU1.VI[REG_CLIP_FLAG].UL)));
		}
		return;
	}
	// This retire may replace the sticky bits.
	if (flagreg < 0 || (flagreg & (1 << REG_STATUS_FLAG)))
		_vu1JitStickyFold();
	_vu1JitStatusLoad();
	if (flagreg < 0)
	{
		a64::Label no_clip, sticky, done;
		armAsm->Ldr(a64::w1, entry(offsetof(fmacPipe, flagreg)));
		armAsm->Tbz(a64::w1, REG_CLIP_FLAG, &no_clip);
		armAsm->Ldr(a64::w2, entry(offsetof(fmacPipe, clipflag)));
		armAsm->Str(a64::w2, _vu1JitMem(_vu1Off(VU1.VI[REG_CLIP_FLAG].UL)));
		armAsm->Bind(&no_clip);
		armAsm->Ldr(a64::w3, entry(offsetof(fmacPipe, statusflag)));
		armAsm->And(a64::w2, a64::w3, 0xf);
		armAsm->Tbnz(a64::w1, REG_STATUS_FLAG, &sticky);
		armAsm->And(a64::w28, a64::w28, 0xff0);
		armAsm->Orr(a64::w28, a64::w28, a64::w2);
		armAsm->Orr(a64::w28, a64::w28, a64::Operand(a64::w2, a64::LSL, 6));
		armAsm->B(&done);
		armAsm->Bind(&sticky);
		armAsm->And(a64::w28, a64::w28, 0x30);
		armAsm->Orr(a64::w28, a64::w28, a64::w2);
		armAsm->And(a64::w2, a64::w3, 0xfc0);
		armAsm->Orr(a64::w28, a64::w28, a64::w2);
		armAsm->Bind(&done);
	}
	else
	{
		if (flagreg & (1 << REG_CLIP_FLAG))
		{
			armAsm->Ldr(a64::w2, entry(offsetof(fmacPipe, clipflag)));
			armAsm->Str(a64::w2, _vu1JitMem(_vu1Off(VU1.VI[REG_CLIP_FLAG].UL)));
		}
		const a64::Register& st = a64::w3;
		if (fl >= 0)
			armAsm->Lsr(st, _vu1JitFlReg(fl), 16);
		else
			armAsm->Ldr(st, entry(offsetof(fmacPipe, statusflag)));
		armAsm->And(a64::w2, st, 0xf);
		if (flagreg & (1 << REG_STATUS_FLAG))
		{
			armAsm->And(a64::w28, a64::w28, 0x30);
			armAsm->Orr(a64::w28, a64::w28, a64::w2);
			armAsm->And(a64::w2, st, 0xfc0);
			armAsm->Orr(a64::w28, a64::w28, a64::w2);
		}
		else
		{
			armAsm->And(a64::w28, a64::w28, 0xff0);
			armAsm->Orr(a64::w28, a64::w28, a64::w2);
			armAsm->Orr(a64::w28, a64::w28, a64::Operand(a64::w2, a64::LSL, 6));
		}
	}
	if (fl >= 0)
	{
		armAsm->And(a64::w15, _vu1JitFlReg(fl), 0xffff);
		s_vu1_jit_fl.live &= ~(1u << (fl & 3));
	}
	else
		armAsm->Ldr(a64::w15, entry(offsetof(fmacPipe, macflag)));
	s_vu1_jit_fl.st_dirty = s_vu1_jit_fl.mac_dirty = true;
}

// The pipe entry's flagreg for this pair.
static int _vu1FmacFlagreg(const VU1DecodedPair& d)
{
	return (d.uregs.pipe == VUPIPE_FMAC ? d.uregs.VIwrite : 0) | (d.lregs.pipe == VUPIPE_FMAC ? d.lregs.VIwrite : 0);
}

// _vuClearFMAC + _vuAddUpperStalls/_vuAddLowerStalls for the FMAC pipe: every field of the entry
// is stored (the static ones as constants), as the memset of _vuClearFMAC does. rb = the entry;
// the caller tracks the write position and count. flags_only: the entry retires within the trace
// and no code reads the pipe before (no handler does, stalls are compile time), so only the flags
// its retire reads are kept: mac and status in the flag register of pipe position fl.
static void _vu1JitEmitAddFMAC(const VU1DecodedPair& d, const a64::Register& rb, const a64::Register& rbase,
	u32 cycle_rel, bool flags_only, u32 fl, bool dead = false)
{
	const bool up = d.uregs.pipe == VUPIPE_FMAC;
	const bool lo = d.lregs.pipe == VUPIPE_FMAC;
	const s64 off_fmac = _vu1Off(VU1.fmac[0]);
	const auto entry = [&rb, off_fmac](size_t field) { return a64::MemOperand(rb, off_fmac + field); };
	static_assert(offsetof(fmacPipe, macflag) + 4 == offsetof(fmacPipe, statusflag) &&
				  offsetof(VURegs, macflag) + 4 == offsetof(VURegs, statusflag));
	if (flags_only && dead)
	{
		if (_vu1FmacFlagreg(d) & (1 << REG_CLIP_FLAG))
		{
			armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.clipflag)));
			armAsm->Str(a64::w2, entry(offsetof(fmacPipe, clipflag)));
		}
		return;
	}
	if (flags_only)
	{
		// mac and status (both < 0x10000) to the flag register of the entry.
		const a64::Register r = _vu1JitFlReg(fl);
		if (s_vu1_jit_fl.scratch)
			armAsm->Mov(r, a64::w14);
		else
		{
			armAsm->Ldr(a64::w1, _vu1JitMem(_vu1Off(VU1.macflag)));
			armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.statusflag)));
			armAsm->Orr(r, a64::w1, a64::Operand(a64::w2, a64::LSL, 16));
		}
		s_vu1_jit_fl.live |= 1u << (fl & 3);
		if (_vu1FmacFlagreg(d) & (1 << REG_CLIP_FLAG))
		{
			armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.clipflag)));
			armAsm->Str(a64::w2, entry(offsetof(fmacPipe, clipflag)));
		}
		return;
	}
	armAsm->Mov(a64::w1, up ? d.uregs.VFwrite : 0);
	armAsm->Mov(a64::w2, lo ? d.lregs.VFwrite : 0);
	armAsm->Stp(a64::w1, a64::w2, entry(offsetof(fmacPipe, regupper)));
	armAsm->Mov(a64::w1, _vu1FmacFlagreg(d));
	armAsm->Mov(a64::w2, up ? d.uregs.VFwxyzw : 0);
	armAsm->Stp(a64::w1, a64::w2, entry(offsetof(fmacPipe, flagreg)));
	armAsm->Mov(a64::w1, lo ? d.lregs.VFwxyzw : 0);
	armAsm->Stp(a64::w1, a64::wzr, entry(offsetof(fmacPipe, xyzwlower)));
	static_assert(offsetof(fmacPipe, xyzwlower) + 8 == offsetof(fmacPipe, sCycle));
	armAsm->Add(a64::x1, rbase, cycle_rel);
	armAsm->Str(a64::x1, entry(offsetof(fmacPipe, sCycle)));
	armAsm->Mov(a64::w1, 4);
	armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.macflag)));
	armAsm->Stp(a64::w1, a64::w2, entry(offsetof(fmacPipe, Cycle)));
	armAsm->Ldr(a64::w1, _vu1JitMem(_vu1Off(VU1.statusflag)));
	armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.clipflag)));
	armAsm->Stp(a64::w1, a64::w2, entry(offsetof(fmacPipe, statusflag)));
	static_assert(offsetof(fmacPipe, Cycle) + 4 == offsetof(fmacPipe, macflag) &&
				  offsetof(fmacPipe, statusflag) + 4 == offsetof(fmacPipe, clipflag) && sizeof(fmacPipe) == 48);
}

// _vuAddIALUStalls (cycles != 0)
static void _vu1JitEmitAddIALU(const _VURegsNum& r, const a64::Register& rbase, u32 cycle_rel)
{
	const s64 off_ialu = _vu1Off(VU1.ialu[0]);
	armAsm->Ldr(a64::w0, _vu1JitMem(_vu1Off(VU1.ialuwritepos)));
	armAsm->Mov(a64::w6, sizeof(ialuPipe));
	armAsm->Umaddl(a64::x5, a64::w0, a64::w6, s_vu1_jit_rvu);
	armAsm->Mov(a64::w1, r.VIwrite);
	armAsm->Str(a64::w1, a64::MemOperand(a64::x5, off_ialu + offsetof(ialuPipe, reg)));
	armAsm->Add(a64::x1, rbase, cycle_rel);
	armAsm->Str(a64::x1, a64::MemOperand(a64::x5, off_ialu + offsetof(ialuPipe, sCycle)));
	armAsm->Mov(a64::w1, r.cycles);
	armAsm->Str(a64::w1, a64::MemOperand(a64::x5, off_ialu + offsetof(ialuPipe, Cycle)));
	_vu1JitEmitPosStep(_vu1Off(VU1.ialuwritepos), _vu1Off(VU1.ialucount), 1);
}

// _vuFDIVAdd / _vuEFUAdd: Q or P and the status flag are already stored by the op.
static void _vu1JitEmitAddFdivEfu(const _VURegsNum& r, const a64::Register& rbase, u32 cycle_rel)
{
	const bool fdiv = r.pipe == VUPIPE_FDIV;
	armAsm->Mov(a64::w0, 1);
	armAsm->Str(a64::w0, _vu1JitMem(fdiv ? _vu1Off(VU1.fdiv.enable) : _vu1Off(VU1.efu.enable)));
	armAsm->Add(a64::x1, rbase, cycle_rel);
	armAsm->Str(a64::x1, _vu1JitMem(fdiv ? _vu1Off(VU1.fdiv.sCycle) : _vu1Off(VU1.efu.sCycle)));
	armAsm->Mov(a64::w0, r.cycles);
	armAsm->Str(a64::w0, _vu1JitMem(fdiv ? _vu1Off(VU1.fdiv.Cycle) : _vu1Off(VU1.efu.Cycle)));
	armAsm->Ldr(a64::w0, _vu1JitMem(fdiv ? _vu1Off(VU1.q.UL) : _vu1Off(VU1.p.UL)));
	armAsm->Str(a64::w0, _vu1JitMem(fdiv ? _vu1Off(VU1.fdiv.reg.UL) : _vu1Off(VU1.efu.reg.UL)));
	if (fdiv)
	{
		armAsm->Ldr(a64::w0, _vu1JitMem(_vu1Off(VU1.statusflag)));
		armAsm->And(a64::w1, a64::w0, 0x30);
		armAsm->Orr(a64::w0, a64::w0, a64::Operand(a64::w1, a64::LSL, 6));
		armAsm->Str(a64::w0, _vu1JitMem(_vu1Off(VU1.fdiv.statusflag)));
	}
}

// Upper FMAC ops emitted natively (applyBinaryMACOp/applyTernaryMACOp in VUops.cpp).
// ADDi is left out (TriAce hack), so are Fd = 0 forms (they write RDzero).
enum class VU1FmacOp : u8 { Add, Sub, Mul, Madd, Msub };
static constexpr u8 VU1_BC_VEC = 4, VU1_BC_I = 5, VU1_BC_Q = 6, VU1_BC_OP = 7; // 0..3: Ft.x..w; OP: Fs.yzx, Ft.zxy
struct VU1FmacDesc
{
	FnPtr_VuVoid fn;
	VU1FmacOp op;
	bool acc;
	u8 bc;
};
#define VU1_FMAC_BC(name, op, acc) \
	{VU1MI_##name##x, op, acc, 0}, {VU1MI_##name##y, op, acc, 1}, {VU1MI_##name##z, op, acc, 2}, \
		{VU1MI_##name##w, op, acc, 3}, {VU1MI_##name##q, op, acc, VU1_BC_Q}, {VU1MI_##name, op, acc, VU1_BC_VEC}
#define VU1_FMAC_ALL(name, op) \
	VU1_FMAC_BC(name, op, false), {VU1MI_##name##i, op, false, VU1_BC_I}, VU1_FMAC_BC(name##A, op, true), \
		{VU1MI_##name##Ai, op, true, VU1_BC_I}
static const VU1FmacDesc s_vu1_fmac_ops[] = {
	VU1_FMAC_BC(ADD, VU1FmacOp::Add, false), VU1_FMAC_BC(ADDA, VU1FmacOp::Add, true),
	{VU1MI_ADDAi, VU1FmacOp::Add, true, VU1_BC_I}, VU1_FMAC_ALL(SUB, VU1FmacOp::Sub),
	VU1_FMAC_ALL(MUL, VU1FmacOp::Mul), VU1_FMAC_ALL(MADD, VU1FmacOp::Madd), VU1_FMAC_ALL(MSUB, VU1FmacOp::Msub),
	{VU1MI_OPMULA, VU1FmacOp::Mul, true, VU1_BC_OP}, {VU1MI_OPMSUB, VU1FmacOp::Msub, false, VU1_BC_OP}};
#undef VU1_FMAC_ALL
#undef VU1_FMAC_BC

// x21 points here in JIT code: exponent, sign, max float, MAC flag bits Z/S/U/O of x y z w.
alignas(16) static const u32 s_vu1_jit_fconst[7][4] = {
	{0x7f800000, 0x7f800000, 0x7f800000, 0x7f800000}, {0x80000000, 0x80000000, 0x80000000, 0x80000000},
	{0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff}, {0x0008, 0x0004, 0x0002, 0x0001},
	{0x0080, 0x0040, 0x0020, 0x0010}, {0x0800, 0x0400, 0x0200, 0x0100}, {0x8000, 0x4000, 0x2000, 0x1000}};

// Clamp settings the JIT code was compiled for (bits 0-1: vu0/vu1 Overflow, 2-3: SignOverflow,
// 4-5: ExtraOverflow).
static u8 s_vu1_jit_ovf = 0xff;
static u8 _vu1JitOvf()
{
	const auto& r = EmuConfig.Cpu.Recompiler;
	return (r.vu0Overflow ? 1 : 0) | (r.vu1Overflow ? 2 : 0) | (r.vu0SignOverflow ? 4 : 0) |
		   (r.vu1SignOverflow ? 8 : 0) | (r.vu0ExtraOverflow ? 16 : 0) | (r.vu1ExtraOverflow ? 32 : 0);
}

// vuDouble on 4 lanes: exponent 0 -> signed 0, exponent 255 -> signed max float.
static void _vu1JitEmitVuDouble(const a64::VRegister& v)
{
	armAsm->And(a64::v24.V16B(), v.V16B(), a64::v16.V16B());
	armAsm->Cmeq(a64::v25.V4S(), a64::v24.V4S(), 0);
	armAsm->Cmeq(a64::v24.V4S(), a64::v24.V4S(), a64::v16.V4S());
	armAsm->And(a64::v26.V16B(), v.V16B(), a64::v17.V16B());
	armAsm->Orr(a64::v27.V16B(), a64::v26.V16B(), a64::v18.V16B());
	armAsm->Bit(v.V16B(), a64::v27.V16B(), a64::v24.V16B());
	armAsm->Bit(v.V16B(), a64::v26.V16B(), a64::v25.V16B());
}

// x86 microVU operand clamp (vuX86ClampOperands): a NaN of either sign becomes +max.
static void _vuJitEmitClampNaN(const a64::VRegister& v)
{
	armAsm->Bic(a64::v24.V16B(), v.V16B(), a64::v17.V16B());
	armAsm->Cmgt(a64::v25.V4S(), a64::v24.V4S(), a64::v16.V4S());
	armAsm->Bit(v.V16B(), a64::v18.V16B(), a64::v25.V16B());
}

// DAZ on 4 lanes: exponent 0 -> signed 0.
static void _vuJitEmitDaz(const a64::VRegister& v)
{
	armAsm->Cmtst(a64::v24.V4S(), v.V4S(), a64::v16.V4S());
	armAsm->Orr(a64::v24.V16B(), a64::v24.V16B(), a64::v17.V16B());
	armAsm->And(v.V16B(), v.V16B(), a64::v24.V16B());
}

// _vuJitEmitClampNaN then vuDouble: NaN -> +max, Inf -> signed max (Inf - 1), exponent 0 -> signed 0.
static void _vuJitEmitClampDouble(const a64::VRegister& v, bool daz = true)
{
	_vuJitEmitClampNaN(v);
	armAsm->And(a64::v24.V16B(), v.V16B(), a64::v16.V16B());
	armAsm->Cmeq(a64::v25.V4S(), a64::v24.V4S(), a64::v16.V4S());
	armAsm->Add(v.V4S(), v.V4S(), a64::v25.V4S());
	if (!daz)
		return;
	armAsm->Cmeq(a64::v24.V4S(), a64::v24.V4S(), 0);
	armAsm->And(a64::v26.V16B(), v.V16B(), a64::v17.V16B());
	armAsm->Bit(v.V16B(), a64::v26.V16B(), a64::v24.V16B());
}

// r = a op b as x86 SSE (_vuX86Arith): r holds the ARM64 result. A NaN in a, else in b, passes
// through quieted; any other NaN result becomes the x86 default NaN; FTZ unless !daz.
// v5 = 0x00400000, v6 = 0xffc00000.
static void _vuJitEmitX86Nan(const a64::VRegister& r, const a64::VRegister& a, const a64::VRegister& b, bool daz = true)
{
	armAsm->Bic(a64::v24.V16B(), r.V16B(), a64::v17.V16B());
	armAsm->Cmgt(a64::v24.V4S(), a64::v24.V4S(), a64::v16.V4S());
	armAsm->Bit(r.V16B(), a64::v6.V16B(), a64::v24.V16B());
	armAsm->Bic(a64::v24.V16B(), b.V16B(), a64::v17.V16B());
	armAsm->Cmgt(a64::v24.V4S(), a64::v24.V4S(), a64::v16.V4S());
	armAsm->Orr(a64::v25.V16B(), b.V16B(), a64::v5.V16B());
	armAsm->Bit(r.V16B(), a64::v25.V16B(), a64::v24.V16B());
	armAsm->Bic(a64::v24.V16B(), a.V16B(), a64::v17.V16B());
	armAsm->Cmgt(a64::v24.V4S(), a64::v24.V4S(), a64::v16.V4S());
	armAsm->Orr(a64::v25.V16B(), a.V16B(), a64::v5.V16B());
	armAsm->Bit(r.V16B(), a64::v25.V16B(), a64::v24.V16B());
	if (daz)
		_vuJitEmitDaz(r);
}

// Emits the upper op natively when it is in s_vu1_fmac_ops and vu0Overflow is set (as vuDouble
// tests): the inputs are clamped, or a NaN follows the x86 rules (vuX86Fmac), so the result does
// not depend on the operand order the compiler picked. Returns false when the handler must be called.
static const VU1FmacDesc* _vuJitFmacDesc(const VU1FmacDesc* ops, size_t count, FnPtr_VuVoid f, u32 code)
{
	if (!EmuConfig.Cpu.Recompiler.vu0Overflow)
		return nullptr;
	const VU1FmacDesc* desc = nullptr;
	for (size_t i = 0; i < count; i++)
		if (ops[i].fn == f)
			desc = &ops[i];
	if (!desc || (!desc->acc && ((code >> 6) & 0x1f) == 0))
		return nullptr;
	return desc;
}

// The FMAC op on the VURegs at rvu (VU0 or VU1: same layout), s_vu1_jit_fconst at rconst.
// out_clamp: CHECK_VU_OVERFLOW of that VU; x86: vuX86Fmac of that VU; clamp: vuX86ClampOperands.
// v16-v22 = s_vu1_jit_fconst, v5 = 0x00400000, v6 = 0xffc00000 (rconst = s_vu1_jit_fconst).
static void _vuJitEmitFconst(const a64::Register& rconst)
{
	armAsm->Ldp(a64::q16, a64::q17, a64::MemOperand(rconst, 0));
	armAsm->Ldp(a64::q18, a64::q19, a64::MemOperand(rconst, 32));
	armAsm->Ldp(a64::q20, a64::q21, a64::MemOperand(rconst, 64));
	armAsm->Ldr(a64::q22, a64::MemOperand(rconst, 96));
	armAsm->Movi(a64::v5.V4S(), 0x40, a64::LSL, 16);
	armAsm->Orr(a64::v6.V16B(), a64::v16.V16B(), a64::v17.V16B());
	armAsm->Orr(a64::v6.V16B(), a64::v6.V16B(), a64::v5.V16B());
}

// The x86 FMAC op with a NaN in its result, out of line after the block: v3 from v0, v1 (v2 =
// ACC), all x86 NaN rules, no final DAZ.
struct VuJitCold
{
	a64::Label call, back;
	const VU1FmacDesc* desc;
};
static std::deque<VuJitCold>* s_vu_jit_cold = nullptr;

static void _vuJitEmitX86Slow(const VU1FmacDesc* desc)
{
	const bool ternary = desc->op == VU1FmacOp::Madd || desc->op == VU1FmacOp::Msub;
	switch (desc->op)
	{
		case VU1FmacOp::Add: armAsm->Fadd(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S()); break;
		case VU1FmacOp::Sub: armAsm->Fsub(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S()); break;
		case VU1FmacOp::Mul: armAsm->Fmul(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S()); break;
		case VU1FmacOp::Madd:
		case VU1FmacOp::Msub:
			armAsm->Fmul(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S());
			_vuJitEmitX86Nan(a64::v3, a64::v0, a64::v1);
			armAsm->Mov(a64::v0.V16B(), a64::v3.V16B());
			if (desc->op == VU1FmacOp::Madd)
				armAsm->Fadd(a64::v3.V4S(), a64::v2.V4S(), a64::v3.V4S());
			else
				armAsm->Fsub(a64::v3.V4S(), a64::v2.V4S(), a64::v3.V4S());
			break;
	}
	// x86 MADD to Fd adds ACC to the product; MADDA and MSUB* put ACC first.
	if (!ternary)
		_vuJitEmitX86Nan(a64::v3, a64::v0, a64::v1, false);
	else if (desc->op == VU1FmacOp::Madd && !desc->acc)
		_vuJitEmitX86Nan(a64::v3, a64::v0, a64::v2, false);
	else
		_vuJitEmitX86Nan(a64::v3, a64::v2, a64::v0, false);
}

// fconst_live: whether _vuJitEmitFconst's registers are loaded (set here); nullptr = load them.
// clean (x86 only, bits as clamp: 2 = Fs, 1 = Ft, 4 = ACC): the lanes the op uses have no
// denormal. acc_live: whether v30 holds ACC (updated here); nullptr = ACC only in memory.
// sticky: non-null = the flags are dead, only the MAC bits go to v31 (*sticky: v31 holds some).
static void _vuJitEmitFmacCore(const VU1FmacDesc* desc, u32 code, const a64::Register& rvu,
	const a64::Register& rconst, bool out_clamp, bool x86, u32 clamp, bool* fconst_live = nullptr,
	u32 clean = 0, bool* acc_live = nullptr, bool* sticky = nullptr)
{
	if (x86)
		out_clamp = false;
	const auto _vu1JitMem = [&rvu](s64 off) { return a64::MemOperand(rvu, off); };
	const u32 ft = (code >> 16) & 0x1f, fs = (code >> 11) & 0x1f, fd = (code >> 6) & 0x1f;

	if (!fconst_live || !*fconst_live)
	{
		_vuJitEmitFconst(rconst);
		if (fconst_live)
			*fconst_live = true;
	}

	armAsm->Ldr(a64::q0, _vu1JitMem(_vu1Off(VU1.VF[fs])));
	if (desc->bc == VU1_BC_VEC || desc->bc == VU1_BC_OP)
		armAsm->Ldr(a64::q1, _vu1JitMem(_vu1Off(VU1.VF[ft])));
	if (desc->bc == VU1_BC_OP)
	{
		// v0 = Fs.yzx, v1 = Ft.zxy (lane w unused)
		armAsm->Ext(a64::v24.V16B(), a64::v0.V16B(), a64::v0.V16B(), 4);
		armAsm->Mov(a64::v24.V4S(), 2, a64::v0.V4S(), 0);
		armAsm->Ext(a64::v25.V16B(), a64::v1.V16B(), a64::v1.V16B(), 8);
		armAsm->Mov(a64::v25.V4S(), 1, a64::v1.V4S(), 0);
		armAsm->Mov(a64::v25.V4S(), 2, a64::v1.V4S(), 1);
		armAsm->Mov(a64::v0.V16B(), a64::v24.V16B());
		armAsm->Mov(a64::v1.V16B(), a64::v25.V16B());
	}
	else if (desc->bc != VU1_BC_VEC)
	{
		const s64 off = desc->bc == VU1_BC_I ? _vu1Off(VU1.VI[REG_I].UL) :
						desc->bc == VU1_BC_Q ? _vu1Off(VU1.VI[REG_Q].UL) :
											   _vu1Off(VU1.VF[ft].UL[desc->bc]);
		armAsm->Ldr(a64::s1, _vu1JitMem(off));
		armAsm->Dup(a64::v1.V4S(), a64::v1.V4S(), 0);
	}
	// x86: the operands x86 does not clamp only get DAZ.
	const auto prep = [x86](const a64::VRegister& v, bool c, bool cl) {
		if (c)
			_vuJitEmitClampDouble(v, !(x86 && cl));
		else if (!x86)
			_vu1JitEmitVuDouble(v);
		else if (!cl)
			_vuJitEmitDaz(v);
	};
	prep(a64::v0, clamp & 2, clean & 2);
	prep(a64::v1, clamp & 1, clean & 1);
	const bool ternary = desc->op == VU1FmacOp::Madd || desc->op == VU1FmacOp::Msub;
	if (ternary)
	{
		if (acc_live && *acc_live)
			armAsm->Mov(a64::v2.V16B(), a64::v30.V16B());
		else
		{
			armAsm->Ldr(a64::q2, _vu1JitMem(_vu1Off(VU1.ACC)));
			if (acc_live)
			{
				armAsm->Mov(a64::v30.V16B(), a64::v2.V16B());
				*acc_live = true;
			}
		}
		prep(a64::v2, clamp & 4, clean & 4);
	}
	if (x86)
	{
		// A NaN operand or product gives a NaN result, so with no NaN lane in the result the x86
		// NaN rules change nothing and only FTZ is left; else _vuJitEmitX86Slow.
		switch (desc->op)
		{
			case VU1FmacOp::Add: armAsm->Fadd(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S()); break;
			case VU1FmacOp::Sub: armAsm->Fsub(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S()); break;
			case VU1FmacOp::Mul: armAsm->Fmul(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S()); break;
			case VU1FmacOp::Madd:
			case VU1FmacOp::Msub:
				armAsm->Fmul(a64::v4.V4S(), a64::v0.V4S(), a64::v1.V4S());
				_vuJitEmitDaz(a64::v4);
				if (desc->op == VU1FmacOp::Madd)
					armAsm->Fadd(a64::v3.V4S(), a64::v2.V4S(), a64::v4.V4S());
				else
					armAsm->Fsub(a64::v3.V4S(), a64::v2.V4S(), a64::v4.V4S());
				break;
		}
		armAsm->Fminv(a64::s24, a64::v3.V4S());
		armAsm->Fcmp(a64::s24, a64::s24);
		if (s_vu_jit_cold)
		{
			VuJitCold& c = s_vu_jit_cold->emplace_back();
			c.desc = desc;
			armAsm->B(&c.call, a64::vs);
			armAsm->Bind(&c.back);
		}
		else
		{
			a64::Label done;
			armAsm->B(&done, a64::vc);
			_vuJitEmitX86Slow(desc);
			armAsm->Bind(&done);
		}
		_vuJitEmitDaz(a64::v3);
		// mVUupdateFlags: Z = +-0 (FTZ leaves no other exponent 0), S = sign, never O or U.
		armAsm->Fcmeq(a64::v26.V4S(), a64::v3.V4S(), 0.0);
		armAsm->Sshr(a64::v27.V4S(), a64::v3.V4S(), 31);
		armAsm->And(a64::v28.V16B(), a64::v26.V16B(), a64::v19.V16B());
		armAsm->And(a64::v29.V16B(), a64::v27.V16B(), a64::v20.V16B());
		armAsm->Orr(a64::v28.V16B(), a64::v28.V16B(), a64::v29.V16B());
	}
	else
	{
		switch (desc->op)
		{
			case VU1FmacOp::Add: armAsm->Fadd(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S()); break;
			case VU1FmacOp::Sub: armAsm->Fsub(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S()); break;
			case VU1FmacOp::Mul: armAsm->Fmul(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S()); break;
			case VU1FmacOp::Madd:
			case VU1FmacOp::Msub:
				armAsm->Fmul(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S());
				if (desc->op == VU1FmacOp::Madd)
					armAsm->Fadd(a64::v3.V4S(), a64::v2.V4S(), a64::v3.V4S());
				else
					armAsm->Fsub(a64::v3.V4S(), a64::v2.V4S(), a64::v3.V4S());
				break;
		}
		// VU_MAC_UPDATE: an enabled lane's 4 MAC bits depend on its result only, others are cleared.
		// Z = (f == 0) or exponent 0, U = exponent 0 and f != 0, O = exponent 255, S = sign.
		armAsm->And(a64::v24.V16B(), a64::v3.V16B(), a64::v16.V16B());
		armAsm->Cmeq(a64::v25.V4S(), a64::v24.V4S(), 0);
		armAsm->Cmeq(a64::v24.V4S(), a64::v24.V4S(), a64::v16.V4S());
		armAsm->Fcmeq(a64::v26.V4S(), a64::v3.V4S(), 0.0);
		armAsm->Sshr(a64::v27.V4S(), a64::v3.V4S(), 31);
		armAsm->Orr(a64::v28.V16B(), a64::v26.V16B(), a64::v25.V16B());
		armAsm->Bic(a64::v25.V16B(), a64::v25.V16B(), a64::v26.V16B());
		armAsm->And(a64::v28.V16B(), a64::v28.V16B(), a64::v19.V16B());
		armAsm->And(a64::v29.V16B(), a64::v27.V16B(), a64::v20.V16B());
		armAsm->Orr(a64::v28.V16B(), a64::v28.V16B(), a64::v29.V16B());
		armAsm->And(a64::v29.V16B(), a64::v25.V16B(), a64::v21.V16B());
		armAsm->Orr(a64::v28.V16B(), a64::v28.V16B(), a64::v29.V16B());
		armAsm->And(a64::v29.V16B(), a64::v24.V16B(), a64::v22.V16B());
		armAsm->Orr(a64::v28.V16B(), a64::v28.V16B(), a64::v29.V16B());
	}
	// OPMULA/OPMSUB write x, y, z whatever the dest bits and leave the w MAC bits as they are.
	const bool op = desc->bc == VU1_BC_OP;
	const u32 lanes = op ? 0xe : (code >> 21) & 0xf; // bit 3 = x
	const int one_lane = lanes == 8 ? 0 : lanes == 4 ? 1 : lanes == 2 ? 2 : lanes == 1 ? 3 : -1;
	if (sticky)
	{
		// Dead flags (not OPMULA/OPMSUB): the enabled lanes' MAC bits to v31, VU1.macflag/statusflag
		// are left as they are.
		if (lanes != 0)
		{
			for (int i = 0; i < 4; i++)
				if (!(lanes & (8 >> i)))
					armAsm->Mov(a64::v28.V4S(), i, a64::wzr);
			if (*sticky)
				armAsm->Orr(a64::v31.V16B(), a64::v31.V16B(), a64::v28.V16B());
			else
				armAsm->Mov(a64::v31.V16B(), a64::v28.V16B());
			*sticky = true;
		}
	}
	else if (lanes == 0)
		armAsm->Mov(a64::w0, 0);
	else if (one_lane >= 0)
		armAsm->Mov(a64::w0, a64::v28.V4S(), one_lane);
	else
	{
		for (int i = 0; i < 4; i++)
			if (!(lanes & (8 >> i)))
				armAsm->Mov(a64::v28.V4S(), i, a64::wzr);
		armAsm->Addv(a64::s28, a64::v28.V4S());
		armAsm->Fmov(a64::w0, a64::s28);
	}
	if (!sticky)
	{
		if (op)
		{
			armAsm->Ldr(a64::w1, _vu1JitMem(_vu1Off(VU1.macflag)));
			armAsm->And(a64::w1, a64::w1, 0x1111);
			armAsm->Orr(a64::w0, a64::w0, a64::w1);
		}
		armAsm->Str(a64::w0, _vu1JitMem(_vu1Off(VU1.macflag)));
		// VU_STAT_UPDATE: bit n = any bit in MAC nibble n. Bit 4n of w1 is that (MAC < 0x10000), the
		// multiply moves bits 0, 4, 8, 12 to 12..15 with no overlapping partial products.
		armAsm->Orr(a64::w1, a64::w0, a64::Operand(a64::w0, a64::LSR, 1));
		armAsm->Orr(a64::w1, a64::w1, a64::Operand(a64::w1, a64::LSR, 2));
		armAsm->And(a64::w1, a64::w1, 0x11111111);
		armAsm->Mov(a64::w2, 0x1248);
		armAsm->Mul(a64::w1, a64::w1, a64::w2);
		armAsm->Ubfx(a64::w1, a64::w1, 12, 4);
		armAsm->Str(a64::w1, _vu1JitMem(_vu1Off(VU1.statusflag)));
	}

	// Value: U -> signed 0 (x86: FTZ done), O -> signed max float if vu1Overflow.
	a64::VRegister res = a64::v3;
	if (!x86)
	{
		armAsm->And(a64::v26.V16B(), a64::v3.V16B(), a64::v17.V16B());
		armAsm->Bsl(a64::v25.V16B(), a64::v26.V16B(), a64::v3.V16B());
		res = a64::v25;
	}
	if (out_clamp)
	{
		armAsm->Orr(a64::v27.V16B(), a64::v26.V16B(), a64::v18.V16B());
		armAsm->Bsl(a64::v24.V16B(), a64::v27.V16B(), a64::v25.V16B());
		res = a64::v24;
	}
	const s64 off_dst = desc->acc ? _vu1Off(VU1.ACC) : _vu1Off(VU1.VF[fd]);
	// Full 16-byte stores: a later 16-byte load of the register is forwarded only from one.
	const bool acc_reg = desc->acc && acc_live;
	if (lanes != 0xf && lanes != 0)
	{
		if (acc_reg && *acc_live)
			armAsm->Mov(a64::v4.V16B(), a64::v30.V16B());
		else
			armAsm->Ldr(a64::q4, _vu1JitMem(off_dst));
		for (int i = 0; i < 4; i++)
			if (lanes & (8 >> i))
				armAsm->Mov(a64::v4.V4S(), i, res.V4S(), i);
		res = a64::v4;
	}
	if (lanes != 0)
	{
		armAsm->Str(res.Q(), _vu1JitMem(off_dst));
		if (acc_reg)
		{
			armAsm->Mov(a64::v30.V16B(), res.V16B());
			*acc_live = true;
		}
	}
}

static bool _vu1JitEmitFmac(FnPtr_VuVoid f, u32 code, bool dead = false)
{
	const VU1FmacDesc* desc = _vuJitFmacDesc(s_vu1_fmac_ops, std::size(s_vu1_fmac_ops), f, code);
	if (!desc)
		return false;
	// Lanes each operand is used in: the dest lanes, the broadcast lane, OPMULA/OPMSUB x, y, z.
	const u32 ft = (code >> 16) & 0x1f, fs = (code >> 11) & 0x1f, fd = (code >> 6) & 0x1f;
	const bool op = desc->bc == VU1_BC_OP;
	const u32 lanes = op ? 0xe : (code >> 21) & 0xf;
	const u8* cl = s_vu1_jit_vf.clean;
	const u32 ft_lanes = desc->bc == VU1_BC_VEC || op ? lanes : desc->bc < 4 ? 8u >> desc->bc : 0x10;
	const bool ternary = desc->op == VU1FmacOp::Madd || desc->op == VU1FmacOp::Msub;
	const u32 clean = ((cl[fs] & lanes) == lanes ? 2 : 0) | ((cl[ft] & ft_lanes) == ft_lanes ? 1 : 0) |
					  (ternary && (cl[32] & lanes) == lanes ? 4 : 0);
	_vuJitEmitFmacCore(desc, code, s_vu1_jit_rvu, a64::x21, EmuConfig.Cpu.Recompiler.vu1Overflow, vuX86Fmac(1),
		vuX86ClampOperands(code, 1, false), &s_vu1_jit_fconst_live, vuX86Fmac(1) ? clean : 0,
		&s_vu1_jit_vf.acc_live, dead && !op ? &s_vu1_jit_fl.sticky : nullptr);
	s_vu1_jit_vf.out_reg = desc->acc ? 32 : static_cast<s8>(fd);
	s_vu1_jit_vf.out_lanes = vuX86Fmac(1) ? static_cast<u8>(op ? 0xe : lanes) : 0;
	if (dead && !op)
	{
		s_vu1_jit_fl.scratch = false;
		return true;
	}
	// The core leaves VU1.macflag in w0 and VU1.statusflag in w1.
	armAsm->Orr(a64::w14, a64::w0, a64::Operand(a64::w1, a64::LSL, 16));
	s_vu1_jit_fl.scratch = true;
	return true;
}

// EE COP2 macro FMAC ops (VU0 macro mode): the same ops on VU0, then SYNCMSFLAGS.
#define VU0_FMAC_BC(name, op, acc) \
	{V##name##x, op, acc, 0}, {V##name##y, op, acc, 1}, {V##name##z, op, acc, 2}, \
		{V##name##w, op, acc, 3}, {V##name##q, op, acc, VU1_BC_Q}, {V##name, op, acc, VU1_BC_VEC}
#define VU0_FMAC_ALL(name, op) \
	VU0_FMAC_BC(name, op, false), {V##name##i, op, false, VU1_BC_I}, VU0_FMAC_BC(name##A, op, true), \
		{V##name##Ai, op, true, VU1_BC_I}
static const VU1FmacDesc s_vu0_macro_fmac_ops[] = {
	VU0_FMAC_BC(ADD, VU1FmacOp::Add, false), VU0_FMAC_BC(ADDA, VU1FmacOp::Add, true),
	{VADDAi, VU1FmacOp::Add, true, VU1_BC_I}, VU0_FMAC_ALL(SUB, VU1FmacOp::Sub),
	VU0_FMAC_ALL(MUL, VU1FmacOp::Mul), VU0_FMAC_ALL(MADD, VU1FmacOp::Madd), VU0_FMAC_ALL(MSUB, VU1FmacOp::Msub),
	{VOPMULA, VU1FmacOp::Mul, true, VU1_BC_OP}, {VOPMSUB, VU1FmacOp::Msub, false, VU1_BC_OP}};
#undef VU0_FMAC_ALL
#undef VU0_FMAC_BC

static const VU1FmacDesc* _vu0MacroDesc(u32 code)
{
	if ((code >> 25) != 0x25) // COP2 with CO set
		return nullptr;
	const u32 funct = code & 63;
	void (*f)() = funct < 0x3c ? Int_COP2SPECIAL1PrintTable[funct] :
								 Int_COP2SPECIAL2PrintTable[(code & 3) | ((code >> 4) & 0x7c)];
	for (const VU1FmacDesc& o : s_vu0_macro_fmac_ops)
		if (reinterpret_cast<void (*)()>(o.fn) == f)
			return _vuJitFmacDesc(&o, 1, o.fn, code);
	return nullptr;
}

bool vu0MacroCanEmit(u32 code)
{
	return _vu0MacroDesc(code) != nullptr;
}

// The handler's work after _vu0FinishMicro (the caller tests VPU_STAT): VU0.code = code, the op,
// SYNCMSFLAGS. rvu = &VU0; uses x0-x2, v0-v4 and v16-v29.
void vu0MacroEmit(u32 code, const a64::Register& rvu)
{
	const auto mem = [&rvu](s64 off) { return a64::MemOperand(rvu, off); };
	armAsm->Mov(a64::w0, code);
	armAsm->Str(a64::w0, mem(_vu1Off(VU1.code)));
	armMoveAddressToReg(a64::x1, s_vu1_jit_fconst);
	_vuJitEmitFmacCore(_vu0MacroDesc(code), code, rvu, a64::x1, EmuConfig.Cpu.Recompiler.vu0Overflow, vuX86Fmac(0),
		vuX86ClampOperands(code, 0, true));
	armAsm->Ldr(a64::w1, mem(_vu1Off(VU1.statusflag)));
	armAsm->And(a64::w1, a64::w1, 0xf);
	armAsm->Ldr(a64::w2, mem(_vu1Off(VU1.VI[REG_STATUS_FLAG].UL)));
	armAsm->And(a64::w2, a64::w2, VU0_MACRO_STATUS_KEEP);
	armAsm->Orr(a64::w2, a64::w2, a64::w1);
	armAsm->Orr(a64::w2, a64::w2, a64::Operand(a64::w1, a64::LSL, 6));
	armAsm->Str(a64::w2, mem(_vu1Off(VU1.VI[REG_STATUS_FLAG].UL)));
	armAsm->Ldr(a64::w0, mem(_vu1Off(VU1.macflag)));
	armAsm->Str(a64::w0, mem(_vu1Off(VU1.VI[REG_MAC_FLAG].UL)));
}

// VF[ft] = v0 on the lanes of xyzw (8 = x).
static void _vu1JitEmitStoreVF(u32 ft, u32 xyzw)
{
	const s64 off = _vu1Off(VU1.VF[ft]);
	if (xyzw != 0xf)
	{
		armAsm->Ldr(a64::q1, _vu1JitMem(off));
		for (int i = 0; i < 4; i++)
			if (xyzw & (8 >> i))
				armAsm->Mov(a64::v1.V4S(), i, a64::v0.V4S(), i);
		armAsm->Mov(a64::v0.V16B(), a64::v1.V16B());
	}
	armAsm->Str(a64::q0, _vu1JitMem(off));
}

// v = w0 on all lanes
static void _vu1JitEmitDupConst(const a64::VRegister& v, u32 value)
{
	armAsm->Mov(a64::w0, value);
	armAsm->Dup(v.V4S(), a64::w0);
}

// Upper ops working on the integer representation of the floats (MAX, MINI, FTOI, ITOF, ABS,
// CLIP), as VUops.cpp. FTOI and ITOF scale with the same float multiply as the interpreter, under
// the same FPCR. Returns false for the ops left to the handler.
static bool _vu1JitEmitUpperInt(FnPtr_VuVoid f, u32 code)
{
	const u32 xyzw = (code >> 21) & 0xf;
	const u32 ft = (code >> 16) & 0x1f;
	const u32 fs = (code >> 11) & 0x1f;
	const u32 fd = (code >> 6) & 0x1f;

	struct MinMaxDesc
	{
		FnPtr_VuVoid fn;
		bool max;
		u8 bc;
	};
	static const MinMaxDesc minmax[] = {{VU1MI_MAX, true, VU1_BC_VEC}, {VU1MI_MAXi, true, VU1_BC_I},
		{VU1MI_MAXx, true, 0}, {VU1MI_MAXy, true, 1}, {VU1MI_MAXz, true, 2}, {VU1MI_MAXw, true, 3},
		{VU1MI_MINI, false, VU1_BC_VEC}, {VU1MI_MINIi, false, VU1_BC_I}, {VU1MI_MINIx, false, 0},
		{VU1MI_MINIy, false, 1}, {VU1MI_MINIz, false, 2}, {VU1MI_MINIw, false, 3}};
	for (const MinMaxDesc& m : minmax)
	{
		if (m.fn != f)
			continue;
		if (fd == 0 || xyzw == 0)
			return true;
		armAsm->Ldr(a64::q1, _vu1JitMem(_vu1Off(VU1.VF[fs])));
		if (m.bc == VU1_BC_VEC)
		{
			armAsm->Ldr(a64::q2, _vu1JitMem(_vu1Off(VU1.VF[ft])));
		}
		else
		{
			armAsm->Ldr(a64::w0, _vu1JitMem(m.bc == VU1_BC_I ? _vu1Off(VU1.VI[REG_I].UL) : _vu1Off(VU1.VF[ft].UL[m.bc])));
			armAsm->Dup(a64::v2.V4S(), a64::w0);
		}
		// fp_max/fp_min: signed max/min, the other one when both are negative.
		armAsm->Smax(a64::v3.V4S(), a64::v1.V4S(), a64::v2.V4S());
		armAsm->Smin(a64::v4.V4S(), a64::v1.V4S(), a64::v2.V4S());
		armAsm->And(a64::v0.V16B(), a64::v1.V16B(), a64::v2.V16B());
		armAsm->Cmlt(a64::v0.V4S(), a64::v0.V4S(), 0);
		armAsm->Bsl(a64::v0.V16B(), m.max ? a64::v4.V16B() : a64::v3.V16B(), m.max ? a64::v3.V16B() : a64::v4.V16B());
		_vu1JitEmitStoreVF(fd, xyzw);
		return true;
	}

	static const FnPtr_VuVoid ftoi[] = {VU1MI_FTOI0, VU1MI_FTOI4, VU1MI_FTOI12, VU1MI_FTOI15};
	static const FnPtr_VuVoid itof[] = {VU1MI_ITOF0, VU1MI_ITOF4, VU1MI_ITOF12, VU1MI_ITOF15};
	static const u32 shift[] = {0, 4, 12, 15};
	for (int i = 0; i < 4; i++)
	{
		if (f != ftoi[i] && f != itof[i])
			continue;
		if (ft == 0 || xyzw == 0)
			return true;
		armAsm->Ldr(a64::q1, _vu1JitMem(_vu1Off(VU1.VF[fs])));
		if (f == ftoi[i])
		{
			if (shift[i])
			{
				_vu1JitEmitDupConst(a64::v2, 0x3f800000 + (shift[i] << 23));
				armAsm->Fmul(a64::v1.V4S(), a64::v1.V4S(), a64::v2.V4S());
			}
			// floatToInt: exponent >= 2^31 (incl. inf, NaN) saturates by sign, else truncates.
			armAsm->Fcvtzs(a64::v0.V4S(), a64::v1.V4S());
			_vu1JitEmitDupConst(a64::v2, 0x7f800000);
			armAsm->And(a64::v2.V16B(), a64::v1.V16B(), a64::v2.V16B());
			_vu1JitEmitDupConst(a64::v3, 0x4f000000);
			armAsm->Cmhs(a64::v2.V4S(), a64::v2.V4S(), a64::v3.V4S());
			armAsm->Ushr(a64::v3.V4S(), a64::v1.V4S(), 31);
			_vu1JitEmitDupConst(a64::v4, 0x7fffffff);
			armAsm->Add(a64::v3.V4S(), a64::v3.V4S(), a64::v4.V4S());
			armAsm->Bsl(a64::v2.V16B(), a64::v3.V16B(), a64::v0.V16B());
			armAsm->Mov(a64::v0.V16B(), a64::v2.V16B());
		}
		else
		{
			armAsm->Scvtf(a64::v0.V4S(), a64::v1.V4S());
			if (shift[i])
			{
				_vu1JitEmitDupConst(a64::v2, 0x3f800000 - (shift[i] << 23));
				armAsm->Fmul(a64::v0.V4S(), a64::v0.V4S(), a64::v2.V4S());
			}
		}
		_vu1JitEmitStoreVF(ft, xyzw);
		return true;
	}

	if (f == VU1MI_ABS)
	{
		if (ft == 0 || xyzw == 0)
			return true;
		armAsm->Ldr(a64::q0, _vu1JitMem(_vu1Off(VU1.VF[fs])));
		_vu1JitEmitDupConst(a64::v2, 0x7fffffff);
		armAsm->And(a64::v0.V16B(), a64::v0.V16B(), a64::v2.V16B());
		_vu1JitEmitStoreVF(ft, xyzw);
		return true;
	}

	if (f == VU1MI_CLIP)
	{
		// w0 = |Ft.w|, denormals as the largest denormal
		armAsm->Ldr(a64::w0, _vu1JitMem(_vu1Off(VU1.VF[ft].UL[3])));
		armAsm->And(a64::w1, a64::w0, 0x7fffffff);
		armAsm->Mov(a64::w2, 0x007fffff);
		armAsm->Tst(a64::w0, 0x7f800000);
		armAsm->Csel(a64::w0, a64::w1, a64::w2, a64::ne);
		armAsm->Ldr(a64::w1, _vu1JitMem(_vu1Off(VU1.clipflag)));
		armAsm->Lsl(a64::w1, a64::w1, 6);
		for (int i = 0; i < 3; i++)
		{
			armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.VF[fs].UL[i])));
			armAsm->Cmp(a64::w2, a64::w0);
			armAsm->Cset(a64::w3, a64::gt);
			armAsm->Orr(a64::w1, a64::w1, a64::Operand(a64::w3, a64::LSL, 2 * i));
			armAsm->Eor(a64::w2, a64::w2, 0x80000000);
			armAsm->Cmp(a64::w2, a64::w0);
			armAsm->Cset(a64::w3, a64::gt);
			armAsm->Orr(a64::w1, a64::w1, a64::Operand(a64::w3, a64::LSL, 2 * i + 1));
		}
		armAsm->And(a64::w1, a64::w1, 0xffffff);
		armAsm->Str(a64::w1, _vu1JitMem(_vu1Off(VU1.clipflag)));
		return true;
	}
	return false;
}

static void _vu1JitEmitUpper(FnPtr_VuVoid f, u32 code, bool dead = false)
{
	if (!_vu1JitEmitFmac(f, code, dead) && !_vu1JitEmitUpperInt(f, code))
		_vu1JitEmitHandler(f);
}

static s32 _vu1Imm11(u32 code)
{
	return (code & 0x400) ? static_cast<s32>(code & 0x3ff) - 0x400 : static_cast<s32>(code & 0x3ff);
}

// x1 = VU1.Mem + ((VI[reg] + imm) & 0x3ff) * 16, as GET_VU_MEM for VU1 after the u16 truncation.
static void _vu1JitEmitMemAddrImm(u32 reg, s32 imm)
{
	armAsm->Ldrh(a64::w0, _vu1JitMem(_vu1Off(VU1.VI[reg])));
	if (imm)
		armAsm->Add(a64::w0, a64::w0, imm);
	armAsm->And(a64::w0, a64::w0, 0x3ff);
	armAsm->Ldr(a64::x1, _vu1JitMem(_vu1Off(VU1.Mem)));
	armAsm->Add(a64::x1, a64::x1, a64::Operand(a64::x0, a64::LSL, 4));
}

static void _vu1JitEmitMemAddr(u32 reg, u32 code)
{
	_vu1JitEmitMemAddrImm(reg, _vu1Imm11(code));
}

static a64::MemOperand _vu1JitVI(u32 reg)
{
	return _vu1JitMem(_vu1Off(VU1.VI[reg]));
}

// _vuBackupVI(&VU1, reg)
static void _vu1JitEmitBackupVI(u32 reg)
{
	a64::Label set, done;
	armAsm->Ldrb(a64::w2, _vu1JitMem(_vu1Off(VU1.VIBackupCycles)));
	armAsm->Cbz(a64::w2, &set);
	armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.VIRegNumber)));
	armAsm->Cmp(a64::w2, reg);
	armAsm->B(a64::eq, &done);
	armAsm->Bind(&set);
	armAsm->Mov(a64::w2, reg);
	armAsm->Str(a64::w2, _vu1JitMem(_vu1Off(VU1.VIRegNumber)));
	armAsm->Ldrh(a64::w2, _vu1JitVI(reg));
	armAsm->Str(a64::w2, _vu1JitMem(_vu1Off(VU1.VIOldValue)));
	armAsm->Bind(&done);
	armAsm->Mov(a64::w2, 2);
	armAsm->Strb(a64::w2, _vu1JitMem(_vu1Off(VU1.VIBackupCycles)));
}

// w = VI[reg] as s16, the value before the last write while VIBackupCycles is set (conditional branches).
static void _vu1JitEmitBranchSrc(const a64::Register& w, u32 reg)
{
	a64::Label done;
	armAsm->Ldrsh(w, _vu1JitVI(reg));
	armAsm->Ldrb(a64::w3, _vu1JitMem(_vu1Off(VU1.VIBackupCycles)));
	armAsm->Cbz(a64::w3, &done);
	armAsm->Ldr(a64::w3, _vu1JitMem(_vu1Off(VU1.VIRegNumber)));
	armAsm->Cmp(a64::w3, reg);
	armAsm->B(a64::ne, &done);
	armAsm->Ldrsh(w, _vu1JitMem(_vu1Off(VU1.VIOldValue)));
	armAsm->Bind(&done);
}

// w1 = _branchAddr(&VU1)
static void _vu1JitEmitBranchAddr(u32 code)
{
	armAsm->Mov(a64::w1, s_vu1_jit_pending.tpc_v);
	armAsm->Add(a64::w1, a64::w1, _vu1Imm11(code) * 8);
	armAsm->And(a64::w1, a64::w1, 0x3fff);
}

// _setBranch(&VU1, w1)
static void _vu1JitEmitSetBranch()
{
	a64::Label first, done;
	armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.branch)));
	armAsm->Cmp(a64::w2, 1);
	armAsm->B(a64::ne, &first);
	armAsm->Str(a64::w1, _vu1JitMem(_vu1Off(VU1.delaybranchpc)));
	armAsm->Mov(a64::w2, 1);
	armAsm->Strb(a64::w2, _vu1JitMem(_vu1Off(VU1.takedelaybranch)));
	armAsm->B(&done);
	armAsm->Bind(&first);
	armAsm->Mov(a64::w2, 2);
	armAsm->Str(a64::w2, _vu1JitMem(_vu1Off(VU1.branch)));
	armAsm->Str(a64::w1, _vu1JitMem(_vu1Off(VU1.branchpc)));
	armAsm->Bind(&done);
}

// VI[it] = link address of BAL/JALR (it != 0)
static void _vu1JitEmitLink(u32 it)
{
	a64::Label first, done;
	armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.branch)));
	armAsm->Cmp(a64::w2, 1);
	armAsm->B(a64::ne, &first);
	armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.branchpc)));
	armAsm->B(&done);
	armAsm->Bind(&first);
	armAsm->Mov(a64::w2, s_vu1_jit_pending.tpc_v);
	armAsm->Bind(&done);
	armAsm->Add(a64::w2, a64::w2, 8);
	armAsm->Lsr(a64::w2, a64::w2, 3);
	armAsm->Strh(a64::w2, _vu1JitVI(it));
}

// VI[reg].US[0] += delta
static void _vu1JitEmitIncVI(u32 reg, int delta)
{
	armAsm->Ldrh(a64::w2, _vu1JitVI(reg));
	armAsm->Add(a64::w2, a64::w2, delta);
	armAsm->Strh(a64::w2, _vu1JitVI(reg));
}

// [x1] = VF[fs] on the lanes of xyzw (8 = x).
static void _vu1JitEmitStoreMem(u32 fs, u32 xyzw)
{
	if (xyzw == 0xf)
	{
		armAsm->Ldr(a64::q0, _vu1JitMem(_vu1Off(VU1.VF[fs])));
		armAsm->Str(a64::q0, a64::MemOperand(a64::x1));
		return;
	}
	for (int i = 0; i < 4; i++)
	{
		if (xyzw & (8 >> i))
		{
			armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.VF[fs].UL[i])));
			armAsm->Str(a64::w2, a64::MemOperand(a64::x1, i * 4));
		}
	}
}

// Integer, branch and VI-addressed load/store lower ops as their _vu* functions in VUops.cpp,
// including the VI backup. Returns false for the ops left to the handler.
static bool _vu1JitEmitLowerInt(FnPtr_VuVoid f, u32 code)
{
	const u32 xyzw = (code >> 21) & 0xf;
	const u32 ft = (code >> 16) & 0x1f;
	const u32 fs = (code >> 11) & 0x1f;
	const u32 it = ft & 0xf;
	const u32 is = fs & 0xf;
	const u32 id = (code >> 6) & 0xf;

	if (f == VU1MI_IADD || f == VU1MI_ISUB || f == VU1MI_IAND || f == VU1MI_IOR)
	{
		if (id == 0)
			return true;
		_vu1JitEmitBackupVI(id);
		armAsm->Ldrh(a64::w0, _vu1JitVI(is));
		armAsm->Ldrh(a64::w1, _vu1JitVI(it));
		if (f == VU1MI_IADD)
			armAsm->Add(a64::w0, a64::w0, a64::w1);
		else if (f == VU1MI_ISUB)
			armAsm->Sub(a64::w0, a64::w0, a64::w1);
		else if (f == VU1MI_IAND)
			armAsm->And(a64::w0, a64::w0, a64::w1);
		else
			armAsm->Orr(a64::w0, a64::w0, a64::w1);
		armAsm->Strh(a64::w0, _vu1JitVI(id));
	}
	else if (f == VU1MI_IADDI || f == VU1MI_IADDIU || f == VU1MI_ISUBIU)
	{
		if (it == 0)
			return true;
		_vu1JitEmitBackupVI(it);
		armAsm->Ldrh(a64::w0, _vu1JitVI(is));
		if (f == VU1MI_IADDI)
		{
			const s32 imm5 = static_cast<s32>(((code >> 6) & 0x1f) << 27) >> 27;
			armAsm->Add(a64::w0, a64::w0, imm5);
		}
		else
		{
			const u32 imm15 = ((code >> 10) & 0x7800) | (code & 0x7ff);
			if (f == VU1MI_IADDIU)
				armAsm->Add(a64::w0, a64::w0, imm15);
			else
				armAsm->Sub(a64::w0, a64::w0, imm15);
		}
		armAsm->Strh(a64::w0, _vu1JitVI(it));
	}
	else if (f == VU1MI_ILW || f == VU1MI_ILWR)
	{
		if (it == 0)
			return true;
		// The last of x, y, z, w selected is the one read.
		int lane = -1;
		for (int i = 0; i < 4; i++)
			if (xyzw & (8 >> i))
				lane = i;
		if (lane < 0)
			return true;
		_vu1JitEmitMemAddrImm(is, f == VU1MI_ILW ? _vu1Imm11(code) : 0);
		armAsm->Ldrh(a64::w2, a64::MemOperand(a64::x1, lane * 4));
		armAsm->Strh(a64::w2, _vu1JitVI(it));
	}
	else if (f == VU1MI_ISW || f == VU1MI_ISWR)
	{
		_vu1JitEmitMemAddrImm(is, f == VU1MI_ISW ? _vu1Imm11(code) : 0);
		armAsm->Ldrh(a64::w2, _vu1JitVI(it));
		for (int i = 0; i < 4; i++)
			if (xyzw & (8 >> i))
				armAsm->Str(a64::w2, a64::MemOperand(a64::x1, i * 4));
	}
	else if (f == VU1MI_LQI || f == VU1MI_LQD)
	{
		_vu1JitEmitBackupVI(is);
		if (f == VU1MI_LQD && is != 0)
			_vu1JitEmitIncVI(is, -1);
		if (ft != 0 && xyzw != 0)
		{
			_vu1JitEmitMemAddrImm(is, 0);
			armAsm->Ldr(a64::q0, a64::MemOperand(a64::x1));
			_vu1JitEmitStoreVF(ft, xyzw);
		}
		if (f == VU1MI_LQI && fs != 0)
			_vu1JitEmitIncVI(is, 1);
	}
	else if (f == VU1MI_SQI || f == VU1MI_SQD)
	{
		_vu1JitEmitBackupVI(it);
		if (f == VU1MI_SQD && ft != 0)
			_vu1JitEmitIncVI(it, -1);
		if (xyzw != 0)
		{
			_vu1JitEmitMemAddrImm(it, 0);
			_vu1JitEmitStoreMem(fs, xyzw);
		}
		if (f == VU1MI_SQI && ft != 0)
			_vu1JitEmitIncVI(it, 1);
	}
	else if (f == VU1MI_MFIR)
	{
		if (ft == 0 || xyzw == 0)
			return true;
		armAsm->Ldrsh(a64::w0, _vu1JitVI(is));
		armAsm->Dup(a64::v0.V4S(), a64::w0);
		_vu1JitEmitStoreVF(ft, xyzw);
	}
	else if (f == VU1MI_MTIR)
	{
		if (it == 0)
			return true;
		_vu1JitEmitBackupVI(it);
		armAsm->Ldrh(a64::w0, _vu1JitMem(_vu1Off(VU1.VF[fs].UL[(code >> 21) & 3])));
		armAsm->Strh(a64::w0, _vu1JitVI(it));
	}
	else if (f == VU1MI_MR32)
	{
		if (ft == 0 || xyzw == 0)
			return true;
		armAsm->Ldr(a64::q0, _vu1JitMem(_vu1Off(VU1.VF[fs])));
		armAsm->Ext(a64::v0.V16B(), a64::v0.V16B(), a64::v0.V16B(), 4);
		_vu1JitEmitStoreVF(ft, xyzw);
	}
	else if (f == VU1MI_IBEQ || f == VU1MI_IBNE || f == VU1MI_IBGEZ || f == VU1MI_IBGTZ || f == VU1MI_IBLEZ ||
			 f == VU1MI_IBLTZ)
	{
		a64::Label not_taken;
		_vu1JitEmitBranchSrc(a64::w4, is);
		if (f == VU1MI_IBEQ || f == VU1MI_IBNE)
		{
			_vu1JitEmitBranchSrc(a64::w5, it);
			armAsm->Cmp(a64::w5, a64::w4);
			armAsm->B(f == VU1MI_IBEQ ? a64::ne : a64::eq, &not_taken);
		}
		else
		{
			armAsm->Cmp(a64::w4, 0);
			armAsm->B(f == VU1MI_IBGEZ ? a64::lt : f == VU1MI_IBGTZ ? a64::le : f == VU1MI_IBLEZ ? a64::gt : a64::ge,
				&not_taken);
		}
		_vu1JitEmitBranchAddr(code);
		_vu1JitEmitSetBranch();
		armAsm->Bind(&not_taken);
	}
	else if (f == VU1MI_B || f == VU1MI_BAL)
	{
		_vu1JitEmitBranchAddr(code);
		if (f == VU1MI_BAL && it != 0)
			_vu1JitEmitLink(it);
		_vu1JitEmitSetBranch();
	}
	else if (f == VU1MI_JR || f == VU1MI_JALR)
	{
		armAsm->Ldrh(a64::w1, _vu1JitVI(is));
		armAsm->Lsl(a64::w1, a64::w1, 3);
		if (f == VU1MI_JALR && it != 0)
			_vu1JitEmitLink(it);
		_vu1JitEmitSetBranch();
	}
	else if (f == VU1MI_MFP)
	{
		if (ft == 0 || xyzw == 0)
			return true;
		armAsm->Ldr(a64::w0, _vu1JitVI(REG_P));
		armAsm->Dup(a64::v0.V4S(), a64::w0);
		_vu1JitEmitStoreVF(ft, xyzw);
	}
	else if (f == VU1MI_FSAND || f == VU1MI_FSEQ || f == VU1MI_FSOR)
	{
		if (it == 0)
			return true;
		const u32 imm12 = (((code >> 21) & 1) << 11) | (code & 0x7ff);
		armAsm->Ldrh(a64::w0, _vu1JitVI(REG_STATUS_FLAG));
		armAsm->And(a64::w0, a64::w0, 0xfff);
		if (f == VU1MI_FSAND)
		{
			armAsm->And(a64::w0, a64::w0, imm12);
		}
		else if (f == VU1MI_FSOR)
		{
			armAsm->Orr(a64::w0, a64::w0, imm12);
		}
		else
		{
			armAsm->Cmp(a64::w0, imm12);
			armAsm->Cset(a64::w0, a64::eq);
		}
		armAsm->Strh(a64::w0, _vu1JitVI(it));
	}
	else if (f == VU1MI_FSSET)
	{
		const u32 imm12 = (((code >> 21) & 1) << 11) | (code & 0x7ff);
		armAsm->Ldr(a64::w0, _vu1JitMem(_vu1Off(VU1.statusflag)));
		armAsm->And(a64::w0, a64::w0, 0x3f);
		if (imm12 & 0xfc0)
			armAsm->Orr(a64::w0, a64::w0, imm12 & 0xfc0);
		armAsm->Str(a64::w0, _vu1JitMem(_vu1Off(VU1.statusflag)));
		s_vu1_jit_fl.scratch = false;
	}
	else if (f == VU1MI_FMAND || f == VU1MI_FMEQ || f == VU1MI_FMOR)
	{
		if (it == 0)
			return true;
		armAsm->Ldrh(a64::w0, _vu1JitVI(REG_MAC_FLAG));
		armAsm->Ldrh(a64::w1, _vu1JitVI(is));
		if (f == VU1MI_FMAND)
		{
			armAsm->And(a64::w0, a64::w0, a64::w1);
		}
		else if (f == VU1MI_FMOR)
		{
			armAsm->Orr(a64::w0, a64::w0, a64::w1);
		}
		else
		{
			armAsm->Cmp(a64::w0, a64::w1);
			armAsm->Cset(a64::w0, a64::eq);
		}
		armAsm->Strh(a64::w0, _vu1JitVI(it));
	}
	else if (f == VU1MI_FCAND || f == VU1MI_FCEQ || f == VU1MI_FCOR)
	{
		const u32 imm24 = code & 0xffffff;
		armAsm->Ldr(a64::w0, _vu1JitVI(REG_CLIP_FLAG));
		armAsm->And(a64::w0, a64::w0, 0xffffff);
		if (f == VU1MI_FCAND)
		{
			armAsm->Tst(a64::w0, imm24);
			armAsm->Cset(a64::w0, a64::ne);
		}
		else
		{
			if (f == VU1MI_FCOR)
				armAsm->Orr(a64::w0, a64::w0, imm24);
			armAsm->Cmp(a64::w0, f == VU1MI_FCOR ? 0xffffff : imm24);
			armAsm->Cset(a64::w0, a64::eq);
		}
		armAsm->Strh(a64::w0, _vu1JitVI(1));
	}
	else if (f == VU1MI_FCSET)
	{
		armAsm->Mov(a64::w0, code & 0xffffff);
		armAsm->Str(a64::w0, _vu1JitMem(_vu1Off(VU1.clipflag)));
	}
	else if (f == VU1MI_FCGET)
	{
		if (it == 0)
			return true;
		armAsm->Ldr(a64::w0, _vu1JitVI(REG_CLIP_FLAG));
		armAsm->And(a64::w0, a64::w0, 0xfff);
		armAsm->Strh(a64::w0, _vu1JitVI(it));
	}
	else if (f == VU1MI_WAITQ || f == VU1MI_WAITP)
	{
	}
	else
	{
		return false;
	}
	return true;
}

// MINSS/MAXSS against +-max (a NaN becomes +max), as _vuFdivClamp.
static void _vu1JitEmitFdivClamp(const a64::VRegister& s, bool max_only)
{
	if (!EmuConfig.Cpu.Recompiler.vu1Overflow)
		return;
	armAsm->Mov(a64::w5, 0x7f7fffff);
	armAsm->Fmov(a64::s7, a64::w5);
	armAsm->Fcmp(s, a64::s7);
	armAsm->Fcsel(s, s, a64::s7, a64::mi);
	if (max_only)
		return;
	armAsm->Mov(a64::w5, 0xff7fffff);
	armAsm->Fmov(a64::s7, a64::w5);
	armAsm->Fcmp(s, a64::s7);
	armAsm->Fcsel(s, s, a64::s7, a64::gt);
}

// DIV, SQRT, RSQRT as _vuDIV/_vuSQRT/_vuRSQRT (VUops.cpp, ARM64 = x86 microVU). Returns false when
// the handler must be called.
static bool _vu1JitEmitFdivOp(FnPtr_VuVoid f, u32 code)
{
	if (f != VU1MI_DIV && f != VU1MI_SQRT && f != VU1MI_RSQRT)
		return false;
	const u32 ft = (code >> 16) & 0x1f;
	const u32 fs = (code >> 11) & 0x1f;
	const u32 fsf = (code >> 21) & 3;
	const u32 ftf = (code >> 23) & 3;
	a64::Label nonzero, store;
	// w0 = fs, w1 = ft, w2 = statusflag, w3 = q
	if (f != VU1MI_SQRT)
		armAsm->Ldr(a64::w0, _vu1JitMem(_vu1Off(VU1.VF[fs].UL[fsf])));
	armAsm->Ldr(a64::w1, _vu1JitMem(_vu1Off(VU1.VF[ft].UL[ftf])));
	armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.statusflag)));
	armAsm->And(a64::w2, a64::w2, ~0x30u);
	if (f == VU1MI_SQRT)
	{
		// I when the sign bit is set.
		armAsm->Lsr(a64::w4, a64::w1, 31);
		armAsm->Orr(a64::w2, a64::w2, a64::Operand(a64::w4, a64::LSL, 4));
		armAsm->And(a64::w1, a64::w1, 0x7fffffff);
		armAsm->Fmov(a64::s0, a64::w1);
		_vu1JitEmitFdivClamp(a64::s0, true);
		armAsm->Fsqrt(a64::s0, a64::s0);
		armAsm->Fmov(a64::w3, a64::s0);
	}
	else
	{
		// w4 = divisor bits: ft, or sqrt(|ft|) in s1
		if (f == VU1MI_RSQRT)
		{
			armAsm->And(a64::w4, a64::w1, 0x7fffffff);
			armAsm->Fmov(a64::s1, a64::w4);
			armAsm->Fsqrt(a64::s1, a64::s1);
			armAsm->Fmov(a64::w4, a64::s1);
		}
		else
		{
			armAsm->Mov(a64::w4, a64::w1);
			armAsm->Fmov(a64::s1, a64::w1);
		}
		armAsm->Tst(a64::w4, 0x7f800000);
		armAsm->B(&nonzero, a64::ne);
		// zero divisor: 0/0 = I, else D; q = signed max
		armAsm->Tst(a64::w0, 0x7f800000);
		armAsm->Mov(a64::w3, 0x10);
		armAsm->Mov(a64::w6, 0x20);
		armAsm->Csel(a64::w3, a64::w3, a64::w6, a64::eq);
		armAsm->Orr(a64::w2, a64::w2, a64::w3);
		if (f == VU1MI_DIV)
			armAsm->Eor(a64::w4, a64::w0, a64::w1);
		else
			armAsm->Mov(a64::w4, a64::w0);
		armAsm->And(a64::w4, a64::w4, 0x80000000);
		armAsm->Orr(a64::w3, a64::w4, 0x7f7fffff);
		armAsm->B(&store);
		armAsm->Bind(&nonzero);
		if (f == VU1MI_RSQRT)
		{
			armAsm->Lsr(a64::w4, a64::w1, 31);
			armAsm->Orr(a64::w2, a64::w2, a64::Operand(a64::w4, a64::LSL, 4));
		}
		armAsm->Fmov(a64::s0, a64::w0);
		armAsm->Fdiv(a64::s0, a64::s0, a64::s1);
		_vu1JitEmitFdivClamp(a64::s0, false);
		armAsm->Fmov(a64::w3, a64::s0);
	}
	armAsm->Bind(&store);
	armAsm->Str(a64::w3, _vu1JitMem(_vu1Off(VU1.q.UL)));
	armAsm->Str(a64::w2, _vu1JitMem(_vu1Off(VU1.statusflag)));
	s_vu1_jit_fl.scratch = false;
	return true;
}

// EFU ops but EATAN/EEXP/ESIN as _vuESADD.._vuESUM (VUops.cpp, ARM64 = x86 microVU): the register
// bits unclamped, the same single precision ops in the same order, the x86 default NaN. P is written
// here; the pipe entry follows. Returns false when the handler must be called.
static bool _vu1JitEmitEfuOp(FnPtr_VuVoid f, u32 code)
{
	const bool sum3 = f == VU1MI_ESADD || f == VU1MI_ERSADD || f == VU1MI_ELENG || f == VU1MI_ERLENG;
	const bool one = f == VU1MI_ERCPR || f == VU1MI_ESQRT || f == VU1MI_ERSQRT;
	if (!sum3 && !one && f != VU1MI_ESUM)
		return false;
	const u32 fs = (code >> 11) & 0x1f;
	const u32 fsf = (code >> 21) & 3;
	const a64::VRegister s[4] = {a64::s0, a64::s1, a64::s2, a64::s3};
	const u32 n = one ? 1 : sum3 ? 3 : 4;
	for (u32 i = 0; i < n; i++)
	{
		armAsm->Ldr(a64::w0, _vu1JitMem(_vu1Off(VU1.VF[fs].UL[one ? fsf : i])));
		if (f == VU1MI_ESQRT || f == VU1MI_ERSQRT)
			armAsm->And(a64::w0, a64::w0, 0x7fffffff);
		if (n == 4)
		{
			// NaN quieted, as _vuEfuR: x + w and y + z add two register operands
			armAsm->And(a64::w1, a64::w0, 0x7fffffff);
			armAsm->Orr(a64::w2, a64::w0, 0x400000);
			armAsm->Cmp(a64::w1, 0x7f800000);
			armAsm->Csel(a64::w0, a64::w2, a64::w0, a64::hi);
		}
		armAsm->Fmov(s[i], a64::w0);
	}
	if (sum3)
	{
		// (x*x + y*y) + z*z
		armAsm->Fmul(a64::s0, a64::s0, a64::s0);
		armAsm->Fmul(a64::s1, a64::s1, a64::s1);
		armAsm->Fadd(a64::s0, a64::s0, a64::s1);
		armAsm->Fmul(a64::s2, a64::s2, a64::s2);
		armAsm->Fadd(a64::s0, a64::s0, a64::s2);
	}
	else if (!one)
	{
		// (x + w) + (y + z)
		armAsm->Fadd(a64::s0, a64::s0, a64::s3);
		armAsm->Fadd(a64::s1, a64::s1, a64::s2);
		armAsm->Fadd(a64::s0, a64::s0, a64::s1);
	}
	if (f == VU1MI_ELENG || f == VU1MI_ERLENG || f == VU1MI_ESQRT || f == VU1MI_ERSQRT)
		armAsm->Fsqrt(a64::s0, a64::s0);
	if (f == VU1MI_ERSADD || f == VU1MI_ERLENG || f == VU1MI_ERSQRT || f == VU1MI_ERCPR)
	{
		armAsm->Fmov(a64::s1, 1.0f);
		armAsm->Fdiv(a64::s0, a64::s1, a64::s0);
	}
	armAsm->Fmov(a64::w0, a64::s0);
	armAsm->Mov(a64::w1, 0xffc00000);
	armAsm->Cmp(a64::w0, 0x7fc00000);
	armAsm->Csel(a64::w0, a64::w1, a64::w0, a64::eq);
	armAsm->Str(a64::w0, _vu1JitMem(_vu1Off(VU1.p.UL)));
	return true;
}

// MOVE, LQ, SQ and _vu1JitEmitLowerInt's ops as native code; other lower ops call the interpreter handler.
static void _vu1JitEmitLower(FnPtr_VuVoid f, u32 code)
{
	const u32 xyzw = (code >> 21) & 0xf;
	const u32 ft = (code >> 16) & 0x1f;
	const u32 fs = (code >> 11) & 0x1f;
	if (f == VU1MI_MOVE)
	{
		if (ft != 0 && xyzw != 0)
		{
			armAsm->Ldr(a64::q0, _vu1JitMem(_vu1Off(VU1.VF[fs])));
			_vu1JitEmitStoreVF(ft, xyzw);
		}
	}
	else if (f == VU1MI_LQ)
	{
		if (ft != 0 && xyzw != 0)
		{
			_vu1JitEmitMemAddr(fs, code);
			armAsm->Ldr(a64::q0, a64::MemOperand(a64::x1));
			_vu1JitEmitStoreVF(ft, xyzw);
		}
	}
	else if (f == VU1MI_SQ)
	{
		if (xyzw != 0)
		{
			_vu1JitEmitMemAddr(ft, code);
			if (xyzw == 0xf)
			{
				armAsm->Ldr(a64::q0, _vu1JitMem(_vu1Off(VU1.VF[fs])));
				armAsm->Str(a64::q0, a64::MemOperand(a64::x1));
			}
			else
			{
				for (int i = 0; i < 4; i++)
				{
					if (xyzw & (8 >> i))
					{
						armAsm->Ldr(a64::w2, _vu1JitMem(_vu1Off(VU1.VF[fs].UL[i])));
						armAsm->Str(a64::w2, a64::MemOperand(a64::x1, i * 4));
					}
				}
			}
		}
	}
	else if (!_vu1JitEmitFdivOp(f, code) && !_vu1JitEmitEfuOp(f, code) && !_vu1JitEmitLowerInt(f, code))
	{
		_vu1JitEmitHandler(f);
	}
}

static void _vu1JitReset()
{
	if (!s_vu1_jit_start)
	{
		s_vu1_jit_start = SysMemory::GetVU1Rec() + VU1_JIT_POOL_SIZE;
		s_vu1_jit_end = SysMemory::GetVU1RecEnd();
	}
	s_vu1_jit_pool.Init(SysMemory::GetVU1Rec(), VU1_JIT_POOL_SIZE);
	s_vu1_jit_ptr = s_vu1_jit_start;
	if (s_vu1_jit_map)
		std::fprintf(s_vu1_jit_map, "reset\n");
	for (VU1TraceSlot& slot : s_vu1_traces)
		for (auto& v : slot.variants)
			if (v)
				v->code = nullptr;
}

// The lower ops that call _vuBackupVI, the only writer of VIBackupCycles but the per-step decrement.
static bool _vu1BacksUpVI(FnPtr_VuVoid f)
{
	return f == VU1MI_IADD || f == VU1MI_IADDI || f == VU1MI_IADDIU || f == VU1MI_IAND || f == VU1MI_IOR ||
		   f == VU1MI_ISUB || f == VU1MI_ISUBIU || f == VU1MI_LQD || f == VU1MI_LQI || f == VU1MI_MTIR ||
		   f == VU1MI_SQD || f == VU1MI_SQI;
}

// After the epilogue: the checks of InterpVU1::Execute and _vu1TraceStep for running the next
// trace, then a tail jump to it, else return. Only x0-x15 are used (x16/x17 are VIXL scratch).
static void _vu1JitEmitChain(VU1Trace& t)
{
	_vu1JitMark("chain");
	const auto field = [&t](const void* p) { return static_cast<s64>(static_cast<const u8*>(p) - reinterpret_cast<const u8*>(&t)); };
	const s64 off_tcode = field(&t.code);
	const s64 off_tgen = field(&t.micro_gen);
	const s64 off_trel = field(&t.last_start_rel);
	a64::Label out, found;
	armMoveAddressToReg(a64::x9, &VU1);
	const auto vu = [](s64 off) { return a64::MemOperand(a64::x9, off); };
	armAsm->Ldr(a64::w10, vu(_vu1Off(VU1.branch)));
	armAsm->Cbnz(a64::w10, &out);
	armAsm->Ldr(a64::w10, vu(_vu1Off(VU1.ebit)));
	armAsm->Cbnz(a64::w10, &out);
	armMoveAddressToReg(a64::x11, &VU0.VI[REG_VPU_STAT].UL);
	armAsm->Ldr(a64::w10, a64::MemOperand(a64::x11));
	armAsm->Tbz(a64::w10, 8, &out);
	armAsm->Ldr(a64::w12, vu(_vu1Off(VU1.VI[REG_TPC].UL)));
	armAsm->And(a64::w12, a64::w12, VU1_PROGMASK);
	armMoveAddressToReg(a64::x11, &s_vu1_trace_epoch);
	armAsm->Ldr(a64::w11, a64::MemOperand(a64::x11));
	for (const auto& s : t.succ)
	{
		a64::Label next;
		armMoveAddressToReg(a64::x13, &s);
		armAsm->Ldr(a64::x14, a64::MemOperand(a64::x13, offsetof(std::remove_reference_t<decltype(s)>, t)));
		armAsm->Cbz(a64::x14, &next);
		armAsm->Ldp(a64::w15, a64::w10, a64::MemOperand(a64::x13, offsetof(std::remove_reference_t<decltype(s)>, pc)));
		static_assert(offsetof(std::remove_reference_t<decltype(s)>, pc) + 4 == offsetof(std::remove_reference_t<decltype(s)>, epoch));
		armAsm->Cmp(a64::w15, a64::w12);
		armAsm->Ccmp(a64::w10, a64::w11, a64::NoFlag, a64::eq);
		armAsm->B(&found, a64::eq);
		armAsm->Bind(&next);
	}
	armAsm->B(&out);

	armAsm->Bind(&found); // x14 = the successor
	armAsm->Ldr(a64::w10, a64::MemOperand(a64::x14, off_tgen));
	armMoveAddressToReg(a64::x11, &s_vu1_micro_gen);
	armAsm->Ldr(a64::w11, a64::MemOperand(a64::x11));
	armAsm->Cmp(a64::w10, a64::w11);
	armAsm->B(&out, a64::ne);
	armAsm->Ldr(a64::x15, a64::MemOperand(a64::x14, off_tcode));
	armAsm->Cbz(a64::x15, &out);
	armAsm->Ldr(a64::w10, a64::MemOperand(a64::x14, off_trel));
	armAsm->Ldr(a64::x11, vu(_vu1Off(VU1.cycle)));
	armAsm->Add(a64::x11, a64::x11, a64::x10);
	armMoveAddressToReg(a64::x13, &s_vu1_chain);
	armAsm->Ldr(a64::x10, a64::MemOperand(a64::x13, offsetof(decltype(s_vu1_chain), limit)));
	armAsm->Cmp(a64::x11, a64::x10);
	armAsm->B(&out, a64::hs);
	armAsm->Str(a64::w12, vu(_vu1Off(VU1.VI[REG_TPC].UL)));
	armAsm->Str(a64::x14, a64::MemOperand(a64::x13, offsetof(decltype(s_vu1_chain), last)));
	armAsm->Br(a64::x15);
	armAsm->Bind(&out);
}

// PCSX2_VU1_JIT_SELFTEST=<n>: at the first trace compile, run each native FDIV/EFU op and its
// handler on the same <n> generated inputs (special exponents, zeros, random bits) and log the
// count whose Q, P or status flag differ. VU1 is restored afterwards.
static void _vu1JitSelfTest()
{
	static const u32 count = [] {
		const char* env = std::getenv("PCSX2_VU1_JIT_SELFTEST");
		return env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0u;
	}();
	static bool done = false;
	if (!count || done)
		return;
	done = true;

	static const struct { FnPtr_VuVoid f; const char* name; } ops[] = {
		{VU1MI_DIV, "DIV"}, {VU1MI_SQRT, "SQRT"}, {VU1MI_RSQRT, "RSQRT"}, {VU1MI_ESADD, "ESADD"},
		{VU1MI_ERSADD, "ERSADD"}, {VU1MI_ELENG, "ELENG"}, {VU1MI_ERLENG, "ERLENG"}, {VU1MI_ESUM, "ESUM"},
		{VU1MI_ERCPR, "ERCPR"}, {VU1MI_ESQRT, "ESQRT"}, {VU1MI_ERSQRT, "ERSQRT"}, {VU1MI_OPMULA, "OPMULA"},
		{VU1MI_OPMSUB, "OPMSUB"}};
	u32 rng = 0x12345678;
	const auto next = [&rng] {
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		return rng;
	};
	const auto value = [&next] {
		const u32 r = next(), m = next();
		switch (r & 7)
		{
			case 0: return m & 0x807fffff; // exponent 0
			case 1: return m | 0x7f800000; // exponent 255
			case 2: return m & 0x80000000; // +-0
			case 3: return (m & 0x807fffff) | ((r >> 3 & 1) ? 0x7f000000 : 0x00800000); // exponent 254 or 1
			case 4: return (m & 0x807fffff) | 0x3f800000; // [1, 2)
			default: return m;
		}
	};

	const std::unique_ptr<VURegs> saved = std::make_unique<VURegs>(VU1);
	for (const auto& op : ops)
	{
		// One function per fsf/ftf or dest (code bits 21-24); fs = 1, ft = 2, fd = 3.
		void (*fns[16])();
		for (u32 sel = 0; sel < 16; sel++)
		{
			const u32 code = (sel << 21) | (2 << 16) | (1 << 11) | (3 << 6);
			armSetAsmPtr(s_vu1_jit_ptr, s_vu1_jit_end - s_vu1_jit_ptr, &s_vu1_jit_pool);
			u8* const fn = armStartBlock();
			armAsm->Stp(a64::x29, a64::lr, a64::MemOperand(a64::sp, -16, a64::PreIndex));
			armAsm->Stp(a64::x19, a64::x20, a64::MemOperand(a64::sp, -16, a64::PreIndex));
			armAsm->Stp(a64::x21, a64::x22, a64::MemOperand(a64::sp, -16, a64::PreIndex));
			armMoveAddressToReg(s_vu1_jit_rvu, &VU1);
			armMoveAddressToReg(a64::x21, s_vu1_jit_fconst);
			s_vu1_jit_vf = {};
			if (!_vu1JitEmitFdivOp(op.f, code) && !_vu1JitEmitEfuOp(op.f, code) && !_vu1JitEmitFmac(op.f, code))
				armEmitCall(reinterpret_cast<const void*>(op.f));
			armAsm->Ldp(a64::x21, a64::x22, a64::MemOperand(a64::sp, 16, a64::PostIndex));
			armAsm->Ldp(a64::x19, a64::x20, a64::MemOperand(a64::sp, 16, a64::PostIndex));
			armAsm->Ldp(a64::x29, a64::lr, a64::MemOperand(a64::sp, 16, a64::PostIndex));
			armAsm->Ret();
			s_vu1_jit_ptr = armEndBlock();
			fns[sel] = reinterpret_cast<void (*)()>(fn);
		}
		// Outputs: Q, P, status, MAC, ACC, VF[3] (OPMSUB's Fd).
		const auto get = [](u32* o) {
			o[0] = VU1.q.UL;
			o[1] = VU1.p.UL;
			o[2] = VU1.statusflag;
			o[3] = VU1.macflag;
			std::memcpy(&o[4], &VU1.ACC, 16);
			std::memcpy(&o[8], &VU1.VF[3], 16);
		};
		const auto set = [](const u32* o) {
			VU1.q.UL = o[0];
			VU1.p.UL = o[1];
			VU1.statusflag = o[2];
			VU1.macflag = o[3];
			std::memcpy(&VU1.ACC, &o[4], 16);
			std::memcpy(&VU1.VF[3], &o[8], 16);
		};
		u32 failed = 0;
		for (u32 i = 0; i < count; i++)
		{
			const u32 sel = next() & 15;
			u32 in[12], want[12], got[12];
			for (u32 j = 0; j < 4; j++)
			{
				VU1.VF[1].UL[j] = value();
				VU1.VF[2].UL[j] = value();
			}
			for (u32 j = 0; j < 12; j++)
				in[j] = j >= 4 ? value() : j == 3 ? next() & 0xffff : next(); // MAC is 16 bits
			VU1.code = (sel << 21) | (2 << 16) | (1 << 11) | (3 << 6);
			set(in);
			op.f();
			get(want);
			set(in);
			fns[sel]();
			get(got);
			if (std::memcmp(want, got, sizeof(want)) != 0 && failed++ < 3)
			{
				u32 k = 0;
				while (want[k] == got[k])
					k++;
				Console.WriteLn("VU1 JIT selftest: %s fs %08x %08x %08x %08x ft %08x %08x %08x %08x acc %08x %08x %08x "
								"%08x mac %08x sel %u: output %u = %08x, JIT %08x",
					op.name, VU1.VF[1].UL[0], VU1.VF[1].UL[1], VU1.VF[1].UL[2], VU1.VF[1].UL[3], VU1.VF[2].UL[0],
					VU1.VF[2].UL[1], VU1.VF[2].UL[2], VU1.VF[2].UL[3], in[4], in[5], in[6], in[7], in[3], sel, k,
					want[k], got[k]);
			}
		}
		Console.WriteLn("VU1 JIT selftest: %s %u checked, %u differ", op.name, count, failed);
	}
	VU1 = *saved;
	_vu1JitReset();
}

// FMAC entries whose mac and status are dead: the next retire replaces VI[MAC] and the
// non-sticky VI[STATUS] before any read, and the next native FMAC op replaces
// VU1.macflag/statusflag before any read; only the entry's sticky bits are kept (v31).
// Entry k and the next FMAC pipe entry m: native FMAC ops other than OPMULA/OPMSUB, no STATUS in
// the flagreg, retire within the trace. No lower op touches VI MAC/STATUS from k to the retire
// of m, none reads VU1.statusflag (FDIV) from k to m. The entries pending at k are known and do
// not write STATUS (their retire would replace sticky bits a fold already set).
static std::vector<bool> _vu1JitFlagsDead(const VU1Trace& t, const std::vector<bool>& in_trace)
{
	constexpr u32 vi_flags = (1u << REG_STATUS_FLAG) | (1u << REG_MAC_FLAG);
	const size_t n = t.steps.size();
	const auto no_status = [&t](int k) { return !(_vu1FmacFlagreg(t.steps[k].d) & (1u << REG_STATUS_FLAG)); };
	const auto native = [&t, &in_trace, &no_status](size_t k) {
		const VU1DecodedPair& d = t.steps[k].d;
		const VU1FmacDesc* desc = _vuJitFmacDesc(s_vu1_fmac_ops, std::size(s_vu1_fmac_ops), d.upper,
			static_cast<u32>(t.steps[k].code >> 32));
		return desc && desc->bc != VU1_BC_OP && in_trace[k] && no_status(static_cast<int>(k)) &&
			   (d.ibit || d.lregs.pipe != VUPIPE_FMAC);
	};
	const auto lo_flags = [&t](size_t k) {
		const VU1DecodedPair& d = t.steps[k].d;
		return !d.ibit && ((d.lregs.VIread | d.lregs.VIwrite) & vi_flags);
	};
	const auto lo_fdiv = [&t](size_t k) { return !t.steps[k].d.ibit && t.steps[k].d.lregs.pipe == VUPIPE_FDIV; };
	std::vector<size_t> retire(n, n);
	std::vector<bool> older_ok(n);
	std::deque<int> pipe(t.sig.fmaccount, -1);
	for (size_t k = 0; k < n; k++)
	{
		for (u32 i = 0; i < t.steps[k].fmac_retire && !pipe.empty(); i++)
		{
			if (pipe.front() >= 0)
				retire[pipe.front()] = k;
			pipe.pop_front();
		}
		if (!t.steps[k].d.fmac)
			continue;
		older_ok[k] = std::all_of(pipe.begin(), pipe.end(), [&no_status](int e) { return e >= 0 && no_status(e); });
		pipe.push_back(static_cast<int>(k));
	}
	std::vector<bool> dead(n);
	for (size_t k = 0; k < n; k++)
	{
		if (!t.steps[k].d.fmac || !older_ok[k] || !native(k) || lo_flags(k) || lo_fdiv(k))
			continue;
		size_t m = k + 1;
		while (m < n && !t.steps[m].d.fmac)
			m++;
		if (m == n || !native(m) || retire[m] == n)
			continue;
		bool ok = true;
		for (size_t j = k + 1; j < m; j++)
			ok = ok && !lo_flags(j) && !lo_fdiv(j);
		for (size_t j = m; j < retire[m]; j++)
			ok = ok && !lo_flags(j);
		dead[k] = ok;
	}
	return dead;
}

static void _vu1JitCompile(VU1Trace& t)
{
	if (!s_vu1_jit_start)
		_vu1JitReset();
	_vu1JitSelfTest();
	if (!s_vu1_jit_start ||
		static_cast<size_t>(s_vu1_jit_end - s_vu1_jit_ptr) < t.steps.size() * VU1_JIT_MAX_STEP_CODE + 512)
		_vu1JitReset();

	armSetAsmPtr(s_vu1_jit_ptr, s_vu1_jit_end - s_vu1_jit_ptr, &s_vu1_jit_pool);
	u8* const fn = armStartBlock();
	s_vu1_jit_pending = {};
	s_vu1_jit_fconst_live = false;
	s_vu1_jit_fl = {};
	s_vu1_jit_vf = {};
	s_vu1_jit_vf.out_reg = -1;
	// The XGKICK transfer calls, out of line after the trace: kicks are rare and taken branches cost.
	struct VU1JitKick
	{
		a64::Label call, back;
		VU1JitPending pending;
		bool fconst_live, acc_live, sticky;
	};
	std::deque<VU1JitKick> kicks;
	std::deque<VuJitCold> cold;
	s_vu_jit_cold = &cold;
	_vu1JitMark("pro");

	const a64::Register& rvu = a64::x19;
	const a64::Register& rbase = a64::x20;
	const auto vu = [&](s64 off) { return a64::MemOperand(rvu, off); };
	const auto slot = [](int i) { return a64::MemOperand(a64::sp, 32 + i * 16); };
	const s64 off_cycle = _vu1Off(VU1.cycle);

	armAsm->Stp(a64::x29, a64::lr, a64::MemOperand(a64::sp, -208, a64::PreIndex));
	armAsm->Stp(rvu, rbase, a64::MemOperand(a64::sp, 16));
	armAsm->Stp(a64::x21, a64::x22, a64::MemOperand(a64::sp, 96));
	armAsm->Stp(a64::x23, a64::x24, a64::MemOperand(a64::sp, 112));
	armAsm->Stp(a64::x25, a64::x26, a64::MemOperand(a64::sp, 128));
	armAsm->Stp(a64::x27, a64::x28, a64::MemOperand(a64::sp, 144));
	armMoveAddressToReg(a64::x21, s_vu1_jit_fconst);
	armMoveAddressToReg(rvu, &VU1);
	armAsm->Ldr(rbase, vu(off_cycle));
	armAsm->Ldr(a64::w27, vu(_vu1Off(VU1.fmacreadpos)));
	// FMAC entries by pipe position: -1 = pending at trace entry, else the step that adds it.
	// Which added entries retire within the trace, and which entry bases the code uses.
	int fmac_src[4] = {-1, -1, -1, -1};
	std::vector<bool> fmac_in_trace(t.steps.size());
	u32 fmac_bases = 0;
	{
		u32 ret = 0, cnt = t.sig.fmaccount;
		for (size_t k = 0; k < t.steps.size(); k++)
		{
			for (u32 i = 0; i < t.steps[k].fmac_retire; i++, ret++, cnt--)
			{
				if (fmac_src[ret & 3] >= 0)
					fmac_in_trace[fmac_src[ret & 3]] = true;
				fmac_bases |= 1u << (ret & 3);
			}
			if (t.steps[k].d.fmac)
			{
				fmac_src[(ret + cnt) & 3] = static_cast<int>(k);
				fmac_bases |= 1u << ((ret + cnt) & 3);
				cnt++;
			}
		}
		std::fill(std::begin(fmac_src), std::end(fmac_src), -1);
	}
	const std::vector<bool> fmac_dead = _vu1JitFlagsDead(t, fmac_in_trace);
	armAsm->Mov(a64::w6, sizeof(fmacPipe));
	for (u32 i = 0; i < 4; i++)
	{
		if (!(fmac_bases & (1u << i)))
			continue;
		armAsm->Add(a64::w0, a64::w27, i);
		armAsm->And(a64::w0, a64::w0, 3);
		armAsm->Umaddl(_vu1JitFmacBase(i), a64::w0, a64::w6, rvu);
	}
	s_vu1_jit_pending.fmac_cnt = t.sig.fmaccount;

	u32 pc = VU1.VI[REG_TPC].UL;
	bool branch_seen = false;
	// w22 = VU1.xgkickenable: only XGKICK (never in a trace) sets it, only _vu1JitXGKICK clears it.
	armAsm->Ldr(a64::w22, vu(_vu1Off(VU1.xgkickenable)));
	// Steps that may still see VIBackupCycles != 0: it is at most 2, set by the VI-writing lower
	// handlers, and every step takes at least one cycle off it.
	u32 vib_steps = 2;
	u32 prev_rel = 0;
	for (const VU1TraceStep& s : t.steps)
	{
		const VU1DecodedPair& d = s.d;
		const size_t step = &s - t.steps.data();
		const u32 lo = static_cast<u32>(s.code);
		const u32 hi = static_cast<u32>(s.code >> 32);
		branch_seen |= _vu1IsBranch(reinterpret_cast<const u32*>(&s.code), d);

		_vu1JitMark("step");
		pc += 8;
		s_vu1_jit_pending.tpc = s_vu1_jit_pending.cycle = true;
		s_vu1_jit_pending.tpc_v = pc;
		s_vu1_jit_pending.cycle_rel = s.cycle_rel;

		_vu1JitMark("vib");
		// VIBackupCycles -= min((u8)(cycle - cyclesBeforeOp), VIBackupCycles)
		if (vib_steps)
		{
			vib_steps--;
			a64::Label no_vib;
			armAsm->Ldrb(a64::w0, vu(_vu1Off(VU1.VIBackupCycles)));
			armAsm->Cbz(a64::w0, &no_vib);
			armAsm->Mov(a64::w3, static_cast<u8>(s.cycle_rel - prev_rel));
			armAsm->Cmp(a64::w0, a64::w3);
			armAsm->Csel(a64::w4, a64::w3, a64::w0, a64::hi);
			armAsm->Sub(a64::w0, a64::w0, a64::w4);
			armAsm->Strb(a64::w0, vu(_vu1Off(VU1.VIBackupCycles)));
			armAsm->Bind(&no_vib);
		}
		prev_rel = s.cycle_rel;

		_vu1JitMark("retire");
		if (s.efu_dec)
		{
			armAsm->Ldr(a64::w0, vu(_vu1Off(VU1.efu.Cycle)));
			armAsm->Sub(a64::w0, a64::w0, 1);
			armAsm->Str(a64::w0, vu(_vu1Off(VU1.efu.Cycle)));
		}
		for (u32 i = 0; i < s.fmac_retire; i++)
		{
			const int src = fmac_src[s_vu1_jit_pending.fmac_ret & 3];
			_vu1JitEmitFMACretire(_vu1JitFmacBase(s_vu1_jit_pending.fmac_ret), src < 0 ? -1 : _vu1FmacFlagreg(t.steps[src].d),
				src >= 0 && fmac_in_trace[src] ? static_cast<int>(s_vu1_jit_pending.fmac_ret & 3) : -1,
				src >= 0 && fmac_dead[src]);
			s_vu1_jit_pending.fmac_ret++;
			s_vu1_jit_pending.fmac_cnt--;
			s_vu1_jit_pending.fmac = true;
		}
		if (s.fdiv_retire)
		{
			// Q = fdiv.reg, status bits 4-5 and 10-11 from the FDIV entry.
			armAsm->Str(a64::wzr, vu(_vu1Off(VU1.fdiv.enable)));
			armAsm->Ldr(a64::w0, vu(_vu1Off(VU1.fdiv.reg.UL)));
			armAsm->Str(a64::w0, vu(_vu1Off(VU1.VI[REG_Q].UL)));
			_vu1JitStatusLoad();
			armAsm->Ldr(a64::w2, vu(_vu1Off(VU1.fdiv.statusflag)));
			armAsm->And(a64::w28, a64::w28, 0xFCF);
			armAsm->And(a64::w2, a64::w2, 0xC30);
			armAsm->Orr(a64::w28, a64::w28, a64::w2);
			s_vu1_jit_fl.st_dirty = true;
		}
		if (s.efu_retire)
		{
			armAsm->Str(a64::wzr, vu(_vu1Off(VU1.efu.enable)));
			armAsm->Ldr(a64::w0, vu(_vu1Off(VU1.efu.reg.UL)));
			armAsm->Str(a64::w0, vu(_vu1Off(VU1.VI[REG_P].UL)));
		}
		for (u32 i = 0; i < s.ialu_retire; i++)
			_vu1JitEmitPosStep(_vu1Off(VU1.ialureadpos), _vu1Off(VU1.ialucount), -1);

		_vu1JitMark("kick");
		kicks.emplace_back();
		kicks.back().pending = s_vu1_jit_pending;
		kicks.back().fconst_live = s_vu1_jit_fconst_live;
		kicks.back().acc_live = s_vu1_jit_vf.acc_live;
		kicks.back().sticky = s_vu1_jit_fl.sticky;
		armAsm->Cbnz(a64::w22, &kicks.back().call);
		armAsm->Bind(&kicks.back().back);

		_vu1JitMark("bk");
		// Native lower ops read and write VI[STATUS]/VI[MAC] in memory.
		constexpr u32 vi_flags = (1u << REG_STATUS_FLAG) | (1u << REG_MAC_FLAG);
		const bool lo_flags = !d.ibit && ((d.lregs.VIread | d.lregs.VIwrite) & vi_flags);
		if (lo_flags)
			_vu1JitFlagsStore();
		if (d.ibit)
		{
			s_vu1_jit_pending.code = true;
			s_vu1_jit_pending.code_v = hi;
			_vu1JitMark("U", reinterpret_cast<const void*>(d.upper));
			_vu1JitEmitUpper(d.upper, hi, fmac_dead[step]);
			_vu1JitMark("bk");
			armAsm->Mov(a64::w0, lo);
			armAsm->Str(a64::w0, vu(_vu1Off(VU1.VI[REG_I].UL)));
		}
		else
		{
			const s64 off_vf = _vu1Off(VU1.VF[d.vfreg]);
			const s64 off_vi = _vu1Off(VU1.VI[d.vireg]);
			if (d.vfreg)
			{
				armAsm->Ldr(a64::q0, vu(off_vf));
				armAsm->Str(a64::q0, slot(0));
			}
			if (d.vireg)
			{
				armAsm->Ldr(a64::q0, vu(off_vi));
				armAsm->Str(a64::q0, slot(2));
			}
			s_vu1_jit_pending.code = true;
			s_vu1_jit_pending.code_v = hi;
			_vu1JitMark("U", reinterpret_cast<const void*>(d.upper));
			_vu1JitEmitUpper(d.upper, hi, fmac_dead[step]);
			_vu1JitMark("bk");
			if (d.discard == 0)
			{
				if (d.vfreg)
				{
					armAsm->Ldr(a64::q0, vu(off_vf));
					armAsm->Str(a64::q0, slot(1));
					armAsm->Ldr(a64::q0, slot(0));
					armAsm->Str(a64::q0, vu(off_vf));
				}
				if (d.vireg)
				{
					armAsm->Ldr(a64::q0, vu(off_vi));
					armAsm->Str(a64::q0, slot(3));
					armAsm->Ldr(a64::q0, slot(2));
					armAsm->Str(a64::q0, vu(off_vi));
				}
				s_vu1_jit_pending.code = true;
				s_vu1_jit_pending.code_v = lo;
				_vu1JitMark("L", reinterpret_cast<const void*>(d.lower));
				_vu1JitEmitLower(d.lower, lo);
				_vu1JitMark("bk");
				if (d.vfreg)
				{
					armAsm->Ldr(a64::q0, slot(1));
					armAsm->Str(a64::q0, vu(off_vf));
				}
				if (d.vireg)
				{
					armAsm->Ldr(a64::q0, slot(3));
					armAsm->Str(a64::q0, vu(off_vi));
				}
			}
		}

		{
			u8* const cl = s_vu1_jit_vf.clean;
			const s8 out = s_vu1_jit_vf.out_reg;
			const u8 keep = out >= 0 ? cl[out] : 0;
			const u32 lo_vf = d.ibit ? 0 : d.lregs.VFwrite;
			cl[d.uregs.VFwrite] = 0;
			cl[lo_vf] = 0;
			if ((d.uregs.VIwrite | (d.ibit ? 0 : d.lregs.VIwrite)) & (1u << REG_ACC_FLAG))
				cl[32] = 0;
			if (out >= 0 && (out == 32 || static_cast<u32>(out) != lo_vf))
				cl[out] = keep | s_vu1_jit_vf.out_lanes;
			cl[0] = 0;
			s_vu1_jit_vf.out_reg = -1;
		}
		if (!d.ibit && _vu1BacksUpVI(d.lower))
			vib_steps = 2;
		if (lo_flags && (d.lregs.VIwrite & vi_flags))
			s_vu1_jit_fl.st_loaded = false;
		_vu1JitMark("addpipe");
		if (d.fmac)
		{
			VU1JitPending& p = s_vu1_jit_pending;
			const size_t k = &s - t.steps.data();
			fmac_src[(p.fmac_ret + p.fmac_cnt) & 3] = static_cast<int>(k);
			_vu1JitEmitAddFMAC(d, _vu1JitFmacBase(p.fmac_ret + p.fmac_cnt), rbase, s.cycle_rel, fmac_in_trace[k],
				(p.fmac_ret + p.fmac_cnt) & 3, fmac_dead[k]);
			p.fmac_cnt++;
			p.fmac = true;
		}
		if (d.lregs.pipe == VUPIPE_IALU && d.lregs.cycles != 0)
			_vu1JitEmitAddIALU(d.lregs, rbase, s.cycle_rel);
		else if ((d.lregs.pipe == VUPIPE_FDIV && (d.lregs.VIwrite & (1 << REG_Q))) ||
				 (d.lregs.pipe == VUPIPE_EFU && (d.lregs.VIwrite & (1 << REG_P))))
			_vu1JitEmitAddFdivEfu(d.lregs, rbase, s.cycle_rel);

		_vu1JitMark("branch");
		// VU1.branch is 0 at trace entry: it can only be set from the first branch pair on.
		if (branch_seen)
		{
			// The branch countdown of _vu1Exec, which may write TPC: nothing pending may overwrite it later.
			_vu1JitFlush(false, false);
			a64::Label no_branch;
			armAsm->Ldr(a64::w0, vu(_vu1Off(VU1.branch)));
			armAsm->Cbz(a64::w0, &no_branch);
			armAsm->Sub(a64::w1, a64::w0, 1);
			armAsm->Str(a64::w1, vu(_vu1Off(VU1.branch)));
			armAsm->Cmp(a64::w0, 1);
			armAsm->B(&no_branch, a64::ne);
			armAsm->Ldr(a64::w1, vu(_vu1Off(VU1.branchpc)));
			armAsm->Str(a64::w1, vu(_vu1Off(VU1.VI[REG_TPC].UL)));
			armAsm->Ldrb(a64::w2, vu(_vu1Off(VU1.takedelaybranch)));
			armAsm->Cbz(a64::w2, &no_branch);
			armAsm->Mov(a64::w1, 1);
			armAsm->Str(a64::w1, vu(_vu1Off(VU1.branch)));
			armAsm->Ldr(a64::w1, vu(_vu1Off(VU1.delaybranchpc)));
			armAsm->Str(a64::w1, vu(_vu1Off(VU1.branchpc)));
			armAsm->Strb(a64::wzr, vu(_vu1Off(VU1.takedelaybranch)));
			armAsm->Bind(&no_branch);
		}

	}

	_vu1JitMark("epi");
	_vu1JitFlush();
	_vu1JitFlagsStore();
	armAsm->Ldp(rvu, rbase, a64::MemOperand(a64::sp, 16));
	armAsm->Ldp(a64::x21, a64::x22, a64::MemOperand(a64::sp, 96));
	armAsm->Ldp(a64::x23, a64::x24, a64::MemOperand(a64::sp, 112));
	armAsm->Ldp(a64::x25, a64::x26, a64::MemOperand(a64::sp, 128));
	armAsm->Ldp(a64::x27, a64::x28, a64::MemOperand(a64::sp, 144));
	armAsm->Ldp(a64::x29, a64::lr, a64::MemOperand(a64::sp, 208, a64::PostIndex));
	if (s_vu1_jit_mode == VU1JitMode::On)
		_vu1JitEmitChain(t);
	armAsm->Ret();
	_vu1JitMark("kickcall");
	for (VU1JitKick& k : kicks)
	{
		armAsm->Bind(&k.call);
		s_vu1_jit_pending = k.pending;
		_vu1JitFlush(true, false);
		// The transfer does not touch the VI flags: only the flag registers are kept.
		const auto fl_save = [](int i) { return a64::MemOperand(a64::sp, VU1_JIT_FL_SAVE + i * 4); };
		armAsm->Stp(a64::w9, a64::w10, fl_save(0));
		armAsm->Stp(a64::w11, a64::w12, fl_save(2));
		armAsm->Stp(a64::w13, a64::w14, fl_save(4));
		armAsm->Str(a64::w15, fl_save(6));
		if (k.sticky)
			armAsm->Str(a64::q31, a64::MemOperand(a64::sp, VU1_JIT_STICKY_SAVE));
		armEmitCall(reinterpret_cast<const void*>(&_vu1JitXGKICK));
		if (k.sticky)
			armAsm->Ldr(a64::q31, a64::MemOperand(a64::sp, VU1_JIT_STICKY_SAVE));
		armAsm->Ldp(a64::w9, a64::w10, fl_save(0));
		armAsm->Ldp(a64::w11, a64::w12, fl_save(2));
		armAsm->Ldp(a64::w13, a64::w14, fl_save(4));
		armAsm->Ldr(a64::w15, fl_save(6));
		armAsm->Ldr(a64::w22, vu(_vu1Off(VU1.xgkickenable)));
		if (k.acc_live)
			armAsm->Ldr(a64::q30, vu(_vu1Off(VU1.ACC)));
		if (k.fconst_live)
			_vuJitEmitFconst(a64::x21);
		armAsm->B(&k.back);
	}
	_vu1JitMark("cold");
	for (VuJitCold& c : cold)
	{
		armAsm->Bind(&c.call);
		_vuJitEmitX86Slow(c.desc);
		armAsm->B(&c.back);
	}
	s_vu_jit_cold = nullptr;
	_vu1JitMark("end");
	if (s_vu1_jit_map)
		std::fflush(s_vu1_jit_map);

	s_vu1_jit_ptr = armEndBlock();
	t.code = reinterpret_cast<void (*)()>(fn);
}

static void _vu1TraceExec(VU1Trace& t)
{
	if (s_vu1_jit_ovf != _vu1JitOvf())
	{
		_vu1JitReset();
		s_vu1_jit_ovf = _vu1JitOvf();
	}
	if (!t.code)
		_vu1JitCompile(t);
	t.code();
}

// PCSX2_VU1_JIT=check: every trace also runs through the interpreter from the same start, and
// the VU1 registers and data memory of both runs are compared. The interpreter's result is kept.
static bool _vu1TraceCheck(VU1Trace& t)
{
	alignas(16) static VURegs start, ref;
	static u8 start_mem[VU1_MEMSIZE], ref_mem[VU1_MEMSIZE];
	static u64 checked, failed;

	// XGKICK transfers reach the GIF: they cannot run twice.
	if (VU1.xgkickenable)
		return false;

	const u32 start_stat = VU0.VI[REG_VPU_STAT].UL;
	std::memcpy(&start, &VU1, sizeof(VURegs));
	std::memcpy(start_mem, VU1.Mem, VU1_MEMSIZE);
	for (size_t i = 0; i < t.steps.size(); i++)
		vu1Exec(&VU1);
	std::memcpy(&ref, &VU1, sizeof(VURegs));
	std::memcpy(ref_mem, VU1.Mem, VU1_MEMSIZE);
	const u32 ref_stat = VU0.VI[REG_VPU_STAT].UL;

	std::memcpy(&VU1, &start, sizeof(VURegs));
	std::memcpy(VU1.Mem, start_mem, VU1_MEMSIZE);
	VU0.VI[REG_VPU_STAT].UL = start_stat;
	_vu1TraceExec(t);

	// _vu1Exec leaves an FMAC entry's write mask uninitialized for ops that write no VF register
	// (stack contents); the traces store 0. _vuFMACTestStall never matches register 0.
	// Retired entries are never read again; the traces store only the flags of those retiring inside.
	alignas(16) static VURegs ref_cmp;
	std::memcpy(&ref_cmp, &ref, sizeof(VURegs));
	for (VURegs* r : {&VU1, &ref_cmp})
	{
		for (u32 i = r->fmaccount; i < 4; i++)
			std::memset(&r->fmac[(r->fmacreadpos + i) & 3], 0, sizeof(fmacPipe));
		for (fmacPipe& f : r->fmac)
		{
			if (f.regupper == 0)
				f.xyzwupper = 0;
			if (f.reglower == 0)
				f.xyzwlower = 0;
		}
	}

	checked++;
	const u8* a = reinterpret_cast<const u8*>(&VU1);
	const u8* b = reinterpret_cast<const u8*>(&ref_cmp);
	size_t diff = sizeof(VURegs);
	for (size_t i = 0; i < sizeof(VURegs); i++)
	{
		if (a[i] != b[i])
		{
			diff = i;
			break;
		}
	}
	const bool mem_diff = std::memcmp(VU1.Mem, ref_mem, VU1_MEMSIZE) != 0;
	if (diff != sizeof(VURegs) || mem_diff || VU0.VI[REG_VPU_STAT].UL != ref_stat)
	{
		failed++;
		if (failed <= 20)
			Console.Error("VU1 trace check: pc %04x steps %zu differs: regs offset %zd, mem %d, stat %d (%llu of %llu)",
				start.VI[REG_TPC].UL, t.steps.size(), diff == sizeof(VURegs) ? -1 : static_cast<ptrdiff_t>(diff), mem_diff,
				VU0.VI[REG_VPU_STAT].UL != ref_stat, failed, checked);
	}
	if ((checked & (checked - 1)) == 0 && checked >= 1024)
		Console.WriteLn("VU1 trace check: %llu traces checked, %llu differ", checked, failed);

	std::memcpy(&VU1, &ref, sizeof(VURegs));
	std::memcpy(VU1.Mem, ref_mem, VU1_MEMSIZE);
	VU0.VI[REG_VPU_STAT].UL = ref_stat;
	return true;
}

// The trace for VU1's TPC and the given pipe state (nullptr: taken from VU1), or nullptr when
// the caller must step instead.
static VU1Trace* _vu1TraceLookup(u32 pc, const VU1PipeSig* known)
{
	const u32* ptr = reinterpret_cast<const u32*>(&VU1.Micro[pc]);
	if (_vu1TraceEnds(ptr, _vu1DecodedAt(pc)))
		return nullptr;

	VU1PipeSig sig;
	if (known)
		sig = *known;
	else if (!_vu1PipeSig(&VU1, &sig))
		return nullptr;

	VU1TraceSlot& slot = s_vu1_traces[pc >> 3];
	// Variants differ by pipe state and by code: programs that take turns at the same address
	// keep their traces.
	VU1Trace* t = nullptr;
	for (auto& v : slot.variants)
	{
		if (!v || std::memcmp(&v->sig, &sig, sizeof(sig)) != 0)
			continue;
		if (v->micro_gen != s_vu1_micro_gen)
		{
			u32 p = pc;
			bool same = true;
			for (const VU1TraceStep& s : v->steps)
			{
				const u32* sp = reinterpret_cast<const u32*>(&VU1.Micro[p]);
				if (s.code != (static_cast<u64>(sp[0]) | (static_cast<u64>(sp[1]) << 32)))
				{
					same = false;
					break;
				}
				p += 8;
			}
			if (!same)
				continue;
			v->micro_gen = s_vu1_micro_gen;
		}
		t = v.get();
		break;
	}
	if (!t)
	{
		std::unique_ptr<VU1Trace> nt = _vu1TraceCompile(pc, sig);
		if (nt->steps.empty())
			return nullptr;
		auto it = std::find_if(std::begin(slot.variants), std::end(slot.variants),
			[](const std::unique_ptr<VU1Trace>& v) { return !v; });
		if (it == std::end(slot.variants))
		{
			it = std::begin(slot.variants) + slot.next;
			slot.next = (slot.next + 1) % VU1_TRACE_VARIANTS;
		}
		if (*it)
			s_vu1_trace_epoch++;
		*it = std::move(nt);
		t = it->get();
		t->micro_gen = s_vu1_micro_gen;
	}
	return t;
}

// Runs one trace from VU1's TPC if it fits in the cycle budget of InterpVU1::Execute
// (which checks the budget before each step). Returns false when the caller must step instead.
bool _vu1TraceStep(u64 startcycles, u32 cycles)
{
	if (s_vu1_jit_mode == VU1JitMode::Off)
		return false;
	VU1.VI[REG_TPC].UL &= VU1_PROGMASK;
	const u32 pc = VU1.VI[REG_TPC].UL;

	// Straight after another trace the pipe state is that trace's exit_sig.
	VU1Trace* const prev = s_vu1_prev;
	s_vu1_prev = nullptr;
	VU1Trace* t = nullptr;
	if (prev)
	{
		for (const auto& s : prev->succ)
		{
			if (s.t && s.pc == pc && s.epoch == s_vu1_trace_epoch && s.t->micro_gen == s_vu1_micro_gen)
			{
				t = s.t;
				break;
			}
		}
		if (!t)
		{
			t = _vu1TraceLookup(pc, &prev->exit_sig);
			if (!t)
				return false;
			const bool keep0 = prev->succ[0].t && prev->succ[0].epoch == s_vu1_trace_epoch;
			auto& s = prev->succ[keep0 ? 1 : 0];
			s.pc = pc;
			s.epoch = s_vu1_trace_epoch;
			s.t = t;
		}
	}
	else
	{
		t = _vu1TraceLookup(pc, nullptr);
		if (!t)
			return false;
	}

	if ((VU1.cycle + t->last_start_rel - startcycles) >= cycles)
		return false;

	if (!(s_vu1_jit_mode == VU1JitMode::Check && _vu1TraceCheck(*t)))
	{
		s_vu1_chain.limit = startcycles + cycles;
		s_vu1_chain.last = t;
		_vu1TraceExec(*t);
		t = s_vu1_chain.last;
	}

	if (t->exit_known == 0)
	{
		t->exit_known = _vu1PipeSig(&VU1, &t->exit_sig) ? 1 : 2;
	}
	else if (s_vu1_jit_mode == VU1JitMode::Check)
	{
		static u64 checked, failed;
		VU1PipeSig s;
		const u8 known = _vu1PipeSig(&VU1, &s) ? 1 : 2;
		checked++;
		if (known != t->exit_known || (known == 1 && std::memcmp(&s, &t->exit_sig, sizeof(s)) != 0))
		{
			failed++;
			if (failed <= 20)
				Console.Error("VU1 trace check: exit pipe state of pc %04x differs (%llu of %llu)",
					pc, failed, checked);
		}
		if ((checked & (checked - 1)) == 0 && checked >= 1024)
			Console.WriteLn("VU1 trace check: %llu exit pipe states checked, %llu differ", checked, failed);
	}
	if (t->exit_known == 1)
		s_vu1_prev = t;
	return true;
}
