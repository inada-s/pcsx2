// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Replay playback: frame loop, seek keys, point of view, takeover, the control bar and its hotkeys.

#include "Zdxsv/DeltaState.h"
#include "Zdxsv/GgpoShared.h"
#include "Zdxsv/MediaHooks.h"
#include "Zdxsv/ReplayList.h"
#include "Zdxsv/TestOptions.h"
#include "Zdxsv/UiHooks.h"

namespace Zdxsv
{
	const char* const s_to_test = TestEnv("ZDXSV_REPLAY_TAKEOVER");

	namespace
	{
		struct PlayCheck
		{
			int hashes = 0, hash_bad = 0, hash_first_bad = -1, rngs = 0, rng_bad = 0;
			int hash_cut = 0;
			int late = 0, late_loads = 0, late_max = 0, late_run = 0;
			int wait = 0, wait_loads = 0, wait_max = 0, wait_run = 0;
		} s_check;
		constexpr int LOAD_WAIT_MAX = 600;
		const int s_play_key_every = [] {
			const char* e = TestEnv("ZDXSV_REPLAY_KEY");
			return e ? std::atoi(e) : 600;
		}();
		const bool s_play_key_nohle = TestEnv("ZDXSV_REPLAY_KEY_NOHLE") != nullptr;
		const bool s_play_round_take = TestEnv("ZDXSV_REPLAY_ROUND_TAKE") != nullptr;
		const bool s_play_round_bad = TestEnv("ZDXSV_REPLAY_ROUND_BAD") != nullptr;
		// ZDXSV_CYCLE_LOG=n: EE and IOP cycle counters every n played frames (exact timing check of a CPU core change).
		const int s_cycle_log = [] {
			const char* e = TestEnv("ZDXSV_CYCLE_LOG");
			return e ? std::atoi(e) : 0;
		}();
		u8 s_tick_st = 0;
		u32 s_round_rec = 0;
		// control bar (ReplayBarInfo): written per played frame on the CPU thread, read by the GS thread's ImGui
		struct ReplayBar
		{
			std::atomic<int> frame{-1}; // -1 = no replay playing
			std::atomic<int> frames{0}, pov{-1}, target{-1};
			std::atomic<u32> povs{0}; // bit p = position p has a file
			std::atomic<int> lo{0}, hi{0}, round{-1}, rounds{0}; // round -1: no spans
			std::atomic<bool> battle{false};
		};
		ReplayBar s_bar;

		void PlayBarPublish()
		{
			u32 povs = 0;
			for (int p = 0; p < s_opt.players; p++)
				povs |= s_play.pov_ok[p] ? (1u << p) : 0u;
			s_bar.frames = s_play.frames;
			s_bar.pov = s_battle.me;
			s_bar.povs = povs;
			s_bar.target = s_play.target;
			const int span = PlaySpanAt(s_play.target >= 0 ? s_play.target : s_battle.frame);
			s_bar.lo = span >= 0 ? s_play.spans[span].first : 0;
			s_bar.hi = span >= 0 ? s_play.spans[span].second : s_play.frames;
			s_bar.round = span;
			s_bar.rounds = static_cast<int>(s_play.spans.size());
			if (PlayLiveNextRound(s_play.target >= 0 ? s_play.target : s_battle.frame))
			{
				s_bar.lo = s_play.spans.back().second;
				s_bar.hi = s_play.frames;
				s_bar.round = s_bar.rounds++;
			}
			s_bar.battle = !s_live_down && PlayInBattle(s_battle.frame);
			s_bar.frame = s_battle.frame;
		}

		// key display (ReplayKeys): runs of the shown position's B word (ZdPadAB) up to the played frame, newest
		// first, with their frame counts. Per frame one more frame; after a seek or a switch, rebuilt from the file.
		static constexpr size_t KEY_RUNS = 14;
		struct KeyDisplay
		{
			std::mutex mtx;
			std::deque<std::pair<u16, int>> runs; // guarded by mtx (the GS thread draws it)
			int frame = -1, me = -1; // frame and position runs ends at
		};
		KeyDisplay s_keys;

		u16 PlayB(int f, int p)
		{
			u16 ab[2];
			std::memcpy(ab, &s_play.inputs[static_cast<size_t>(f) * s_opt.players + p].pad, sizeof(ab));
			return ab[1] & ~1u; // bit 0 = game state, not a key
		}

		void PlayKeysPublish(int f)
		{
			if (!s_keys_on || f < 0 || f >= s_play.frames)
				return;
			std::lock_guard lock(s_keys.mtx);
			if (f == s_keys.frame + 1 && s_battle.me == s_keys.me)
			{
				const u16 b = PlayB(f, s_battle.me);
				if (!s_keys.runs.empty() && s_keys.runs.front().first == b)
					s_keys.runs.front().second++;
				else
				{
					s_keys.runs.emplace_front(b, 1);
					if (s_keys.runs.size() > KEY_RUNS)
						s_keys.runs.pop_back();
				}
			}
			else if (f != s_keys.frame || s_battle.me != s_keys.me)
			{
				s_keys.runs.clear();
				for (int g = f; g >= 0; g--)
				{
					const u16 b = PlayB(g, s_battle.me);
					if (!s_keys.runs.empty() && s_keys.runs.back().first == b)
						s_keys.runs.back().second++;
					else if (s_keys.runs.size() == KEY_RUNS)
						break;
					else
						s_keys.runs.emplace_back(b, 1);
				}
			}
			s_keys.frame = f;
			s_keys.me = s_battle.me;
			if (f % 600 == 0) // test drivers (zdxsv keycheck.py) compare these with the file
			{
				std::string s;
				for (const auto& [b, n] : s_keys.runs)
					s += fmt::format(" {:04x}*{}", b, n);
				Console.WriteLn("ZdxsvGgpo: replay keys frame %d pos %d:%s", f, s_battle.me, s.c_str());
			}
		}

