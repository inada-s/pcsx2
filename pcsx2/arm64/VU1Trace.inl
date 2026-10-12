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

static constexpr u32 VU1_TRACE_VARIANTS = 4;
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
static void _vu1JitFDIVretire()
{
	VU1.fdiv.enable = 0;
	VU1.VI[REG_Q].UL = VU1.fdiv.reg.UL;
	VU1.VI[REG_STATUS_FLAG].UL = (VU1.VI[REG_STATUS_FLAG].UL & 0xFCF) | (VU1.fdiv.statusflag & 0xC30);
}
static void _vu1JitEFUretire()
{
	VU1.efu.enable = 0;
	VU1.VI[REG_P].UL = VU1.efu.reg.UL;
}
static void _vu1JitXGKICK() { _vuXGKICKTransfer((VU1.cycle - VU1.xgkicklastcycle) - 1, false); }
static void _vu1JitBranch()
{
	if (VU1.branch-- == 1)
	{
		VU1.VI[REG_TPC].UL = VU1.branchpc;
		if (VU1.takedelaybranch)
		{
			VU1.branch = 1;
			VU1.branchpc = VU1.delaybranchpc;
			VU1.takedelaybranch = false;
		}
	}
}
static void _vu1JitAddLowerStalls(_VURegsNum* r) { _vuAddLowerStalls(&VU1, r); }

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
		armEmitCall(reinterpret_cast<const void*>(f));
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

// Retires the oldest FMAC entry (x5 = its base), as _vuFMACflush does; the caller tracks the
// read position and count.
static void _vu1JitEmitFMACretire()
{
	const s64 off_fmac = _vu1Off(VU1.fmac[0]);
	const s64 off_status = _vu1Off(VU1.VI[REG_STATUS_FLAG].UL);
	a64::Label no_clip, sticky, done;
	armAsm->Ldr(a64::w1, a64::MemOperand(a64::x5, off_fmac + offsetof(fmacPipe, flagreg)));
	armAsm->Tbz(a64::w1, REG_CLIP_FLAG, &no_clip);
	armAsm->Ldr(a64::w2, a64::MemOperand(a64::x5, off_fmac + offsetof(fmacPipe, clipflag)));
	armAsm->Str(a64::w2, _vu1JitMem(_vu1Off(VU1.VI[REG_CLIP_FLAG].UL)));
	armAsm->Bind(&no_clip);
	armAsm->Ldr(a64::w3, a64::MemOperand(a64::x5, off_fmac + offsetof(fmacPipe, statusflag)));
	armAsm->Ldr(a64::w4, _vu1JitMem(off_status));
	armAsm->And(a64::w2, a64::w3, 0xf);
	armAsm->Tbnz(a64::w1, REG_STATUS_FLAG, &sticky);
	armAsm->And(a64::w4, a64::w4, 0xff0);
	armAsm->Orr(a64::w4, a64::w4, a64::w2);
	armAsm->Orr(a64::w4, a64::w4, a64::Operand(a64::w2, a64::LSL, 6));
	armAsm->B(&done);
	armAsm->Bind(&sticky);
	armAsm->And(a64::w4, a64::w4, 0x30);
	armAsm->Orr(a64::w4, a64::w4, a64::w2);
	armAsm->And(a64::w2, a64::w3, 0xfc0);
	armAsm->Orr(a64::w4, a64::w4, a64::w2);
	armAsm->Bind(&done);
	armAsm->Str(a64::w4, _vu1JitMem(off_status));
	armAsm->Ldr(a64::w2, a64::MemOperand(a64::x5, off_fmac + offsetof(fmacPipe, macflag)));
	armAsm->Str(a64::w2, _vu1JitMem(_vu1Off(VU1.VI[REG_MAC_FLAG].UL)));
}

