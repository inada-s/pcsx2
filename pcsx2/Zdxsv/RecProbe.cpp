// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// EE recompiler tracing (ZDXSV_EE_PROBE, ZDXSV_EE_WATCH) emitted through RecEmitHooks, and the
// rerun-frame sampling profiler (ZDXSV_EE_PROFILE); see docs/zdxsv/options.md.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

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
#include <array>
#include <chrono>
#include <map>
#include <thread>
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

#ifdef _WIN32
	namespace
	{
		// "module!symbol" of a host address (DbgHelp, loaded on first use; the exe's PDB sits next to it).
		std::string HostSymbol(uptr ip)
		{
			using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
			using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
			static SymFromAddrFn from_addr = [] {
				const HMODULE m = LoadLibraryW(L"dbghelp.dll");
				const auto init = m ? reinterpret_cast<SymInitializeFn>(GetProcAddress(m, "SymInitialize")) : nullptr;
				return init && init(GetCurrentProcess(), nullptr, TRUE) ? reinterpret_cast<SymFromAddrFn>(GetProcAddress(m, "SymFromAddr")) : nullptr;
			}();
			char module[MAX_PATH] = "?";
			HMODULE hm;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(ip), &hm))
				GetModuleFileNameA(hm, module, sizeof(module));
			const char* base = std::strrchr(module, '\\');
			std::string s = base ? base + 1 : module;
			alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
			auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen = 255;
			DWORD64 disp;
			return s + "!" + (from_addr && from_addr(GetCurrentProcess(), ip, &disp, sym) ? sym->Name : "?");
		}

		// "file:line" of a host address, "?" without line info. Call after HostSymbol (it initializes DbgHelp).
		std::string HostLine(uptr ip)
		{
			using SymGetLineFromAddr64Fn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
			static const auto get_line = [] {
				const HMODULE m = GetModuleHandleW(L"dbghelp.dll");
				return m ? reinterpret_cast<SymGetLineFromAddr64Fn>(GetProcAddress(m, "SymGetLineFromAddr64")) : nullptr;
			}();
			IMAGEHLP_LINE64 line = {};
			line.SizeOfStruct = sizeof(line);
			DWORD disp;
			if (!get_line || !get_line(GetCurrentProcess(), ip, &disp, &line) || !line.FileName)
				return "?";
			const char* base = std::strrchr(line.FileName, '\\');
			return std::string(base ? base + 1 : line.FileName) + ":" + std::to_string(line.LineNumber);
		}

	} // namespace

	// ZDXSV_EE_PROFILE: started by the first rerun frame, on the CPU thread.
	void EeProfileOnRerun()
	{
		static bool started = false;
		if (std::exchange(started, true))
			return;
		const char* out = TestEnv("ZDXSV_EE_PROFILE");
		if (!out || !*out)
			return;
		const HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, GetCurrentThreadId());
		if (!th)
			return;
		std::thread([th, path = std::string(out) + "-" + std::to_string(GetCurrentProcessId()) + ".txt"] {
			auto in = [](uptr ip, const u8* a, const u8* b) { return ip >= reinterpret_cast<uptr>(a) && ip < reinterpret_cast<uptr>(b); };
			std::map<std::string, u32> hist;
			std::map<uptr, u32> native; // host pc outside recompiled code
			std::map<std::pair<u32, uptr>, u32> native_at; // (EE pc, host pc) of the same samples
			std::map<std::array<uptr, 3>, u32> chains; // host pc and its two callers
			u32 n = 0;
			for (;;)
			{
				std::this_thread::sleep_for(std::chrono::microseconds(200));
				if (!g_ggpo_in_rollback || SuspendThread(th) == static_cast<DWORD>(-1))
					continue;
				// No allocation while the thread is suspended: it may hold the heap lock.
				CONTEXT c = {};
				c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
				const bool ok = GetThreadContext(th, &c) && g_ggpo_in_rollback;
				const uptr ip = ok ? c.Rip : 0;
				// Native callers by unwind data, up to recompiled code (no unwind data there).
				std::array<uptr, 3> chain = {ip, 0, 0};
				CONTEXT u = c;
				for (size_t i = 1; ok && i < chain.size(); i++)
				{
					DWORD64 image;
					const PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(u.Rip, &image, nullptr);
					if (!fe && i == 1)
					{
						// Leaf function (no unwind data): the return address is at rsp.
						u.Rip = *reinterpret_cast<const DWORD64*>(u.Rsp);
						u.Rsp += 8;
						chain[i] = u.Rip;
						continue;
					}
					if (!fe)
						break;
					void* handler;
					DWORD64 frame;
					RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, u.Rip, fe, &u, &handler, &frame, nullptr);
					chain[i] = u.Rip;
				}
				const u32 pc = cpuRegs.pc;
				const u32 sp = cpuRegs.GPR.n.sp.UL[0];
				const bool ee = ok && in(ip, SysMemory::GetEERec(), SysMemory::GetEERecEnd());
				const u32 block = ee ? recEeBlockPc(ip) : 0;
				// Return addresses on the EE stack: words in sp..sp+8 KB whose call site (word - 8) is a
				// jal/jalr; "ra <addr>" counts each once per sample = inclusive time of that call.
				u32 ras[64];
				u32 nras = 0;
				for (u32 a = sp & ~3u; ok && a < (sp & ~3u) + 0x2000 && a + 4 <= Ps2MemSize::MainRam && nras < std::size(ras); a += 4)
				{
					const u32 v = *reinterpret_cast<const u32*>(eeMem->Main + a);
					if (v < 0x100008 || v >= Ps2MemSize::MainRam || (v & 3))
						continue;
					const u32 op = *reinterpret_cast<const u32*>(eeMem->Main + v - 8);
					if ((op >> 26) == 3 || ((op >> 26) == 0 && (op & 0x3f) == 9))
						ras[nras++] = v;
				}
				ResumeThread(th);
				if (!ok)
					continue;
				char key[64];
				if (ee)
					std::snprintf(key, sizeof(key), "ee %08x", block);
				else if (in(ip, SysMemory::GetVU1Rec(), SysMemory::GetVU1RecEnd()))
					std::snprintf(key, sizeof(key), "vu1");
				else if (in(ip, SysMemory::GetVU0Rec(), SysMemory::GetVU0RecEnd()))
					std::snprintf(key, sizeof(key), "vu0");
				else if (in(ip, SysMemory::GetIOPRec(), SysMemory::GetIOPRecEnd()))
					std::snprintf(key, sizeof(key), "iop");
				else if (in(ip, SysMemory::GetVIFUnpackRec(), SysMemory::GetVIFUnpackRecEnd()))
					std::snprintf(key, sizeof(key), "vifunpack");
				else
				{
					native[ip]++;
					native_at[{pc, ip}]++;
					chains[chain]++;
					std::snprintf(key, sizeof(key), "native pc %08x", pc);
				}
				hist[key]++;
				std::snprintf(key, sizeof(key), "sp %05x %s", sp >> 12, ee ? "ee" : "other");
				hist[key]++;
				std::sort(ras, ras + nras);
				for (u32 i = 0; i < nras; i++)
				{
					if (i > 0 && ras[i] == ras[i - 1])
						continue;
					std::snprintf(key, sizeof(key), "ra %08x", ras[i]);
					hist[key]++;
				}
				if (++n % 2000 != 0)
					continue;
				std::map<std::string, u32> fns;
				for (const auto& [a, c] : native)
				{
					fns["fn " + HostSymbol(a)] += c;
					fns["ln " + HostLine(a)] += c;
				}
				for (const auto& [k, c] : native_at)
				{
					char at[24];
					std::snprintf(at, sizeof(at), "at %08x fn ", k.first);
					fns[at + HostSymbol(k.second)] += c;
				}
				for (const auto& [k, c] : chains)
				{
					std::string s = "ch " + HostSymbol(k[0]);
					for (size_t i = 1; i < k.size() && k[i]; i++)
						s += " < " + HostSymbol(k[i]);
					fns[s] += c;
				}
				std::vector<std::pair<std::string, u32>> v(hist.begin(), hist.end());
				v.insert(v.end(), fns.begin(), fns.end());
				std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
				if (FILE* f = std::fopen(path.c_str(), "w"))
				{
					std::fprintf(f, "samples %u\n", n);
					for (const auto& [k, c] : v)
						std::fprintf(f, "%u %s\n", c, k.c_str());
					std::fclose(f);
				}
			}
		}).detach();
	}
#else
	void EeProfileOnRerun() {}
#endif
} // namespace Zdxsv
