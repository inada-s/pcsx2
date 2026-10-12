// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARM64 EE recompiler.
// A block runs from its pc up to a branch (and its delay slot, run by the branch's handler), a COP0
// instruction or the end of the 4 KB page. Each instruction is compiled into the state updates execI
// makes (cpuRegs.code, cpuRegs.pc, cpuBlockCycles) and a call of its interpreter handler, so a block
// computes exactly what the interpreter computes: branches, events, exceptions and TLB misses take the
// interpreter's paths (CpuRunsIntBranches).

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "VMManager.h"
#include "vtlb.h"
#include "arm64/AsmHelpers.h"
#include "Zdxsv/CpuHooks.h"

#include "common/Console.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace a64 = vixl::aarch64;

using namespace R5900;

// x0 = &cpuRegs, x1 = &cpuBlockCycles.
using BlockFn = void (*)(cpuRegisters*, u32*);

static constexpr u32 POOL_SIZE = 0x100000;
static constexpr u32 MAX_BLOCK_CODE = 0x20000;
static constexpr u32 PAGE_ENTRIES = 0x10000 / 4;

// Guest code by 64 KB page: RAM mirrors share one array, as recLUT in x86/ix86-32/iR5900.cpp.
static BlockFn* s_lut[0x10000];
static u32 s_hw_page[0x10000]; // physical 64 KB page of a mapped page
static BlockFn* s_ram_blocks;
static BlockFn* s_rom_blocks;
static BlockFn* s_rom1_blocks;
static BlockFn* s_rom2_blocks;

static ArmConstantPool s_pool;
static u8* s_code_start;
static u8* s_code_ptr;
static u8* s_code_end;
// CP0 Config bit 18 (cycle factor of execI) the blocks were compiled with.
static u32 s_config_bit18;
// PCSX2_ARM64_EE_NATIVE=0: every instruction calls its interpreter handler (A/B check of the native ops).
static bool s_native = true;

static void SetPages(BlockFn* blocks, u32 pagebase, u32 first, u32 count)
{
	for (u32 i = 0; i < count; i++)
	{
		s_lut[pagebase + first + i] = &blocks[i * PAGE_ENTRIES];
		s_hw_page[pagebase + first + i] = first + i;
	}
}

static void recReserve()
{
	const auto alloc = [](u32 bytes) { return static_cast<BlockFn*>(std::calloc(bytes / 4, sizeof(BlockFn))); };
	s_ram_blocks = alloc(Ps2MemSize::ExposedRam);
	s_rom_blocks = alloc(Ps2MemSize::Rom);
	s_rom1_blocks = alloc(Ps2MemSize::Rom1);
	s_rom2_blocks = alloc(Ps2MemSize::Rom2);
	const char* native = std::getenv("PCSX2_ARM64_EE_NATIVE");
	s_native = !native || std::strcmp(native, "0") != 0;
	Console.WriteLn("ARM64 EE rec: reserved, native ops %s", s_native ? "on" : "off");

	std::memset(s_lut, 0, sizeof(s_lut));
	const u32 ram_pages = Ps2MemSize::ExposedRam / 0x10000;
	for (u32 base : {0x0000u, 0x2000u, 0x3000u, 0x8000u, 0xa000u, 0xb000u, 0xc000u, 0xd000u})
		SetPages(s_ram_blocks, base, 0, ram_pages);
	for (u32 base : {0x0000u, 0x8000u, 0xa000u})
	{
		SetPages(s_rom_blocks, base, 0x1fc0, Ps2MemSize::Rom / 0x10000);
		SetPages(s_rom1_blocks, base, 0x1e00, Ps2MemSize::Rom1 / 0x10000);
		SetPages(s_rom2_blocks, base, 0x1e40, Ps2MemSize::Rom2 / 0x10000);
	}
}

static void recShutdown()
{
	std::memset(s_lut, 0, sizeof(s_lut));
	for (BlockFn** p : {&s_ram_blocks, &s_rom_blocks, &s_rom1_blocks, &s_rom2_blocks})
	{
		std::free(*p);
		*p = nullptr;
	}
	s_pool.Destroy();
}

