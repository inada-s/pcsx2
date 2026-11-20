// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// ZDXSV_DELTA_TEST (docs/zdxsv/options.md): rolls back at a fixed interval and compares each rerun with the one
// before. Only reruns 2+ are judged: the first rerun can differ from the first run because the recompilers end
// blocks where the next PC is already compiled, so events land at other cycles.

#include "Zdxsv/DeltaState.h"
#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/DeltaFreeze.h"
#include "Zdxsv/SaveStateHooks.h"
#include "Zdxsv/TestOptions.h"

#include "IopMem.h"
#include "Memory.h"
#include "SaveState.h"
#include "vtlb.h"
#include "SPU2/spu2.h"

#include "common/Console.h"
#include "common/HostSys.h"
#include "common/ScopedGuard.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include "xxhash.h"
#include "fmt/format.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Zdxsv
{
	bool g_delta_state_test_enabled = Zdxsv::TestEnv("ZDXSV_DELTA_TEST") != nullptr;

	namespace
	{
		// vtlb watches and reports host pages (__pagesize: 16 KiB on arm64, 4 KiB on x86).
		constexpr u32 PAGE_BYTES = __pagesize;

		struct SavedPage
		{
			u32 page;
			std::unique_ptr<u8[]> data;
		};
		using Delta = std::vector<SavedPage>;

		struct DeltaStore
		{
			std::map<int, std::vector<u8>> states;
			// deltas[f]: old data of the pages written after the save of f, until the next save.
			std::map<int, Delta> deltas;
			// Pages written since the last save or load.
			Delta open;
			std::vector<std::unique_ptr<u8[]>> page_pool;
			std::vector<std::vector<u8>> buffer_pool;
			bool armed = false;
			int save_calls = 0;
		};
		DeltaStore s_store;
		bool s_break_ee = false, s_break_host = false;

		// The Times sums are report-only: the clock is read only with ZDXSV_RERUN_SPLIT.
		class SplitTimer
		{
		public:
			double GetTimeMilliseconds() const
			{
				return g_rerun_split_time ? Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - m_start) : 0.0;
			}
			void Reset() { m_start = g_rerun_split_time ? Common::Timer::GetCurrentValue() : 0; }

		private:
			Common::Timer::Value m_start = g_rerun_split_time ? Common::Timer::GetCurrentValue() : 0;
		};

		struct Times
		{
			int saves = 0, loads = 0;
			double watch_ms = 0, state_ms = 0, total_ms = 0;
			double hot_pages = 0;
			u64 faults = 0; // write faults (each one unprotect, later one protect)
			double lrestore_ms = 0, lwatch_ms = 0, lstate_ms = 0, lpages = 0;
		};
		Times s_times;

		// Hot pages: written in a save interval. They stay writable and are copied at
		// every save and load instead, which saves the protect calls (one per fastmem alias, ~17 us a
		// page) and the fault. Unchanged for COLD_AFTER intervals: watched again.
		constexpr u8 COLD_AFTER = 32;
		// Each fastmem alias is its own view, so cooling pages cannot share a protect call: at most
		// COOL_MAX cool per save, the rest stay hot to the next one, so a burst does not stall a frame.
		constexpr u32 COOL_MAX = 64;

		// One page index space: EE RAM, IOP RAM, SPU2 RAM. vtlb watches the EE pages; the IOP and
		// SPU2 pages (one host view each, written on the CPU thread only) are watched here.
		constexpr u32 EE_PAGES = Ps2MemSize::TotalRam / PAGE_BYTES;
		constexpr u32 IOP_FIRST = EE_PAGES;
		constexpr u32 SPU_FIRST = IOP_FIRST + Ps2MemSize::TotalIopRam / PAGE_BYTES;
		constexpr u32 SPU_BYTES = 0x200000;
		constexpr u32 ALL_PAGES = SPU_FIRST + SPU_BYTES / PAGE_BYTES;

		u8* PagePtr(u32 page)
		{
			if (page < IOP_FIRST)
				return &eeMem->Main[page * PAGE_BYTES];
			if (page < SPU_FIRST)
				return &iopMem->Main[(page - IOP_FIRST) * PAGE_BYTES];
			return SPU2DeltaMem() + (page - SPU_FIRST) * PAGE_BYTES;
		}

		u32 IopPages()
		{
			return Ps2MemSize::ExposedIopRam / PAGE_BYTES;
		}

		struct PageHeat
		{
			bool is_hot[ALL_PAGES] = {};
			u8 run[ALL_PAGES] = {}; // hot pages: save intervals in a row without a change
			std::vector<u32> hot;
			u32 restored[ALL_PAGES] = {}; // load_calls of the load that restored the page
			u32 reused[ALL_PAGES] = {}; // load_calls of the load whose s_store.open took the page's restored buffer
			int load_calls = 0;
			bool host_watched[ALL_PAGES - IOP_FIRST] = {};
		};
		PageHeat s_heat;

		std::unique_ptr<u8[]> TakePage()
		{
			if (s_store.page_pool.empty())
				return std::make_unique<u8[]>(PAGE_BYTES);
			std::unique_ptr<u8[]> p = std::move(s_store.page_pool.back());
			s_store.page_pool.pop_back();
			return p;
		}

		void ReleaseDelta(Delta& delta)
		{
			for (SavedPage& p : delta)
			{
				if (p.data)
					s_store.page_pool.push_back(std::move(p.data));
			}
			delta.clear();
		}

		// Fault handler (vtlb): the page still holds the data of the last save.
		void OnWrite(u32 page)
		{
			std::unique_ptr<u8[]> data = TakePage();
			std::memcpy(data.get(), PagePtr(page), PAGE_BYTES);
			s_store.open.push_back({page, std::move(data)});
			s_times.faults++;
		}

		void HostProtect(u32 page, bool read_only)
		{
			HostSys::MemProtect(PagePtr(page), PAGE_BYTES, read_only ? PageAccess_ReadOnly() : PageAccess_ReadWrite());
		}

		bool OnHostFault(uptr addr)
		{
			u32 page;
			const uptr iop = reinterpret_cast<uptr>(iopMem->Main), spu = reinterpret_cast<uptr>(SPU2DeltaMem());
			if (addr - iop < Ps2MemSize::ExposedIopRam)
				page = IOP_FIRST + static_cast<u32>((addr - iop) / PAGE_BYTES);
			else if (addr - spu < SPU_BYTES)
				page = SPU_FIRST + static_cast<u32>((addr - spu) / PAGE_BYTES);
			else
				return false;
			if (!s_heat.host_watched[page - IOP_FIRST])
				return false;
			OnWrite(page);
			s_heat.host_watched[page - IOP_FIRST] = false;
			HostProtect(page, false);
			return true;
		}

		bool HostRange(u32 page)
		{
			return page >= IOP_FIRST;
		}

		void WatchAll()
		{
			mmap_DeltaWatchAll();
			std::fill_n(&s_heat.host_watched[0], IopPages(), true);
			std::fill_n(&s_heat.host_watched[SPU_FIRST - IOP_FIRST], ALL_PAGES - SPU_FIRST, true);
			HostSys::MemProtect(iopMem->Main, Ps2MemSize::ExposedIopRam, PageAccess_ReadOnly());
			HostSys::MemProtect(SPU2DeltaMem(), SPU_BYTES, PageAccess_ReadOnly());
		}

		// Sorted pages: one protect per run of adjacent IOP or SPU2 pages.
		void WatchPages(std::vector<u32>& pages)
		{
			std::sort(pages.begin(), pages.end());
			const auto host = std::find_if(pages.begin(), pages.end(), HostRange);
			mmap_DeltaWatchPages(std::vector<u32>(pages.begin(), host));
			for (auto i = host; i != pages.end();)
			{
				auto j = i;
				do
				{
					s_heat.host_watched[*j - IOP_FIRST] = true;
					++j;
				} while (j != pages.end() && *j == *(j - 1) + 1 && *j != SPU_FIRST);
				HostSys::MemProtect(PagePtr(*i), static_cast<size_t>(j - i) * PAGE_BYTES, PageAccess_ReadOnly());
				i = j;
			}
		}

		void UnwatchHost()
		{
			for (u32 page = IOP_FIRST; page < ALL_PAGES; page++)
			{
				if (std::exchange(s_heat.host_watched[page - IOP_FIRST], false))
					HostProtect(page, false);
			}
		}

		void RestorePage(u32 page, const u8* data)
		{
			if (page < IOP_FIRST)
			{
				mmap_DeltaRestorePage(page, data);
				return;
			}
			if (std::exchange(s_heat.host_watched[page - IOP_FIRST], false))
				HostProtect(page, false);
			if (page < SPU_FIRST)
				DeltaFreezeRestoreIopPage((page - IOP_FIRST) * PAGE_BYTES, data, PAGE_BYTES);
			else
				SPU2DeltaRestoreMem((page - SPU_FIRST) * PAGE_BYTES, data, PAGE_BYTES);
		}

		// A restored hot page's buffer holds the page as of the load: it moves to reuse, the next
		// s_store.open, instead of a copy of the page (~365 hot pages a load).
		void RestoreDelta(Delta& delta, std::vector<u32>& touched, Delta& reuse)
		{
			for (SavedPage& p : delta)
			{
				if (s_heat.restored[p.page] == static_cast<u32>(s_heat.load_calls))
					continue;
				s_heat.restored[p.page] = s_heat.load_calls;
				touched.push_back(p.page);
				if (p.page < IOP_FIRST ? s_break_ee : s_break_host)
					continue;
				RestorePage(p.page, p.data.get());
				if (s_heat.is_hot[p.page])
				{
					s_heat.reused[p.page] = s_heat.load_calls;
					reuse.push_back(std::move(p));
				}
			}
			ReleaseDelta(delta);
		}

		void SnapshotHot(const std::vector<u32>& pages)
		{
			for (u32 page : pages)
			{
				std::unique_ptr<u8[]> data = TakePage();
				std::memcpy(data.get(), PagePtr(page), PAGE_BYTES);
				s_store.open.push_back({page, std::move(data)});
			}
		}

		// At a save: updates the hot set from s_store.open (the interval that ends) and returns the
		// pages to watch again. A hot page unchanged over the interval needs no delta entry (a
		// load takes it from a later one): its buffer moves to next, the new s_store.open, as its
		// snapshot. snapshot gets the hot pages that need a copy.
		std::vector<u32> UpdateHot(Delta& next, std::vector<u32>& snapshot)
		{
			std::vector<u32> watch;
			watch.reserve(s_store.open.size());
			u32 cooled = 0;
			for (SavedPage& p : s_store.open)
			{
				const u32 page = p.page;
				if (s_heat.is_hot[page])
				{
					if (std::memcmp(p.data.get(), PagePtr(page), PAGE_BYTES) != 0)
					{
						s_heat.run[page] = 0;
						snapshot.push_back(page);
					}
					else if (s_heat.run[page] + 1 < COLD_AFTER || cooled == COOL_MAX)
					{
						s_heat.run[page] = static_cast<u8>(std::min<int>(s_heat.run[page] + 1, COLD_AFTER));
						next.push_back(std::move(p));
					}
					else
					{
						cooled++;
						s_heat.is_hot[page] = false;
						s_heat.run[page] = 0;
						s_heat.hot.erase(std::find(s_heat.hot.begin(), s_heat.hot.end(), page));
						watch.push_back(page);
						s_store.page_pool.push_back(std::move(p.data));
					}
					continue;
				}
				s_heat.is_hot[page] = true;
				s_heat.run[page] = 0;
				s_heat.hot.push_back(page);
				snapshot.push_back(page);
			}
			s_store.open.erase(std::remove_if(s_store.open.begin(), s_store.open.end(), [](const SavedPage& p) { return !p.data; }), s_store.open.end());
			return watch;
		}

	} // namespace

	std::string DeltaStateTimes()
	{
		const double n = std::max(s_times.saves, 1), l = std::max(s_times.loads, 1);
		return fmt::format("Save ms: watch {:.3f} state {:.3f} total {:.3f} hot pages {:.1f} faults {:.1f}", s_times.watch_ms / n, s_times.state_ms / n,
			s_times.total_ms / n, s_times.hot_pages / n, s_times.faults / n) +
			fmt::format(" | Load ms: pages {:.3f} watch {:.3f} state {:.3f} pages restored {:.1f}", s_times.lrestore_ms / l,
				s_times.lwatch_ms / l, s_times.lstate_ms / l, s_times.lpages / l);
	}

	void DeltaStateTimesReset()
	{
		s_times = {};
		DeltaFreezeTimesReset();
	}

	bool DeltaStateSave(int frame)
	{
		SplitTimer total;
		ScopedGuard add_total([&total]() { s_times.total_ms += total.GetTimeMilliseconds(); });
		s_store.save_calls++;
		s_times.saves++;
		if (!s_store.states.empty())
		{
			const int last = s_store.states.rbegin()->first;
			if (frame <= last)
			{
				Console.Error("ZdxsvDelta: save of frame %d after %d", frame, last);
				return false;
			}
			SplitTimer watch;
			Delta next;
			std::vector<u32> snapshot;
			std::vector<u32> pages = UpdateHot(next, snapshot);
			WatchPages(pages);
			s_store.deltas[last] = std::move(s_store.open);
			s_store.open = std::move(next);
			SnapshotHot(snapshot);
			s_times.hot_pages += s_heat.hot.size();
			s_times.watch_ms += watch.GetTimeMilliseconds();
		}
		else if (std::exchange(s_store.armed, false))
		{
			std::vector<u32> pages;
			pages.reserve(s_store.open.size());
			for (const SavedPage& p : s_store.open)
				pages.push_back(p.page);
			ReleaseDelta(s_store.open);
			DeltaFreezeSetPagedRam(true);
			WatchPages(pages);
		}
		else
		{
			// Never with DeltaStateDiscardBefore keeping the newest frame (a load of this frame would restore them).
			if (!s_store.open.empty())
				Console.Error("ZdxsvDelta: save of frame %d with no saved frame left, %zu stale open pages", frame, s_store.open.size());
			DeltaFreezeSetPagedRam(true);
			mmap_DeltaSetHook(&OnWrite);
			mmap_DeltaSetFaultHook(&OnHostFault);
			WatchAll();
		}
		std::vector<u8> buffer;
		if (!s_store.buffer_pool.empty())
		{
			buffer = std::move(s_store.buffer_pool.back());
			s_store.buffer_pool.pop_back();
		}
		SplitTimer state;
		if (!DeltaFreezeSave(buffer))
			return false;
		s_times.state_ms += state.GetTimeMilliseconds();
		s_store.states[frame] = std::move(buffer);
		return true;
	}

	bool DeltaStateLoad(int frame)
	{
		const auto state = s_store.states.find(frame);
		if (state == s_store.states.end())
		{
			Console.Error("ZdxsvDelta: load of unsaved frame %d", frame);
			return false;
		}

		// Oldest first, each page once: it gets its data from the earliest delta at or after frame.
		SplitTimer timer;
		s_heat.load_calls++;
		s_times.loads++;
		std::vector<u32> touched;
		Delta reuse;
		for (auto it = s_store.deltas.lower_bound(frame); it != s_store.deltas.end(); ++it)
			RestoreDelta(it->second, touched, reuse);
		RestoreDelta(s_store.open, touched, reuse);
		s_store.deltas.erase(s_store.deltas.lower_bound(frame), s_store.deltas.end());
		s_times.lpages += touched.size();
		s_times.lrestore_ms += timer.GetTimeMilliseconds();
		timer.Reset();
		touched.erase(std::remove_if(touched.begin(), touched.end(), [](u32 page) { return s_heat.is_hot[page]; }), touched.end());
		WatchPages(touched);
		s_store.open = std::move(reuse);
		for (u32 page : s_heat.hot)
		{
			if (s_heat.reused[page] != static_cast<u32>(s_heat.load_calls))
			{
				std::unique_ptr<u8[]> data = TakePage();
				std::memcpy(data.get(), PagePtr(page), PAGE_BYTES);
				s_store.open.push_back({page, std::move(data)});
			}
		}
		s_times.lwatch_ms += timer.GetTimeMilliseconds();

		while (s_store.states.rbegin()->first > frame)
		{
			const auto it = std::prev(s_store.states.end());
			s_store.buffer_pool.push_back(std::move(it->second));
			s_store.states.erase(it);
		}

		timer.Reset();
		const bool ok = DeltaFreezeLoad(state->second);
		s_times.lstate_ms += timer.GetTimeMilliseconds();
		return ok;
	}

	const std::vector<u8>* DeltaStateGetState(int frame)
	{
		const auto it = s_store.states.find(frame);
		return it == s_store.states.end() ? nullptr : &it->second;
	}

	u64 DeltaStateHash(const std::vector<u8>& state)
	{
		XXH3_state_t xs;
		XXH3_64bits_reset(&xs);
		size_t at = 0;
		for (const auto& [pos, size] : DeltaFreezeScratch())
		{
			if (pos < at || pos + size > state.size())
				continue;
			XXH3_64bits_update(&xs, state.data() + at, pos - at);
			at = pos + size;
		}
		XXH3_64bits_update(&xs, state.data() + at, state.size() - at);
		return XXH3_64bits_digest(&xs);
	}

	void DeltaStateHashRam(std::vector<u64>& pages)
	{
		pages.clear();
		for (u32 i = 0; i < Ps2MemSize::ExposedRam / PAGE_BYTES; i++)
			pages.push_back(XXH3_64bits(PagePtr(i), PAGE_BYTES));
		for (u32 i = IOP_FIRST; i < IOP_FIRST + IopPages(); i++)
			pages.push_back(XXH3_64bits(PagePtr(i), PAGE_BYTES));
		for (u32 i = SPU_FIRST; i < ALL_PAGES; i++)
			pages.push_back(XXH3_64bits(PagePtr(i), PAGE_BYTES));
	}

	std::string DeltaStateRamPageName(size_t i)
	{
		const size_t ee = Ps2MemSize::ExposedRam / PAGE_BYTES, iop = IopPages();
		if (i < ee)
			return fmt::format("EE 0x{:08x}", i * PAGE_BYTES);
		if (i < ee + iop)
			return fmt::format("IOP 0x{:06x}", (i - ee) * PAGE_BYTES);
		return fmt::format("SPU2 0x{:06x}", (i - ee - iop) * PAGE_BYTES);
	}

	void DeltaStateMaskScratch(std::vector<u8>& state)
	{
		for (const auto& [pos, size] : DeltaFreezeScratch())
		{
			if (pos + size <= state.size())
				std::memset(state.data() + pos, 0, size);
		}
	}

	void DeltaStateDiscardBefore(int frame)
	{
		// The newest saved frame stays: s_store.open holds the pages written since it. Without it the next
		// save starts over with s_store.open still holding that older interval, and a load of that save puts
		// those pages back to their old data (GGPO's confirmed-frame save skip; with 0 ms latency and packet loss it froze the game).
		while (s_store.states.size() > 1 && s_store.states.begin()->first < frame)
		{
			s_store.buffer_pool.push_back(std::move(s_store.states.begin()->second));
			s_store.states.erase(s_store.states.begin());
		}
		while (!s_store.deltas.empty() && s_store.deltas.begin()->first < frame)
		{
			ReleaseDelta(s_store.deltas.begin()->second);
			s_store.deltas.erase(s_store.deltas.begin());
		}
	}

	void DeltaStateArm(size_t states)
	{
		if (s_store.armed || !s_store.states.empty())
			return;
		ReleaseDelta(s_store.open);
		mmap_DeltaSetHook(&OnWrite);
		mmap_DeltaSetFaultHook(&OnHostFault);
		WatchAll();
		s_store.armed = true;
		std::vector<u8> sized;
		DeltaFreezeSetPagedRam(true);
		const bool ok = DeltaFreezeSave(sized);
		DeltaFreezeSetPagedRam(false);
		if (ok)
		{
			sized.resize(sized.capacity());
			while (s_store.buffer_pool.size() < states)
				s_store.buffer_pool.push_back(sized);
		}
	}

	void DeltaStateHostWrite(void* data, size_t size)
	{
		if (!mmap_DeltaHooked() || size == 0)
			return;
		// Each page is written once from here, so the fault handlers record and unwatch it before the read.
		const uptr begin = reinterpret_cast<uptr>(data);
		for (uptr addr = begin; addr < begin + size; addr = (addr & ~static_cast<uptr>(PAGE_BYTES - 1)) + PAGE_BYTES)
		{
			volatile u8* p = reinterpret_cast<u8*>(addr);
			*p = *p;
		}
	}

	void DeltaStateClear()
	{
		s_store.armed = false;
		mmap_DeltaSetHook(nullptr);
		mmap_DeltaSetFaultHook(nullptr);
		UnwatchHost();
		DeltaStateDiscardBefore(INT_MAX);
		for (auto& [frame, state] : s_store.states)
			s_store.buffer_pool.push_back(std::move(state));
		s_store.states.clear();
		ReleaseDelta(s_store.open);
		s_heat.hot.clear();
		std::fill_n(s_heat.is_hot, ALL_PAGES, false);
		std::fill_n(s_heat.run, ALL_PAGES, 0);
		s_store.page_pool.clear();
		s_store.buffer_pool.clear();
	}

	// ---- synctest -------------------------------------------------------------------------

	namespace
	{
		struct Sample
		{
			std::vector<u64> ee_pages; // DeltaStateHashRam
			std::vector<u8> state; // the rest
		};

		struct Stat
		{
			double sum = 0, max = 0;
			int n = 0;
			void Add(double v)
			{
				sum += v;
				max = std::max(max, v);
				n++;
			}
			double Mean() const { return n ? sum / n : 0; }
		};

		struct DeltaTest
		{
			bool parsed = false;
			int start = 3000, frames = 1800, depth = 8, every = 20;
			int frame = 0; // vsyncs since boot; set back by a rollback
			int replay_until = -1, next_rollback = 0;
			// replays=N: a window is replayed N times, each pass compared with the one before it (pass 0 =
			// the first run). Pass 2 vs 1 runs both from a load, so it separates code cache history.
			int replays = 2, pass = 0;
			// preload=1: the first run also loads the window start right after saving it.
			bool preload = false;
			int preload_logged = 0;
			int mismatched_pass[2] = {};
			bool done = false;
			std::map<int, Sample> samples;
			int gap = 0, gap_skipped = 0;
			int rollbacks = 0, compared = 0, mismatched = 0, ee_mismatched = 0;
			std::map<std::string, int> fields; // DeltaFreezeDescribe of differing state runs -> count
			Stat save_ms, load_ms, pages, state_kb;
		};
		DeltaTest s_test;

		void Parse()
		{
			s_test.parsed = true;
			for (const std::string_view item : StringUtil::SplitString(Zdxsv::TestEnv("ZDXSV_DELTA_TEST"), ','))
			{
				const size_t eq = item.find('=');
				if (eq == std::string_view::npos)
					continue;
				const std::string_view key = item.substr(0, eq);
				const std::string_view value = item.substr(eq + 1);
				const int n = StringUtil::FromChars<int>(value).value_or(0);
				if (key == "start")
					s_test.start = n;
				else if (key == "frames")
					s_test.frames = n;
				else if (key == "depth")
					s_test.depth = std::max(n, 1);
				else if (key == "every")
					s_test.every = std::max(n, 1);
				else if (key == "preload")
					s_test.preload = n != 0;
				else if (key == "replays")
					s_test.replays = std::max(n, 1);
				else if (key == "gap")
					s_test.gap = std::max(n, 0);
				else if (key == "break")
				{
					s_break_ee = (value == "ee");
					s_break_host = (value == "host");
				}
				else
					Console.Warning("ZdxsvDelta: unknown key '%.*s'", static_cast<int>(key.size()), key.data());
			}
			s_test.next_rollback = s_test.start + s_test.depth;
			if (s_test.gap > s_test.every - s_test.depth - 1)
			{
				Console.Warning("ZdxsvDelta: gap %d cut to every - depth - 1", s_test.gap);
				s_test.gap = std::max(s_test.every - s_test.depth - 1, 0);
			}
			Console.WriteLn("ZdxsvDelta: test start=%d frames=%d depth=%d every=%d gap=%d break_ee=%d break_host=%d",
				s_test.start, s_test.frames, s_test.depth, s_test.every, s_test.gap, s_break_ee, s_break_host);
		}

		Sample TakeSample(int frame)
		{
			Sample s;
			DeltaStateHashRam(s.ee_pages);
			s.state = s_store.states[frame];
			DeltaStateMaskScratch(s.state);
			return s;
		}

		void Compare(int frame, const Sample& first, const Sample& again)
		{
			s_test.compared++;
			int ee_diff = 0, ee_first = -1;
			for (size_t i = 0; i < first.ee_pages.size(); i++)
			{
				if (first.ee_pages[i] != again.ee_pages[i])
				{
					if (ee_first < 0)
						ee_first = static_cast<int>(i);
					ee_diff++;
				}
			}
			int st_diff = 0, st_first = -1;
			size_t st_last = 0;
			std::vector<size_t> st_runs; // first offset of each differing run (gap > 8 bytes)
			const size_t n = std::min(first.state.size(), again.state.size());
			for (size_t i = 0; i < n; i++)
			{
				if (first.state[i] != again.state[i])
				{
					if (st_first < 0)
						st_first = static_cast<int>(i);
					if (st_runs.empty() || i > st_last + 8)
						st_runs.push_back(i);
					st_last = i;
					st_diff++;
				}
			}
			if (ee_diff == 0 && st_diff == 0 && first.state.size() == again.state.size())
				return;
			s_test.mismatched_pass[std::min(s_test.pass, 2) - 1]++;
			if (ee_diff)
				s_test.ee_mismatched++;
			for (size_t k = 0; k < st_runs.size() && k < 16; k++)
			{
				std::string field = DeltaFreezeDescribe(first.state, st_runs[k]);
				field = field.substr(0, field.find(" (block at"));
				s_test.fields[field]++;
			}
			if (s_test.mismatched++ < 20)
			{
				Console.WriteLn("ZdxsvDelta: MISMATCH pass %d frame %d: ee pages %d (first %s), state bytes %d (first offset %d), size %zu/%zu",
					s_test.pass, frame, ee_diff, DeltaStateRamPageName(std::max(ee_first, 0)).c_str(), st_diff, st_first, first.state.size(), again.state.size());
				for (size_t k = 0; k < st_runs.size() && k < 6; k++)
				{
					const size_t o = st_runs[k];
					Console.WriteLn("ZdxsvDelta:   state offset %zu = %s: %02x -> %02x", o,
						DeltaFreezeDescribe(first.state, o).c_str(), first.state[o], again.state[o]);
				}
			}
		}

		void Report(const char* what)
		{
			Console.WriteLn("ZdxsvDelta: %s frame %d rollbacks %d compared %d mismatched %d | save ms mean %.3f max %.3f | load ms mean %.3f max %.3f | ee pages/frame mean %.1f max %.0f | state KB %.0f | mismatched pass 1 %d pass 2+ %d | gap skipped %d",
				what, s_test.frame, s_test.rollbacks, s_test.compared, s_test.mismatched, s_test.save_ms.Mean(), s_test.save_ms.max, s_test.load_ms.Mean(), s_test.load_ms.max,
				s_test.pages.Mean(), s_test.pages.max, s_test.state_kb.Mean(), s_test.mismatched_pass[0], s_test.mismatched_pass[1], s_test.gap_skipped);
			// which state differed over all mismatches (first 16 differing runs of each), not just the 20 logged
			std::string fields;
			for (const auto& [name, count] : s_test.fields)
				fields += fmt::format(" | {} x{}", name, count);
			Console.WriteLn("ZdxsvDelta: %s mismatched with ee pages %d, state fields%s", what, s_test.ee_mismatched, fields.empty() ? " none" : fields.c_str());
			if (std::strcmp(what, "done") == 0)
				Console.WriteLn("ZdxsvDelta: result %s: pass 2+ mismatched %d (pass 1, code cache: %d)",
					s_test.replays < 2 ? "none (replays=1)" : s_test.mismatched_pass[1] ? "FAIL" : "PASS", s_test.mismatched_pass[1], s_test.mismatched_pass[0]);
		}
	} // namespace

	void DeltaStateOnVsync()
	{
		if (!s_test.parsed)
			Parse();
		const int frame = ++s_test.frame;
		if (s_test.done || frame < s_test.start)
			return;
		if (frame >= s_test.start + s_test.frames)
		{
			s_test.done = true;
			Report("done");
			DeltaStateClear();
			s_test.samples.clear();
			return;
		}

		const int window_start = s_test.next_rollback - s_test.depth;
		if (s_test.pass == 0 && frame > s_test.start && frame < window_start && frame >= window_start - s_test.gap)
		{
			s_test.gap_skipped++;
			DeltaStateDiscardBefore(frame - s_test.depth);
			return;
		}
		const size_t open_pages = s_store.open.size();
		Common::Timer timer;
		if (!DeltaStateSave(frame))
		{
			s_test.done = true;
			DeltaStateClear();
			return;
		}
		if (frame > s_test.start)
			s_test.pages.Add(static_cast<double>(open_pages));
		s_test.save_ms.Add(timer.GetTimeMilliseconds());
		s_test.state_kb.Add(s_store.states[frame].size() / 1024.0);

		if (s_test.preload && s_test.pass == 0 && frame == s_test.next_rollback - s_test.depth)
		{
			if (!DeltaStateLoad(frame))
			{
				s_test.done = true;
				DeltaStateClear();
				return;
			}
			// A load must be a no-op here: the state saved again has to match byte for byte.
			std::vector<u8> after;
			if (DeltaFreezeSave(after))
			{
				std::vector<u8> before = s_store.states[frame];
				DeltaStateMaskScratch(before);
				DeltaStateMaskScratch(after);
				size_t last = 0;
				int logged = 0;
				for (size_t i = 0; i < std::min(before.size(), after.size()) && s_test.preload_logged < 60; i++)
				{
					if (before[i] != after[i] && (logged == 0 || i > last + 8))
					{
						Console.WriteLn("ZdxsvDelta: LOAD CHANGED frame %d offset %zu = %s: %02x -> %02x", frame, i,
							DeltaFreezeDescribe(before, i).c_str(), before[i], after[i]);
						logged++;
						s_test.preload_logged++;
					}
					if (before[i] != after[i])
						last = i;
				}
			}
		}

		Sample sample = TakeSample(frame);
		if (frame <= s_test.replay_until)
			Compare(frame, s_test.samples[frame], sample);
		s_test.samples[frame] = std::move(sample);

		DeltaStateDiscardBefore(frame - s_test.depth);
		while (!s_test.samples.empty() && s_test.samples.begin()->first < frame - s_test.depth)
			s_test.samples.erase(s_test.samples.begin());

		if ((frame - s_test.start) % 600 == 0)
			Report("progress");

		const bool window_end = s_test.pass == 0 && frame >= s_test.next_rollback;
		const bool replay_end = s_test.pass > 0 && frame == s_test.replay_until;
		if (replay_end && s_test.pass >= s_test.replays)
			s_test.pass = 0;
		else if (window_end || replay_end)
		{
			s_test.pass++;
			timer.Reset();
			if (!DeltaStateLoad(frame - s_test.depth))
			{
				s_test.done = true;
				DeltaStateClear();
				return;
			}
			s_test.load_ms.Add(timer.GetTimeMilliseconds());
			s_test.rollbacks++;
			if (window_end)
			{
				s_test.replay_until = frame;
				s_test.next_rollback = frame + s_test.every;
			}
			s_test.frame = frame - s_test.depth;
		}
	}
} // namespace Zdxsv
