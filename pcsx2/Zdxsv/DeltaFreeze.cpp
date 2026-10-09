// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/DeltaFreeze.h"

#include "COP0.h"
#include "Cache.h"
#include "Counters.h"
#include "DEV9/DEV9.h"
#include "MTVU.h"
#include "R3000A.h"
#include "SIO/Pad/Pad.h"
#include "SPU2/spu2.h"
#include "SaveState.h"
#include "StateWrapper.h"
#include "VUmicro.h"
#include "Zdxsv/SaveStateHooks.h"

#include "common/Error.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstring>

using namespace R5900;

// The state minus EE RAM (tracked per page by Zdxsv/DeltaState.cpp) and the GS thread state (an
// output device, not rolled back). In memory, no zip. Loaded on the CPU thread at the point
// it was saved (vsync), like a hotkey load.

// Code memory: on load only the 4 KB chunks that differ are written and their blocks cleared.
template <typename ClearFn>
static void DeltaFreezeCode(SaveStateBase& s, u8* mem, u32 size, ClearFn clear)
{
	if (s.IsSaving())
	{
		s.FreezeMem(mem, size);
		return;
	}

	s.PrepBlock(size);
	if (!s.IsOkay())
		return;

	const u8* src = s.GetBlockPtr();
	for (u32 off = 0; off < size; off += 4096)
	{
		const u32 n = std::min<u32>(4096, size - off);
		if (std::memcmp(mem + off, src + off, n) != 0)
		{
			std::memcpy(mem + off, src + off, n);
			clear(off, n);
		}
	}
	s.CommitBlock(size);
}

static bool DeltaFreezeWrapper(SaveStateBase& s, bool (*do_state_func)(StateWrapper&))
{
	if (s.IsSaving())
	{
		StateWrapper::VectorMemoryStream stream(16 * 1024);
		StateWrapper sw(&stream, StateWrapper::Mode::Write, g_SaveVersion);
		if (!do_state_func(sw))
			return false;
		u32 size = static_cast<u32>(stream.GetPosition());
		s.Freeze(size);
		s.FreezeMem(const_cast<u8*>(stream.GetBuffer().data()), size);
	}
	else
	{
		u32 size = 0;
		s.Freeze(size);
		s.PrepBlock(size);
		if (!s.IsOkay())
			return false;
		StateWrapper::ReadOnlyMemoryStream stream(s.GetBlockPtr(), size);
		StateWrapper sw(&stream, StateWrapper::Mode::Read, g_SaveVersion);
		if (!do_state_func(sw))
			return false;
		s.CommitBlock(size);
	}
	return s.IsOkay();
}

static tlbs s_tlb_backup[std::size(tlb)];

// Section starts of the last delta save, for SaveState_DeltaDescribe.
static std::vector<std::pair<const char*, size_t>> s_delta_marks;

bool g_SaveStateDeltaLoad = false;

static bool s_delta_saving = false;
static std::vector<std::pair<size_t, size_t>> s_delta_scratch;

const std::vector<std::pair<size_t, size_t>>& SaveState_DeltaScratch()
{
	return s_delta_scratch;
}

void SaveState_DeltaMarkScratch(size_t pos, size_t size)
{
	if (s_delta_saving)
		s_delta_scratch.emplace_back(pos, size);
}

// Wall ms per section summed over delta saves [0] and loads [1], in section order.
static std::vector<std::pair<const char*, double>> s_delta_ms[2];
static int s_delta_calls[2] = {};

std::string SaveState_DeltaTimes()
{
	std::string out;
	for (int io = 0; io < 2; io++)
	{
		out += io ? " | load ms:" : "save ms:";
		for (const auto& [name, ms] : s_delta_ms[io])
			out += fmt::format(" {} {:.3f}", name, ms / std::max(s_delta_calls[io], 1));
	}
	return out;
}