		// At the start of frame f, before its inputs (the point of the frame 0 state).
		// The state before frame f runs, zipped to `name` in the cache folder on a thread.
		std::unique_ptr<PlayKey> PlayKeyMake(int f, const std::string& name)
		{
			Error error;
			std::unique_ptr<ArchiveEntryList> list = SaveState_DownloadState(&error);
			if (!list)
			{
				Console.Error("ZdxsvGgpo: replay key %d: state download failed: %s", f, error.GetDescription().c_str());
				return nullptr;
			}
			auto key = std::make_unique<PlayKey>();
			PlayKey* k = key.get();
			k->path = Path::Combine(EmuFolders::Cache, name);
			k->rb = s_rb;
			k->rings = s_rings;
			k->zip = std::thread([list = std::move(list), k]() mutable {
				Error error;
				k->ok = SaveState_ZipToDisk(std::move(list), nullptr, k->path.c_str(), &error);
				if (!k->ok)
					Console.Error("ZdxsvGgpo: replay key: state zip failed: %s", error.GetDescription().c_str());
			});
			return key;
		}

		void PlayKeySave(int f)
		{
			const auto first_load = s_play.rngs.upper_bound(0);
			const bool first_state = (s_play_round_take || !s_play.round_states.empty()) && first_load != s_play.rngs.end() && f == first_load->first;
			if (s_play_key_every <= 0 || (f % s_play_key_every != 0 && !first_state) || s_play_keys[s_battle.me].contains(f))
				return;
			// after a round jump, up to the next load end the player work still holds the first round's values: a key there
			// would bring them back without the jump's unchecked frames
			if (s_play.jump_check >= 0 && f > s_play.jump_check)
				return;
			Common::Timer timer;
			std::unique_ptr<PlayKey> key = PlayKeyMake(f, fmt::format("zdxsv-replay-key-p{}-{}.p2s", s_battle.me, f));
			if (!key)
				return;
			s_play_keys[s_battle.me].emplace(f, std::move(key));
			Console.WriteLn("ZdxsvGgpo: replay key %d (download %.1f ms)", f, timer.GetTimeMilliseconds());
		}

		Input PadFromB(u16 b)
		{
			Input in = {};
			in.lx = in.ly = in.rx = in.ry = Pad::ANALOG_NEUTRAL_POSITION;
			for (u32 i = 0; i < BUTTONS; i++)
			{
				Input one = in;
				one.buttons = static_cast<u16>(1u << i);
				u16 a1, b1;
				ZdPadAB(one, a1, b1);
				if (b1 && (b & b1) == b1)
					in.buttons |= one.buttons;
			}
			return in;
		}

		void PlayTrackLoads(int f)
		{
			const u8 st = eeMem->Main[TICK_STATE];
			if (f > s_play.hi)
			{
				if (f == s_play.tick_f + 1 && s_tick_st == TICK_LOAD && st != TICK_LOAD)
				{
					std::lock_guard lock(s_battle_loads_mtx);
					s_play_loads.battle.push_back(f);
					Console.WriteLn("ZdxsvGgpo: replay: load %zu ends at frame %d, vsync %u", s_play_loads.battle.size() - 1, f, g_FrameCount);
				}
				if (f == s_play.tick_f + 1)
				{
					if (const int win = RoundResult(s_round_rec, RoundRecord()))
					{
						std::lock_guard lock(s_battle_loads_mtx);
						const size_t i = s_play_loads.results.size();
						s_play_loads.results.push_back(win);
						s_play_loads.ms.push_back(MsIds());
						Console.WriteLn("ZdxsvGgpo: replay: round result %zu: win_team %d at frame %d, MS ids %08x", i + 1, win, f - 1, MsIds());
						if (i < s_play_loads.file_rounds.size() && s_play_loads.file_rounds[i] != win)
							Console.Error("ZdxsvGgpo: replay: round result %zu differs from the file's win_team %d", i + 1, s_play_loads.file_rounds[i]);
					}
				}
				s_play.hi = f;
			}
			s_play.tick_f = f;
			s_tick_st = st;
			s_round_rec = RoundRecord();
			if (s_play.run_load >= 0 && s_play.run_load < static_cast<int>(s_play_loads.battle.size()) && s_play_loads.battle[s_play.run_load] == f)
				PlayRunEnd(s_play.run_load == 1 ? "briefing" : "start", f);
		}
		const char s_to_test_src = [] {
			const char* c = s_to_test ? std::strchr(s_to_test, ':') : nullptr;
			const std::string_view src = c ? c + 1 : "";
			return src == "replay" ? 'r' : src == "rand" ? 'n' : '\0';
		}();
		std::mt19937 s_to_rng{1};

		const NetInput& PlayRow(int f, int p) { return s_play.inputs[static_cast<size_t>(f) * s_opt.players + p]; }

		u16 PlayOwnB(int f)
		{
			if (f < 0 || f >= s_play.frames)
				return 0;
			u16 ab[2];
			std::memcpy(ab, &PlayRow(f, s_battle.me).pad, sizeof(ab));
			return ab[1];
		}

		Input TakeoverPad(int f)
		{
			if (s_to_test_src == 'r')
				return PadFromB(PlayOwnB(f + s_to.delay));
			if (s_to_test_src == 'n')
			{
				static Input in = PadFromB(0);
				if (f % 5 == 0)
				{
					using I = PadDualshock2::Inputs;
					in.buttons = static_cast<u16>(s_to_rng() & s_to_rng() & ~((1u << I::PAD_START) | (1u << I::PAD_SELECT)));
				}
				return in;
			}
			return TakeoverHostInput();
		}

