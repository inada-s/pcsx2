// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARM64 IOP recompiler with the timing of the x86 IOP recompiler (x86/iR3000A.cpp), so that ARM64 and x86
// hosts compute the same IOP cycles.
// Blocks as x86 iopRecRecompile: from their pc up to a jump or branch and its delay slot, up to a block
// already compiled, or up to the target of a backward branch inside the block. Each instruction is 1 cycle,
// MULT/MULTU +7, DIV/DIVU +40, and psxRegs.cycle is updated at the block end only. A jump or branch (taken
// or not) adds the cycles, returns to the EE when iopCycleEE <= 0, else tests events once the cycle reaches
// iopNextEventCycle; a nop loop jumping back to its start fast-forwards to the next event. Other block ends
// and SYSCALL/BREAK add the cycles without a test. Integer ALU ops are inline, the others calls of the
// interpreter's handlers.

#include "Common.h"
#include "IopBios.h"
#include "IopDma.h"
#include "IopHw.h"
#include "IopMem.h"
#include "R3000A.h"
#include "arm64/AsmHelpers.h"
#include "DebugTools/SymbolGuardian.h"

#include "common/Console.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace a64 = vixl::aarch64;

// How a block ended (bits 32+ of a block's return value, its cycles in bits 0-31).
enum BlockEnd : u64
{
	END_PLAIN, // add the cycles
	END_TEST, // add the cycles, then the timeslice and event tests
	END_FF, // END_TEST, or the nop loop fast-forward when the loop jumped back to its start
	END_NONE, // an IRX import HLE function returned from the import stub: no cycles
};

using BlockFn = u64 (*)();

static constexpr u32 POOL_SIZE = 0x100000;
static constexpr u32 MAX_BLOCK_CODE = 0x20000;
static constexpr u32 MAX_BLOCK_INSTS = 0x800;
static constexpr u32 PAGE_ENTRIES = 0x10000 / 4;

// Blocks by physical 64 KB page (allocated on first use), shared by the RAM mirrors as x86 psxRecLUT.
static BlockFn* s_lut[0x10000];
static std::vector<u32> s_pages;
// x86 recBlocks: start (pc & 0x1fffffff) -> size in words, for the clears.
static std::map<u32, u32> s_blocks;
// x86 g_psxMaxRecMem: writes at or above it clear nothing.
static u32 s_max_rec_mem;

static ArmConstantPool s_pool;
static u8* s_code_ptr;
static u8* s_code_end;

static bool InRam(u32 pc)
{
	return (pc & 0x1fffffff) < 0x800000;
}

static bool Compilable(u32 pc)
{
	return !(pc & 3) && (InRam(pc) || (pc & 0x1fffffff) >= 0x1fc00000);
}

static BlockFn* Entry(u32 pc, bool alloc)
{
	const u32 phys = InRam(pc) ? (pc & (Ps2MemSize::ExposedIopRam - 1)) : (pc & 0x1fffffff);
	BlockFn*& page = s_lut[phys >> 16];
	if (!page)
	{
		if (!alloc)
			return nullptr;
		page = static_cast<BlockFn*>(std::calloc(PAGE_ENTRIES, sizeof(BlockFn)));
		s_pages.push_back(phys >> 16);
	}
	return &page[(phys & 0xffff) >> 2];
}

// As x86: code outside RAM and ROM counts as compiled (a block ends before it).
static bool Compiled(u32 pc)
{
	if (!Compilable(pc))
		return true;
	BlockFn* const entry = Entry(pc, false);
	return entry && *entry;
}

static void ClearAllBlocks()
{
	for (u32 page : s_pages)
		std::memset(s_lut[page], 0, PAGE_ENTRIES * sizeof(BlockFn));
	s_blocks.clear();
	s_max_rec_mem = 0;

	s_pool.Init(SysMemory::GetIOPRec(), POOL_SIZE);
	s_code_ptr = SysMemory::GetIOPRec() + POOL_SIZE;
	s_code_end = SysMemory::GetIOPRecEnd();
}

static void recReserve()
{
}

