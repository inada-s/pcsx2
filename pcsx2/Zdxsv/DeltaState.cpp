// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ZDXSV_DELTA_TEST="key=value,..." (any value, even empty, turns it on): synctest in a running
// game. Every frame from start: save; every `every` frames: load the frame `depth` back and run
// those frames again, `replays` times. Each pass is compared with the one before it (EE RAM per
// page, the rest of the state byte by byte). Pass 1 (first run vs first rerun) differs by the code
// cache: the recompilers end a block where the next PC is already compiled, so the first run and
// the rerun test events at other cycles (an IOP event start cycle off by 4 in most of them). Pass 2
// runs from the same load with the same code cache, so it must match pass 1 exactly: the result line
// (PASS/FAIL) counts pass 2+ only.
//   start=3000   vsync (counted from boot) of the first save
//   frames=1800  frames to test
//   depth=8      frames rolled back
//   every=20     frames from one rollback to the next
//   replays=2    reruns of each window (1: pass 1 only, no result)
//   break=ee     control: a load does not restore EE RAM (the result must be FAIL)
//   gap=0        frames before each rollback window that are not saved, older saves still
//                discarded (GGPO's confirmed-frame save skip; gap > depth drops all of them)
// Results go to the log, lines start with "ZdxsvDelta".

#include "Zdxsv/DeltaState.h"
#include "Zdxsv/Dev9Hooks.h"
#include "Zdxsv/TestOptions.h"

#include "Memory.h"
#include "SaveState.h"
#include "vtlb.h"