		void PlayFrameTakeover(int f)
		{
			const u8 st = eeMem->Main[TICK_STATE];
			if (st == TICK_END && s_tick_st == TICK_PLAY && !s_to_test && s_to_shared.req == TO_REQ_NONE)
			{
				Console.WriteLn("ZdxsvGgpo: replay takeover: the battle ended at frame %d, retrying", f);
				s_to_shared.req = TO_REQ_RETRY;
			}
			s_tick_st = st;
			s_battle.frame = f;
			PlayBarPublish();
			NetSaved(f);
			NetInput in[GGPO_MAX_PLAYERS];
			std::memcpy(in, &PlayRow(std::min(f, s_play.frames - 1), 0), sizeof(NetInput) * s_opt.players);
			if (f >= s_play.frames)
				for (int p = 0; p < s_opt.players; p++)
					std::memset(&in[p].pad, 0, sizeof(u16) * 2); // (A, B); seq unchanged = no new msgs
			s_to.queue.push_back(NetPack(TakeoverPad(f), s_battle.frame));
			in[s_battle.me] = s_to.queue.front();
			s_to.queue.pop_front();
			NetApply(in);
		}

		// The file's state hash and RNGs of frame f against the emulated state (a desync check; nothing changes).
		void PlayCheckState(int f)
		{
			const int bad0 = s_check.hash_bad + s_check.rng_bad;
			if (s_cycle_log > 0 && f % s_cycle_log == 0)
				Console.WriteLn("ZdxsvGgpo: cycles at frame %d: ee %llu iop %llu", f, static_cast<unsigned long long>(cpuRegs.cycle), static_cast<unsigned long long>(psxRegs.cycle));
			// own load late past PlayLoadWait's bound: the file released the play-start barrier, this machine
			// reached the load step after it and releases later. Counted apart, not judged.
			const bool late = f < static_cast<int>(s_play.ps_rel.size()) && static_cast<u8>(s_play.ps_rel[f] - s_rb.ps.rel - 1) < 127;
			if (late)
			{
				s_check.late++;
				if (s_check.late_run++ == 0)
					s_check.late_loads++;
				s_check.late_max = std::max(s_check.late_max, s_check.late_run);
			}
			else if (s_check.late_run > 0)
			{
				Console.WriteLn("ZdxsvGgpo: replay own load late by %d frames: play start at frame %d, file %d", s_check.late_run, f - 1, f - 1 - s_check.late_run);
				s_check.late_run = 0;
			}
			const auto next_load = s_play.jump_check >= 0 ? s_play.rngs.upper_bound(s_play.jump_check) : s_play.rngs.end();
			const bool jumped = s_play.jump_check >= 0 && f >= s_play.jump_check && (next_load == s_play.rngs.end() || f < next_load->first);
			const bool cut = late || jumped || std::any_of(s_play.cut.begin(), s_play.cut.end(), [f](const auto& c) { return f >= c.first && f < c.second; });
			const bool hashed = !late && f < static_cast<int>(s_play.hashes.size());
			if (hashed && cut)
				s_check.hash_cut++;
			else if (hashed)
			{
				s_check.hashes++;
				if (const u32 h = ReplayStateHash(s_play.version < 20261118); h != s_play.hashes[f])
				{
					if (s_check.hash_bad++ < 10)
						Console.Warning("ZdxsvGgpo: replay state hash differs at frame %d: file %08x, here %08x", f, s_play.hashes[f], h);
					if (s_check.hash_first_bad < 0)
						s_check.hash_first_bad = f;
				}
			}
			if (const auto it = s_play.rngs.find(f); !cut && it != s_play.rngs.end() && s_battle.me == s_play.rng_pos)
			{
				// judged on RNG B (low half): RNG A also differs from the recorder's under the common start
				s_check.rngs++;
				const u32 rng = GameRng();
				const bool bad = static_cast<u16>(rng) != static_cast<u16>(it->second);
				if (bad)
					s_check.rng_bad++;
				Console.WriteLn("ZdxsvGgpo: replay rng at frame %d: file %08x, here %08x%s", f, it->second, rng, bad ? " DIFFERS" : "");
			}
			if (s_play.jump_check >= 0 && !jumped && !cut && f > s_play.jump_check)
			{
				if (s_check.hash_bad + s_check.rng_bad > bad0)
				{
					Console.Error("ZdxsvGgpo: replay round jump from %d: state differs at frame %d, seeking again without round states", s_play.jump_check, f);
					s_play.round_states.clear();
					s_play.states_off = true;
					auto& keys = s_play_keys[s_battle.me];
					keys.erase(keys.upper_bound(s_play.jump_check), keys.end());
					s_play_req = s_play.target >= 0 ? s_play.target : f;
				}
				s_play.jump_check = -1;
			}
		}
	} // namespace


	// Replay play: from the start state, each frame gets the file's synced inputs as a live frame without
	// rollback, so the battle socket HLE and both barriers run as live.
	// The file's state checks (replay.proto 52..55, optional): ReplayStateHash per frame, GameRng at frame 0 and at
	// each load end, of position s_play.rng_pos (RNG A is per machine). Checked before a frame's inputs (PlayFrame).
	// s_play.cut: [game end, next play start) and [round end, tick 6) of the file (replay.proto 57..59): not checked,
	// the peers differ there.
	// Battle loads (s_play_loads.battle): the frames where the tick state leaves 8, in order. Load 0 ends at MS select,
	// load 1 at the briefing, load 1 + N at the start of round N. Complete up to s_play.hi. A round jump to a known
	// start is a seek; to an unknown one it runs unlimited from s_play.hi until that load ends.
	// Round results (win_team 1 / 2, -1 = draw): the file's round_data (s_play_loads.file_rounds), and the ones played so
	// far (s_play_loads.results); s_round_rec = RoundRecord at s_play.tick_f.
	PlayState s_play;
	PlayLoads s_play_loads;
	std::mutex s_battle_loads_mtx;
	std::vector<std::vector<u8>> s_play_answers; // common start: the file's lobby answers (RbkBody)
	LimiterModeType s_play_rearm_limiter = LimiterModeType::Nominal;
	std::map<int, std::unique_ptr<PlayKey>> s_play_keys[GGPO_MAX_PLAYERS]; // by point of view (position)
	std::atomic<int> s_play_req{INT_MIN}; // requested seek target, INT_MIN = none
	// Skip MS selection: from frame 0 the replay runs unlimited to the briefing (the end of battle load 1).
	bool s_play_skip_ms = true;
	std::atomic<bool> s_keys_on{false}; // setting ZdxsvReplayKeyDisplay at the first PlayBegin, then the hotkey
	std::atomic<int> s_play_round_req{INT_MIN}; // requested round, 0 = briefing, INT_MIN = none
	bool g_replay_hidden_frame = false;
	bool g_replay_silent_frame = false;
	bool g_replay_step_hold = false;
	bool g_replay_fast_frame = false;

