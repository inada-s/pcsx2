// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARM64 IOP recompiler.
// A block runs from its pc up to a jump, branch, SYSCALL or BREAK (its handler runs the delay slot and
// iopEventTest) or the end of the 4 KB page. Each instruction is compiled into the state updates execI
// makes (psxRegs.code, psxRegs.pc, psxRegs.cycle) and a call of its interpreter handler, and the blocks
// run in the interpreter's loop (psxIntExecuteBlocks), so the IOP computes exactly what the interpreter
// computes.

#include "Common.h"
#include "IopHw.h"
#include "IopMem.h"
#include "R3000A.h"
#include "arm64/AsmHelpers.h"

#include "common/Console.h"

#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <vector>

namespace a64 = vixl::aarch64;

// Returns true for an idle loop (j to itself, nop in the delay slot).
using BlockFn = bool (*)();

static constexpr u32 POOL_SIZE = 0x100000;
static constexpr u32 MAX_BLOCK_CODE = 0x20000;
static constexpr u32 PAGE_ENTRIES = 0x10000 / 4;

// Blocks by virtual 64 KB page (allocated on first use): the pc a block stores is the one it ran from.
static BlockFn* s_lut[0x10000];
static std::vector<u32> s_pages;
// One bit per RAM word that is code of a block: a write there clears the blocks of its 4 KB page.
static u32 s_code_bits[Ps2MemSize::TotalIopRam / 4 / 32];
// Set by a clear: the running block ends after its store.
static bool s_cleared;

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

static void ClearAllBlocks()
{
	for (u32 page : s_pages)
		std::memset(s_lut[page], 0, PAGE_ENTRIES * sizeof(BlockFn));
	std::memset(s_code_bits, 0, sizeof(s_code_bits));

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
	s_pool.Destroy();
}

static void recReset()
{
	ClearAllBlocks();
}

// The blocks of a 4 KB RAM page (offset in RAM), at all the virtual addresses of that RAM.
static void ClearRamPage(u32 offset)
{
	const u32 mask = Ps2MemSize::ExposedIopRam - 1;
	for (u32 page : s_pages)
	{
		const u32 base = page << 16;
		if (InRam(base) && (base & mask) == (offset & ~0xffffu))
			std::memset(&s_lut[page][(offset & 0xf000) >> 2], 0, 0x1000 / 4 * sizeof(BlockFn));
	}
	std::memset(&s_code_bits[offset >> 7], 0, 0x1000 / 4 / 8);
	s_cleared = true;
}

// Size in words. Called with virtual and with physical addresses.
static void recClear(u32 addr, u32 size)
{
	if (!InRam(addr))
		return;
	const u32 mask = Ps2MemSize::ExposedIopRam - 1;
	for (u32 i = 0; i < size;)
	{
		const u32 offset = (addr + i * 4) & mask & ~3u;
		const u32 word = offset >> 2;
		if ((word & 31) == 0 && size - i >= 32 && s_code_bits[word >> 5] == 0)
		{
			i += 32;
			continue;
		}
		if (s_code_bits[word >> 5] & (1u << (word & 31)))
		{
			ClearRamPage(offset & ~0xfffu);
			i += (0x1000 - (offset & 0xfff)) / 4;
			continue;
		}
		i++;
	}
}

static void IrxHack()
{
	// As execI at pc 0x1630.
	if (EmuConfig.CurrentIRX.length() > 3 && iopMemRead32(0x20018) == 0x1F)
		iopMemWrite32(0x20094, 0xbffc0000);
}

static bool EndsBlock(u32 code)
{
	const u32 op = code >> 26;
	const u32 funct = code & 0x3f;
	return (op >= 1 && op <= 7) || (op == 0 && (funct == 8 || funct == 9 || funct == 12 || funct == 13));
}

