// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ZDXSV_DELTA_TEST="key=value,..." (any value, even empty, turns it on): synctest in a running
// game. Every frame from start: save; every `every` frames: load the frame `depth` back and run
// those frames again. A rerun frame must hash the same as its first run (EE RAM per page, the
// rest of the state byte by byte).
//   start=3000   vsync (counted from boot) of the first save
//   frames=1800  frames to test
//   depth=8      frames rolled back
//   every=20     frames from one rollback to the next
//   break=ee     control: a load does not restore EE RAM (must report mismatches)
// Results go to the log, lines start with "ZdxsvDelta".

#include "ZdxsvDeltaState.h"

#include "Memory.h"
#include "SaveState.h"
#include "vtlb.h"

#include "common/Console.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include "xxhash.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ZdxsvDeltaState
{
	bool g_test_enabled = std::getenv("ZDXSV_DELTA_TEST") != nullptr;
	// A control run: ZDXSV_DELTA_TEST=...,blocks=linked keeps the recompilers' history-dependent block ends.
	bool g_fixed_blocks = (g_test_enabled && !std::strstr(std::getenv("ZDXSV_DELTA_TEST"), "blocks=linked")) ||
		std::getenv("ZDXSV_GGPO") != nullptr;

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
	} // namespace

	bool Save(int frame)
	{
		if (!s_states.empty())
		{
			const int last = s_states.rbegin()->first;
			if (frame <= last)
			{
				Console.Error("ZdxsvDelta: save of frame %d after %d", frame, last);
				return false;
			}
			for (const SavedPage& p : s_open)
				mmap_DeltaWatchPage(p.page);
			s_deltas[last] = std::move(s_open);
			s_open = Delta();
		}
		else
		{
			mmap_DeltaSetHook(&OnWrite);
			mmap_DeltaWatchAll();
		}

		std::vector<u8> buffer;
		if (!s_buffer_pool.empty())
		{
			buffer = std::move(s_buffer_pool.back());
			s_buffer_pool.pop_back();
		}
		if (!SaveState_DeltaSave(buffer))
			return false;
		s_states[frame] = std::move(buffer);
		return true;
	}

	bool Load(int frame)
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
		for (u32 page : touched)
			mmap_DeltaWatchPage(page);

		while (s_states.rbegin()->first > frame)
		{
			const auto it = std::prev(s_states.end());
			s_buffer_pool.push_back(std::move(it->second));
			s_states.erase(it);
		}

		return SaveState_DeltaLoad(state->second);
	}

	const std::vector<u8>* GetState(int frame)
	{
		const auto it = s_states.find(frame);
		return it == s_states.end() ? nullptr : &it->second;
	}

	void DiscardBefore(int frame)
	{
		while (!s_states.empty() && s_states.begin()->first < frame)
		{
			s_buffer_pool.push_back(std::move(s_states.begin()->second));
			s_states.erase(s_states.begin());
		}
		while (!s_deltas.empty() && s_deltas.begin()->first < frame)
		{
			ReleaseDelta(s_deltas.begin()->second);
			s_deltas.erase(s_deltas.begin());
		}
	}

	void Clear()
	{
		mmap_DeltaSetHook(nullptr);
		DiscardBefore(INT_MAX);
		ReleaseDelta(s_open);
		s_page_pool.clear();
		s_buffer_pool.clear();
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
		int s_replays = 1, s_pass = 0;
		// preload=1: the first run also loads the window start right after saving it.
		bool s_preload = false;
		int s_preload_logged = 0;
		int s_mismatched_pass[2] = {};
		bool s_done = false;
		std::map<int, Sample> s_samples;
		int s_rollbacks = 0, s_compared = 0, s_mismatched = 0;
		Stat s_save_ms, s_load_ms, s_pages, s_state_kb;

		void Parse()
		{
			s_parsed = true;
			for (const std::string_view item : StringUtil::SplitString(std::getenv("ZDXSV_DELTA_TEST"), ','))
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
				else if (key == "break")
					s_break_ee = (value == "ee");
				else if (key == "blocks")
					; // read at startup (g_fixed_blocks)
				else
					Console.Warning("ZdxsvDelta: unknown key '%.*s'", static_cast<int>(key.size()), key.data());
			}
			s_next_rollback = s_start + s_depth;
			Console.WriteLn("ZdxsvDelta: test start=%d frames=%d depth=%d every=%d break_ee=%d",
				s_start, s_frames, s_depth, s_every, s_break_ee);
		}

		Sample TakeSample(int frame)
		{
			Sample s;
			const u32 pages = Ps2MemSize::ExposedRam / PAGE_SIZE;
			s.ee_pages.resize(pages);
			for (u32 i = 0; i < pages; i++)
				s.ee_pages[i] = XXH3_64bits(&eeMem->Main[i * PAGE_SIZE], PAGE_SIZE);
			s.state = s_states[frame];
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
			Console.WriteLn("ZdxsvDelta: %s frame %d rollbacks %d compared %d mismatched %d | save ms mean %.3f max %.3f | load ms mean %.3f max %.3f | ee pages/frame mean %.1f max %.0f | state KB %.0f | mismatched pass 1 %d pass 2+ %d",
				what, s_frame, s_rollbacks, s_compared, s_mismatched, s_save_ms.Mean(), s_save_ms.max, s_load_ms.Mean(), s_load_ms.max,
				s_pages.Mean(), s_pages.max, s_state_kb.Mean(), s_mismatched_pass[0], s_mismatched_pass[1]);
		}
	} // namespace

	void OnVsync()
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
			Clear();
			s_samples.clear();
			return;
		}

		const size_t open_pages = s_open.size();
		Common::Timer timer;
		if (!Save(frame))
		{
			s_done = true;
			Clear();
			return;
		}
		if (frame > s_start)
			s_pages.Add(static_cast<double>(open_pages));
		s_save_ms.Add(timer.GetTimeMilliseconds());
		s_state_kb.Add(s_states[frame].size() / 1024.0);

		if (s_preload && s_pass == 0 && frame == s_next_rollback - s_depth)
		{
			if (!Load(frame))
			{
				s_done = true;
				Clear();
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

		DiscardBefore(frame - s_depth);
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
			if (!Load(frame - s_depth))
			{
				s_done = true;
				Clear();
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
} // namespace ZdxsvDeltaState