	namespace
	{
		// PlayFrame runs after the vsync queued its present: flags set for frame f apply to f's own present, which
		// shows what f drew from the list the frame before built.
		constexpr int SEEK_DRAWN_FRAMES = 2;
		struct PlayHideState
		{
			int drawn = 0;
		} s_hide;
		const bool s_seek_draw_skip = [] {
			const char* e = TestEnv("ZDXSV_REPLAY_DRAW_SKIP");
			return !(e && e[0] == '0');
		}();

		// Not the unlimited limiter mode: it changes the vsync mode, which can blank the window.
		void PlayHide(bool undrawn, bool fast)
		{
			g_rerun_draw_skip = g_rerun_vu1_skip = undrawn && s_seek_draw_skip;
			RerunKeepSeekFrame(g_rerun_draw_skip);
			g_replay_silent_frame = undrawn || fast;
			g_replay_fast_frame = undrawn || fast;
			s_hide.drawn = undrawn ? 0 : std::min(s_hide.drawn + 1, SEEK_DRAWN_FRAMES);
			g_replay_hidden_frame = s_hide.drawn < SEEK_DRAWN_FRAMES || fast;
		}

		void PlayHideReset()
		{
			g_rerun_draw_skip = g_rerun_vu1_skip = g_replay_silent_frame = g_replay_fast_frame = g_replay_hidden_frame = false;
			RerunKeepSeekFrame(false);
			s_hide = {};
			s_hide.drawn = SEEK_DRAWN_FRAMES;
		}
	} // namespace

	void PlayHideAfterLoad()
	{
		s_hide.drawn = 0;
	}

	void PlayKeyApply(const PlayKey& k)
	{
		s_rb = k.rb;
		s_rings = k.rings;
	}

	// The file's round state nearest at or before target, if past `from`: written over the key of the first load end.
	// Returns its frame, -1 if none was applied (nothing changed).
	int PlayRoundJump(int target, int from)
	{
		const auto& states = s_play.round_states;
		auto st = states.upper_bound(target);
		if (states.empty() || st == states.begin() || (--st)->first <= from || st == states.begin())
			return -1;
		const auto key = s_play_keys[s_battle.me].find(states.begin()->first);
		if (key == s_play_keys[s_battle.me].end())
			return -1;
		PlayKey& k = *key->second;
		if (k.zip.joinable())
			k.zip.join();
		Common::Timer timer;
		Error error;
		PlayStepRingClear();
		if (!k.ok || !ReplayLoadState(k.path.c_str(), &error) || (PlayKeyApply(k), !RoundStateApply(st->second, s_play.inputs)))
		{
			Console.Error("ZdxsvGgpo: replay round jump to %d failed: %s", st->first, error.GetDescription().c_str());
			return -1;
		}
		const int r = st->first;
		{
			std::lock_guard lock(s_battle_loads_mtx);
			for (const auto& [f, rng] : s_play.rngs)
				if (f > s_play.hi && f < r)
					s_play_loads.battle.push_back(f);
			// r is an MS-select load end: every round shown before it has ended (the cut also holds the rematch and MS-select
			// screens, so it is not counted)
			const size_t ended = std::count_if(s_play.spans.begin(), s_play.spans.end(), [r](const auto& s) { return s.first < r; });
			for (size_t i = s_play_loads.results.size(); i < std::min(ended, s_play_loads.file_rounds.size()); i++)
			{
				s_play_loads.results.push_back(s_play_loads.file_rounds[i]);
				s_play_loads.ms.push_back(i < s_play_loads.file_ms.size() ? s_play_loads.file_ms[i] : 0xffffffffu);
			}
		}
		if (s_play.hi < r - 1)
		{
			s_play.hi = r - 1;
			s_play.tick_f = r - 1;
			s_tick_st = TICK_LOAD;
			s_round_rec = RoundRecord();
		}
		if (s_play_round_bad)
			eeMem->Main[RNG_B] ^= 1;
		s_play.jump_check = r;
		Console.WriteLn("ZdxsvGgpo: replay round jump: key %d + round state %d (%.1f ms)", key->first, r, timer.GetTimeMilliseconds());
		return r;
	}

	// The first load end, if a round state past `from` would apply but its key is missing (a point of view not run
	// to it yet); -1 otherwise.
	int PlayRoundJumpFirst(int target, int from)
	{
		const auto& states = s_play.round_states;
		if (states.empty() || from >= states.begin()->first)
			return -1;
		const int first = states.begin()->first;
		auto st = states.upper_bound(target);
		if (st == states.begin() || (--st)->first <= first || s_play_keys[s_battle.me].contains(first))
			return -1;
		return first;
	}