static void ClearAllBlocks()
{
	std::memset(s_ram_blocks, 0, Ps2MemSize::ExposedRam / 4 * sizeof(BlockFn));
	std::memset(s_rom_blocks, 0, Ps2MemSize::Rom / 4 * sizeof(BlockFn));
	std::memset(s_rom1_blocks, 0, Ps2MemSize::Rom1 / 4 * sizeof(BlockFn));
	std::memset(s_rom2_blocks, 0, Ps2MemSize::Rom2 / 4 * sizeof(BlockFn));

	s_pool.Init(SysMemory::GetEERec(), POOL_SIZE);
	s_code_start = SysMemory::GetEERec() + POOL_SIZE;
	s_code_ptr = s_code_start;
	s_code_end = SysMemory::GetEERecEnd();
	s_config_bit18 = (cpuRegs.CP0.n.Config >> 18) & 1;
}

static void recReset()
{
	cpuRegs.branch = 0;
	ClearAllBlocks();
}

// Size in words. Called with virtual and with physical addresses: RAM mirrors share their blocks.
static void recClear(u32 addr, u32 size)
{
	addr &= ~3u;
	const u32 end = addr + size * 4;
	while (addr < end)
	{
		const u32 page_end = std::min<u64>((addr & ~0xffffu) + 0x10000u, end);
		if (BlockFn* page = s_lut[addr >> 16])
			std::memset(&page[(addr & 0xffff) >> 2], 0, (page_end - addr) / 4 * sizeof(BlockFn));
		if (page_end == 0)
			break;
		addr = page_end;
	}
}

static bool HookAnswered(u32 pc)
{
	return Zdxsv::IntHooksOn() && Zdxsv::IntRunHooks(pc);
}

static bool EndsBlock(const OPCODE& opcode, u32 code)
{
	return (opcode.flags & (IS_BRANCH | BRANCHTYPE_MASK)) || (code >> 26) == 0x10; // COP0: Config, Status, ERET
}

static s64 GprOffset(u32 reg)
{
	return offsetof(cpuRegisters, GPR.r[0]) + reg * sizeof(GPR_reg);
}