static void recShutdown()
{
	for (u32 page : s_pages)
	{
		std::free(s_lut[page]);
		s_lut[page] = nullptr;
	}
	s_pages.clear();
	s_blocks.clear();
	s_pool.Destroy();
}

static void recReset()
{
	ClearAllBlocks();
}

// x86 psxRecClearMem: a write to a block start clears that block and the blocks overlapping it; a write
// elsewhere leaves the blocks as compiled. Returns the bytes to skip.
static u32 ClearMem(u32 mem)
{
	if (mem >= s_max_rec_mem || !Compilable(mem) || !Compiled(mem))
		return 4;
	const u32 pc = mem & 0x1fffffff;
	auto it = s_blocks.upper_bound(pc);
	if (it == s_blocks.begin())
		return 4;
	--it;
	if (pc >= it->first + it->second * 4)
		return 4;

	u32 lower = pc, upper = pc + 4;
	while (it != s_blocks.begin())
	{
		const auto prev = std::prev(it);
		if (prev->first + prev->second * 4 <= lower)
			break;
		lower = std::min(lower, prev->first);
		it = prev;
	}
	const auto first = it;
	for (; it != s_blocks.end() && it->first < upper; ++it)
	{
		lower = std::min(lower, it->first);
		upper = std::max(upper, it->first + it->second * 4);
	}
	s_blocks.erase(first, it);

	for (u32 addr = lower; addr < upper; addr += 4)
	{
		if (BlockFn* const entry = Entry(addr, false))
			*entry = nullptr;
	}
	return upper - pc;
}

// Size in words.
static void recClear(u32 addr, u32 size)
{
	for (u32 mem = addr; mem < addr + size * 4;)
		mem += ClearMem(mem);
}

static void IrxHack()
{
	// As x86 iopRecRecompile for a block at 0x1630.
	if (EmuConfig.CurrentIRX.length() > 3 && iopMemRead32(0x20018) == 0x1F)
		iopMemWrite32(0x20094, 0xbffc0000);
}

// 1 = JR/JALR, 2 = J/JAL, 3 = conditional branch (the ones x86 iopRecRecompile ends a block at).
static int BranchKind(u32 code)
{
	const u32 op = code >> 26;
	if (op == 0)
		return ((code & 0x3f) == 8 || (code & 0x3f) == 9) ? 1 : 0;
	if (op == 1)
	{
		const u32 rt = (code >> 16) & 31;
		return (rt == 0 || rt == 1 || rt == 16 || rt == 17) ? 3 : 0;
	}
	if (op == 2 || op == 3)
		return 2;
	return op <= 7 ? 3 : 0;
}

static u32 Penalty(u32 code)
{
	if (code >> 26)
		return 0;
	switch (code & 0x3f)
	{
		case 0x18: case 0x19: return 7; // MULT, MULTU
		case 0x1a: case 0x1b: return 40; // DIV, DIVU
		default: return 0;
	}
}

// SYSCALL, BREAK: the handler moves the pc to the exception vector.
static bool IsException(u32 code)
{
	return (code >> 26) == 0 && ((code & 0x3f) == 12 || (code & 0x3f) == 13);
}

// x86 psxRecompileIrxImport: ADDIU zero, zero, index in an IRX import stub calls the HLE function of that
// import, which returns from the stub when it handles the call.
static irxHLE IrxImportHle(u32 pc, u32 code)
{
	if ((code >> 16) != 0x2400)
		return nullptr;
	const u32 import_table = R3000A::irxImportTableAddr(pc);
	if (!import_table)
		return nullptr;
	return R3000A::irxImportHLE(iopMemReadString(import_table + 12, 8), code & 0xffff);
}

static void (*Handler(u32 code))()
{
	switch (code >> 26)
	{
		case 0x00: return psxSPC[code & 0x3f];
		case 0x01: return psxREG[(code >> 16) & 0x1f];
		case 0x10: return psxCP0[(code >> 21) & 0x1f];
		default: return psxBSC[code >> 26];
	}
}

static s64 GprOffset(u32 reg)
{
	return offsetof(psxRegisters, GPR) + reg * 4;
}