	// Returns the frame to run next. pov_switch: s_battle.me just changed, so a key of it is always loaded (the
	// running state is the old point of view's).
	int PlaySeek(int target, bool pov_switch)
	{
		const u64 t0 = Common::Timer::GetCurrentValue();
		target = std::clamp(target, 0, s_play.frames - 1);
		const int next = s_battle.frame + 1;
		int from = next;
		bool loaded = false;
		auto& keys = s_play_keys[s_battle.me];
		// A key load resets the GS, so it has no picture until frames run after it.
		auto it = keys.upper_bound(target - SEEK_DRAWN_FRAMES);
		if (it == keys.begin())
			it = keys.upper_bound(target);
		if (const int l = (pov_switch || target >= next) ? -1 : PlayStepRingLoad(target); l >= 0)
		{
			from = l;
			loaded = true;
			it = keys.begin();
		}
		while (it != keys.begin())
		{
			--it;
			if (!pov_switch && target >= next && it->first <= next)
				break; // running on is as close
			PlayKey& k = *it->second;
			if (k.zip.joinable())
				k.zip.join();
			if (!k.ok)
				continue;
			Common::Timer timer;
			Error error;
			PlayStepRingClear();
			if (!ReplayLoadState(k.path.c_str(), &error))
			{
				Console.Error("ZdxsvGgpo: replay seek: key %d load failed: %s", it->first, error.GetDescription().c_str());
				return pov_switch ? -1 : next;
			}
			if (!s_play_key_nohle)
				PlayKeyApply(k);
			from = it->first;
			loaded = true;
			Console.WriteLn("ZdxsvGgpo: replay seek: key %d loaded (%.1f ms)", from, timer.GetTimeMilliseconds());
			break;
		}
		s_play.then = -1;
		if (const int r = PlayRoundJump(target, from); r >= 0)
		{
			from = r;
			loaded = true;
		}
		else if (const int first = PlayRoundJumpFirst(target, from); first >= 0)
		{
			Console.WriteLn("ZdxsvGgpo: replay seek: no key at the first load end %d, running there before the round jump", first);
			s_play.then = target;
			target = first;
		}
		if (from > target || (pov_switch && !loaded))
		{
			Console.Error("ZdxsvGgpo: replay seek %d -> %d: no key at or before it", s_battle.frame, target);
			return pov_switch ? -1 : next;
		}
		Console.WriteLn("ZdxsvGgpo: replay seek %d -> %d: from %d, %d frames to run", s_battle.frame, target, from, target - from);
		if (from < target)
		{
			s_play.target = target;
			s_play.seek_from = from;
			s_play.seek_t0 = t0;
		}
		return from;
	}

	// Runs unthrottled until load `load` has ended (after a seek to s_play.hi, if one was started).
	void PlayRunBegin(int load)
	{
		s_play.run_load = load;
		if (load == 1)
			Console.WriteLn("ZdxsvGgpo: replay skip MS selection: running to the briefing");
		else
			Console.WriteLn("ZdxsvGgpo: replay round %d: running to load %d", load - 1, load);
	}

	void PlayRunEnd(const char* why, int f)
	{
		if (s_play.run_load < 0)
			return;
		const int load = s_play.run_load;
		s_play.run_load = -1;
		g_rerun_draw_skip = g_rerun_vu1_skip = false;
		if (load == 1)
			Console.WriteLn("ZdxsvGgpo: replay skip MS selection: %s at frame %d, vsync %u", why, f, g_FrameCount);
		else
			Console.WriteLn("ZdxsvGgpo: replay round %d: %s at frame %d, vsync %u", load - 1, why, f, g_FrameCount);
	}
	TakeoverShared s_to_shared;
	Takeover s_to;
	int s_to_test_at = s_to_test ? std::atoi(s_to_test) : -1;
	int s_to_test_retry = [] {
		const char* e = TestEnv("ZDXSV_REPLAY_TAKEOVER_RETRY");
		return e ? std::atoi(e) : -1;
	}();

	// At T = the next frame (the running state = before T): keeps the state and the own sent msgs.
	bool TakeoverBegin(int t)
	{
		s_to.delay = s_min_delay;
		if (t < 1 || t + s_to.delay > s_play.frames)
		{
			Console.Error("ZdxsvGgpo: replay takeover at frame %d: not within the replay (%d frames)", t, s_play.frames);
			return false;
		}
		s_to.key = PlayKeyMake(t, fmt::format("zdxsv-replay-takeover-p{}.p2s", s_battle.me));
		if (!s_to.key)
			return false;
		const size_t pos = std::min(s_rb.net_pos, s_battle.sent.size());
		s_to.sent.assign(s_battle.sent.begin(), s_battle.sent.begin() + pos);
		s_to.sent_at.assign(s_battle.sent_at.begin(), s_battle.sent_at.begin() + pos);
		// the own msg bytes of the file's inputs up to T + delay - 1 = the oldest kind-3 sends, in order
		size_t bytes = 0;
		u8 seq = 0;
		for (int f = 0; f < t + s_to.delay; f++)
		{
			const NetInput& in = PlayRow(f, s_battle.me);
			if (in.seq != seq)
				bytes += in.len;
			seq = in.seq;
		}
		s_to.out.clear();
		for (size_t i = 0; i < pos; i++)
		{
			const std::vector<u8>& m = s_to.sent[i];
			if (m.size() < 2 || (m[1] >> 4) != 3)
				continue;
			if (s_to.out.empty() && (m.size() > sizeof(NetInput::data) || m.size() <= bytes))
			{
				if (m.size() <= sizeof(NetInput::data))
					bytes -= m.size();
				continue;
			}
			s_to.out.push_back({s_to.sent_at[i], i, m});
		}
		s_to.local = PlayRow(t + s_to.delay - 1, s_battle.me);
		s_to.frame = t;
		s_to_shared.target = PlayOwnB(t);
		Console.WriteLn("ZdxsvGgpo: replay takeover at frame %d, delay %d, own sends %zu, kind-3 not in an input %zu%s", t,
			s_to.delay, pos, s_to.out.size(), bytes ? " (msg bytes of the file left over)" : "");
		return true;
	}