static bool IsStore(u32 code)
{
	const u32 op = code >> 26;
	return (op >= 0x28 && op <= 0x2e) || op == 0x3a;
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

static bool IsIdleLoop(u32 pc)
{
	const u32 code = iopMemRead32(pc);
	return (code >> 26) == 2 && (((pc + 4) & 0xf0000000) | ((code & 0x03ffffff) << 2)) == pc && iopMemRead32(pc + 4) == 0;
}

static BlockFn Compile(u32 startpc, BlockFn* entry)
{
	if (static_cast<size_t>(s_code_end - s_code_ptr) < MAX_BLOCK_CODE)
	{
		Console.WriteLn("ARM64 IOP rec: code buffer full, clearing all blocks");
		ClearAllBlocks();
	}

	u32 count = 0;
	for (u32 pc = startpc;; pc += 4)
	{
		count++;
		if (EndsBlock(iopMemRead32(pc)) || ((pc + 4) & 0xfff) == 0)
			break;
	}
	if (InRam(startpc))
	{
		const u32 mask = Ps2MemSize::ExposedIopRam - 1;
		for (u32 i = 0; i < count; i++)
		{
			const u32 word = ((startpc + i * 4) & mask) >> 2;
			s_code_bits[word >> 5] |= 1u << (word & 31);
		}
	}

	armSetAsmPtr(s_code_ptr, s_code_end - s_code_ptr, &s_pool);
	u8* const fn = armStartBlock();

	const a64::Register& rregs = a64::x19;
	a64::Label exit;

	armAsm->Stp(a64::x29, a64::lr, a64::MemOperand(a64::sp, -32, a64::PreIndex));
	armAsm->Str(rregs, a64::MemOperand(a64::sp, 16));
	armMoveAddressToReg(rregs, &psxRegs);

	const s64 off_pc = offsetof(psxRegisters, pc);
	const s64 off_code = offsetof(psxRegisters, code);
	const s64 off_cycle = offsetof(psxRegisters, cycle);
	// Native ops leave code, pc and cycle to the next handler call or the block end.
	u32 pending_cycles = 0;
	const auto flush = [&](u32 code, u32 pc) {
		armAsm->Mov(a64::w9, code);
		armAsm->Str(a64::w9, a64::MemOperand(rregs, off_code));
		armAsm->Mov(a64::w9, pc);
		armAsm->Str(a64::w9, a64::MemOperand(rregs, off_pc));
		if (pending_cycles)
		{
			armAsm->Ldr(a64::x9, a64::MemOperand(rregs, off_cycle));
			armAsm->Add(a64::x9, a64::x9, pending_cycles);
			armAsm->Str(a64::x9, a64::MemOperand(rregs, off_cycle));
			pending_cycles = 0;
		}
	};
	bool last_native = false;
	for (u32 i = 0; i < count; i++)
	{
		const u32 pc = startpc + i * 4;
		const u32 code = iopMemRead32(pc);

		if (pc == 0x1630)
			armEmitCall(reinterpret_cast<const void*>(&IrxHack));

		pending_cycles++;
		last_native = EmitNative(rregs, code);
		if (last_native)
			continue;

		flush(code, pc + 4);
		armEmitCall(reinterpret_cast<const void*>(Handler(code)));

		if (i + 1 < count)
		{
			// An exception moved the pc.
			armAsm->Ldr(a64::w9, a64::MemOperand(rregs, off_pc));
			armAsm->Mov(a64::w10, pc + 4);
			armAsm->Cmp(a64::w9, a64::w10);
			armAsm->B(a64::ne, &exit);
			// The store cleared blocks, maybe this one.
			if (IsStore(code))
			{
				armMoveAddressToReg(a64::x9, &s_cleared);
				armAsm->Ldrb(a64::w9, a64::MemOperand(a64::x9));
				armAsm->Cbnz(a64::w9, &exit);
			}
		}
	}
	if (last_native)
	{
		const u32 lastpc = startpc + (count - 1) * 4;
		flush(iopMemRead32(lastpc), lastpc + 4);
	}

	armAsm->Bind(&exit);
	armAsm->Mov(RWRET, IsIdleLoop(startpc));
	armAsm->Ldr(rregs, a64::MemOperand(a64::sp, 16));
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
	if (psxRegs.pc == 0x1630)
		IrxHack();
	psxRegs.code = iopMemRead32(psxRegs.pc);
	psxRegs.pc += 4;
	psxRegs.cycle++;
	psxBSC[psxRegs.code >> 26]();
}

// The loop at pc ran once (its iopEventTest at the current cycle). The next iterations whose iopEventTest
// comes before iopNextEventCycle change nothing but the cycle: add their cycles, then run the next one.
// psxIntExecuteBlocks subtracts 8 EE cycles per IOP cycle since start from iopCycleEE after a run_block call
// and calls it again while iopCycleEE stays above 0 (without ICFG bit 3: no PS1 clock, no BIOS call check).
static void SkipIdle(u32 pc, BlockFn block, u64 start)
{
	while (psxRegs.pc == pc && !s_cleared)
	{
		const u64 cycle = psxRegs.cycle;
		// iopNextEventCycle is not lowered to the counter target when the counters were updated.
		const s64 counter = psxNextDeltaCounter - static_cast<s64>(static_cast<s32>(cycle - psxNextStartCounter));
		const s64 quiet = (std::min(static_cast<s64>(psxRegs.iopNextEventCycle - cycle), counter) - 1) / 2;
		const s64 budget = (psxRegs.iopCycleEE - 8 * static_cast<s64>(cycle - start) - 1) / 16;
		const s64 skip = std::min(quiet, budget);
		if (skip <= 0)
			return;
		psxRegs.cycle += 2 * skip;
		block();
	}
}

static void recRunBlock(u64 start)
{
	const u32 pc = psxRegs.pc;
	if (!Compilable(pc)) [[unlikely]]
	{
		InterpretOne();
		return;
	}

	BlockFn*& page = s_lut[pc >> 16];
	if (!page) [[unlikely]]
	{
		page = static_cast<BlockFn*>(std::calloc(PAGE_ENTRIES, sizeof(BlockFn)));
		s_pages.push_back(pc >> 16);
	}
	BlockFn* const entry = &page[(pc & 0xffff) >> 2];
	BlockFn block = *entry;
	if (!block) [[unlikely]]
		block = Compile(pc, entry);
	s_cleared = false;
	if (block() && !(psxHu32(HW_ICFG) & 8))
		SkipIdle(pc, block, start);
}

static s32 recExecuteBlock(s32 eeCycles)
{
	return psxIntExecuteBlocks(eeCycles, recRunBlock);
}

R3000Acpu psxRec = {
	recReserve,
	recReset,
	recExecuteBlock,
	recClear,
	recShutdown,
};