// Integer ops without memory access, emitted inline as their handlers compute them (no overflow
// exceptions on the IOP; variable shifts by the low 5 bits, as the handlers compile on ARM64 and x86).
// Returns false for the ops left to the handler.
static bool EmitNative(const a64::Register& rregs, u32 code)
{
	const u32 op = code >> 26;
	const u32 rs = (code >> 21) & 31;
	const u32 rt = (code >> 16) & 31;
	const u32 rd = (code >> 11) & 31;
	const u32 sa = (code >> 6) & 31;
	const u32 funct = code & 63;
	const s32 imm = static_cast<s16>(code);
	const u32 immu = code & 0xffff;
	const auto load = [&](const a64::Register& r, u32 reg) -> const a64::Register& {
		if (reg == 0)
			return a64::wzr;
		armAsm->Ldr(r, a64::MemOperand(rregs, GprOffset(reg)));
		return r;
	};
	const auto store = [&](const a64::Register& r, u32 reg) {
		armAsm->Str(r, a64::MemOperand(rregs, GprOffset(reg)));
	};
	const a64::Register& d = a64::w12;

	if (op == 0)
	{
		switch (funct)
		{
			case 0x00: // SLL
			case 0x02: // SRL
			case 0x03: // SRA
			case 0x04: // SLLV
			case 0x06: // SRLV
			case 0x07: // SRAV
			{
				if (!rd)
					return true;
				const a64::Register& t = load(a64::w11, rt);
				if (funct >= 0x04)
				{
					const a64::Register& s = load(a64::w10, rs);
					if (funct == 0x04)
						armAsm->Lsl(d, t, s);
					else if (funct == 0x06)
						armAsm->Lsr(d, t, s);
					else
						armAsm->Asr(d, t, s);
				}
				else if (funct == 0x00)
					armAsm->Lsl(d, t, sa);
				else if (funct == 0x02)
					armAsm->Lsr(d, t, sa);
				else
					armAsm->Asr(d, t, sa);
				store(d, rd);
				return true;
			}
			case 0x10: // MFHI
			case 0x12: // MFLO
				if (rd)
					store(load(d, funct == 0x10 ? 32 : 33), rd);
				return true;
			case 0x11: // MTHI
			case 0x13: // MTLO
				store(load(d, rs), funct == 0x11 ? 32 : 33);
				return true;
			case 0x20: // ADD
			case 0x21: // ADDU
			case 0x22: // SUB
			case 0x23: // SUBU
			case 0x24: // AND
			case 0x25: // OR
			case 0x26: // XOR
			case 0x27: // NOR
			case 0x2a: // SLT
			case 0x2b: // SLTU
			{
				if (!rd)
					return true;
				const a64::Register& s = load(a64::w10, rs);
				const a64::Register& t = load(a64::w11, rt);
				switch (funct)
				{
					case 0x20: case 0x21: armAsm->Add(d, s, t); break;
					case 0x22: case 0x23: armAsm->Sub(d, s, t); break;
					case 0x24: armAsm->And(d, s, t); break;
					case 0x25: armAsm->Orr(d, s, t); break;
					case 0x26: armAsm->Eor(d, s, t); break;
					case 0x27: armAsm->Orr(d, s, t); armAsm->Mvn(d, d); break;
					case 0x2a: armAsm->Cmp(s, t); armAsm->Cset(d, a64::lt); break;
					default: armAsm->Cmp(s, t); armAsm->Cset(d, a64::lo); break;
				}
				store(d, rd);
				return true;
			}
			default:
				return false;
		}
	}

	switch (op)
	{
		case 0x08: // ADDI
		case 0x09: // ADDIU
		case 0x0a: // SLTI
		case 0x0b: // SLTIU
		case 0x0c: // ANDI
		case 0x0d: // ORI
		case 0x0e: // XORI
		{
			if (!rt)
				return true;
			const a64::Register& s = load(a64::w10, rs);
			switch (op)
			{
				case 0x08: case 0x09: armAsm->Add(d, s, imm); break;
				case 0x0a: armAsm->Mov(a64::w11, imm); armAsm->Cmp(s, a64::w11); armAsm->Cset(d, a64::lt); break;
				case 0x0b: armAsm->Mov(a64::w11, static_cast<u32>(imm)); armAsm->Cmp(s, a64::w11); armAsm->Cset(d, a64::lo); break;
				case 0x0c: armAsm->And(d, s, immu); break;
				case 0x0d: armAsm->Orr(d, s, immu); break;
				default: armAsm->Eor(d, s, immu); break;
			}
			store(d, rt);
			return true;
		}
		case 0x0f: // LUI
			if (rt)
			{
				armAsm->Mov(d, code << 16);
				store(d, rt);
			}
			return true;
		default:
			return false;
	}
}