// Integer ops without memory access or exceptions, emitted inline. ADD, SUB, ADDI, DADD, DSUB and DADDI
// write their result on overflow, as the x86 recompiler (the interpreter skips the write).
// Returns false for the ops left to the interpreter's handler.
static bool EmitNative(const a64::Register& rregs, u32 code)
{
	if (!s_native)
		return false;

	const u32 op = code >> 26;
	const u32 rs = (code >> 21) & 31;
	const u32 rt = (code >> 16) & 31;
	const u32 rd = (code >> 11) & 31;
	const u32 sa = (code >> 6) & 31;
	const s64 imm = static_cast<s16>(code & 0xffff);
	const u64 immu = code & 0xffff;

	const a64::Register& s = a64::x9;
	const a64::Register& t = a64::x10;
	const a64::Register& d = a64::x11;
	const auto load = [&](const a64::Register& r, u32 reg) { armAsm->Ldr(r, a64::MemOperand(rregs, GprOffset(reg))); };
	const auto store = [&](u32 reg) { armAsm->Str(d, a64::MemOperand(rregs, GprOffset(reg))); };
	const auto sext32 = [&]() { armAsm->Sxtw(d, d.W()); };

	if (op == 0)
	{
		const u32 funct = code & 63;
		switch (funct)
		{
			case 0x11: // MTHI
			case 0x13: // MTLO
				load(d, rs);
				armAsm->Str(d, a64::MemOperand(rregs, funct == 0x11 ? offsetof(cpuRegisters, HI) : offsetof(cpuRegisters, LO)));
				return true;
			case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07:
			case 0x0a: case 0x0b: case 0x10: case 0x12: case 0x14: case 0x16: case 0x17:
			case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
			case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
			case 0x38: case 0x3a: case 0x3b: case 0x3c: case 0x3e: case 0x3f:
				break;
			default:
				return false;
		}
		if (rd == 0)
			return true;

		load(s, rs);
		load(t, rt);
		switch (funct)
		{
			case 0x00: armAsm->Lsl(d.W(), t.W(), sa); sext32(); break; // SLL
			case 0x02: armAsm->Lsr(d.W(), t.W(), sa); sext32(); break; // SRL
			case 0x03: armAsm->Asr(d.W(), t.W(), sa); sext32(); break; // SRA
			case 0x04: armAsm->Lsl(d.W(), t.W(), s.W()); sext32(); break; // SLLV
			case 0x06: armAsm->Lsr(d.W(), t.W(), s.W()); sext32(); break; // SRLV
			case 0x07: armAsm->Asr(d.W(), t.W(), s.W()); sext32(); break; // SRAV
			case 0x0a: // MOVZ
			case 0x0b: // MOVN
				load(d, rd);
				armAsm->Cmp(t, 0);
				armAsm->Csel(d, s, d, funct == 0x0a ? a64::eq : a64::ne);
				break;
			case 0x10: armAsm->Ldr(d, a64::MemOperand(rregs, offsetof(cpuRegisters, HI))); break; // MFHI
			case 0x12: armAsm->Ldr(d, a64::MemOperand(rregs, offsetof(cpuRegisters, LO))); break; // MFLO
			case 0x14: armAsm->Lsl(d, t, s); break; // DSLLV
			case 0x16: armAsm->Lsr(d, t, s); break; // DSRLV
			case 0x17: armAsm->Asr(d, t, s); break; // DSRAV
			case 0x20: case 0x21: armAsm->Add(d.W(), s.W(), t.W()); sext32(); break; // ADD, ADDU
			case 0x22: case 0x23: armAsm->Sub(d.W(), s.W(), t.W()); sext32(); break; // SUB, SUBU
			case 0x24: armAsm->And(d, s, t); break;
			case 0x25: armAsm->Orr(d, s, t); break;
			case 0x26: armAsm->Eor(d, s, t); break;
			case 0x27: armAsm->Orr(d, s, t); armAsm->Mvn(d, d); break; // NOR
			case 0x2a: armAsm->Cmp(s, t); armAsm->Cset(d, a64::lt); break; // SLT
			case 0x2b: armAsm->Cmp(s, t); armAsm->Cset(d, a64::lo); break; // SLTU
			case 0x2c: case 0x2d: armAsm->Add(d, s, t); break; // DADD, DADDU
			case 0x2e: case 0x2f: armAsm->Sub(d, s, t); break; // DSUB, DSUBU
			case 0x38: armAsm->Lsl(d, t, sa); break; // DSLL
			case 0x3a: armAsm->Lsr(d, t, sa); break; // DSRL
			case 0x3b: armAsm->Asr(d, t, sa); break; // DSRA
			case 0x3c: armAsm->Lsl(d, t, sa + 32); break; // DSLL32
			case 0x3e: armAsm->Lsr(d, t, sa + 32); break; // DSRL32
			case 0x3f: armAsm->Asr(d, t, sa + 32); break; // DSRA32
		}
		store(rd);
		return true;
	}

	if (op < 0x08 || (op > 0x0f && op != 0x18 && op != 0x19))
		return false;
	if (rt == 0)
		return true;

	if (op != 0x0f)
		load(s, rs);
	switch (op)
	{
		case 0x08: case 0x09: armAsm->Add(d.W(), s.W(), imm); sext32(); break; // ADDI, ADDIU
		case 0x0a: armAsm->Cmp(s, imm); armAsm->Cset(d, a64::lt); break; // SLTI
		case 0x0b: armAsm->Cmp(s, static_cast<u64>(imm)); armAsm->Cset(d, a64::lo); break; // SLTIU
		case 0x0c: armAsm->And(d, s, immu); break; // ANDI
		case 0x0d: armAsm->Orr(d, s, immu); break; // ORI
		case 0x0e: armAsm->Eor(d, s, immu); break; // XORI
		case 0x0f: armAsm->Mov(d, static_cast<s64>(static_cast<s32>(code << 16))); break; // LUI
		case 0x18: case 0x19: armAsm->Add(d, s, imm); break; // DADDI, DADDIU
	}
	store(rt);
	return true;
}