#include "common/Console.h"
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
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Zdxsv
{
	bool g_delta_state_test_enabled = Zdxsv::TestEnv("ZDXSV_DELTA_TEST") != nullptr;

	namespace
	{
		constexpr u32 PAGE_SIZE = 4096;

		struct SavedPage
		{
			u32 page;
			std::unique_ptr<u8[]> data;
		};
		using Delta = std::vector<SavedPage>;

		std::map<int, std::vector<u8>> s_states;
		// s_deltas[f]: old data of the pages written after the save of f, until the next save.
		std::map<int, Delta> s_deltas;
		// Pages written since the last save or load.
		Delta s_open;
		std::vector<std::unique_ptr<u8[]>> s_page_pool;
		std::vector<std::vector<u8>> s_buffer_pool;
		bool s_break_ee = false;
		int s_save_calls = 0;
		double s_watch_ms = 0, s_state_ms = 0, s_total_ms = 0; // DeltaStateTimes()

		// Hot pages: written in HOT_AFTER save intervals in a row. They stay writable and get a
		// copy in s_open at every save and load instead (= their data as of that save, like a
		// fault copy), which saves the protect calls (one per fastmem alias, ~17 us a page) and the
		// fault. Unchanged for COLD_AFTER intervals: watched again. ZDXSV_DELTA_HOT=0: off.
		constexpr u8 HOT_AFTER = 2, COLD_AFTER = 8;
		constexpr u32 EE_PAGES = Ps2MemSize::TotalRam / PAGE_SIZE;
		int s_hot_enabled = -1;
		bool s_is_hot[EE_PAGES] = {};
		u8 s_run[EE_PAGES] = {}; // cold: save intervals in a row with a write; hot: without a change
		int s_last_write[EE_PAGES] = {}; // cold: s_save_calls of the last interval with a write
		std::vector<u32> s_hot;
		double s_hot_pages = 0; // DeltaStateTimes(): mean hot pages per save

		std::unique_ptr<u8[]> TakePage()
		{
			if (s_page_pool.empty())
				return std::make_unique<u8[]>(PAGE_SIZE);
			std::unique_ptr<u8[]> p = std::move(s_page_pool.back());
			s_page_pool.pop_back();
			return p;
		}

		void ReleaseDelta(Delta& delta)
		{
			for (SavedPage& p : delta)
				s_page_pool.push_back(std::move(p.data));
			delta.clear();
		}

		// Fault handler (vtlb): the page still holds the data of the last save.
		void OnWrite(u32 page)
		{
			std::unique_ptr<u8[]> data = TakePage();
			std::memcpy(data.get(), &eeMem->Main[page * PAGE_SIZE], PAGE_SIZE);
			s_open.push_back({page, std::move(data)});
		}

		void RestoreDelta(Delta& delta, std::vector<u32>& touched)
		{
			for (const SavedPage& p : delta)
			{
				if (!s_break_ee)
					mmap_DeltaRestorePage(p.page, p.data.get());
				touched.push_back(p.page);
			}
			ReleaseDelta(delta);
		}

		void SnapshotHot()
		{
			for (u32 page : s_hot)
			{
				std::unique_ptr<u8[]> data = TakePage();
				std::memcpy(data.get(), &eeMem->Main[page * PAGE_SIZE], PAGE_SIZE);
				s_open.push_back({page, std::move(data)});
			}
		}

		// At a save: updates the hot set from s_open (the interval that ends) and returns the
		// pages to watch again.
		std::vector<u32> UpdateHot()
		{
			std::vector<u32> watch;
			watch.reserve(s_open.size());
			for (const SavedPage& p : s_open)
			{
				const u32 page = p.page;
				if (s_is_hot[page])
				{
					if (std::memcmp(p.data.get(), &eeMem->Main[page * PAGE_SIZE], PAGE_SIZE) != 0)
						s_run[page] = 0;
					else if (++s_run[page] >= COLD_AFTER)
					{
						s_is_hot[page] = false;
						s_run[page] = 0;
						s_hot.erase(std::find(s_hot.begin(), s_hot.end(), page));
						watch.push_back(page);
					}
					continue;
				}
				s_run[page] = (s_last_write[page] == s_save_calls - 1) ? static_cast<u8>(std::min(s_run[page] + 1, 255)) : 1;
				s_last_write[page] = s_save_calls;
				if (s_hot_enabled && s_run[page] >= HOT_AFTER)
				{
					s_is_hot[page] = true;
					s_run[page] = 0;
					s_hot.push_back(page);
				}
				else
					watch.push_back(page);
			}
			return watch;
		}

		// SMAP frames received while delta states are kept (Dev9Hooks.h). s_rx_mutex is taken
		// inside rx_mutex, or alone (never around a deliver call).
		struct RxFrame
		{
			u64 seq;
			int frame; // newest saved frame when received
			std::vector<u8> data;
		};
		std::mutex s_rx_mutex;
		std::deque<RxFrame> s_rx;
		u64 s_rx_seq = 0; // seq of the next logged frame
		bool s_rx_logging = false;
		bool s_rx_redelivering = false;
		int s_rx_newest = INT_MIN;
		int s_rx_redeliver_logs = 0;
		size_t s_rx_redelivered = 0;

		void RxSetNewest(int frame)
		{
			std::lock_guard lock(s_rx_mutex);
			s_rx_logging = true;
			s_rx_newest = frame;
		}
	} // namespace

	void DeltaStateOnRx(const void* data, int size)
	{
		std::lock_guard lock(s_rx_mutex);
		if (!s_rx_logging || s_rx_redelivering)
			return;
		const u8* p = static_cast<const u8*>(data);
		s_rx.push_back({s_rx_seq++, s_rx_newest, std::vector<u8>(p, p + size)});
	}

	u64 DeltaStateRxSeq()
	{
		std::lock_guard lock(s_rx_mutex);
		return s_rx_seq;
	}

	void DeltaStateRedeliverRx(u64 seq, void (*deliver)(const void* data, int size))
	{
		std::vector<std::vector<u8>> frames;
		{
			std::lock_guard lock(s_rx_mutex);
			for (const RxFrame& f : s_rx)
			{
				if (f.seq >= seq)
					frames.push_back(f.data);
			}
			if (frames.empty())
				return;
			s_rx_redelivering = true;
		}
		// The caller holds rx_mutex: no other thread reaches DeltaStateOnRx meanwhile.
		for (const std::vector<u8>& f : frames)
			deliver(f.data(), static_cast<int>(f.size()));
		std::lock_guard lock(s_rx_mutex);
		s_rx_redelivering = false;
		s_rx_redelivered += frames.size();
		if (s_rx_redeliver_logs++ < 20)
			Console.WriteLn("ZdxsvDelta: load received %zu frames again (from seq %llu, %zu so far)", frames.size(),
				static_cast<unsigned long long>(seq), s_rx_redelivered);
	}

	std::string DeltaStateTimes()
	{
		const double n = std::max(s_save_calls, 1);
		return fmt::format("Save ms: watch {:.3f} state {:.3f} total {:.3f} hot pages {:.1f}", s_watch_ms / n, s_state_ms / n,
			s_total_ms / n, s_hot_pages / n);
	}

	bool DeltaStateSave(int frame)
	{
		Common::Timer total;
		ScopedGuard add_total([&total]() { s_total_ms += total.GetTimeMilliseconds(); });
		s_save_calls++;
		if (s_hot_enabled < 0)
		{
			const char* env = std::getenv("ZDXSV_DELTA_HOT");
			s_hot_enabled = !(env && env[0] == '0');
		}
		if (!s_states.empty())
		{
			const int last = s_states.rbegin()->first;
			if (frame <= last)
			{
				Console.Error("ZdxsvDelta: save of frame %d after %d", frame, last);
				return false;
			}
			Common::Timer watch;
			mmap_DeltaWatchPages(UpdateHot());
			s_deltas[last] = std::move(s_open);
			s_open = Delta();
			SnapshotHot();
			s_hot_pages += s_hot.size();
			s_watch_ms += watch.GetTimeMilliseconds();
		}
		else
		{
			// Never with DeltaStateDiscardBefore keeping the newest frame (a load of this frame would restore them).
			if (!s_open.empty())
				Console.Error("ZdxsvDelta: save of frame %d with no saved frame left, %zu stale open pages", frame, s_open.size());
			mmap_DeltaSetHook(&OnWrite);
			mmap_DeltaWatchAll();
		}
		RxSetNewest(frame);

		std::vector<u8> buffer;
		if (!s_buffer_pool.empty())
		{
			buffer = std::move(s_buffer_pool.back());
			s_buffer_pool.pop_back();
		}
		Common::Timer state;
		if (!SaveState_DeltaSave(buffer))
			return false;
		s_state_ms += state.GetTimeMilliseconds();
		s_states[frame] = std::move(buffer);
		return true;
	}

	bool DeltaStateLoad(int frame)
	{
		const auto state = s_states.find(frame);
		if (state == s_states.end())
		{
			Console.Error("ZdxsvDelta: load of unsaved frame %d", frame);
			return false;
		}

		// Newest first, so a page ends up with its data from the earliest delta at or after frame.
		std::vector<u32> touched;
		RestoreDelta(s_open, touched);
		while (!s_deltas.empty() && s_deltas.rbegin()->first >= frame)
		{
			const auto it = std::prev(s_deltas.end());
			RestoreDelta(it->second, touched);
			s_deltas.erase(it);
		}
		touched.erase(std::remove_if(touched.begin(), touched.end(), [](u32 page) { return s_is_hot[page]; }), touched.end());
		mmap_DeltaWatchPages(touched);
		SnapshotHot();

		while (s_states.rbegin()->first > frame)
		{
			const auto it = std::prev(s_states.end());
			s_buffer_pool.push_back(std::move(it->second));
			s_states.erase(it);
		}

		RxSetNewest(frame);
		return SaveState_DeltaLoad(state->second);
	}

	const std::vector<u8>* DeltaStateGetState(int frame)
	{
		const auto it = s_states.find(frame);
		return it == s_states.end() ? nullptr : &it->second;
	}

	u64 DeltaStateHash(const std::vector<u8>& state)
	{
		XXH3_state_t xs;
		XXH3_64bits_reset(&xs);
		size_t at = 0;
		for (const auto& [pos, size] : SaveState_DeltaScratch())
		{
			if (pos < at || pos + size > state.size())
				continue;
			XXH3_64bits_update(&xs, state.data() + at, pos - at);
			at = pos + size;
		}
		XXH3_64bits_update(&xs, state.data() + at, state.size() - at);
		return XXH3_64bits_digest(&xs);
	}

	void DeltaStateMaskScratch(std::vector<u8>& state)
	{
		for (const auto& [pos, size] : SaveState_DeltaScratch())
		{
			if (pos + size <= state.size())
				std::memset(state.data() + pos, 0, size);
		}
	}

	void DeltaStateDiscardBefore(int frame)
	{
		// The newest saved frame stays: s_open holds the pages written since it. Without it the next
		// save starts over with s_open still holding that older interval, and a load of that save puts
		// those pages back to their old data (GGPO's confirmed-frame save skip, 0 ms + loss froze the game, s663).
		while (s_states.size() > 1 && s_states.begin()->first < frame)
		{
			s_buffer_pool.push_back(std::move(s_states.begin()->second));
			s_states.erase(s_states.begin());
		}
		while (!s_deltas.empty() && s_deltas.begin()->first < frame)
		{
			ReleaseDelta(s_deltas.begin()->second);
			s_deltas.erase(s_deltas.begin());
		}

		// A frame received before the oldest kept save is in all of them. Tags may be one frame
		// early (received during a save), so one more is kept.
		std::lock_guard lock(s_rx_mutex);
		const int oldest = s_states.empty() ? INT_MAX : s_states.begin()->first;
		while (!s_rx.empty() && s_rx.front().frame < oldest - 1)
			s_rx.pop_front();
	}

	void DeltaStateClear()
	{
		mmap_DeltaSetHook(nullptr);
		DeltaStateDiscardBefore(INT_MAX);
		for (auto& [frame, state] : s_states)
			s_buffer_pool.push_back(std::move(state));
		s_states.clear();
		ReleaseDelta(s_open);
		s_hot.clear();
		std::fill_n(s_is_hot, EE_PAGES, false);
		std::fill_n(s_run, EE_PAGES, 0);
		s_page_pool.clear();
		s_buffer_pool.clear();
		std::lock_guard lock(s_rx_mutex);
		s_rx.clear();
		s_rx_logging = false;
		s_rx_newest = INT_MIN;
	}

	// ---- synctest -------------------------------------------------------------------------

	namespace
	{
		struct Sample
		{
			std::vector<u64> ee_pages; // hash per EE RAM page
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

		bool s_parsed = false;
		int s_start = 3000, s_frames = 1800, s_depth = 8, s_every = 20;
		int s_frame = 0; // vsyncs since boot; set back by a rollback
		int s_replay_until = -1, s_next_rollback = 0;
		// replays=N: a window is replayed N times, each pass compared with the one before it (pass 0 =
		// the first run). Pass 2 vs 1 runs both from a load, so it separates code cache history.
		int s_replays = 2, s_pass = 0;
		// preload=1: the first run also loads the window start right after saving it.
		bool s_preload = false;
		int s_preload_logged = 0;
		int s_mismatched_pass[2] = {};
		bool s_done = false;
		std::map<int, Sample> s_samples;
		int s_gap = 0, s_gap_skipped = 0;
		int s_rollbacks = 0, s_compared = 0, s_mismatched = 0, s_ee_mismatched = 0;
		std::map<std::string, int> s_fields; // SaveState_DeltaDescribe of differing state runs -> count
		Stat s_save_ms, s_load_ms, s_pages, s_state_kb;

		void Parse()
		{
			s_parsed = true;
			for (const std::string_view item : StringUtil::SplitString(Zdxsv::TestEnv("ZDXSV_DELTA_TEST"), ','))
			{
				const size_t eq = item.find('=');
				if (eq == std::string_view::npos)
					continue;
				const std::string_view key = item.substr(0, eq);
				const std::string_view value = item.substr(eq + 1);
				const int n = StringUtil::FromChars<int>(value).value_or(0);
				if (key == "start")
					s_start = n;
				else if (key == "frames")
					s_frames = n;
				else if (key == "depth")
					s_depth = std::max(n, 1);
				else if (key == "every")
					s_every = std::max(n, 1);
				else if (key == "preload")
					s_preload = n != 0;
				else if (key == "replays")
					s_replays = std::max(n, 1);
				else if (key == "gap")
					s_gap = std::max(n, 0);
				else if (key == "break")
					s_break_ee = (value == "ee");
				else
					Console.Warning("ZdxsvDelta: unknown key '%.*s'", static_cast<int>(key.size()), key.data());
			}
			s_next_rollback = s_start + s_depth;
			if (s_gap > s_every - s_depth - 1)
			{
				Console.Warning("ZdxsvDelta: gap %d cut to every - depth - 1", s_gap);
				s_gap = std::max(s_every - s_depth - 1, 0);
			}
			Console.WriteLn("ZdxsvDelta: test start=%d frames=%d depth=%d every=%d gap=%d break_ee=%d",
				s_start, s_frames, s_depth, s_every, s_gap, s_break_ee);
		}

		Sample TakeSample(int frame)
		{
			Sample s;
			const u32 pages = Ps2MemSize::ExposedRam / PAGE_SIZE;
			s.ee_pages.resize(pages);
			for (u32 i = 0; i < pages; i++)
				s.ee_pages[i] = XXH3_64bits(&eeMem->Main[i * PAGE_SIZE], PAGE_SIZE);
			s.state = s_states[frame];
			DeltaStateMaskScratch(s.state);
			return s;
		}

		void Compare(int frame, const Sample& first, const Sample& again)
		{
			s_compared++;
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
			s_mismatched_pass[std::min(s_pass, 2) - 1]++;
			if (ee_diff)
				s_ee_mismatched++;
			for (size_t k = 0; k < st_runs.size() && k < 16; k++)
			{
				std::string field = SaveState_DeltaDescribe(first.state, st_runs[k]);
				field = field.substr(0, field.find(" (block at"));
				s_fields[field]++;
			}
			if (s_mismatched++ < 20)
			{
				Console.WriteLn("ZdxsvDelta: MISMATCH pass %d frame %d: ee pages %d (first 0x%08x), state bytes %d (first offset %d), size %zu/%zu",
					s_pass, frame, ee_diff, ee_first < 0 ? 0 : ee_first * PAGE_SIZE, st_diff, st_first, first.state.size(), again.state.size());
				for (size_t k = 0; k < st_runs.size() && k < 6; k++)
				{
					const size_t o = st_runs[k];
					Console.WriteLn("ZdxsvDelta:   state offset %zu = %s: %02x -> %02x", o,
						SaveState_DeltaDescribe(first.state, o).c_str(), first.state[o], again.state[o]);
				}
			}
		}

		void Report(const char* what)
		{
			Console.WriteLn("ZdxsvDelta: %s frame %d rollbacks %d compared %d mismatched %d | save ms mean %.3f max %.3f | load ms mean %.3f max %.3f | ee pages/frame mean %.1f max %.0f | state KB %.0f | mismatched pass 1 %d pass 2+ %d | gap skipped %d",
				what, s_frame, s_rollbacks, s_compared, s_mismatched, s_save_ms.Mean(), s_save_ms.max, s_load_ms.Mean(), s_load_ms.max,
				s_pages.Mean(), s_pages.max, s_state_kb.Mean(), s_mismatched_pass[0], s_mismatched_pass[1], s_gap_skipped);
			// which state differed over all mismatches (first 16 differing runs of each), not just the 20 logged
			std::string fields;
			for (const auto& [name, count] : s_fields)
				fields += fmt::format(" | {} x{}", name, count);
			Console.WriteLn("ZdxsvDelta: %s mismatched with ee pages %d, state fields%s", what, s_ee_mismatched, fields.empty() ? " none" : fields.c_str());
			if (std::strcmp(what, "done") == 0)
				Console.WriteLn("ZdxsvDelta: result %s: pass 2+ mismatched %d (pass 1, code cache: %d)",
					s_replays < 2 ? "none (replays=1)" : s_mismatched_pass[1] ? "FAIL" : "PASS", s_mismatched_pass[1], s_mismatched_pass[0]);
		}
	} // namespace

	void DeltaStateOnVsync()
	{
		if (!s_parsed)
			Parse();
		const int frame = ++s_frame;
		if (s_done || frame < s_start)
			return;
		if (frame >= s_start + s_frames)
		{
			s_done = true;
			Report("done");
			DeltaStateClear();
			s_samples.clear();
			return;
		}

		const int window_start = s_next_rollback - s_depth;
		if (s_pass == 0 && frame > s_start && frame < window_start && frame >= window_start - s_gap)
		{
			s_gap_skipped++;
			DeltaStateDiscardBefore(frame - s_depth);
			return;
		}
		const size_t open_pages = s_open.size();
		Common::Timer timer;
		if (!DeltaStateSave(frame))
		{
			s_done = true;
			DeltaStateClear();
			return;
		}
		if (frame > s_start)
			s_pages.Add(static_cast<double>(open_pages));
		s_save_ms.Add(timer.GetTimeMilliseconds());
		s_state_kb.Add(s_states[frame].size() / 1024.0);

		if (s_preload && s_pass == 0 && frame == s_next_rollback - s_depth)
		{
			if (!DeltaStateLoad(frame))
			{
				s_done = true;
				DeltaStateClear();
				return;
			}
			// A load must be a no-op here: the state saved again has to match byte for byte.
			std::vector<u8> after;
			if (SaveState_DeltaSave(after))
			{
				const std::vector<u8>& before = s_states[frame];
				size_t last = 0;
				int logged = 0;
				for (size_t i = 0; i < std::min(before.size(), after.size()) && s_preload_logged < 60; i++)
				{
					if (before[i] != after[i] && (logged == 0 || i > last + 8))
					{
						Console.WriteLn("ZdxsvDelta: LOAD CHANGED frame %d offset %zu = %s: %02x -> %02x", frame, i,
							SaveState_DeltaDescribe(before, i).c_str(), before[i], after[i]);
						logged++;
						s_preload_logged++;
					}
					if (before[i] != after[i])
						last = i;
				}
			}
		}

		Sample sample = TakeSample(frame);
		if (frame <= s_replay_until)
			Compare(frame, s_samples[frame], sample);
		s_samples[frame] = std::move(sample);

		DeltaStateDiscardBefore(frame - s_depth);
		while (!s_samples.empty() && s_samples.begin()->first < frame - s_depth)
			s_samples.erase(s_samples.begin());

		if ((frame - s_start) % 600 == 0)
			Report("progress");

		const bool window_end = s_pass == 0 && frame >= s_next_rollback;
		const bool replay_end = s_pass > 0 && frame == s_replay_until;
		if (replay_end && s_pass >= s_replays)
			s_pass = 0;
		else if (window_end || replay_end)
		{
			s_pass++;
			timer.Reset();
			if (!DeltaStateLoad(frame - s_depth))
			{
				s_done = true;
				DeltaStateClear();
				return;
			}
			s_load_ms.Add(timer.GetTimeMilliseconds());
			s_rollbacks++;
			if (window_end)
			{
				s_replay_until = frame;
				s_next_rollback = frame + s_every;
			}
			s_frame = frame - s_depth;
		}
	}
} // namespace Zdxsv
