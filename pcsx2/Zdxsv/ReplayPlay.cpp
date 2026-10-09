// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Replay playback: frame loop, seek keys, point of view, takeover, the control bar and its hotkeys.

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{
	const char* const s_to_test = std::getenv("ZDXSV_REPLAY_TAKEOVER");

	namespace
	{
		int s_play_hash_checks = 0, s_play_hash_bad = 0, s_play_hash_first_bad = -1, s_play_rng_checks = 0, s_play_rng_bad = 0;
		int s_play_hash_cut = 0;
		const int s_play_key_every = [] {
			const char* e = std::getenv("ZDXSV_REPLAY_KEY");
			return e ? std::atoi(e) : 600;
		}();
		const bool s_play_key_nohle = std::getenv("ZDXSV_REPLAY_KEY_NOHLE") != nullptr;
		u8 s_tick_st = 0;
		u32 s_round_rec = 0;
		// control bar (ReplayBarInfo): written per played frame on the CPU thread, read by the GS thread's ImGui
		std::atomic<int> s_bar_frame{-1}; // -1 = no replay playing
		std::atomic<int> s_bar_frames{0}, s_bar_pov{-1}, s_bar_target{-1};
		std::atomic<u32> s_bar_povs{0}; // bit p = position p has a file

		void PlayBarPublish()
		{
			u32 povs = 0;
			for (int p = 0; p < s_players; p++)
				povs |= s_play_pov_ok[p] ? (1u << p) : 0u;
			s_bar_frames = s_play_frames;
			s_bar_pov = s_net_me;
			s_bar_povs = povs;
			s_bar_target = s_play_target;
			s_bar_frame = s_net_frame;
		}

		// key display (ReplayKeys): runs of the shown position's B word (ZdPadAB) up to the played frame, newest
		// first, with their frame counts. Per frame one more frame; after a seek or a switch, rebuilt from the file.
		static constexpr size_t KEY_RUNS = 14;
		std::mutex s_keys_mtx;
		std::deque<std::pair<u16, int>> s_keys; // guarded by s_keys_mtx (the GS thread draws it)
		int s_keys_f = -1, s_keys_me = -1; // frame and position s_keys ends at

		u16 PlayB(int f, int p)
		{
			u16 ab[2];
			std::memcpy(ab, &s_play_inputs[static_cast<size_t>(f) * s_players + p].pad, sizeof(ab));
			return ab[1] & ~1u; // bit 0 = game state, not a key
		}

		void PlayKeysPublish(int f)
		{
			if (!s_keys_on || f < 0 || f >= s_play_frames)
				return;
			std::lock_guard lock(s_keys_mtx);
			if (f == s_keys_f + 1 && s_net_me == s_keys_me)
			{
				const u16 b = PlayB(f, s_net_me);
				if (!s_keys.empty() && s_keys.front().first == b)
					s_keys.front().second++;
				else
				{
					s_keys.emplace_front(b, 1);
					if (s_keys.size() > KEY_RUNS)
						s_keys.pop_back();
				}
			}
			else if (f != s_keys_f || s_net_me != s_keys_me)
			{
				s_keys.clear();
				for (int g = f; g >= 0; g--)
				{
					const u16 b = PlayB(g, s_net_me);
					if (!s_keys.empty() && s_keys.back().first == b)
						s_keys.back().second++;
					else if (s_keys.size() == KEY_RUNS)
						break;
					else
						s_keys.emplace_back(b, 1);
				}
			}
			s_keys_f = f;
			s_keys_me = s_net_me;
			if (f % 600 == 0) // test drivers (zdxsv keycheck.py) compare these with the file
			{
				std::string s;
				for (const auto& [b, n] : s_keys)
					s += fmt::format(" {:04x}*{}", b, n);
				Console.WriteLn("ZdxsvGgpo: replay keys frame %d pos %d:%s", f, s_net_me, s.c_str());
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
			if (s_play_key_every <= 0 || f % s_play_key_every != 0 || s_play_keys[s_net_me].contains(f))
				return;
			Common::Timer timer;
			std::unique_ptr<PlayKey> key = PlayKeyMake(f, fmt::format("zdxsv-replay-key-p{}-{}.p2s", s_net_me, f));
			if (!key)
				return;
			s_play_keys[s_net_me].emplace(f, std::move(key));
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
			if (f > s_play_hi)
			{
				if (f == s_tick_f + 1 && s_tick_st == TICK_LOAD && st != TICK_LOAD)
				{
					std::lock_guard lock(s_battle_loads_mtx);
					s_battle_loads.push_back(f);
					Console.WriteLn("ZdxsvGgpo: replay: load %zu ends at frame %d, vsync %u", s_battle_loads.size() - 1, f, g_FrameCount);
				}
				if (f == s_tick_f + 1)
				{
					if (const int win = RoundResult(s_round_rec, RoundRecord()))
					{
						std::lock_guard lock(s_battle_loads_mtx);
						const size_t i = s_round_results.size();
						s_round_results.push_back(win);
						Console.WriteLn("ZdxsvGgpo: replay: round result %zu: win_team %d at frame %d, MS ids %08x", i + 1, win, f - 1, MsIds());
						if (i < s_play_file_rounds.size() && s_play_file_rounds[i] != win)
							Console.Error("ZdxsvGgpo: replay: round result %zu differs from the file's win_team %d", i + 1, s_play_file_rounds[i]);
					}
				}
				s_play_hi = f;
			}
			s_tick_f = f;
			s_tick_st = st;
			s_round_rec = RoundRecord();
			if (s_run_load >= 0 && s_run_load < static_cast<int>(s_battle_loads.size()) && s_battle_loads[s_run_load] == f)
				PlayRunEnd(s_run_load == 1 ? "briefing" : "start", f);
		}
		int s_to_delay = 0;
		std::unique_ptr<PlayKey> s_to_key; // the state before T
		std::vector<std::vector<u8>> s_to_sent; // own sends before T
		std::vector<int> s_to_sent_at;
		std::deque<NetOut> s_to_out; // own kind-3 msgs before T not in the file's own inputs up to T + delay - 1
		NetInput s_to_local; // the file's own input at T + delay - 1
		std::deque<NetInput> s_to_queue; // packed own inputs, `delay` frames ahead
		Common::Timer::Value s_to_count_t0 = 0;
		const char s_to_test_src = [] {
			const char* c = s_to_test ? std::strchr(s_to_test, ':') : nullptr;
			const std::string_view src = c ? c + 1 : "";
			return src == "replay" ? 'r' : src == "rand" ? 'n' : '\0';
		}();
		std::mt19937 s_to_rng{1};

		const NetInput& PlayRow(int f, int p) { return s_play_inputs[static_cast<size_t>(f) * s_players + p]; }

		u16 PlayOwnB(int f)
		{
			if (f < 0 || f >= s_play_frames)
				return 0;
			u16 ab[2];
			std::memcpy(ab, &PlayRow(f, s_net_me).pad, sizeof(ab));
			return ab[1];
		}

		Input TakeoverPad(int f)
		{
			if (s_to_test_src == 'r')
				return PadFromB(PlayOwnB(f + s_to_delay));
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
			s_net_frame = f;
			PlayBarPublish();
			NetSaved(f);
			NetInput in[GGPO_MAX_PLAYERS];
			std::memcpy(in, &PlayRow(std::min(f, s_play_frames - 1), 0), sizeof(NetInput) * s_players);
			if (f >= s_play_frames)
				for (int p = 0; p < s_players; p++)
					std::memset(&in[p].pad, 0, sizeof(u16) * 2); // (A, B); seq unchanged = no new msgs
			s_to_queue.push_back(NetPack(TakeoverPad(f)));
			in[s_net_me] = s_to_queue.front();
			s_to_queue.pop_front();
			NetApply(in);
		}

		// The file's state hash and RNGs of frame f against the emulated state (a desync check; nothing changes).
		void PlayCheckState(int f)
		{
			const bool cut = std::any_of(s_play_cut.begin(), s_play_cut.end(), [f](const auto& c) { return f >= c.first && f < c.second; });
			if (cut && f < static_cast<int>(s_play_hashes.size()))
				s_play_hash_cut++;
			else if (f < static_cast<int>(s_play_hashes.size()))
			{
				s_play_hash_checks++;
				if (const u32 h = ReplayStateHash(); h != s_play_hashes[f])
				{
					if (s_play_hash_bad++ < 10)
						Console.Warning("ZdxsvGgpo: replay state hash differs at frame %d: file %08x, here %08x", f, s_play_hashes[f], h);
					if (s_play_hash_first_bad < 0)
						s_play_hash_first_bad = f;
				}
			}
			if (const auto it = s_play_rngs.find(f); !cut && it != s_play_rngs.end() && s_net_me == s_play_rng_pos)
			{
				// judged on RNG B (low half): RNG A also differs from the recorder's under the common start
				s_play_rng_checks++;
				const u32 rng = GameRng();
				const bool bad = static_cast<u16>(rng) != static_cast<u16>(it->second);
				if (bad)
					s_play_rng_bad++;
				Console.WriteLn("ZdxsvGgpo: replay rng at frame %d: file %08x, here %08x%s", f, it->second, rng, bad ? " DIFFERS" : "");
			}
		}
	} // namespace


	// ZDXSV_REPLAY (s_play_env): the file's frame 0 state is loaded, the HLE state of frame 0 restored
	// (net_rx0 / hle0), then each frame gets the file's synced inputs (NetApply, as a live frame without
	// rollback): the battle sock HLE, zd step copy and both barriers run as live. Own pad 0 = the own
	// position's B bits mapped back to buttons (no sticks in the file). ZDXSV_REPLAY_EXIT=1: exit at the
	// end (else pause); ZDXSV_REPLAY_TURBO=1: turbo limiter. With ZDXSV_PW_HASH + ZDXSV_NET_TRACE the H lines
	// compare to the live battle's (zdxsv/pwcheck.py).
	std::vector<NetInput> s_play_inputs;
	// The file's state checks (replay.proto 52..55, optional): ReplayStateHash per frame, GameRng at frame 0 and at
	// each load end, of position s_play_rng_pos (RNG A is per machine). Checked before a frame's inputs (PlayFrame).
	std::vector<u32> s_play_hashes;
	std::map<int, u32> s_play_rngs; // frame -> GameRng
	int s_play_rng_pos = -1;
	// [game end, next play start) and [round end, tick 6) of the file (replay.proto 57..59): not checked, the peers differ there
	std::vector<std::pair<int, int>> s_play_cut;
	int s_play_frames = 0;
	std::vector<std::vector<u8>> s_play_answers; // common start: the file's lobby answers (RbkBody)
	int s_play_rearm_seek = -1; // a switch's common start: the frame to seek to from its key 0
	LimiterModeType s_play_rearm_limiter = LimiterModeType::Nominal; // and the limiter before it
	bool s_play_booted_saved = false; // ZDXSV_REPLAY_STATE empty: the booted state is in the cache (PlayCommonState)
	std::map<int, std::unique_ptr<PlayKey>> s_play_keys[GGPO_MAX_PLAYERS]; // by point of view (position)
	std::deque<std::pair<int, int>> s_play_seeks; // ZDXSV_REPLAY_SEEK
	std::atomic<int> s_play_req{INT_MIN}; // requested seek target, INT_MIN = none
	bool s_play_at_end = false; // paused after the last frame (no ZDXSV_REPLAY_EXIT)
	int s_play_target = -1; // seeking: frames run unlimited up to this one
	LimiterModeType s_play_limiter = LimiterModeType::Nominal;
	// skip MS selection (setting ZdxsvReplaySkipMs, ZDXSV_REPLAY_SKIP_MS; default on as flycast's
	// gdxsv:ReplaySkipMsSelection; read at each replay start in PlayBegin): from
	// frame 0 the replay runs unlimited to the briefing = the frame tick state 0xc627b4 leaves 8 (battle load) the
	// 2nd time (load 1 below; round 2 loads again with no MS select). Playing from frame 0 again jumps to the
	// briefing.
	bool s_play_skip_ms = true;
	std::atomic<bool> s_keys_on{false}; // setting ZdxsvReplayKeyDisplay at the first PlayBegin, then the hotkey
	// Battle loads: the frames where the tick state leaves 8, in order. Load 0 ends at MS select, load 1 at the
	// briefing, load 1 + N at the start of round N (a 2-round 1v1: 216, 4184, 4945, 16103, the same
	// frames for both positions). Frames are played in order up to s_play_hi (a forward seek runs every frame between), so
	// the list is complete up to it. Round jump (ZDXSV_REPLAY_ROUND_AT, hotkeys, control bar): a known round
	// start is a seek; an unknown one runs unlimited from s_play_hi until that load ends.
	std::mutex s_battle_loads_mtx;
	std::vector<int> s_battle_loads; // written on the CPU thread; the GS thread reads it under s_battle_loads_mtx
	int s_play_hi = -1, s_tick_f = -1;
	// Round results (win_team 1 / 2, -1 = draw): the file's round_data, and the ones played so far (also under
	// s_battle_loads_mtx); s_round_rec = RoundRecord at s_tick_f
	std::vector<int> s_play_file_rounds, s_round_results;
	int s_run_load = -1; // running unlimited until this load has ended, -1 = none (skip MS selection = 1)
	std::atomic<int> s_play_round_req{INT_MIN}; // requested round, 0 = briefing, INT_MIN = none
	std::deque<std::pair<int, int>> s_play_round_at; // ZDXSV_REPLAY_ROUND_AT=frame:round,...
	bool s_play_pov_ok[GGPO_MAX_PLAYERS] = {};

	void PlayKeyApply(const PlayKey& k)
	{
		s_rb = k.rb;
		s_rings = k.rings;
	}

	// Returns the frame to run next. pov_switch: s_net_me just changed, so a key of it is always loaded (the
	// running state is the old point of view's).
	int PlaySeek(int target, bool pov_switch)
	{
		target = std::clamp(target, 0, s_play_frames - 1);
		const int next = s_net_frame + 1;
		int from = next;
		bool loaded = false;
		auto& keys = s_play_keys[s_net_me];
		auto it = keys.upper_bound(target);
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
			if (!VMManager::LoadState(k.path.c_str(), &error))
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
		if (from > target || (pov_switch && !loaded))
		{
			Console.Error("ZdxsvGgpo: replay seek %d -> %d: no key at or before it", s_net_frame, target);
			return pov_switch ? -1 : next;
		}
		Console.WriteLn("ZdxsvGgpo: replay seek %d -> %d: from %d, %d frames to run", s_net_frame, target, from, target - from);
		if (from < target)
		{
			if (s_play_target < 0)
				s_play_limiter = VMManager::GetLimiterMode();
			s_play_target = target;
			VMManager::SetLimiterMode(LimiterModeType::Unlimited);
		}
		return from;
	}

	// Runs unlimited until load `load` has ended (after a seek to s_play_hi, if one was started).
	void PlayRunBegin(int load)
	{
		if (s_play_target < 0) // else the seek saved it
			s_play_limiter = VMManager::GetLimiterMode();
		s_run_load = load;
		VMManager::SetLimiterMode(LimiterModeType::Unlimited);
		if (load == 1)
			Console.WriteLn("ZdxsvGgpo: replay skip MS selection: running to the briefing");
		else
			Console.WriteLn("ZdxsvGgpo: replay round %d: running to load %d", load - 1, load);
	}

	void PlayRunEnd(const char* why, int f)
	{
		if (s_run_load < 0)
			return;
		const int load = s_run_load;
		s_run_load = -1;
		if (s_play_target < 0)
			VMManager::SetLimiterMode(s_play_limiter);
		if (load == 1)
			Console.WriteLn("ZdxsvGgpo: replay skip MS selection: %s at frame %d, vsync %u", why, f, g_FrameCount);
		else
			Console.WriteLn("ZdxsvGgpo: replay round %d: %s at frame %d, vsync %u", load - 1, why, f, g_FrameCount);
	}
	std::atomic<int> s_to_phase{TO_OFF};
	std::atomic<int> s_to_req{TO_REQ_NONE};
	std::atomic<u16> s_to_target{0}; // own B of the file at T
	int s_to_frame = -1; // T
	bool s_to_skip = false, s_to_start_held = false;
	int s_to_test_at = s_to_test ? std::atoi(s_to_test) : -1;
	int s_to_test_retry = [] {
		const char* e = std::getenv("ZDXSV_REPLAY_TAKEOVER_RETRY");
		return e ? std::atoi(e) : -1;
	}();

	// At T = the next frame (the running state = before T): keeps the state and the own sent msgs.
	bool TakeoverBegin(int t)
	{
		s_to_delay = s_min_delay;
		if (t < 1 || t + s_to_delay > s_play_frames)
		{
			Console.Error("ZdxsvGgpo: replay takeover at frame %d: not within the replay (%d frames)", t, s_play_frames);
			return false;
		}
		s_to_key = PlayKeyMake(t, fmt::format("zdxsv-replay-takeover-p{}.p2s", s_net_me));
		if (!s_to_key)
			return false;
		const size_t pos = std::min(s_rb.net_pos, s_net_sent.size());
		s_to_sent.assign(s_net_sent.begin(), s_net_sent.begin() + pos);
		s_to_sent_at.assign(s_net_sent_at.begin(), s_net_sent_at.begin() + pos);
		// the own msg bytes of the file's inputs up to T + delay - 1 = the oldest kind-3 sends, in order
		size_t bytes = 0;
		u8 seq = 0;
		for (int f = 0; f < t + s_to_delay; f++)
		{
			const NetInput& in = PlayRow(f, s_net_me);
			if (in.seq != seq)
				bytes += in.len;
			seq = in.seq;
		}
		s_to_out.clear();
		for (size_t i = 0; i < pos; i++)
		{
			const std::vector<u8>& m = s_to_sent[i];
			if (m.size() < 2 || (m[1] >> 4) != 3)
				continue;
			if (s_to_out.empty() && (m.size() > sizeof(NetInput::data) || m.size() <= bytes))
			{
				if (m.size() <= sizeof(NetInput::data))
					bytes -= m.size();
				continue;
			}
			s_to_out.push_back({s_to_sent_at[i], i, m});
		}
		s_to_local = PlayRow(t + s_to_delay - 1, s_net_me);
		s_to_frame = t;
		s_to_target = PlayOwnB(t);
		Console.WriteLn("ZdxsvGgpo: replay takeover at frame %d, delay %d, own sends %zu, kind-3 not in an input %zu%s", t,
			s_to_delay, pos, s_to_out.size(), bytes ? " (msg bytes of the file left over)" : "");
		return true;
	}

	// Back to the state before T with the own sent msgs of then. Returns T, or -1.
	int TakeoverLoad()
	{
		PlayKey& k = *s_to_key;
		if (k.zip.joinable())
			k.zip.join();
		Error error;
		if (!k.ok || !VMManager::LoadState(k.path.c_str(), &error))
		{
			Console.Error("ZdxsvGgpo: replay takeover: state load failed: %s", error.GetDescription().c_str());
			return -1;
		}
		PlayKeyApply(k);
		s_net_sent = s_to_sent;
		s_net_sent_at = s_to_sent_at;
		s_net_out.clear();
		s_to_start_held = true; // a START held now retries on its release + press only
		return s_to_frame;
	}

	void TakeoverStart()
	{
		s_net_sent = s_to_sent;
		s_net_sent_at = s_to_sent_at;
		s_net_out = s_to_out;
		s_net_local = s_to_local;
		s_to_queue.clear();
		for (int i = 0; i < s_to_delay; i++)
			s_to_queue.push_back(PlayRow(s_to_frame + i, s_net_me));
		s_to_phase = TO_ON;
		Console.WriteLn("ZdxsvGgpo: replay takeover starts at frame %d, vsync %u", s_to_frame, g_FrameCount);
	}

	void TakeoverOff(const char* why)
	{
		s_to_phase = TO_OFF;
		s_to_queue.clear();
		s_to_key.reset();
		Console.WriteLn("ZdxsvGgpo: replay takeover %s at frame %d, vsync %u", why, s_net_frame, g_FrameCount);
	}

	void PlayFrame(int f)
	{
		if (s_to_phase == TO_ON)
		{
			PlayFrameTakeover(f);
			return;
		}
		PlayTrackLoads(f);
		if (f == s_play_target)
		{
			s_play_target = -1;
			if (s_run_load < 0)
				VMManager::SetLimiterMode(s_play_limiter);
			Console.WriteLn("ZdxsvGgpo: replay seek done at frame %d, vsync %u", f, g_FrameCount);
		}
		PlayKeySave(f);
		s_net_frame = f;
		PlayBarPublish();
		PlayKeysPublish(f);
		NetSaved(f);
		const NetInput* in = &s_play_inputs[static_cast<size_t>(f) * s_players];
		u16 ab[2];
		std::memcpy(ab, &in[s_net_me].pad, sizeof(ab));
		s_rings.zd_pad[f & 127] = PadFromB(ab[1]);
		PlayCheckState(f);
		NetApply(in);
	}

	void PlayReport(const char* what)
	{
		Console.WriteLn("ZdxsvGgpo: replay %s at frame %d of %d, vsync %u", what, s_net_frame, s_play_frames, g_FrameCount);
		Console.WriteLn("ZdxsvGgpo: replay state check: hashes %d checked, %d differ (first at frame %d); rngs %d checked, %d differ; "
						"%d frames after a round or game end not checked",
			s_play_hash_checks, s_play_hash_bad, s_play_hash_first_bad, s_play_rng_checks, s_play_rng_bad, s_play_hash_cut);
		NetReport();
	}

	void PlayStop(const char* what)
	{
		g_ggpo_active = false;
		s_bar_frame = -1;
		s_net_over = true; // the battle sock goes back to the IOP
		PlayReport(what);
		if (const char* e = std::getenv("ZDXSV_REPLAY_EXIT"); e && e[0] == '1')
			Host::RunOnCPUThread([] { Host::RequestVMShutdown(false, false, false); });
		else if (!s_play_at_end)
			Host::RunOnCPUThread([] { VMManager::SetPaused(true); });
	}

	void PlayReset()
	{
		SyncStop();
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
		s_live_got = {};
		s_live_close.clear();
		s_live_catchup = s_live_close_logged = false;
		s_live_waits = 0;
		s_live_wait_ms = 0;
		s_play_inputs.clear();
		s_play_frames = 0;
		s_play_hashes.clear();
		s_play_rngs.clear();
		s_play_rng_pos = -1;
		s_play_hash_checks = s_play_hash_bad = s_play_rng_checks = s_play_rng_bad = s_play_hash_cut = 0;
		s_play_hash_first_bad = -1;
		s_play_cut.clear();
		s_play_seeks.clear();
		s_play_req = INT_MIN;
		s_play_at_end = false;
		s_play_target = -1;
		{
			std::lock_guard lock(s_battle_loads_mtx);
			s_battle_loads.clear();
			s_play_file_rounds.clear();
			s_round_results.clear();
		}
		s_play_hi = s_tick_f = s_run_load = -1;
		s_round_rec = 0;
		s_play_round_req = INT_MIN;
		s_play_round_at.clear();
		std::fill(std::begin(s_play_pov_ok), std::end(s_play_pov_ok), false);
		s_play_rearm_seek = -1;
		s_play_booted_saved = false;
		s_play_pov_req = -1;
		s_play_pov_at.clear();
		std::fill(std::begin(s_play_sent), std::end(s_play_sent), PlaySent{});
		s_bar_frame = -1;
		s_bar_frames = 0;
		s_bar_pov = s_bar_target = -1;
		s_bar_povs = 0;
	}

	void ReplaySeekBy(int frames)
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		const int req = s_play_req.load();
		s_play_req = (req != INT_MIN ? req : s_net_frame + 1) + frames;
		Console.WriteLn("ZdxsvGgpo: replay seek requested: frame %d", s_play_req.load());
		if (s_play_at_end && VMManager::GetState() == VMState::Paused)
			VMManager::SetPaused(false);
	}

	void ReplayNextPov()
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		const int req = s_play_pov_req.load();
		int p = req >= 0 ? req : s_net_me;
		for (int i = 0; i < s_players; i++)
			if (p = (p + 1) % s_players; s_play_pov_ok[p])
				break;
		if (p == s_net_me)
		{
			Console.WriteLn("ZdxsvGgpo: replay: no other point of view (1 player)");
			return;
		}
		s_play_pov_req = p;
		Console.WriteLn("ZdxsvGgpo: replay point of view requested: %d", p);
		if (s_play_at_end && VMManager::GetState() == VMState::Paused)
			VMManager::SetPaused(false);
	}

	void ReplaySeekTo(int frame)
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		s_play_req = std::clamp(frame, 0, s_play_frames - 1);
		Console.WriteLn("ZdxsvGgpo: replay seek requested: frame %d", s_play_req.load());
		if (VMManager::GetState() == VMState::Paused) // control bar: a seek plays on, also from a pause
			VMManager::SetPaused(false);
	}

	void ReplayTogglePause()
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		if (s_play_at_end)
			ReplaySeekTo(0); // play at the end = from the start
		else if (s_to_phase == TO_ALIGN || s_to_phase == TO_COUNT)
			ReplayTakeoverCancel();
		else
			VMManager::SetPaused(VMManager::GetState() != VMState::Paused);
	}

	void ReplayTakeover()
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		if (s_to_phase == TO_OFF || s_to_phase == TO_ON)
		{
			s_to_req = s_to_phase == TO_OFF ? TO_REQ_TAKE : TO_REQ_RETRY;
			if (VMManager::GetState() == VMState::Paused)
				VMManager::SetPaused(false);
		}
	}

	void ReplayTakeoverSkip()
	{
		if (s_to_phase != TO_ALIGN && s_to_phase != TO_COUNT)
			return;
		s_to_skip = true;
		s_to_phase = TO_COUNT;
		s_to_count_t0 = Common::Timer::GetCurrentValue();
	}

	void ReplayTakeoverCancel()
	{
		if (s_to_phase != TO_ALIGN && s_to_phase != TO_COUNT)
			return;
		s_to_req = TO_REQ_CANCEL;
		VMManager::SetPaused(false);
	}

	void ReplayTakeoverReturn()
	{
		if (s_to_phase == TO_OFF)
			return;
		s_to_req = TO_REQ_RETURN;
		VMManager::SetPaused(false);
	}

	static std::atomic<u16> s_to_current{0};
	static std::atomic<float> s_to_left{0.0f};

	bool ReplayTakeoverInfo(int& phase, u16& target, u16& current, float& countdown)
	{
		if (s_bar_frame < 0)
			return false;
		phase = s_to_phase;
		target = s_to_target;
		current = s_to_current;
		countdown = s_to_left;
		return true;
	}

	void ReplayTakeoverIdle()
	{
		const int phase = s_to_phase;
		if (phase != TO_ALIGN && phase != TO_COUNT)
			return;
		u16 a, b;
		ZdPadAB(TakeoverHostInput(), a, b);
		s_to_current = b;
		const bool start = s_host[PadDualshock2::Inputs::PAD_START] >= 0.5f;
		if (start && !s_to_start_held)
			ReplayTakeoverSkip();
		s_to_start_held = start;
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (s_to_phase == TO_ALIGN && b == s_to_target)
		{
			s_to_phase = TO_COUNT;
			s_to_count_t0 = now;
		}
		else if (s_to_phase == TO_COUNT && !s_to_skip && b != s_to_target)
			s_to_phase = TO_ALIGN;
		const double ms = s_to_phase == TO_COUNT ? Common::Timer::ConvertValueToMilliseconds(now - s_to_count_t0) : 0.0;
		s_to_left = s_to_phase == TO_COUNT ? static_cast<float>(std::max(0.0, 1.0 - ms / 1000.0)) : 1.0f;
		if (s_to_phase == TO_COUNT && ms >= 1000.0)
		{
			Console.WriteLn("ZdxsvGgpo: replay takeover: input %s, starting", s_to_skip ? "matching skipped" : "matched");
			s_to_req = TO_REQ_START;
			VMManager::SetPaused(false);
		}
	}

	bool ReplayBarInfo(int& frame, int& frames, int& pov, u32& povs, int& target)
	{
		frame = s_bar_frame;
		if (!s_play_env || frame < 0)
			return false;
		frames = s_bar_frames;
		pov = s_bar_pov;
		povs = s_bar_povs;
		target = s_bar_target;
		return true;
	}

	bool ReplayKeys(std::vector<std::pair<u16, int>>& runs)
	{
		if (!s_keys_on || s_bar_frame < 0)
			return false;
		std::lock_guard lock(s_keys_mtx);
		runs.assign(s_keys.begin(), s_keys.end());
		return true;
	}

	void ReplayToggleKeys()
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		s_keys_on = !s_keys_on;
		Console.WriteLn("ZdxsvGgpo: replay key display %s", s_keys_on ? "on" : "off");
		{
			std::lock_guard lock(s_keys_mtx);
			s_keys_f = -1; // rebuilt now: a paused replay plays no frame
		}
		PlayKeysPublish(s_net_frame);
	}

	void ReplayJumpRound(int delta)
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		int round = s_play_round_req.load();
		if (round == INT_MIN)
		{
			round = 0; // rounds started at or before the shown frame
			const int f = s_play_target >= 0 ? s_play_target : s_net_frame;
			for (size_t k = 2; k < s_battle_loads.size() && s_battle_loads[k] <= f; k++)
				round++;
		}
		s_play_round_req = std::max(0, round + delta);
		Console.WriteLn("ZdxsvGgpo: replay round requested: %d", s_play_round_req.load());
		if (VMManager::GetState() == VMState::Paused)
			VMManager::SetPaused(false);
	}

	std::vector<int> ReplayRoundStarts()
	{
		std::lock_guard lock(s_battle_loads_mtx);
		return s_battle_loads.size() > 2 ? std::vector<int>(s_battle_loads.begin() + 2, s_battle_loads.end()) : std::vector<int>();
	}

	std::string ReplayRoundResults()
	{
		const int pov = s_bar_pov, players = s_players;
		if (pov < 0 || players < 1)
			return {};
		const int team = pov < players / 2 ? 1 : 2; // team 1 = the first half of the positions
		std::lock_guard lock(s_battle_loads_mtx);
		const std::vector<int>& rounds = s_play_file_rounds.empty() ? s_round_results : s_play_file_rounds;
		std::string s;
		for (const int win : rounds)
			s += win == team ? 'W' : win < 0 ? 'D' : 'L';
		return s;
	}
} // namespace Zdxsv