// Jumps and branches compiled with their delay slot (EmitBranch): J, JAL, JR, JALR, BEQ, BNE, BLEZ,
// BGTZ, BLTZ, BGEZ and the likely forms. The Goemon TLB hack runs in the JAL and JR handlers.
static bool IsNativeBranch(u32 code)
{
	if (!s_native)
		return false;
	const u32 op = code >> 26;
	switch (op)
	{
		case 0x00: return ((code & 63) == 0x08 || (code & 63) == 0x09) && !EmuConfig.Gamefixes.GoemonTlbHack; // JR, JALR
		case 0x01: return ((code >> 16) & 31) <= 0x03; // BLTZ, BGEZ, BLTZL, BGEZL
		case 0x02: return true; // J
		case 0x03: return !EmuConfig.Gamefixes.GoemonTlbHack; // JAL
		case 0x04: case 0x05: case 0x06: case 0x07: // BEQ, BNE, BLEZ, BGTZ
		case 0x14: case 0x15: case 0x16: case 0x17: // BEQL, BNEL, BLEZL, BGTZL
			return true;
		default: return false;
	}
}

static void EmitEpilogue(const a64::Register& rregs, const a64::Register& rcycles)
{
	armAsm->Ldr(a64::x21, a64::MemOperand(a64::sp, 32));
	armAsm->Ldp(rregs, rcycles, a64::MemOperand(a64::sp, 16));
	armAsm->Ldp(a64::x29, a64::lr, a64::MemOperand(a64::sp, 48, a64::PostIndex));
}

// Tail call of the block at the pc in wpc, as recRunBlock would run it next; exit when it is not
// compiled yet or the pc is unmapped or misaligned.
static void EmitDispatch(const a64::Register& wpc, const a64::Register& rregs, const a64::Register& rcycles,
	a64::Label& exit)
{
	armAsm->Tst(wpc, 3);
	armAsm->B(a64::ne, &exit);
	armMoveAddressToReg(a64::x13, s_lut);
	armAsm->Lsr(a64::w14, wpc, 16);
	armAsm->Ldr(a64::x13, a64::MemOperand(a64::x13, a64::x14, a64::LSL, 3));
	armAsm->Cbz(a64::x13, &exit);
	armAsm->Ubfx(a64::w14, wpc, 2, 14);
	armAsm->Ldr(a64::x13, a64::MemOperand(a64::x13, a64::x14, a64::LSL, 3));
	armAsm->Cbz(a64::x13, &exit);
	armAsm->Mov(RXARG1, rregs);
	armAsm->Mov(RXARG2, rcycles);
	EmitEpilogue(rregs, rcycles);
	armAsm->Br(a64::x13);
}

// intEventTest (which returns at once before the cycle target), then the next block.
static void EmitEventTestAndDispatch(const a64::Register& wpc, const a64::Register& rregs,
	const a64::Register& rcycles, a64::Label& exit)
{
	a64::Label no_event;
	armAsm->Ldr(a64::x13, a64::MemOperand(rregs, offsetof(cpuRegisters, cycle)));
	armAsm->Ldr(a64::x14, a64::MemOperand(rregs, offsetof(cpuRegisters, nextEventCycle)));
	armAsm->Sub(a64::x13, a64::x13, a64::x14);
	armAsm->Tbnz(a64::x13, 63, &no_event);
	armEmitCall(reinterpret_cast<const void*>(&intEventTest));
	armAsm->B(&exit);
	armAsm->Bind(&no_event);
	EmitDispatch(wpc, rregs, rcycles, exit);
}

// The x86 recompiler's wait loop test (s_nBlockFF): a block that branches to its own start and only
// runs ALU ops on constants and loaded values, loads, mfc/cfc, cache and sync does the same on every
// iteration until an event, so a taken branch back may skip to the next event.
static bool IsFastForwardLoop(u32 startpc, u32 branchpc)
{
	u32 reads = 0, loads = 1;
	for (u32 pc = startpc; pc < branchpc + 8; pc += 4)
	{
		if (pc == branchpc)
			continue;
		const u32 code = memRead32(pc);
		const u32 op = code >> 26, funct = code & 63;
		const u32 rs = (code >> 21) & 31, rt = (code >> 16) & 31, rd = (code >> 11) & 31;
		if (code == 0 || op == 057 || (op == 0 && funct == 017))
			continue;
		u32 dst;
		if ((op & 070) == 010 || (op & 076) == 030 || (op & 070) == 040 || (op & 076) == 032 || op == 067)
		{
			// imm arithmetic, loads
			if (loads & (1u << rs))
			{
				loads |= 1u << rt;
				continue;
			}
			reads |= 1u << rs;
			dst = rt;
		}
		else if (op == 0 && (funct & 060) == 040 && (funct & 076) != 050)
		{
			if ((loads & (1u << rs)) && (loads & (1u << rt)))
			{
				loads |= 1u << rd;
				continue;
			}
			reads |= (1u << rs) | (1u << rt);
			dst = rd;
		}
		else if ((op & 074) == 020 && rs < 4)
		{
			loads |= 1u << rt;
			continue;
		}
		else
			return false;
		if (reads & (1u << dst))
			return false;
	}
	return true;
}

