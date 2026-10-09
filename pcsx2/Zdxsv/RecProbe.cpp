// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// EE recompiler tracing (ZDXSV_EE_PROBE, ZDXSV_EE_WATCH), emitted through RecEmitHooks.

#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/RecHooks.h"
#include "Zdxsv/TestOptions.h"

#include "Counters.h"
#include "Memory.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"
#include "x86/iR5900.h"

#include "common/Timer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace x86Emitter;
using namespace R5900;

namespace Zdxsv
{
	namespace
	{
		std::vector<u32> s_probe_pcs = [] {
			std::vector<u32> v;
			if (const char* e = TestEnv("ZDXSV_EE_PROBE"))
				for (const char* p = e; *p;)
				{
					char* end;
					v.push_back(static_cast<u32>(std::strtoul(p, &end, 16)));
					p = (*end == ',') ? end + 1 : end;
					if (end == p && *p) break;
				}
			return v;
		}();

		std::vector<std::pair<u32, u32>> s_watch = [] {
			std::vector<std::pair<u32, u32>> v;
			if (const char* e = TestEnv("ZDXSV_EE_WATCH"))
				for (const char* p = e; *p;)
				{
					char* end;
					const u32 a = static_cast<u32>(std::strtoul(p, &end, 16));
					const u32 n = *end == ':' ? static_cast<u32>(std::strtoul(end + 1, &end, 16)) : 4;
					v.emplace_back(a & 0x1fffffff, n);
					if (*end != ',')
						break;
					p = end + 1;
				}
			return v;
		}();

		void ProbeHit()
		{
			static FILE* f = [] {
				const char* o = TestEnv("ZDXSV_EE_PROBE_OUT");
				std::string path = std::string(o ? o : "eeprobe") + "-" + std::to_string(Common::Timer::GetCurrentValue() % 100000) + ".txt";
				return std::fopen(path.c_str(), "w");
			}();
			// ZDXSV_EE_PROBE_MEM=sp: 128 bytes from sp (callers' saved ra) instead of 48 at a fixed address.
			static const char* mem_env = TestEnv("ZDXSV_EE_PROBE_MEM");
			static const bool mem_sp = mem_env && std::strcmp(mem_env, "sp") == 0;
			static const u32 mem = mem_env && !mem_sp ? static_cast<u32>(std::strtoul(mem_env, nullptr, 16)) : 0xc22c98u;
			if (!f)
				return;
			const auto& r = cpuRegs.GPR.n;
			std::fprintf(f, "%u %08x a0=%x a1=%x a2=%x a3=%x v0=%x ra=%08x m=", g_FrameCount, cpuRegs.pc,
				r.a0.UL[0], r.a1.UL[0], r.a2.UL[0], r.a3.UL[0], r.v0.UL[0], r.ra.UL[0]);
			const u32 at = mem_sp ? r.sp.UL[0] : mem;
			const u8* p = eeMem->Main + (at & (Ps2MemSize::MainRam - 1));
			for (int i = 0; i < (mem_sp ? 128 : 48); i++)
				std::fprintf(f, "%02x", p[i]);
			// g = GGPO frame (NET_TRACE H / PW dump numbering), rb = rerun by a rollback.
			std::fprintf(f, " g=%d rb=%d s0=%x s1=%x sp=%x\n", ProbeFrame(), g_ggpo_in_rollback ? 1 : 0,
				r.s0.UL[0], r.s1.UL[0], r.sp.UL[0]);
			std::fflush(f); // the rig kills pcsx2: rare hits must not stay buffered
		}

		void WatchHit(u32 addr, u32 op)
		{
			static FILE* f = [] {
				const char* o = TestEnv("ZDXSV_EE_PROBE_OUT");
				std::string path = std::string(o ? o : "eeprobe") + "-w" + std::to_string(Common::Timer::GetCurrentValue() % 100000) + ".txt";
				return std::fopen(path.c_str(), "w");
			}();
			if (!f)
				return;
			const auto& r = cpuRegs.GPR.n;
			const GPR_reg& rt = cpuRegs.GPR.r[(op >> 16) & 0x1f];
			std::fprintf(f, "%u %08x addr=%x op=%08x rt=%08x%08x ra=%08x s0=%x s1=%x a0=%x g=%d rb=%d st=", g_FrameCount, cpuRegs.pc, addr, op,
				rt.UL[1], rt.UL[0], r.ra.UL[0], r.s0.UL[0], r.s1.UL[0], r.a0.UL[0], ProbeFrame(), g_ggpo_in_rollback ? 1 : 0);
			const u8* p = eeMem->Main + (r.sp.UL[0] & (Ps2MemSize::MainRam - 1) & ~3u);
			for (int i = 0; i < 128; i += 4)
				std::fprintf(f, "%08x ", *reinterpret_cast<const u32*>(p + i));
			std::fputc('\n', f);
			std::fflush(f);
		}

		void EmitWatchOp(u32 op)
		{
			const OPCODE& opcode = GetInstruction(op);
			if (!(opcode.flags & IS_STORE))
				return;
			static constexpr u32 sizes[8] = {0, 1, 2, 4, 8, 16, 0, 0};
			const u32 size = sizes[opcode.flags & MEMTYPE_MASK];
			if (!size)
				return;
			iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
			_eeMoveGPRtoR(ecx, (op >> 21) & 0x1F, false);
			if (static_cast<s16>(op) != 0)
				xADD(ecx, static_cast<s16>(op));
			if (size == 16)
				xAND(ecx, ~0x0F);
			xAND(ecx, 0x1fffffff);
			for (const auto& [start, len] : s_watch)
			{
				// hit: addr < start + len && start < addr + size (unsigned)
				xCMP(ecx, start + len);
				xForwardJAE32 skip1;
				xCMP(ecx, start - size + 1);
				xForwardJB32 skip2;
				xMOV(edx, op);
				xFastCall((void*)WatchHit, ecx, edx);
				skip1.SetTarget();
				skip2.SetTarget();
			}
		}
	} // namespace

	bool g_ee_probe = !s_probe_pcs.empty() || !s_watch.empty();

	void RecEmitProbes(u32 pc)
	{
		if (std::find(s_probe_pcs.begin(), s_probe_pcs.end(), pc) != s_probe_pcs.end())
		{
			iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
			xFastCall((void*)ProbeHit);
		}
		if (s_watch.empty())
			return;
		const u32 op = memRead32(pc);
		EmitWatchOp(op);
		// a branch's delay slot is compiled with the branch, not through recompileNextInstruction(false)
		if (GetInstruction(op).flags & IS_BRANCH)
			EmitWatchOp(memRead32(pc + 4));
	}
} // namespace Zdxsv