static bool DeltaFreezeAll(SaveStateBase& s, Error* error)
{
	const int io = s.IsSaving() ? 0 : 1;
	s_delta_calls[io]++;
	size_t section = 0;
	const char* current = "internals";
	Common::Timer::Value t = Common::Timer::GetCurrentValue();
	auto mark = [&](const char* name) {
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (section >= s_delta_ms[io].size())
			s_delta_ms[io].emplace_back(current, 0.0);
		s_delta_ms[io][section++].second += Common::Timer::ConvertValueToMilliseconds(now - t);
		t = now;
		current = name;
		if (s.IsSaving() && name)
			s_delta_marks.emplace_back(name, s.GetCurrentPos());
	};
	if (s.IsSaving())
		s_delta_marks.clear();

	if (!s.FreezeInternals(error))
		return false;

	mark("iopMem");
	DeltaFreezeCode(s, iopMem->Main, Ps2MemSize::ExposedIopRam, [](u32 addr, u32 n) { psxCpu->Clear(addr, n / 4); });
	mark("eeHw");
	s.FreezeMem(eeHw, sizeof(eeHw));
	mark("iopHw");
	s.FreezeMem(iopHw, sizeof(iopHw));
	mark("Scratch");
	s.FreezeMem(eeMem->Scratch, sizeof(eeMem->Scratch));
	mark("VU0Mem");
	s.FreezeMem(vuRegs[0].Mem, VU0_MEMSIZE);
	mark("VU1Mem");
	s.FreezeMem(vuRegs[1].Mem, VU1_MEMSIZE);
	mark("VU0Micro");
	DeltaFreezeCode(s, vuRegs[0].Micro, VU0_PROGSIZE, [](u32 addr, u32 n) { CpuVU0->Clear(addr, n); });
	mark("VU1Micro");
	DeltaFreezeCode(s, vuRegs[1].Micro, VU1_PROGSIZE, [](u32 addr, u32 n) { CpuVU1->Clear(addr, n); });

	mark("SPU2");
	freezeData fP = {};
	SPU2freeze(FreezeAction::Size, &fP);
	s.PrepBlock(fP.size);
	if (!s.IsOkay())
		return false;
	fP.data = s.GetBlockPtr();
	if (SPU2freeze(s.IsSaving() ? FreezeAction::Save : FreezeAction::Load, &fP) != 0)
		return false;
	s.CommitBlock(fP.size);

	mark("SPU2Voices");
	const u32 voices_size = static_cast<u32>(SPU2DeltaVoicesSize());
	s.PrepBlock(voices_size);
	if (!s.IsOkay())
		return false;
	if (s.IsSaving())
		SPU2DeltaSaveVoices(s.GetBlockPtr());
	else
		SPU2DeltaLoadVoices(s.GetBlockPtr());
	s.CommitBlock(voices_size);

	// Not in full states: set when the IOP must run at the next EE event test (pending IOP
	// interrupt, IOP counter). Left stale, the IOP takes an interrupt at another point.
	mark("iopEventAction");
	s.Freeze(iopEventAction);

	// The IOP's SMAP driver state is in IOP RAM: without DEV9, a rollback across an SMAP TX
	// leaves DEV9's TX descriptor index one ahead of the driver's and the IOP hangs after the battle.
	// Frames received since the loaded save are received again (Zdxsv::DeltaStateLoad).
	mark("DEV9");
	if (Zdxsv::SaveStateDev9Enabled() && !DeltaFreezeWrapper(s, &DEV9DeltaDoState))
		return false;

	mark("Pad");
	const bool ok = DeltaFreezeWrapper(s, &Pad::Freeze);
	mark(nullptr);
	return ok;
}

bool SaveState_DeltaSave(std::vector<u8>& buffer)
{
	memSavingState s(buffer);
	Error error;
	s_delta_scratch.clear();
	s_delta_saving = true;
	const bool saved = DeltaFreezeAll(s, &error);
	s_delta_saving = false;
	if (!saved)
	{
		Console.Error(fmt::format("(ZdxsvDelta) save failed: {}", error.GetDescription()));
		return false;
	}
	buffer.resize(s.GetCurrentPos());
	return true;
}