// What the branch's handler and doBranch do, with pc, code and cpuBlockCycles already set for the branch.
// The delay slot runs with cpuRegs.branch set (no hooks; an exception clears it and ends the block).
static void EmitBranch(u32 code, u32 pc, const a64::Register& rregs, const a64::Register& rcycles,
	u32 cycle_factor, u32 ff_start, a64::Label& exit)
{
	const u32 op = code >> 26;
	const u32 rs = (code >> 21) & 31;
	const u32 rt = (code >> 16) & 31;
	const u32 rd = (code >> 11) & 31;
	const bool reg_jump = (op == 0);
	const bool likely = (op >= 0x14) || (op == 0x01 && (rt & 2));
	u32 target;
	if (op == 0x02 || op == 0x03)
		target = ((code & 0x03ffffff) << 2) + ((pc + 4) & 0xf0000000);
	else
		target = static_cast<u32>(static_cast<s16>(code & 0xffff)) * 4 + pc + 4;

	const s64 off_branch = offsetof(cpuRegisters, branch);
	const a64::Register& s = a64::x9;
	const a64::Register& t = a64::x10;
	a64::Label not_taken;

	if (reg_jump)
		armAsm->Ldr(a64::w21, a64::MemOperand(rregs, GprOffset(rs)));
	else if (op != 0x02 && op != 0x03)
	{
		armAsm->Ldr(s, a64::MemOperand(rregs, GprOffset(rs)));
		a64::Condition skip;
		switch (op & 0xf)
		{
			case 0x04: armAsm->Ldr(t, a64::MemOperand(rregs, GprOffset(rt))); armAsm->Cmp(s, t); skip = a64::ne; break; // BEQ
			case 0x05: armAsm->Ldr(t, a64::MemOperand(rregs, GprOffset(rt))); armAsm->Cmp(s, t); skip = a64::eq; break; // BNE
			case 0x06: armAsm->Cmp(s, 0); skip = a64::gt; break; // BLEZ
			case 0x07: armAsm->Cmp(s, 0); skip = a64::le; break; // BGTZ
			default: armAsm->Cmp(s, 0); skip = (rt & 1) ? a64::lt : a64::ge; break; // BGEZ, BLTZ
		}
		armAsm->B(skip, &not_taken);
	}

	const u32 link = (op == 0x03) ? 31 : (reg_jump && (code & 63) == 0x09) ? rd : 0;
	if (link)
	{
		armAsm->Mov(s, pc + 8);
		armAsm->Str(s, a64::MemOperand(rregs, GprOffset(link)));
	}

	// The delay slot, as execI.
	const u32 dcode = memRead32(pc + 4);
	const OPCODE& dop = GetInstruction(dcode);
	armAsm->Mov(a64::w9, 1);
	armAsm->Str(a64::w9, a64::MemOperand(rregs, off_branch));
	armAsm->Mov(a64::w9, dcode);
	armAsm->Str(a64::w9, a64::MemOperand(rregs, offsetof(cpuRegisters, code)));
	armAsm->Mov(a64::w9, pc + 8);
	armAsm->Str(a64::w9, a64::MemOperand(rregs, offsetof(cpuRegisters, pc)));
	armAsm->Ldr(a64::w9, a64::MemOperand(rcycles));
	armAsm->Add(a64::w9, a64::w9, dop.cycles * cycle_factor);
	armAsm->Str(a64::w9, a64::MemOperand(rcycles));
	if (!EmitNative(rregs, dcode))
	{
		armEmitCall(reinterpret_cast<const void*>(dop.interpret));
		armAsm->Ldr(a64::w9, a64::MemOperand(rregs, off_branch));
		armAsm->Cbz(a64::w9, &exit);
	}

	// The rest of doBranch, inline at the default EE cycle rate. Speedhack changes recompile all blocks.
	// A taken branch to the last taken target is a wait loop candidate: it runs intUpdateCPUCycles once
	// more (one more cycle, as cpuBlockCycles < 8 then) and may skip to the next event, which only
	// intBranchAfterDelaySlot does. PSM reads the fixed physical map, so whether the skip test can pass
	// is known here for a constant target.
	const a64::Register& wtarget = a64::w21;
	if (!reg_jump)
		armAsm->Mov(wtarget, target);
	const bool wait_loop = CpuRunsIntBranches() && EmuConfig.Speedhacks.WaitLoop;
	const bool wait_ff = !reg_jump && EmuConfig.Speedhacks.WaitLoop;
	bool may_skip = reg_jump || target == 0x81fc0;
	if (!may_skip && (pc + 8 - target) < 4 * 10)
	{
		may_skip = true;
		for (u32 i = target; i < pc + 8; i += 4)
		{
			if (PSM(i) != 0)
			{
				may_skip = false;
				break;
			}
		}
	}
	a64::Label slow;
	if (EmuConfig.Speedhacks.EECycleRate != 0)
		armAsm->B(&slow);
	armMoveAddressToReg(a64::x15, &intLastBranchTo);
	armAsm->Ldr(a64::w13, a64::MemOperand(a64::x15));
	armAsm->Cmp(a64::w13, wtarget);
	if (wait_loop && may_skip)
		armAsm->B(a64::eq, &slow);
	if (wait_loop)
		armAsm->Cset(a64::w16, a64::eq);
	armAsm->Str(wtarget, a64::MemOperand(a64::x15));
	armAsm->Str(wtarget, a64::MemOperand(rregs, offsetof(cpuRegisters, pc)));
	armAsm->Str(a64::wzr, a64::MemOperand(rregs, off_branch));
	// intUpdateCPUCycles: cycle += max(cpuBlockCycles >> 3, 1), cpuBlockCycles &= 7.
	armAsm->Ldr(a64::w13, a64::MemOperand(rcycles));
	armAsm->Lsr(a64::w14, a64::w13, 3);
	armAsm->Cmp(a64::w14, 1);
	armAsm->Csinc(a64::w14, a64::w14, a64::wzr, a64::hs);
	if (wait_loop)
		armAsm->Add(a64::w14, a64::w14, a64::w16);
	armAsm->Ldr(a64::x15, a64::MemOperand(rregs, offsetof(cpuRegisters, cycle)));
	armAsm->Add(a64::x15, a64::x15, a64::x14);
	if (wait_ff && target == ff_start)
	{
		// IsFastForwardLoop: cycle = nextEventCycle when that is later.
		armAsm->Ldr(a64::x17, a64::MemOperand(rregs, offsetof(cpuRegisters, nextEventCycle)));
		armAsm->Sub(a64::x16, a64::x17, a64::x15);
		armAsm->Cmp(a64::x16, 0);
		armAsm->Csel(a64::x15, a64::x17, a64::x15, a64::gt);
	}
	armAsm->Str(a64::x15, a64::MemOperand(rregs, offsetof(cpuRegisters, cycle)));
	armAsm->And(a64::w13, a64::w13, 7);
	armAsm->Str(a64::w13, a64::MemOperand(rcycles));
	EmitEventTestAndDispatch(wtarget, rregs, rcycles, exit);

	armAsm->Bind(&slow);
	armAsm->Mov(RWARG1, wtarget);
	armEmitCall(reinterpret_cast<const void*>(&intBranchAfterDelaySlot));
	armAsm->B(&exit);

	armAsm->Bind(&not_taken);
	armAsm->Mov(wtarget, likely ? pc + 8 : pc + 4);
	if (likely)
		armAsm->Str(wtarget, a64::MemOperand(rregs, offsetof(cpuRegisters, pc)));
	// BLEZ, BGTZ, BLTZ and BGEZ do not test events when not taken.
	if (likely || op == 0x04 || op == 0x05)
		EmitEventTestAndDispatch(wtarget, rregs, rcycles, exit);
	else
		EmitDispatch(wtarget, rregs, rcycles, exit);
}