static const a64::Register& LoadGpr(const a64::Register& rregs, const a64::Register& r, u32 reg)
{
	if (reg == 0)
		return a64::wzr;
	armAsm->Ldr(r, a64::MemOperand(rregs, GprOffset(reg)));
	return r;
}

static void StoreLink(const a64::Register& rregs, u32 reg, u32 pc)
{
	armAsm->Mov(a64::w9, pc + 8);
	armAsm->Str(a64::w9, a64::MemOperand(rregs, GprOffset(reg)));
}

static BlockFn Compile(u32 startpc, BlockFn* entry)
{
	// As x86 iopRecRecompile, once per compile of a block at these pcs.
	if (startpc == 0x890)
		R3000SymbolGuardian.ClearIrxModules();
	if (startpc == 0x1630)
		IrxHack();
	if (startpc == 0xbfc4a000)
		psxRegs.GPR.n.a0 = Ps2MemSize::ExposedIopRam >> 20;

	if (static_cast<size_t>(s_code_end - s_code_ptr) < MAX_BLOCK_CODE)
	{
		Console.WriteLn("ARM64 IOP rec: code buffer full, clearing all blocks");
		ClearAllBlocks();
	}

	u32 end;
	u32 branch_to = 0xffffffff;
	for (u32 pc = startpc;; pc += 4)
	{
		if (pc != startpc && (Compiled(pc) || pc - startpc >= MAX_BLOCK_INSTS * 4))
		{
			end = pc;
			break;
		}
		const u32 code = iopMemRead32(pc);
		const int kind = BranchKind(code);
		if (kind == 1)
		{
			end = pc + 8;
			break;
		}
		if (kind == 2)
		{
			branch_to = ((code & 0x03ffffff) << 2) | ((pc + 4) & 0xf0000000);
			end = pc + 8;
			break;
		}
		if (kind == 3)
		{
			branch_to = static_cast<s16>(code) * 4 + pc + 4;
			end = (branch_to > startpc && branch_to < pc) ? branch_to : pc + 8;
			break;
		}
	}
	// x86 s_nBlockFF: a loop of nops (but its branch) back to its start.
	bool ff = branch_to == startpc;
	for (u32 pc = startpc; ff && pc < end; pc += 4)
		ff = pc == end - 8 || iopMemRead32(pc) == 0;

	s_blocks[startpc & 0x1fffffff] = (end - startpc) / 4;
	if (!(end & 0x10000000))
		s_max_rec_mem = std::max(end & ~0xa0000000u, s_max_rec_mem);

	armSetAsmPtr(s_code_ptr, s_code_end - s_code_ptr, &s_pool);
	u8* const fn = armStartBlock();

	const a64::Register& rregs = a64::x19;
	// The pc after a jump or branch, read before its delay slot runs.
	const a64::Register& rtarget = a64::w20;
	a64::Label exit, hle_exit;
	bool has_hle = false;

	armAsm->Stp(a64::x29, a64::lr, a64::MemOperand(a64::sp, -32, a64::PreIndex));
	armAsm->Stp(a64::x19, a64::x20, a64::MemOperand(a64::sp, 16));
	armMoveAddressToReg(rregs, &psxRegs);

	const s64 off_pc = offsetof(psxRegisters, pc);
	const s64 off_code = offsetof(psxRegisters, code);
	const auto set_return = [&](BlockEnd kind, u32 cycles) {
		armAsm->Mov(a64::x0, (static_cast<u64>(kind) << 32) | cycles);
	};
	u32 cycles = 0;
	const auto emit_op = [&](u32 pc, u32 code) {
		cycles += 1 + Penalty(code);
		if (const irxHLE hle = IrxImportHle(pc, code))
		{
			armAsm->Mov(a64::w9, code);
			armAsm->Str(a64::w9, a64::MemOperand(rregs, off_code));
			armAsm->Mov(a64::w9, pc + 4);
			armAsm->Str(a64::w9, a64::MemOperand(rregs, off_pc));
			armEmitCall(reinterpret_cast<const void*>(hle));
			armAsm->Cbnz(RWRET, &hle_exit);
			has_hle = true;
			return;
		}
		if (EmitNative(rregs, code))
			return;
		armAsm->Mov(a64::w9, code);
		armAsm->Str(a64::w9, a64::MemOperand(rregs, off_code));
		armAsm->Mov(a64::w9, pc + 4);
		armAsm->Str(a64::w9, a64::MemOperand(rregs, off_pc));
		armEmitCall(reinterpret_cast<const void*>(Handler(code)));
		// x86 rpsxRFE raises a pending interrupt.
		if ((code >> 26) == 0x10 && ((code >> 21) & 31) == 16)
			armEmitCall(reinterpret_cast<const void*>(&iopTestIntc));
	};

	bool ended = false;
	for (u32 pc = startpc; pc < end && !ended; pc += 4)
	{
		const u32 code = iopMemRead32(pc);
		const int kind = BranchKind(code);
		if (!kind)
		{
			emit_op(pc, code);
			if (IsException(code))
			{
				set_return(END_PLAIN, cycles);
				ended = true;
			}
			continue;
		}

		const u32 op = code >> 26;
		const u32 rs = (code >> 21) & 31;
		const u32 rt = (code >> 16) & 31;
		const u32 rd = (code >> 11) & 31;
		if (kind == 1)
		{
			armAsm->Mov(rtarget, LoadGpr(rregs, a64::w10, rs));
			if ((code & 0x3f) == 9 && rd)
				StoreLink(rregs, rd, pc);
		}
		else if (kind == 2)
		{
			if (op == 3)
				StoreLink(rregs, 31, pc);
			armAsm->Mov(rtarget, ((code & 0x03ffffff) << 2) | ((pc + 4) & 0xf0000000));
		}
		else
		{
			// BLTZAL/BGEZAL link before the test, as the interpreter.
			if (op == 1 && rt >= 16)
				StoreLink(rregs, 31, pc);
			const a64::Register& s = LoadGpr(rregs, a64::w10, rs);
			a64::Condition cond;
			if (op == 1)
			{
				armAsm->Cmp(s, 0);
				cond = (rt & 1) ? a64::ge : a64::lt;
			}
			else if (op <= 5)
			{
				armAsm->Cmp(s, LoadGpr(rregs, a64::w11, rt));
				cond = op == 4 ? a64::eq : a64::ne;
			}
			else
			{
				armAsm->Cmp(s, 0);
				cond = op == 6 ? a64::le : a64::gt;
			}
			armAsm->Mov(a64::w12, static_cast<s16>(code) * 4 + pc + 4);
			armAsm->Mov(a64::w13, pc + 8);
			armAsm->Csel(rtarget, a64::w12, a64::w13, cond);
		}
		cycles++;
		emit_op(pc + 4, iopMemRead32(pc + 4));
		armAsm->Str(rtarget, a64::MemOperand(rregs, off_pc));
		set_return(ff ? END_FF : END_TEST, cycles);
		ended = true;
	}
	if (!ended)
	{
		armAsm->Mov(a64::w9, end);
		armAsm->Str(a64::w9, a64::MemOperand(rregs, off_pc));
		set_return(END_PLAIN, cycles);
	}
	armAsm->B(&exit);

	if (has_hle)
	{
		armAsm->Bind(&hle_exit);
		set_return(END_NONE, 0);
	}

	armAsm->Bind(&exit);
	armAsm->Ldp(a64::x19, a64::x20, a64::MemOperand(a64::sp, 16));
	armAsm->Ldp(a64::x29, a64::lr, a64::MemOperand(a64::sp, 32, a64::PostIndex));
	armAsm->Ret();

	s_code_ptr = armEndBlock();

	const BlockFn block = reinterpret_cast<BlockFn>(fn);
	*entry = block;
	return block;
}