	// Back to the state before T with the own sent msgs of then. Returns T, or -1.
	int TakeoverLoad()
	{
		PlayKey& k = *s_to.key;
		if (k.zip.joinable())
			k.zip.join();
		Error error;
		if (!k.ok || !ReplayLoadState(k.path.c_str(), &error))
		{
			Console.Error("ZdxsvGgpo: replay takeover: state load failed: %s", error.GetDescription().c_str());
			return -1;
		}
		PlayKeyApply(k);
		s_battle.sent = s_to.sent;
		s_battle.sent_at = s_to.sent_at;
		s_battle.out.clear();
		s_to.start_held = true; // a START held now retries on its release + press only
		return s_to.frame;
	}

	void TakeoverStart()
	{
		s_battle.sent = s_to.sent;
		s_battle.sent_at = s_to.sent_at;
		s_battle.out = s_to.out;
		s_battle.local = s_to.local;
		s_to.queue.clear();
		for (int i = 0; i < s_to.delay; i++)
			s_to.queue.push_back(PlayRow(s_to.frame + i, s_battle.me));
		s_to_shared.phase = TO_ON;
		Console.WriteLn("ZdxsvGgpo: replay takeover starts at frame %d, vsync %u", s_to.frame, g_FrameCount);
	}

	void TakeoverOff(const char* why)
	{
		s_to_shared.phase = TO_OFF;
		s_to.queue.clear();
		s_to.key.reset();
		Console.WriteLn("ZdxsvGgpo: replay takeover %s at frame %d, vsync %u", why, s_battle.frame, g_FrameCount);
	}