// PCSX2_EE_JIT_MAP=<file>: host address of each compiled instruction (pc, name, code word), to
// attribute `sample` addresses in block code.
static FILE* const s_ee_map = [] {
	const char* path = std::getenv("PCSX2_EE_JIT_MAP");
	return path ? std::fopen(path, "w") : nullptr;
}();
static BlockFn Compile(u32 startpc, BlockFn* entry)
{
	if (static_cast<size_t>(s_code_end - s_code_ptr) < MAX_BLOCK_CODE)
	{
		Console.WriteLn("ARM64 EE rec: code buffer full, clearing all blocks");
		ClearAllBlocks();
		entry = &s_lut[startpc >> 16][(startpc & 0xffff) >> 2];
	}

	const u32 cycle_factor = 2 - s_config_bit18;
	const u32 hwaddr = (s_hw_page[startpc >> 16] << 16) | (startpc & 0xffff);
	const bool in_ram = (s_lut[startpc >> 16] - s_ram_blocks) >= 0 &&
						static_cast<size_t>(s_lut[startpc >> 16] - s_ram_blocks) < Ps2MemSize::ExposedRam / 4;

	// The instructions, up to the end of the block.
	u32 count = 0;
	for (u32 pc = startpc;; pc += 4)
	{
		count++;
		const u32 code = memRead32(pc);
		if (EndsBlock(GetInstruction(code), code) || ((pc + 4) & 0xfff) == 0)
			break;
	}
	// The block ends with a branch compiled with its delay slot, which then is checked as block code.
	const u32 lastpc = startpc + (count - 1) * 4;
	const bool native_branch = IsNativeBranch(memRead32(lastpc)) && ((lastpc + 4) & 0xfff) != 0 &&
							   !EndsBlock(GetInstruction(memRead32(lastpc + 4)), memRead32(lastpc + 4));
	const u32 code_words = count + native_branch;
	const u32 ff_start = (native_branch && IsFastForwardLoop(startpc, lastpc)) ? startpc : 0xffffffff;

	// Self-modifying code: a written page clears its blocks (vtlb page protection), or, on a page under
	// manual protection, the block compares its code at entry (as memory_protect_recompiled_code on x86).
	bool check_code = false;
	if (in_ram)
	{
		const bool thread_stack = ((startpc >> 12) == 0x81) || ((startpc >> 12) == 0x80001);
		const vtlb_ProtectionMode mode = thread_stack ? ProtMode_Manual : mmap_GetRamPageInfo(hwaddr);
		if (mode == ProtMode_Manual)
			check_code = true;
		else if (mode != ProtMode_NotRequired)
			mmap_MarkCountedRamPage(hwaddr);
	}

	armSetAsmPtr(s_code_ptr, s_code_end - s_code_ptr, &s_pool);
	u8* const fn = armStartBlock();

	const a64::Register& rregs = a64::x19;
	const a64::Register& rcycles = a64::x20;
	a64::Label exit, discard;

	armAsm->Stp(a64::x29, a64::lr, a64::MemOperand(a64::sp, -48, a64::PreIndex));
	armAsm->Stp(rregs, rcycles, a64::MemOperand(a64::sp, 16));
	armAsm->Str(a64::x21, a64::MemOperand(a64::sp, 32));
	armAsm->Mov(rregs, RXARG1);
	armAsm->Mov(rcycles, RXARG2);

	if (check_code)
	{
		armMoveAddressToReg(a64::x9, PSM(startpc));
		for (u32 i = 0; i < code_words; i++)
		{
			armAsm->Ldr(a64::w10, a64::MemOperand(a64::x9, i * 4));
			armAsm->Mov(a64::w11, memRead32(startpc + i * 4));
			armAsm->Cmp(a64::w10, a64::w11);
			armAsm->B(a64::ne, &discard);
		}
	}

	const s64 off_pc = offsetof(cpuRegisters, pc);
	const s64 off_code = offsetof(cpuRegisters, code);
	// Native ops leave pc, code and cpuBlockCycles to the next handler call or the block end.
	u32 pending_cycles = 0;
	const auto flush = [&](u32 code, u32 pc) {
		armAsm->Mov(a64::w9, code);
		armAsm->Str(a64::w9, a64::MemOperand(rregs, off_code));
		armAsm->Mov(a64::w9, pc);
		armAsm->Str(a64::w9, a64::MemOperand(rregs, off_pc));
		if (pending_cycles)
		{
			armAsm->Ldr(a64::w9, a64::MemOperand(rcycles));
			armAsm->Add(a64::w9, a64::w9, pending_cycles);
			armAsm->Str(a64::w9, a64::MemOperand(rcycles));
			pending_cycles = 0;
		}
	};
	bool last_native = false;
	for (u32 i = 0; i < count; i++)
	{
		const u32 pc = startpc + i * 4;
		const u32 code = memRead32(pc);
		const OPCODE& opcode = GetInstruction(code);
		if (s_ee_map)
			std::fprintf(s_ee_map, "%p E %08x:%s:%08x\n", armGetCurrentCodePointer(), pc, opcode.Name, code);

		if (Zdxsv::IsIntHookPc(pc))
		{
			if (last_native)
				flush(memRead32(pc - 4), pc);
			armAsm->Mov(RWARG1, pc);
			armEmitCall(reinterpret_cast<const void*>(&HookAnswered));
			armAsm->Cbnz(RWRET, &exit);
		}

		pending_cycles += opcode.cycles * cycle_factor;
		last_native = EmitNative(rregs, code);
		if (last_native)
			continue;

		flush(code, pc + 4);
		if (native_branch && i + 1 == count)
		{
			EmitBranch(code, pc, rregs, rcycles, cycle_factor, ff_start, exit);
			break;
		}
		armEmitCall(reinterpret_cast<const void*>(opcode.interpret));

		// An exception or a cancelled instruction moved the pc.
		if (i + 1 < count)
		{
			armAsm->Ldr(a64::w9, a64::MemOperand(rregs, off_pc));
			armAsm->Mov(a64::w10, pc + 4);
			armAsm->Cmp(a64::w9, a64::w10);
			armAsm->B(a64::ne, &exit);
		}
	}
	if (last_native)
	{
		const u32 lastpc = startpc + (count - 1) * 4;
		flush(memRead32(lastpc), lastpc + 4);
	}

	armAsm->Bind(&exit);
	EmitEpilogue(rregs, rcycles);
	armAsm->Ret();

	if (check_code)
	{
		// The code changed: the pc is unchanged, the dispatcher recompiles.
		armAsm->Bind(&discard);
		armAsm->Mov(RWARG1, startpc);
		armAsm->Mov(RWARG2, code_words);
		armEmitCall(reinterpret_cast<const void*>(&recClear));
		armAsm->B(&exit);
	}

	if (s_ee_map)
	{
		std::fprintf(s_ee_map, "%p end -\n", armGetCurrentCodePointer());
		std::fflush(s_ee_map);
	}
	s_code_ptr = armEndBlock();

	const BlockFn block = reinterpret_cast<BlockFn>(fn);
	*entry = block;
	return block;
}

static void recRunBlock()
{
	if (((cpuRegs.CP0.n.Config >> 18) & 1) != s_config_bit18) [[unlikely]]
		ClearAllBlocks();

	const u32 pc = cpuRegs.pc;
	BlockFn* const page = s_lut[pc >> 16];
	if (!page || (pc & 3)) [[unlikely]]
	{
		intCpu.Step();
		return;
	}

	BlockFn* const entry = &page[(pc & 0xffff) >> 2];
	BlockFn block = *entry;
	if (!block) [[unlikely]]
		block = Compile(pc, entry);
	block(&cpuRegs, &cpuBlockCycles);
}

static void recExecute()
{
	intExecuteBlocks(recRunBlock);
}

static void recStep()
{
	intCpu.Step();
}

R5900cpu recCpu = {
	recReserve,
	recShutdown,

	recReset,
	recStep,
	recExecute,

	intSafeExitExecution,
	intCancelInstruction,

	recClear,
};
