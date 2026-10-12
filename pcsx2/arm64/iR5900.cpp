// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARM64 EE recompiler.
// Blocks and cycles follow the x86 recompiler, so both count the same EE cycles: the same block layout
// (recRecompile), each block end commits cycle += max(blockcycles >> 3, 1) with the fraction dropped,
// and both paths of a branch commit and test events. Each instruction is compiled into the state
// updates execI makes (cpuRegs.code, cpuRegs.pc, cpuBlockCycles) and a call of its interpreter handler,
// or inline for integer ops; exceptions and TLB misses take the interpreter's paths (CpuRunsIntBranches).

#include "Common.h"
#include "Dmac.h"
#include "R5900OpcodeTables.h"
#include "VMManager.h"
#include "VUmicro.h"
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

// VU1Trace.inl: EE COP2 macro FMAC ops as native code.
bool vu0MacroCanEmit(u32 code);
void vu0MacroEmit(u32 code, const a64::Register& rvu);

static bool HookAnswered(u32 pc)
{
	return Zdxsv::IntHooksOn() && Zdxsv::IntRunHooks(pc);
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
// A branch as the x86 recompiler's block scan (recRecompile) sees it: jumps, regimm branches
// (BLTZ..BGEZL, BLTZAL..BGEZALL), BEQ..BGTZL, and BC0x/BC1x/BC2x.
static bool IsBranch(u32 code)
{
	const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31;
	switch (op)
	{
		case 0x00: return (code & 63) == 0x08 || (code & 63) == 0x09;
		case 0x01: return rt < 4 || (rt >= 16 && rt < 20);
		case 0x02: case 0x03:
		case 0x04: case 0x05: case 0x06: case 0x07:
		case 0x14: case 0x15: case 0x16: case 0x17:
			return true;
		case 0x10: case 0x11: case 0x12: return rs == 8;
		default: return false;
	}
}

static u32 BranchTarget(u32 code, u32 pc)
{
	if ((code >> 26) == 0x02 || (code >> 26) == 0x03)
		return ((code & 0x03ffffff) << 2) | ((pc + 4) & 0xf0000000);
	return static_cast<u32>(static_cast<s16>(code & 0xffff)) * 4 + pc + 4;
}

// COP0 ops the x86 recompiler (iCOP0.cpp) runs after a mid-block commit (scaleblockcycles_clear: the
// fraction stays): MFC0 Count, MFPC to a register, MTC0 Status and Count, MTPS to PCCR, MTPC.
static bool CommitsBeforeOp(u32 code)
{
	if ((code >> 26) != 0x10)
		return false;
	const u32 rs = (code >> 21) & 31, rt = (code >> 16) & 31, rd = (code >> 11) & 31;
	if (rs == 0)
		return rd == 9 || (rd == 25 && rt != 0 && (code & 1));
	if (rs == 4)
		return rd == 12 || rd == 9 || (rd == 25 && ((code & 1) || (code & 0x3e) == 0));
	return false;
}

// ERET, EI and the traps: the x86 recompiler sets nextEventCycle = cycle before the call (recBranchCall),
// so the block end after them always tests events.
static bool ForcesEventTest(u32 code)
{
	const u32 op = code >> 26;
	if (code == 0x42000018 || code == 0x42000038)
		return true;
	if (op == 0x00)
		return (code & 63) >= 0x30 && (code & 63) <= 0x36 && (code & 63) != 0x35;
	if (op == 0x01)
	{
		const u32 rt = (code >> 16) & 31;
		return rt >= 8 && rt <= 14 && rt != 13;
	}
	return false;
}

// The interpreter's LB, LBU, LH, LHU and LW without their event test after an EE counter read
// (0x10000000-0x10001fff): the x86 recompiler tests only after a constant address (Compile).
// A misaligned address takes the interpreter's handler (address error).
template <typename T, void (*Interp)()>
static void LoadNoEventTest()
{
	const u32 addr = cpuRegs.GPR.r[(cpuRegs.code >> 21) & 31].UL[0] + static_cast<s16>(cpuRegs.code & 0xffff);
	if (addr & (sizeof(T) - 1)) [[unlikely]]
	{
		Interp();
		return;
	}
	T value;
	if constexpr (sizeof(T) == 1)
		value = static_cast<T>(memRead8(addr));
	else if constexpr (sizeof(T) == 2)
		value = static_cast<T>(memRead16(addr));
	else
		value = static_cast<T>(memRead32(addr));
	const u32 rt = (cpuRegs.code >> 16) & 31;
	if (!rt)
		return;
	if constexpr (std::is_signed_v<T>)
		cpuRegs.GPR.r[rt].SD[0] = value;
	else
		cpuRegs.GPR.r[rt].UD[0] = value;
}

static void (*LoadHandler(u32 code))()
{
	switch (code >> 26)
	{
		case 0x20: return &LoadNoEventTest<s8, Interpreter::OpcodeImpl::LB>;
		case 0x21: return &LoadNoEventTest<s16, Interpreter::OpcodeImpl::LH>;
		case 0x23: return &LoadNoEventTest<s32, Interpreter::OpcodeImpl::LW>;
		case 0x24: return &LoadNoEventTest<u8, Interpreter::OpcodeImpl::LBU>;
		case 0x25: return &LoadNoEventTest<u16, Interpreter::OpcodeImpl::LHU>;
		default: return nullptr;
	}
}

// Block-local GPR constants, as the x86 recompiler's g_cpuConstRegs, for its constant address loads and
// FlushCache syscalls. Ops not followed here forget the registers they may write.
struct ConstRegs
{
	u32 mask = 1;
	u64 v[32] = {};

	bool Is(u32 r) const { return (mask >> r) & 1; }
	void Set(u32 r, u64 x)
	{
		if (r)
		{
			mask |= 1u << r;
			v[r] = x;
		}
	}
	void Clear(u32 r)
	{
		if (r)
			mask &= ~(1u << r);
	}

	void Track(u32 code)
	{
		const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31, rd = (code >> 11) & 31;
		const u32 sa = (code >> 6) & 31;
		const u64 simm = static_cast<u64>(static_cast<s64>(static_cast<s16>(code & 0xffff)));
		const u64 zimm = code & 0xffff;
		const auto s32x = [](u64 x) { return static_cast<u64>(static_cast<s64>(static_cast<s32>(x))); };
		switch (op)
		{
			case 0x00:
			{
				const bool shift = (code & 63) <= 3;
				if ((shift && Is(rt)) || (!shift && Is(rs) && Is(rt)))
				{
					const u64 a = v[rs], b = v[rt];
					switch (code & 63)
					{
						case 0x00: Set(rd, s32x(static_cast<u32>(b) << sa)); return; // SLL
						case 0x02: Set(rd, s32x(static_cast<u32>(b) >> sa)); return; // SRL
						case 0x03: Set(rd, s32x(static_cast<u64>(static_cast<s32>(b) >> sa))); return; // SRA
						case 0x20: case 0x21: Set(rd, s32x(a + b)); return; // ADD, ADDU
						case 0x22: case 0x23: Set(rd, s32x(a - b)); return; // SUB, SUBU
						case 0x24: Set(rd, a & b); return;
						case 0x25: Set(rd, a | b); return;
						case 0x26: Set(rd, a ^ b); return;
						case 0x27: Set(rd, ~(a | b)); return;
						case 0x2a: Set(rd, static_cast<s64>(a) < static_cast<s64>(b)); return; // SLT
						case 0x2b: Set(rd, a < b); return; // SLTU
						case 0x2c: case 0x2d: Set(rd, a + b); return; // DADD, DADDU
						case 0x2e: case 0x2f: Set(rd, a - b); return; // DSUB, DSUBU
						default: break;
					}
				}
				Clear(rd);
				return;
			}
			case 0x01: if (rt & 16) Clear(31); return; // BLTZAL..BGEZALL
			case 0x03: Clear(31); return; // JAL
			case 0x08: case 0x09: if (Is(rs)) Set(rt, s32x(v[rs] + simm)); else Clear(rt); return; // ADDI, ADDIU
			case 0x0a: if (Is(rs)) Set(rt, static_cast<s64>(v[rs]) < static_cast<s64>(simm)); else Clear(rt); return; // SLTI
			case 0x0b: if (Is(rs)) Set(rt, v[rs] < simm); else Clear(rt); return; // SLTIU
			case 0x0c: if (Is(rs)) Set(rt, v[rs] & zimm); else Clear(rt); return; // ANDI
			case 0x0d: if (Is(rs)) Set(rt, v[rs] | zimm); else Clear(rt); return; // ORI
			case 0x0e: if (Is(rs)) Set(rt, v[rs] ^ zimm); else Clear(rt); return; // XORI
			case 0x0f: Set(rt, s32x(zimm << 16)); return; // LUI
			case 0x18: case 0x19: if (Is(rs)) Set(rt, v[rs] + simm); else Clear(rt); return; // DADDI, DADDIU
			case 0x10: case 0x11: case 0x12: if (rs <= 2) Clear(rt); return; // MFCz, QMFC2, CFCz
			case 0x1c: Clear(rd); return; // MMI
			case 0x02: case 0x04: case 0x05: case 0x06: case 0x07: case 0x14: case 0x15: case 0x16: case 0x17:
			case 0x1f: case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
			case 0x31: case 0x36: case 0x39: case 0x3e: case 0x3f:
				return; // branches, stores, CACHE, LWC1, LQC2
			default: Clear(rt); return; // loads
		}
	}
};

static bool IsCompiled(u32 pc)
{
	const BlockFn* page = s_lut[pc >> 16];
	return page && page[(pc & 0xffff) >> 2];
}

static u32 Bc0Condition()
{
	return ((dmacRegs.stat.CIS | ~dmacRegs.pcr.CPC) & 0x3ff) == 0x3ff;
}

static void EmitEpilogue(const a64::Register& rregs, const a64::Register& rcycles)
{
	armAsm->Ldp(a64::x21, a64::x22, a64::MemOperand(a64::sp, 32));
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

static void CommitScaledCycles()
{
	intUpdateCPUCycles();
	cpuBlockCycles = 0;
}

// Block end, as the x86 recompiler (scaleblockcycles): cycle += max(cpuBlockCycles >> 3, 1) and the
// fraction is dropped, so every block starts at cpuBlockCycles = 0.
static void EmitCommit(const a64::Register& rregs, const a64::Register& rcycles)
{
	if (EmuConfig.Speedhacks.EECycleRate != 0)
	{
		armEmitCall(reinterpret_cast<const void*>(&CommitScaledCycles));
		return;
	}
	armAsm->Ldr(a64::w13, a64::MemOperand(rcycles));
	armAsm->Lsr(a64::w14, a64::w13, 3);
	armAsm->Cmp(a64::w14, 1);
	armAsm->Csinc(a64::w14, a64::w14, a64::wzr, a64::hs);
	armAsm->Ldr(a64::x15, a64::MemOperand(rregs, offsetof(cpuRegisters, cycle)));
	armAsm->Add(a64::x15, a64::x15, a64::x14);
	armAsm->Str(a64::x15, a64::MemOperand(rregs, offsetof(cpuRegisters, cycle)));
	armAsm->Str(a64::wzr, a64::MemOperand(rcycles));
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

// A branch and its delay slot, as the x86 recompiler: both paths commit the block cycles (the delay
// slot's too, unless a likely branch skips it) and test events; a taken branch to the block start of a
// wait loop (IsFastForwardLoop) skips to the next event. pc, code and cpuBlockCycles are already set for
// the branch. The delay slot runs with cpuRegs.branch set (no hooks; an exception clears it).
static void EmitBranch(u32 code, u32 pc, const a64::Register& rregs, const a64::Register& rcycles,
	u32 cycle_factor, u32 ff_start, a64::Label& exit_commit, a64::Label& exit)
{
	const u32 op = code >> 26;
	const u32 rs = (code >> 21) & 31;
	const u32 rt = (code >> 16) & 31;
	const u32 rd = (code >> 11) & 31;
	const bool reg_jump = (op == 0);
	const bool cond = !reg_jump && op != 0x02 && op != 0x03;
	const bool likely = cond && ((op == 0x01 || op >= 0x10 && op <= 0x12) ? (rt & 2) != 0 : op >= 0x14);
	const u32 target = BranchTarget(code, pc);

	const s64 off_branch = offsetof(cpuRegisters, branch);
	const a64::Register& s = a64::x9;
	const a64::Register& t = a64::x10;
	const a64::Register& wtaken = a64::w22;
	const a64::Register& wtarget = a64::w21;

	if (reg_jump)
		armAsm->Ldr(wtarget, a64::MemOperand(rregs, GprOffset(rs)));
	else if (cond)
	{
		a64::Condition taken;
		switch (op)
		{
			case 0x01: case 0x04: case 0x05: case 0x06: case 0x07:
			case 0x14: case 0x15: case 0x16: case 0x17:
				armAsm->Ldr(s, a64::MemOperand(rregs, GprOffset(rs)));
				switch (op & 0xf)
				{
					case 0x04: armAsm->Ldr(t, a64::MemOperand(rregs, GprOffset(rt))); armAsm->Cmp(s, t); taken = a64::eq; break; // BEQ
					case 0x05: armAsm->Ldr(t, a64::MemOperand(rregs, GprOffset(rt))); armAsm->Cmp(s, t); taken = a64::ne; break; // BNE
					case 0x06: armAsm->Cmp(s, 0); taken = a64::le; break; // BLEZ
					case 0x07: armAsm->Cmp(s, 0); taken = a64::gt; break; // BGTZ
					default: armAsm->Cmp(s, 0); taken = (rt & 1) ? a64::ge : a64::lt; break; // BGEZ, BLTZ
				}
				break;
			case 0x10: // BC0F, BC0T: CPCOND0
				armEmitCall(reinterpret_cast<const void*>(&Bc0Condition));
				armAsm->Cmp(RWRET, 0);
				taken = (rt & 1) ? a64::ne : a64::eq;
				break;
			case 0x11: // BC1F, BC1T: FPU condition flag C
				armMoveAddressToReg(a64::x9, &fpuRegs.fprc[31]);
				armAsm->Ldr(a64::w9, a64::MemOperand(a64::x9));
				armAsm->Tst(a64::w9, 0x00800000);
				taken = (rt & 1) ? a64::ne : a64::eq;
				break;
			default: // BC2F, BC2T: VPU_STAT bit 8 (VU1 running)
				armMoveAddressToReg(a64::x9, &VU0.VI[REG_VPU_STAT]);
				armAsm->Ldr(a64::w9, a64::MemOperand(a64::x9));
				armAsm->Tst(a64::w9, 0x100);
				taken = (rt & 1) ? a64::ne : a64::eq;
				break;
		}
		armAsm->Cset(wtaken, taken);
	}

	const u32 link = (op == 0x03 || (op == 0x01 && (rt & 16))) ? 31 : (reg_jump && (code & 63) == 0x09) ? rd : 0;
	if (link)
	{
		armAsm->Mov(s, pc + 8);
		armAsm->Str(s, a64::MemOperand(rregs, GprOffset(link)));
	}

	a64::Label not_taken;
	if (likely)
		armAsm->Cbz(wtaken, &not_taken);

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
		armAsm->Cbz(a64::w9, &exit_commit);
	}
	if (cond && !likely)
		armAsm->Cbz(wtaken, &not_taken);

	if (!reg_jump)
		armAsm->Mov(wtarget, target);
	armAsm->Str(wtarget, a64::MemOperand(rregs, offsetof(cpuRegisters, pc)));
	armAsm->Str(a64::wzr, a64::MemOperand(rregs, off_branch));
	EmitCommit(rregs, rcycles);
	if (EmuConfig.Speedhacks.WaitLoop && !reg_jump && target == ff_start)
	{
		// cycle = nextEventCycle when that is later.
		armAsm->Ldr(a64::x15, a64::MemOperand(rregs, offsetof(cpuRegisters, cycle)));
		armAsm->Ldr(a64::x17, a64::MemOperand(rregs, offsetof(cpuRegisters, nextEventCycle)));
		armAsm->Sub(a64::x16, a64::x17, a64::x15);
		armAsm->Cmp(a64::x16, 0);
		armAsm->Csel(a64::x15, a64::x17, a64::x15, a64::gt);
		armAsm->Str(a64::x15, a64::MemOperand(rregs, offsetof(cpuRegisters, cycle)));
	}
	EmitEventTestAndDispatch(wtarget, rregs, rcycles, exit);

	if (cond)
	{
		armAsm->Bind(&not_taken);
		armAsm->Mov(wtarget, pc + 8);
		armAsm->Str(wtarget, a64::MemOperand(rregs, offsetof(cpuRegisters, pc)));
		armAsm->Str(a64::wzr, a64::MemOperand(rregs, off_branch));
		EmitCommit(rregs, rcycles);
		EmitEventTestAndDispatch(wtarget, rregs, rcycles, exit);
	}
}

// The x86 recompiler's timeout loop skip (recSkipTimeoutLoop): a block of `addiu r, r, -n`, nops and
// `bne r, zero` with a nop delay slot spins r cycles; skip them, up to the next event.
static void EmitTimeoutLoop(u32 reg, u32 endpc, const a64::Register& rregs, const a64::Register& rcycles,
	a64::Label& exit)
{
	a64::Label event;
	armAsm->Ldr(a64::x12, a64::MemOperand(rregs, offsetof(cpuRegisters, cycle)));
	armAsm->Ldr(a64::x13, a64::MemOperand(rregs, offsetof(cpuRegisters, nextEventCycle)));
	armAsm->Cmp(a64::x12, a64::x13);
	armAsm->B(a64::hs, &event);
	armAsm->Ldr(a64::w14, a64::MemOperand(rregs, GprOffset(reg)));
	armAsm->Add(a64::x15, a64::x12, a64::Operand(a64::x14, a64::LSL, 3));
	armAsm->Cmp(a64::x13, a64::x15);
	armAsm->Csel(a64::x15, a64::x13, a64::x15, a64::lo);
	armAsm->Str(a64::x15, a64::MemOperand(rregs, offsetof(cpuRegisters, cycle)));
	armAsm->Sub(a64::x15, a64::x15, a64::x12);
	armAsm->Sub(a64::x14, a64::x14, a64::Operand(a64::x15, a64::LSR, 3));
	armAsm->Str(a64::w14, a64::MemOperand(rregs, GprOffset(reg)));
	armAsm->Cbnz(a64::x14, &event);
	armAsm->Mov(a64::w21, endpc);
	armAsm->Str(a64::w21, a64::MemOperand(rregs, offsetof(cpuRegisters, pc)));
	EmitDispatch(a64::w21, rregs, rcycles, exit);
	armAsm->Bind(&event);
	armEmitCall(reinterpret_cast<const void*>(&intEventTest));
	armAsm->B(&exit);
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

	// The x86 recompiler's block layout: up to a branch and its delay slot (or up to the branch's target
	// when that is inside the block), SYSCALL, BREAK or ERET, or split at a 4 KB page or before a
	// compiled block.
	u32 endpc, branch_to = 0xffffffff;
	s32 timeout_reg = -1;
	bool timeout_loop = true;
	for (u32 pc = startpc;; pc += 4)
	{
		if (pc != startpc && ((pc & 0xffc) == 0 || IsCompiled(pc)))
		{
			endpc = pc;
			break;
		}
		const u32 code = memRead32(pc);
		const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31;
		if (timeout_loop)
		{
			if (op == 0x08 || op == 0x09)
			{
				if (timeout_reg >= 0 || rs != rt || static_cast<s16>(code & 0xffff) >= 0)
					timeout_loop = false;
				else
					timeout_reg = rs;
			}
			else if (op == 0x05)
			{
				if (timeout_reg != static_cast<s32>(rs) || rt != 0 || memRead32(pc + 4) != 0)
					timeout_loop = false;
			}
			else if (code != 0)
				timeout_loop = false;
		}
		if (IsBranch(code))
		{
			endpc = pc + 8;
			if (op != 0x00)
			{
				branch_to = BranchTarget(code, pc);
				if (op != 0x02 && op != 0x03 && branch_to > startpc && branch_to < pc)
					endpc = branch_to;
			}
			break;
		}
		if ((op == 0x00 && ((code & 63) == 0x0c || (code & 63) == 0x0d)) || code == 0x42000018)
		{
			endpc = pc + 4;
			break;
		}
	}
	const u32 code_words = (endpc - startpc) / 4;
	const u32 ff_start =
		(branch_to == startpc && endpc >= startpc + 8 && IsFastForwardLoop(startpc, endpc - 8)) ? startpc : 0xffffffff;

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
	a64::Label exit, exit_commit, hook_exit, discard;

	armAsm->Stp(a64::x29, a64::lr, a64::MemOperand(a64::sp, -48, a64::PreIndex));
	armAsm->Stp(rregs, rcycles, a64::MemOperand(a64::sp, 16));
	armAsm->Stp(a64::x21, a64::x22, a64::MemOperand(a64::sp, 32));
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
	if (Zdxsv::EventLogBlocks())
	{
		armAsm->Mov(RWARG1, startpc);
		armEmitCall(reinterpret_cast<const void*>(&Zdxsv::EventLogBlock));
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
	bool ended = false;
	ConstRegs consts;
	u32 pc = startpc;
	// Only a block that branches to its own start (a block split before a compiled block may be just the addiu).
	if (timeout_loop && timeout_reg >= 0 && branch_to == startpc && EmuConfig.Speedhacks.WaitLoop)
	{
		EmitTimeoutLoop(timeout_reg, endpc, rregs, rcycles, exit);
		ended = true;
	}
	for (; !ended && pc < endpc; pc += 4)
	{
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
			armAsm->Cbnz(RWRET, &hook_exit);
		}

		pending_cycles += opcode.cycles * cycle_factor;
		if (IsBranch(code))
		{
			flush(code, pc + 4);
			EmitBranch(code, pc, rregs, rcycles, cycle_factor, ff_start, exit_commit, exit);
			ended = true;
			break;
		}
		const u32 op = code >> 26, rs = (code >> 21) & 31;
		if (op == 0x00 && (code & 63) == 0x0c && consts.Is(3) && ((consts.v[3] & 0xff) == 0x64 || (consts.v[3] & 0xff) == 0x68))
		{
			// FlushCache: the x86 recompiler adds the cycles of its exception handlers instead of the call.
			pending_cycles += 5650;
			last_native = true;
			continue;
		}
		const bool const_hw_load = (op >= 0x20 && op <= 0x25 && op != 0x22) || op == 0x27 ?
			consts.Is(rs) && ((static_cast<u32>(consts.v[rs]) + static_cast<s16>(code & 0xffff)) & 0xffffe000) == 0x10000000 : false;
		last_native = EmitNative(rregs, code);
		consts.Track(code);
		if (last_native)
			continue;

		if (s_native && vu0MacroCanEmit(code))
		{
			// COP2_SPECIAL: native unless VU0 runs a micro program (_vu0FinishMicro), then the handler.
			flush(code, pc + 4);
			a64::Label busy, done;
			armMoveAddressToReg(a64::x12, &VU0);
			armAsm->Ldr(a64::w9, a64::MemOperand(a64::x12, reinterpret_cast<u8*>(&VU0.VI[REG_VPU_STAT]) -
															  reinterpret_cast<u8*>(&VU0)));
			armAsm->Tbnz(a64::w9, 0, &busy);
			vu0MacroEmit(code, a64::x12);
			armAsm->B(&done);
			armAsm->Bind(&busy);
			armEmitCall(reinterpret_cast<const void*>(opcode.interpret));
			armAsm->Ldr(a64::w9, a64::MemOperand(rregs, off_pc));
			armAsm->Mov(a64::w10, pc + 4);
			armAsm->Cmp(a64::w9, a64::w10);
			armAsm->B(a64::ne, &exit_commit);
			armAsm->Bind(&done);
			continue;
		}

		flush(code, pc + 4);
		const bool force_test = ForcesEventTest(code);
		if (force_test)
		{
			armAsm->Ldr(a64::x9, a64::MemOperand(rregs, offsetof(cpuRegisters, cycle)));
			armAsm->Str(a64::x9, a64::MemOperand(rregs, offsetof(cpuRegisters, nextEventCycle)));
		}
		if (CommitsBeforeOp(code))
			armEmitCall(reinterpret_cast<const void*>(&intUpdateCPUCycles));
		void (*handler)() = LoadHandler(code);
		armEmitCall(reinterpret_cast<const void*>(handler ? handler : opcode.interpret));
		if (force_test || const_hw_load || (code >> 26 == 0x00 && ((code & 63) == 0x0c || (code & 63) == 0x0d)))
		{
			// The x86 recompiler's g_branch = 2: commit, test events, continue at cpuRegs.pc.
			EmitCommit(rregs, rcycles);
			armAsm->Ldr(a64::w21, a64::MemOperand(rregs, off_pc));
			EmitEventTestAndDispatch(a64::w21, rregs, rcycles, exit);
			ended = true;
			break;
		}

		// An exception or a cancelled instruction moved the pc.
		armAsm->Ldr(a64::w9, a64::MemOperand(rregs, off_pc));
		armAsm->Mov(a64::w10, pc + 4);
		armAsm->Cmp(a64::w9, a64::w10);
		armAsm->B(a64::ne, &exit_commit);
	}
	if (!ended)
	{
		// Split block: commit, test events only after more than 6 instructions.
		if (last_native)
			flush(memRead32(endpc - 4), endpc);
		EmitCommit(rregs, rcycles);
		armAsm->Mov(a64::w21, endpc);
		armAsm->Str(a64::w21, a64::MemOperand(rregs, off_pc));
		if (code_words > 6)
			EmitEventTestAndDispatch(a64::w21, rregs, rcycles, exit);
		else
			EmitDispatch(a64::w21, rregs, rcycles, exit);
	}

	// An exception in a handler: commit and test events, the dispatcher continues at cpuRegs.pc.
	armAsm->Bind(&exit_commit);
	EmitCommit(rregs, rcycles);
	armAsm->Ldr(a64::x13, a64::MemOperand(rregs, offsetof(cpuRegisters, cycle)));
	armAsm->Ldr(a64::x14, a64::MemOperand(rregs, offsetof(cpuRegisters, nextEventCycle)));
	armAsm->Sub(a64::x13, a64::x13, a64::x14);
	armAsm->Tbnz(a64::x13, 63, &exit);
	armEmitCall(reinterpret_cast<const void*>(&intEventTest));
	armAsm->B(&exit);

	// An answered hook leaves the block without its cycles, as the x86 recompiler's hook jumps to the
	// dispatcher before the block end adds them.
	armAsm->Bind(&hook_exit);
	armAsm->Str(a64::wzr, a64::MemOperand(rcycles));

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
