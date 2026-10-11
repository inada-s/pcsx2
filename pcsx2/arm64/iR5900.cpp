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
	Console.WriteLn("ARM64 EE rec: reserved");

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

	armAsm->Stp(a64::x29, a64::lr, a64::MemOperand(a64::sp, -32, a64::PreIndex));
	armAsm->Stp(rregs, rcycles, a64::MemOperand(a64::sp, 16));
	armAsm->Mov(rregs, RXARG1);
	armAsm->Mov(rcycles, RXARG2);

	if (check_code)
	{
		armMoveAddressToReg(a64::x9, PSM(startpc));
		for (u32 i = 0; i < count; i++)
		{
			armAsm->Ldr(a64::w10, a64::MemOperand(a64::x9, i * 4));
			armAsm->Mov(a64::w11, memRead32(startpc + i * 4));
			armAsm->Cmp(a64::w10, a64::w11);
			armAsm->B(a64::ne, &discard);
		}
	}

	const s64 off_pc = offsetof(cpuRegisters, pc);
	const s64 off_code = offsetof(cpuRegisters, code);
	for (u32 i = 0; i < count; i++)
	{
		const u32 pc = startpc + i * 4;
		const u32 code = memRead32(pc);
		const OPCODE& opcode = GetInstruction(code);

		if (Zdxsv::IsIntHookPc(pc))
		{
			armAsm->Mov(RWARG1, pc);
			armEmitCall(reinterpret_cast<const void*>(&HookAnswered));
			armAsm->Cbnz(RWRET, &exit);
		}

		armAsm->Mov(a64::w9, code);
		armAsm->Str(a64::w9, a64::MemOperand(rregs, off_code));
		armAsm->Mov(a64::w9, pc + 4);
		armAsm->Str(a64::w9, a64::MemOperand(rregs, off_pc));
		armAsm->Ldr(a64::w9, a64::MemOperand(rcycles));
		armAsm->Add(a64::w9, a64::w9, opcode.cycles * cycle_factor);
		armAsm->Str(a64::w9, a64::MemOperand(rcycles));
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

	armAsm->Bind(&exit);
	armAsm->Ldp(rregs, rcycles, a64::MemOperand(a64::sp, 16));
	armAsm->Ldp(a64::x29, a64::lr, a64::MemOperand(a64::sp, 32, a64::PostIndex));
	armAsm->Ret();

	if (check_code)
	{
		// The code changed: the pc is unchanged, the dispatcher recompiles.
		armAsm->Bind(&discard);
		armAsm->Mov(RWARG1, startpc);
		armAsm->Mov(RWARG2, count);
		armEmitCall(reinterpret_cast<const void*>(&recClear));
		armAsm->B(&exit);
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