// execI, for code outside RAM and ROM.
static void InterpretOne()
{
	psxRegs.code = iopMemRead32(psxRegs.pc);
	psxRegs.pc += 4;
	psxRegs.cycle++;
	psxBSC[psxRegs.code >> 26]();
}

// x86 iPsxAddEECycles: takes EE cycles off iopCycleEE. True when the timeslice is over (x86 jle).
static bool SubEECycles(u32 ee)
{
	const s32 before = psxRegs.iopCycleEE;
	psxRegs.iopCycleEE = static_cast<s32>(static_cast<u32>(before) - ee);
	return static_cast<s64>(before) - static_cast<s32>(ee) <= 0;
}

static bool AddCycles(u32 cycles, bool ps1_clock)
{
	if (!ps1_clock) [[likely]]
		return SubEECycles(cycles * 8);
	// F = gcd(PS2CLK, PSXCLK) = 230400: 1280 = PS2CLK / F, 147 = PSXCLK / F.
	const u32 t = cycles * 1280 + psxRegs.iopCycleEECarry;
	psxRegs.iopCycleEECarry = t % 147;
	return SubEECycles(t / 147);
}

// x86 iPsxBranchTest's nop loop fast-forward: the cycle moves to the next event, at most by the rest of
// the timeslice, and events are tested.
static bool FastForward(bool ps1_clock)
{
	const u64 before = psxRegs.cycle;
	// x86 reads iopCycleEE as 64 bits, iopCycleEECarry above it.
	const u64 rest = (static_cast<u64>(psxRegs.iopCycleEECarry) << 32) | static_cast<u32>(psxRegs.iopCycleEE);
	u64 cycle = before + ((rest + 7) >> 3);
	if (static_cast<s64>(cycle - psxRegs.iopNextEventCycle) >= 0)
		cycle = psxRegs.iopNextEventCycle;
	psxRegs.cycle = cycle;
	const u32 ee = static_cast<u32>((cycle - before) << 3);
	if (!ps1_clock) [[likely]]
		return SubEECycles(ee);
	const u32 t = ee + psxRegs.iopCycleEECarry;
	psxRegs.iopCycleEECarry = t % 147;
	return SubEECycles(t / 147);
}