// _vuClearFMAC + _vuAddUpperStalls/_vuAddLowerStalls for the FMAC pipe: every field of the entry
// is stored (the static ones as constants), as the memset of _vuClearFMAC does. x5 = the entry;
// the caller tracks the write position and count.
static void _vu1JitEmitAddFMAC(const VU1DecodedPair& d, const a64::Register& rbase, u32 cycle_rel)
{
	const bool up = d.uregs.pipe == VUPIPE_FMAC;
	const bool lo = d.lregs.pipe == VUPIPE_FMAC;
	const s64 off_fmac = _vu1Off(VU1.fmac[0]);
	const auto entry = [off_fmac](size_t field) { return a64::MemOperand(a64::x5, off_fmac + field); };
	armAsm->Mov(a64::w1, up ? d.uregs.VFwrite : 0);
	armAsm->Mov(a64::w2, lo ? d.lregs.VFwrite : 0);
	armAsm->Stp(a64::w1, a64::w2, entry(offsetof(fmacPipe, regupper)));
	armAsm->Mov(a64::w1, (up ? d.uregs.VIwrite : 0) | (lo ? d.lregs.VIwrite : 0));
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

// Upper FMAC ops emitted natively (applyBinaryMACOp/applyTernaryMACOp in VUops.cpp).
// ADDi is left out (TriAce hack), so are Fd = 0 forms (they write RDzero).
enum class VU1FmacOp : u8 { Add, Sub, Mul, Madd, Msub };
static constexpr u8 VU1_BC_VEC = 4, VU1_BC_I = 5, VU1_BC_Q = 6; // 0..3: Ft.x..w
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
	VU1_FMAC_ALL(MUL, VU1FmacOp::Mul), VU1_FMAC_ALL(MADD, VU1FmacOp::Madd), VU1_FMAC_ALL(MSUB, VU1FmacOp::Msub)};
#undef VU1_FMAC_ALL
#undef VU1_FMAC_BC

// x21 points here in JIT code: exponent, sign, max float, MAC flag bits Z/S/U/O of x y z w.
alignas(16) static const u32 s_vu1_jit_fconst[7][4] = {
	{0x7f800000, 0x7f800000, 0x7f800000, 0x7f800000}, {0x80000000, 0x80000000, 0x80000000, 0x80000000},
	{0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff}, {0x0008, 0x0004, 0x0002, 0x0001},
	{0x0080, 0x0040, 0x0020, 0x0010}, {0x0800, 0x0400, 0x0200, 0x0100}, {0x8000, 0x4000, 0x2000, 0x1000}};

// Overflow clamp settings the JIT code was compiled for (bit 0: vu0Overflow, bit 1: vu1Overflow).
static u8 s_vu1_jit_ovf = 0xff;
static u8 _vu1JitOvf()
{
	return (EmuConfig.Cpu.Recompiler.vu0Overflow ? 1 : 0) | (EmuConfig.Cpu.Recompiler.vu1Overflow ? 2 : 0);
}

// vuDouble on 4 lanes: exponent 0 -> signed 0, exponent 255 -> signed max float.
static void _vu1JitEmitVuDouble(const a64::VRegister& v)
{
	armAsm->And(a64::v24.V16B(), v.V16B(), a64::v16.V16B());
	armAsm->Cmeq(a64::v25.V4S(), a64::v24.V4S(), 0);
	armAsm->Cmeq(a64::v24.V4S(), a64::v24.V4S(), a64::v16.V4S());
	armAsm->And(a64::v26.V16B(), v.V16B(), a64::v17.V16B());
	armAsm->Orr(a64::v27.V16B(), a64::v26.V16B(), a64::v18.V16B());
	armAsm->Bsl(a64::v24.V16B(), a64::v27.V16B(), v.V16B());
	armAsm->Bsl(a64::v25.V16B(), a64::v26.V16B(), a64::v24.V16B());
	armAsm->Mov(v.V16B(), a64::v25.V16B());
}