	void PlayFrame(int f)
	{
		if (s_to_shared.phase == TO_ON)
		{
			PlayHide(false, false);
			PlayFrameTakeover(f);
			return;
		}
		PlayTrackLoads(f);
		if (f == s_play.target)
		{
			s_play.target = -1;
			const double ms = Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - s_play.seek_t0);
			const int n = f - s_play.seek_from;
			Console.WriteLn("ZdxsvGgpo: replay seek done at frame %d, vsync %u: %d frames from key %d in %.1f ms, %.2f ms/frame",
				f, g_FrameCount, n, s_play.seek_from, ms, n > 0 ? ms / n : 0.0);
			if (const int then = std::exchange(s_play.then, -1); then >= 0)
				s_play_req = then;
			else if (std::exchange(s_play.step_pause, false) && PlayStepPaused(f))
				VMManager::SetPaused(true);
		}
		const bool running = s_play.target >= 0 || s_play.run_load >= 0;
		const int fast_every = PlayFastEvery();
		PlayHide((s_play.target >= 0 && f < s_play.target - SEEK_DRAWN_FRAMES) || s_play.run_load >= 0,
			!running && fast_every > 1 && f % fast_every != 0);
		if (!running)
			PlaySlowWait();
		PlayKeySave(f);
		PlayStepRingSave(f);
		ReplayBenchSave(f);
		s_battle.frame = f;
		PlayBarPublish();
		PlayKeysPublish(f);
		NetSaved(f);
		const NetInput* in = &s_play.inputs[static_cast<size_t>(f) * s_opt.players];
		u16 ab[2];
		std::memcpy(ab, &in[s_battle.me].pad, sizeof(ab));
		s_rings.zd_pad[f & 127] = PadFromB(ab[1]);
		PlayCheckState(f);
		if (s_play_round_take && s_play.select_starts.contains(f) && !s_play.round_states.contains(f))
			s_play.round_states.emplace(f, RoundStateTake(f));
		NetApply(in);
	}

	// The file released a load barrier with frame f - 1's inputs, this machine's load step is not there yet: the
	// load's length is machine-local (state the common start does not carry). With no other position to wait for
	// (a 1-player battle) the game would take every later input that many frames late. So the file stays at f - 1,
	// its barrier counts applied each frame, until the own hold is released. Needs proper testing with more players.
	bool PlayLoadWait(int f)
	{
		const auto ahead = [f](const std::vector<u8>& file, const PS& b) {
			return f < static_cast<int>(file.size()) && static_cast<u8>(file[f] - b.rel - 1) < 127;
		};
		const bool late = s_to_shared.phase != TO_ON && f == s_battle.frame + 1 &&
						  (ahead(s_play.ps_rel, s_rb.ps) || (s_ms_on && ahead(s_play.ms_rel, s_rb.ms)));
		if (!late)
		{
			if (s_check.wait_run > 0 && s_check.wait_run <= LOAD_WAIT_MAX)
				Console.WriteLn("ZdxsvGgpo: replay own load late by %d frames at frame %d: the file waited", s_check.wait_run, s_battle.frame);
			s_check.wait_run = 0;
			return false;
		}
		if (s_check.wait_run >= LOAD_WAIT_MAX)
		{
			if (s_check.wait_run++ == LOAD_WAIT_MAX)
				Console.Warning("ZdxsvGgpo: replay own load step not reached in %d frames at frame %d: playing on", LOAD_WAIT_MAX, s_battle.frame);
			return false;
		}
		if (s_check.wait_run++ == 0)
			s_check.wait_loads++;
		s_check.wait++;
		s_check.wait_max = std::max(s_check.wait_max, s_check.wait_run);
		NetRelease(&s_play.inputs[static_cast<size_t>(s_battle.frame) * s_opt.players]);
		return true;
	}

	void PlayReport(const char* what)
	{
		Console.WriteLn("ZdxsvGgpo: replay %s at frame %d of %d, vsync %u", what, s_battle.frame, s_play.frames, g_FrameCount);
		Console.WriteLn("ZdxsvGgpo: replay state check: hashes %d checked, %d differ (first at frame %d); rngs %d checked, %d differ; "
						"%d frames after a round or game end not checked; own load late %d frames (%d loads, max %d), not checked; "
						"file waited for own loads %d frames (%d loads, max %d)",
			s_check.hashes, s_check.hash_bad, s_check.hash_first_bad, s_check.rngs, s_check.rng_bad, s_check.hash_cut,
			s_check.late, s_check.late_loads, s_check.late_max, s_check.wait, s_check.wait_loads, s_check.wait_max);
		NetReport();
	}

	void PlayStop(const char* what)
	{
		g_ggpo_active = false;
		g_replay_step_hold = false;
		PlayStepRingClear();
		PlayHideReset();
		s_bar.frame = -1;
		s_battle.over = true; // the battle sock goes back to the IOP
		ServerPatchesEnd();
		PlayReport(what);
		if (const char* e = TestEnv("ZDXSV_REPLAY_EXIT"); g_replay_child || (e && e[0] == '1'))
			Host::RunOnCPUThread([] { Host::RequestVMShutdown(false, false, false); });
		else if (!s_play.at_end)
			Host::RunOnCPUThread([] { VMManager::SetPaused(true); });
	}

	u32 s_play_gen = 0;

	void PlayKeysPrune()
	{
		FileSystem::FindResultsArray old;
		FileSystem::FindFiles(EmuFolders::Cache.c_str(), "zdxsv-replay-*.p2s*", FILESYSTEM_FIND_FILES, &old);
		for (const FILESYSTEM_FIND_DATA& f : old)
			FileSystem::DeleteFilePath(f.FileName.c_str());
	}

	void PlayReset()
	{
		s_play_gen++;
		PlayFetchStop();
		SyncStop();
		ReplayBenchReset();
		s_sync_chase = false;
		for (auto& keys : s_play_keys)
		{
			for (auto& [f, k] : keys)
			{
				if (k->zip.joinable())
					k->zip.join();
				FileSystem::DeleteFilePath(k->path.c_str());
			}
			keys.clear();
		}
		s_live_down.reset();
		LivePaceReset();
		s_live_wait.reset();
		s_live_play = {};
		s_play = {};
		s_check = {};
		s_play_req = INT_MIN;
		{
			std::lock_guard lock(s_battle_loads_mtx);
			s_play_loads = {};
		}
		s_round_rec = 0;
		s_play_round_req = INT_MIN;
		PlayHideReset();
		s_play_pov_req = -1;
		s_play_pov_at.clear();
		std::fill(std::begin(s_play_sent), std::end(s_play_sent), PlaySent{});
		s_bar.frame = -1;
		s_bar.frames = 0;
		s_bar.pov = s_bar.target = -1;
		s_bar.povs = 0;
		s_bar.lo = s_bar.hi = s_bar.rounds = 0;
		s_bar.round = -1;
		ReplayControlsReset();
		s_to_shared.phase = TO_OFF;
		s_to_shared.req = TO_REQ_NONE;
		s_to_shared.target = 0;
		s_to = {};
	}

	namespace
	{
		// within the 64 frames of FrameRings::net_seq_at, which a load leaves in place
		constexpr int STEP_RING_FRAMES = 60;

		struct
		{
			bool resume = false;
			bool ring = false;
			Common::Timer::Value t0 = 0;
		} s_play_step;

		void PlayRunFromPause()
		{
			if (VMManager::GetState() != VMState::Paused)
				return;
			if (s_play.at_end)
			{
				VMManager::SetPaused(false);
				return;
			}
			s_play.step_pause = true;
			PlayStepResume();
		}
	} // namespace

	void PlayStepRingClear()
	{
		if (std::exchange(s_play_step.ring, false))
			DeltaStateClear();
	}

	// While stepping, the last frames are kept as delta states, so a step back loads one instead of a key file.
	// Their EE RAM tracking costs every frame: off outside steps.
	void PlayStepRingSave(int f)
	{
		if (!g_replay_step_hold || (s_play.target >= 0 && f < s_play.target - STEP_RING_FRAMES))
		{
			PlayStepRingClear();
			return;
		}
		if (DeltaStateGetState(f) || !DeltaStateSave(f))
			return;
		s_play_step.ring = true;
		DeltaStateDiscardBefore(f - STEP_RING_FRAMES);
	}

	// The newest ring frame in (target - STEP_RING_FRAMES, target - 1], loaded; -1 if none.
	// Not target itself: a ring state has no GS memory, and the picture shown at a vsync was drawn in the
	// frame before it, so at least 2 frames must run after the load to show the target's picture.
	int PlayStepRingLoad(int target)
	{
		if (!s_play_step.ring || !g_replay_step_hold)
			return -1;
		for (int l = target - 1; l > target - STEP_RING_FRAMES && l >= 0; l--)
		{
			if (!DeltaStateGetState(l))
				continue;
			Common::Timer timer;
			if (!DeltaStateLoad(l))
			{
				PlayStepRingClear();
				return -1;
			}
			s_rb = s_net_at[l & 127];
			Console.WriteLn("ZdxsvGgpo: replay seek: step frame %d loaded (%.1f ms)", l, timer.GetTimeMilliseconds());
			return l;
		}
		return -1;
	}

	void PlayStepResume()
	{
		// A pause at a frame leaves that frame's present queued for the next vsync, which comes before the seek.
		g_replay_hidden_frame = true;
		g_replay_step_hold = true;
		s_play_step.resume = true;
		s_play_step.t0 = Common::Timer::GetCurrentValue();
		VMManager::SetPaused(false);
		s_play_step.resume = false;
	}

	std::atomic<int> s_play_step_queue{0};

	bool PlayStepPaused(int f)
	{
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		Console.WriteLn("ZdxsvGgpo: replay step paused at frame %d, vsync %u: %.1f ms after the request", f, g_FrameCount,
			Common::Timer::ConvertValueToMilliseconds(now - s_play_step.t0));
		int q = s_play_step_queue.load();
		while (q != 0 && !s_play_step_queue.compare_exchange_weak(q, q - (q > 0 ? 1 : -1)))
			;
		if (q == 0 || s_play.at_end)
		{
			s_play_step_queue = 0;
			return true;
		}
		s_play_req = f + 1 + (q < 0 ? -2 : 0);
		s_play.step_pause = true;
		s_play_step.t0 = now;
		return false;
	}

	bool ReplayStepHoldsAudio(bool paused)
	{
		if (!paused && !s_play_step.resume)
			g_replay_step_hold = false;
		return g_replay_step_hold;
	}

	void ReplaySeekBy(int frames)
	{
		if (!s_play_src.path || s_play.frames <= 0 || !g_ggpo_active)
			return;
		const int req = s_play_req.load();
		s_play_req = (req != INT_MIN ? req : s_battle.frame + 1) + frames;
		Console.WriteLn("ZdxsvGgpo: replay seek requested: frame %d", s_play_req.load());
		PlayRunFromPause();
	}

	void ReplaySeekTo(int frame)
	{
		if (!s_play_src.path || s_play.frames <= 0 || !g_ggpo_active)
			return;
		s_play_req = std::clamp(frame, 0, s_play.frames - 1);
		Console.WriteLn("ZdxsvGgpo: replay seek requested: frame %d", s_play_req.load());
		PlayRunFromPause();
	}

	void ReplayTogglePause()
	{
		if (!s_play_src.path || s_play.frames <= 0 || !g_ggpo_active)
			return;
		if (s_play.at_end)
			ReplaySeekTo(0); // play at the end = from the start
		else if (s_to_shared.phase == TO_COUNT)
			ReplayTakeoverCancel();
		else
			VMManager::SetPaused(VMManager::GetState() != VMState::Paused);
	}

	bool ReplayBarInfo(int& frame, int& frames, int& pov, u32& povs, int& target)
	{
		frame = s_bar.frame;
		if (!s_play_src.path || frame < 0)
			return false;
		frames = s_bar.frames;
		pov = s_bar.pov;
		povs = s_bar.povs;
		target = s_bar.target;
		return true;
	}

	bool ReplayCanTakeOver()
	{
		return s_bar.battle;
	}

	bool ReplayBarSpan(int& lo, int& hi, int& round, int& rounds)
	{
		if (!s_play_src.path || s_bar.frame < 0)
			return false;
		lo = s_bar.lo;
		hi = s_bar.hi;
		round = s_bar.round;
		rounds = s_bar.rounds;
		return true;
	}

	bool ReplayKeys(std::vector<std::pair<u16, int>>& runs)
	{
		if (!s_keys_on || s_bar.frame < 0)
			return false;
		std::lock_guard lock(s_keys.mtx);
		runs.assign(s_keys.runs.begin(), s_keys.runs.end());
		return true;
	}

	void ReplayToggleKeys()
	{
		if (!s_play_src.path || s_play.frames <= 0 || !g_ggpo_active)
			return;
		s_keys_on = !s_keys_on;
		Console.WriteLn("ZdxsvGgpo: replay key display %s", s_keys_on ? "on" : "off");
		{
			std::lock_guard lock(s_keys.mtx);
			s_keys.frame = -1; // rebuilt now: a paused replay plays no frame
		}
		PlayKeysPublish(s_battle.frame);
	}

	void ReplayJumpRound(int delta)
	{
		if (!s_play_src.path || s_play.frames <= 0 || !g_ggpo_active)
			return;
		int round = s_play_round_req.load();
		const int f = s_play.target >= 0 ? s_play.target : s_battle.frame;
		const int span = PlaySpanAt(f);
		if (round == INT_MIN && PlayLiveNextRound(f))
			round = static_cast<int>(s_play.spans.size()) + 1;
		else if (round == INT_MIN && span >= 0)
			round = span + 1;
		else if (round == INT_MIN)
		{
			round = 0; // rounds started at or before the shown frame
			for (size_t k = 2; k < s_play_loads.battle.size() && s_play_loads.battle[k] <= f; k++)
				round++;
		}
		// an open live stream: one past the last shown round = the round not started yet (the newest frames)
		const int last = static_cast<int>(s_play.spans.size()) + (s_live_down && s_live_play.close.empty() ? 1 : 0);
		s_play_round_req = span >= 0 ? std::clamp(round + delta, 1, last) : std::max(0, round + delta);
		Console.WriteLn("ZdxsvGgpo: replay round requested: %d", s_play_round_req.load());
		PlayRunFromPause();
	}

	std::vector<int> ReplayRoundStarts()
	{
		std::lock_guard lock(s_battle_loads_mtx);
		return s_play_loads.battle.size() > 2 ? std::vector<int>(s_play_loads.battle.begin() + 2, s_play_loads.battle.end()) : std::vector<int>();
	}

	std::string ReplayRoundResults()
	{
		const int pov = s_bar.pov, players = s_opt.players;
		if (pov < 0 || players < 1)
			return {};
		const int team = pov < players / 2 ? 1 : 2; // team 1 = the first half of the positions
		std::lock_guard lock(s_battle_loads_mtx);
		const std::vector<int>& rounds = s_play_loads.file_rounds.empty() ? s_play_loads.results : s_play_loads.file_rounds;
		std::string s;
		for (const int win : rounds)
			s += win == team ? 'W' : win < 0 ? 'D' : 'L';
		return s;
	}

	std::vector<u32> ReplayRoundMs(int& players)
	{
		players = s_opt.players;
		std::lock_guard lock(s_battle_loads_mtx);
		return s_play_loads.file_rounds.empty() ? s_play_loads.ms : s_play_loads.file_ms;
	}
} // namespace Zdxsv