static s32 recExecuteBlock(s32 eeCycles)
{
	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = eeCycles;

	for (;;)
	{
		const u32 pc = psxRegs.pc;
		const bool ps1_clock = psxHu32(HW_ICFG) & (1 << 3);
		if (ps1_clock && ((pc & 0x1fffffff) == 0xa0 || (pc & 0x1fffffff) == 0xb0 || (pc & 0x1fffffff) == 0xc0) &&
			psxBiosCall())
			continue;
		if (!Compilable(pc)) [[unlikely]]
		{
			InterpretOne();
			AddCycles(1, ps1_clock);
			continue;
		}

		BlockFn* const entry = Entry(pc, true);
		BlockFn block = *entry;
		if (!block) [[unlikely]]
			block = Compile(pc, entry);

		const u64 ret = block();
		const u32 cycles = static_cast<u32>(ret);
		switch (static_cast<BlockEnd>(ret >> 32))
		{
			case END_PLAIN:
				psxRegs.cycle += cycles;
				AddCycles(cycles, ps1_clock);
				break;
			case END_FF:
				if (EmuConfig.Speedhacks.WaitLoop && psxRegs.pc == pc)
				{
					if (FastForward(ps1_clock))
						return psxRegs.iopBreak + psxRegs.iopCycleEE;
					iopEventTest();
					break;
				}
				[[fallthrough]];
			case END_TEST:
				psxRegs.cycle += cycles;
				if (AddCycles(cycles, ps1_clock))
					return psxRegs.iopBreak + psxRegs.iopCycleEE;
				if (static_cast<s64>(psxRegs.cycle - psxRegs.iopNextEventCycle) >= 0)
					iopEventTest();
				break;
			case END_NONE:
				break;
		}
	}
}

R3000Acpu psxRec = {
	recReserve,
	recReset,
	recExecuteBlock,
	recClear,
	recShutdown,
};