bool SaveState_DeltaLoad(const std::vector<u8>& buffer)
{
	if (THREAD_VU1)
		vu1Thread.WaitVU();
	std::memcpy(s_tlb_backup, tlb, sizeof(s_tlb_backup));

	memLoadingState s(buffer);
	Error error;
	g_SaveStateDeltaLoad = true;
	const bool loaded = DeltaFreezeAll(s, &error);
	g_SaveStateDeltaLoad = false;
	if (!loaded)
	{
		Console.Error(fmt::format("(ZdxsvDelta) load failed: {}", error.GetDescription()));
		return false;
	}

	resetCache();
	for (int i = 0; i < 48; i++)
	{
		if (std::memcmp(&s_tlb_backup[i], &tlb[i], sizeof(tlbs)) != 0)
		{
			UnmapTLB(s_tlb_backup[i], i);
			MapTLB(tlb[i], i);
		}
	}
	UpdateVSyncRate(false);
	return true;
}

// Names the FreezeInternals register block an offset of a delta state falls in (synctest reports).
std::string SaveState_DeltaDescribe(const std::vector<u8>& buffer, size_t offset)
{
	static const char tag[] = "cpuRegs";
	auto it = std::search(buffer.begin(), buffer.end(), tag, tag + sizeof(tag));
	if (it == buffer.end())
		return "?";
	size_t pos = static_cast<size_t>(it - buffer.begin()) + 32;
	const struct { const char* name; size_t size; } blocks[] = {
		{"cpuRegs", sizeof(cpuRegs)}, {"psxRegs", sizeof(psxRegs)}, {"fpuRegs", sizeof(fpuRegs)},
		{"tlb", sizeof(tlb)}, {"cachedTlbs", sizeof(cachedTlbs)},
	};
	if (offset < pos)
		return fmt::format("before cpuRegs (at {})", pos);
	for (const auto& b : blocks)
	{
		if (offset < pos + b.size)
		{
			const size_t o = offset - pos;
			std::string field;
			auto arr = [&](const char* name, size_t start, size_t size, size_t elem) {
				if (o >= start && o < start + size)
					field = fmt::format(" {}[{}]", name, (o - start) / elem);
			};
			if (b.name[0] == 'c' && b.name[1] == 'p')
			{
				arr("eCycle", offsetof(cpuRegisters, eCycle), sizeof(cpuRegs.eCycle), 4);
				arr("sCycle", offsetof(cpuRegisters, sCycle), sizeof(cpuRegs.sCycle), 8);
				arr("CP0", offsetof(cpuRegisters, CP0), sizeof(cpuRegs.CP0), 4);
				arr("cycle", offsetof(cpuRegisters, cycle), 8, 8);
				arr("nextEventCycle", offsetof(cpuRegisters, nextEventCycle), 8, 8);
			}
			else if (b.name[0] == 'p')
			{
				arr("eCycle", offsetof(psxRegisters, eCycle), sizeof(psxRegs.eCycle), 4);
				arr("sCycle", offsetof(psxRegisters, sCycle), sizeof(psxRegs.sCycle), 8);
				arr("cycle", offsetof(psxRegisters, cycle), 8, 8);
				arr("iopNextEventCycle", offsetof(psxRegisters, iopNextEventCycle), 8, 8);
			}
			return fmt::format("{}+{}{} (block at {})", b.name, o, field, pos);
		}
		pos += b.size;
	}
	// Nearest preceding FreezeTag or delta section.
	static const char* const tags[] = {"Cycles", "EE-Subsystems", "IOP-Subsystems", "cdvd", "cdrom", "GIFdma",
		"Gif Unit", "hostHandles", "iopCounters", "IPU", "IPUdma", "MTVU", "deci2", "SIFdma", "SPRdma", "VIF0dma",
		"VIF1dma", "vuMicroRegs"};
	const char* best = "cachedTlbs end";
	size_t best_pos = pos;
	for (const char* t : tags)
	{
		const size_t len = std::strlen(t) + 1;
		for (auto at = buffer.begin(); (at = std::search(at, buffer.end(), t, t + len)) != buffer.end(); ++at)
		{
			const size_t p = static_cast<size_t>(at - buffer.begin()) + 32;
			if (p <= offset && p > best_pos && (s_delta_marks.empty() || p < s_delta_marks[0].second))
			{
				best = t;
				best_pos = p;
			}
		}
	}
	for (const auto& [name, p] : s_delta_marks)
	{
		if (p <= offset && p > best_pos)
		{
			best = name;
			best_pos = p;
		}
	}
	return fmt::format("{}+{}", best, offset - best_pos);
}