// Emits the upper op natively when it is in s_vu1_fmac_ops and the inputs are clamped
// (vu0Overflow, as vuDouble tests): then no NaN can arise and the result does not depend on
// the operand order the compiler picked. Returns false when the handler must be called.
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
// out_clamp: CHECK_VU_OVERFLOW of that VU.
static void _vuJitEmitFmacCore(const VU1FmacDesc* desc, u32 code, const a64::Register& rvu,
	const a64::Register& rconst, bool out_clamp)
{
	const auto _vu1JitMem = [&rvu](s64 off) { return a64::MemOperand(rvu, off); };
	const u32 ft = (code >> 16) & 0x1f, fs = (code >> 11) & 0x1f, fd = (code >> 6) & 0x1f;

	armAsm->Ldp(a64::q16, a64::q17, a64::MemOperand(rconst, 0));
	armAsm->Ldp(a64::q18, a64::q19, a64::MemOperand(rconst, 32));
	armAsm->Ldp(a64::q20, a64::q21, a64::MemOperand(rconst, 64));
	armAsm->Ldr(a64::q22, a64::MemOperand(rconst, 96));

	armAsm->Ldr(a64::q0, _vu1JitMem(_vu1Off(VU1.VF[fs])));
	if (desc->bc == VU1_BC_VEC)
		armAsm->Ldr(a64::q1, _vu1JitMem(_vu1Off(VU1.VF[ft])));
	else
	{
		const s64 off = desc->bc == VU1_BC_I ? _vu1Off(VU1.VI[REG_I].UL) :
						desc->bc == VU1_BC_Q ? _vu1Off(VU1.VI[REG_Q].UL) :
											   _vu1Off(VU1.VF[ft].UL[desc->bc]);
		armAsm->Ldr(a64::s1, _vu1JitMem(off));
		armAsm->Dup(a64::v1.V4S(), a64::v1.V4S(), 0);
	}
	_vu1JitEmitVuDouble(a64::v0);
	_vu1JitEmitVuDouble(a64::v1);
	const bool ternary = desc->op == VU1FmacOp::Madd || desc->op == VU1FmacOp::Msub;
	if (ternary)
	{
		armAsm->Ldr(a64::q2, _vu1JitMem(_vu1Off(VU1.ACC)));
		_vu1JitEmitVuDouble(a64::v2);
	}
	switch (desc->op)
	{
		case VU1FmacOp::Add: armAsm->Fadd(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S()); break;
		case VU1FmacOp::Sub: armAsm->Fsub(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S()); break;
		case VU1FmacOp::Mul: armAsm->Fmul(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S()); break;
		case VU1FmacOp::Madd:
			armAsm->Fmul(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S());
			armAsm->Fadd(a64::v3.V4S(), a64::v2.V4S(), a64::v3.V4S());
			break;
		case VU1FmacOp::Msub:
			armAsm->Fmul(a64::v3.V4S(), a64::v0.V4S(), a64::v1.V4S());
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
	const u32 lanes = (code >> 21) & 0xf; // bit 3 = x
	for (int i = 0; i < 4; i++)
		if (!(lanes & (8 >> i)))
			armAsm->Mov(a64::v28.V4S(), i, a64::wzr);
	armAsm->Addv(a64::s28, a64::v28.V4S());
	armAsm->Fmov(a64::w0, a64::s28);
	armAsm->Str(a64::w0, _vu1JitMem(_vu1Off(VU1.macflag)));
	// VU_STAT_UPDATE: bit n = any bit in MAC nibble n.
	armAsm->Orr(a64::w1, a64::w0, a64::Operand(a64::w0, a64::LSR, 1));
	armAsm->Orr(a64::w1, a64::w1, a64::Operand(a64::w1, a64::LSR, 2));
	armAsm->And(a64::w1, a64::w1, 0x1111);
	armAsm->Orr(a64::w1, a64::w1, a64::Operand(a64::w1, a64::LSR, 3));
	armAsm->And(a64::w1, a64::w1, 0x0303);
	armAsm->Orr(a64::w1, a64::w1, a64::Operand(a64::w1, a64::LSR, 6));
	armAsm->And(a64::w1, a64::w1, 0xf);
	armAsm->Str(a64::w1, _vu1JitMem(_vu1Off(VU1.statusflag)));

	// Value: U -> signed 0, O -> signed max float if vu1Overflow.
	armAsm->And(a64::v26.V16B(), a64::v3.V16B(), a64::v17.V16B());
	armAsm->Bsl(a64::v25.V16B(), a64::v26.V16B(), a64::v3.V16B());
	a64::VRegister res = a64::v25;
	if (out_clamp)
	{
		armAsm->Orr(a64::v27.V16B(), a64::v26.V16B(), a64::v18.V16B());
		armAsm->Bsl(a64::v24.V16B(), a64::v27.V16B(), a64::v25.V16B());
		res = a64::v24;
	}
	const s64 off_dst = desc->acc ? _vu1Off(VU1.ACC) : _vu1Off(VU1.VF[fd]);
	if (lanes != 0xf)
	{
		armAsm->Ldr(a64::q4, _vu1JitMem(off_dst));
		for (int i = 0; i < 4; i++)
			if (lanes & (8 >> i))
				armAsm->Mov(a64::v4.V4S(), i, res.V4S(), i);
		res = a64::v4;
	}
	if (lanes != 0)
		armAsm->Str(res.Q(), _vu1JitMem(off_dst));
}

static bool _vu1JitEmitFmac(FnPtr_VuVoid f, u32 code)
{
	const VU1FmacDesc* desc = _vuJitFmacDesc(s_vu1_fmac_ops, std::size(s_vu1_fmac_ops), f, code);
	if (!desc)
		return false;
	_vuJitEmitFmacCore(desc, code, s_vu1_jit_rvu, a64::x21, EmuConfig.Cpu.Recompiler.vu1Overflow);
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
	VU0_FMAC_ALL(MUL, VU1FmacOp::Mul), VU0_FMAC_ALL(MADD, VU1FmacOp::Madd), VU0_FMAC_ALL(MSUB, VU1FmacOp::Msub)};
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
	_vuJitEmitFmacCore(_vu0MacroDesc(code), code, rvu, a64::x1, EmuConfig.Cpu.Recompiler.vu0Overflow);
	armAsm->Ldr(a64::w1, mem(_vu1Off(VU1.statusflag)));
	armAsm->And(a64::w1, a64::w1, 0xf);
	armAsm->Ldr(a64::w2, mem(_vu1Off(VU1.VI[REG_STATUS_FLAG].UL)));
	armAsm->And(a64::w2, a64::w2, 0xfc0);
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

static void _vu1JitEmitUpper(FnPtr_VuVoid f, u32 code)
{
	if (!_vu1JitEmitFmac(f, code) && !_vu1JitEmitUpperInt(f, code))
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
	else if (!_vu1JitEmitLowerInt(f, code))
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

static void _vu1JitCompile(VU1Trace& t)
{
	if (!s_vu1_jit_start ||
		static_cast<size_t>(s_vu1_jit_end - s_vu1_jit_ptr) < t.steps.size() * VU1_JIT_MAX_STEP_CODE + 256)
		_vu1JitReset();

	armSetAsmPtr(s_vu1_jit_ptr, s_vu1_jit_end - s_vu1_jit_ptr, &s_vu1_jit_pool);
	u8* const fn = armStartBlock();
	s_vu1_jit_pending = {};
	_vu1JitMark("pro");

	const a64::Register& rvu = a64::x19;
	const a64::Register& rbase = a64::x20;
	const auto vu = [&](s64 off) { return a64::MemOperand(rvu, off); };
	const auto slot = [](int i) { return a64::MemOperand(a64::sp, 32 + i * 16); };
	const s64 off_cycle = _vu1Off(VU1.cycle);

	armAsm->Stp(a64::x29, a64::lr, a64::MemOperand(a64::sp, -160, a64::PreIndex));
	armAsm->Stp(rvu, rbase, a64::MemOperand(a64::sp, 16));
	armAsm->Stp(a64::x21, a64::x22, a64::MemOperand(a64::sp, 96));
	armAsm->Stp(a64::x23, a64::x24, a64::MemOperand(a64::sp, 112));
	armAsm->Stp(a64::x25, a64::x26, a64::MemOperand(a64::sp, 128));
	armAsm->Stp(a64::x27, a64::x28, a64::MemOperand(a64::sp, 144));
	armMoveAddressToReg(a64::x21, s_vu1_jit_fconst);
	armMoveAddressToReg(rvu, &VU1);
	armAsm->Ldr(rbase, vu(off_cycle));
	armAsm->Ldr(a64::w27, vu(_vu1Off(VU1.fmacreadpos)));
	armAsm->Mov(a64::w6, sizeof(fmacPipe));
	for (u32 i = 0; i < 4; i++)
	{
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
			armAsm->Mov(a64::x5, _vu1JitFmacBase(s_vu1_jit_pending.fmac_ret));
			_vu1JitEmitFMACretire();
			s_vu1_jit_pending.fmac_ret++;
			s_vu1_jit_pending.fmac_cnt--;
			s_vu1_jit_pending.fmac = true;
		}
		if (s.fdiv_retire)
			armEmitCall(reinterpret_cast<const void*>(&_vu1JitFDIVretire));
		if (s.efu_retire)
			armEmitCall(reinterpret_cast<const void*>(&_vu1JitEFUretire));
		for (u32 i = 0; i < s.ialu_retire; i++)
			_vu1JitEmitPosStep(_vu1Off(VU1.ialureadpos), _vu1Off(VU1.ialucount), -1);

		_vu1JitMark("kick");
		a64::Label no_kick;
		armAsm->Cbz(a64::w22, &no_kick);
		_vu1JitFlush(true, false);
		armEmitCall(reinterpret_cast<const void*>(&_vu1JitXGKICK));
		armAsm->Ldr(a64::w22, vu(_vu1Off(VU1.xgkickenable)));
		armAsm->Bind(&no_kick);

		_vu1JitMark("bk");
		if (d.ibit)
		{
			s_vu1_jit_pending.code = true;
			s_vu1_jit_pending.code_v = hi;
			_vu1JitMark("U", reinterpret_cast<const void*>(d.upper));
			_vu1JitEmitUpper(d.upper, hi);
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
			_vu1JitEmitUpper(d.upper, hi);
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

		if (!d.ibit && _vu1BacksUpVI(d.lower))
			vib_steps = 2;
		_vu1JitMark("addpipe");
		if (d.fmac)
		{
			VU1JitPending& p = s_vu1_jit_pending;
			armAsm->Mov(a64::x5, _vu1JitFmacBase(p.fmac_ret + p.fmac_cnt));
			_vu1JitEmitAddFMAC(d, rbase, s.cycle_rel);
			p.fmac_cnt++;
			p.fmac = true;
		}
		if (d.lregs.pipe == VUPIPE_IALU && d.lregs.cycles != 0)
			_vu1JitEmitAddIALU(d.lregs, rbase, s.cycle_rel);
		else if ((d.lregs.pipe == VUPIPE_FDIV && (d.lregs.VIwrite & (1 << REG_Q))) ||
				 (d.lregs.pipe == VUPIPE_EFU && (d.lregs.VIwrite & (1 << REG_P))))
		{
			_vu1JitFlush(false, false);
			armMoveAddressToReg(a64::x0, &s.d.lregs);
			armEmitCall(reinterpret_cast<const void*>(&_vu1JitAddLowerStalls));
		}

		_vu1JitMark("branch");
		// VU1.branch is 0 at trace entry: it can only be set from the first branch pair on.
		if (branch_seen)
		{
			// _vu1JitBranch may write TPC: nothing pending may overwrite it later.
			_vu1JitFlush(false, false);
			a64::Label no_branch;
			armAsm->Ldr(a64::w0, vu(_vu1Off(VU1.branch)));
			armAsm->Cbz(a64::w0, &no_branch);
			armEmitCall(reinterpret_cast<const void*>(&_vu1JitBranch));
			armAsm->Bind(&no_branch);
		}

	}

	_vu1JitMark("epi");
	_vu1JitFlush();
	armAsm->Ldp(rvu, rbase, a64::MemOperand(a64::sp, 16));
	armAsm->Ldp(a64::x21, a64::x22, a64::MemOperand(a64::sp, 96));
	armAsm->Ldp(a64::x23, a64::x24, a64::MemOperand(a64::sp, 112));
	armAsm->Ldp(a64::x25, a64::x26, a64::MemOperand(a64::sp, 128));
	armAsm->Ldp(a64::x27, a64::x28, a64::MemOperand(a64::sp, 144));
	armAsm->Ldp(a64::x29, a64::lr, a64::MemOperand(a64::sp, 160, a64::PostIndex));
	armAsm->Ret();
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
	alignas(16) static VURegs ref_cmp;
	std::memcpy(&ref_cmp, &ref, sizeof(VURegs));
	for (VURegs* r : {&VU1, &ref_cmp})
	{
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
	VU1Trace* t = nullptr;
	for (auto& v : slot.variants)
	{
		if (v && std::memcmp(&v->sig, &sig, sizeof(sig)) == 0)
		{
			t = v.get();
			break;
		}
	}

	if (t && t->micro_gen != s_vu1_micro_gen)
	{
		u32 p = pc;
		for (const VU1TraceStep& s : t->steps)
		{
			const u32* sp = reinterpret_cast<const u32*>(&VU1.Micro[p]);
			if (s.code != (static_cast<u64>(sp[0]) | (static_cast<u64>(sp[1]) << 32)))
			{
				t = nullptr;
				break;
			}
			p += 8;
		}
		if (t)
			t->micro_gen = s_vu1_micro_gen;
	}
	if (!t)
	{
		std::unique_ptr<VU1Trace> nt = _vu1TraceCompile(pc, sig);
		if (nt->steps.empty())
			return nullptr;
		auto it = std::find_if(std::begin(slot.variants), std::end(slot.variants),
			[&sig](const std::unique_ptr<VU1Trace>& v) { return v && std::memcmp(&v->sig, &sig, sizeof(sig)) == 0; });
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
		_vu1TraceExec(*t);

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
