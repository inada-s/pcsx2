// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Replay files: parse, load, common start, start of playback, switch of position, next battle.

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{
	namespace
	{

		// A replay file (Zdxsv/replay.proto) as PlayLoadFile uses it.
		struct ReplayFile
		{
			s64 version = 0, players = -1, me = -1, frames = -1, input_size = -1;
			std::string code;
			std::vector<u8> rx0;
			std::vector<int64_t> hle0;
			std::string_view inputs; // into the file's bytes
			std::vector<std::vector<u8>> answers; // lobby frames, 12-byte header + body
			std::string_view hashes; // u32 per frame, "" = none (older files)
			std::optional<u32> start_rng;
			std::vector<int64_t> load_frames, load_rngs;
			std::vector<int64_t> play_starts, game_ends, round_ends;
			std::vector<int> rounds; // round_data win_team
		};

		bool ReplayParse(std::string_view all, ReplayFile& r)
		{
			Pb::Reader rd{reinterpret_cast<const uint8_t*>(all.data()), reinterpret_cast<const uint8_t*>(all.data() + all.size())};
			return rd.Fields([&r](uint32_t field, uint32_t wt, uint64_t v, const uint8_t* b, size_t n) {
				const std::string_view bytes(reinterpret_cast<const char*>(b), n);
				if (wt == 0)
				{
					s64* num = field == 4 ? &r.version : field == 40 ? &r.players : field == 41 ? &r.me : field == 47 ? &r.input_size :
							   field == 48 ? &r.frames : nullptr;
					if (num)
						*num = static_cast<s64>(v);
					else if (field == 46)
						r.hle0.push_back(static_cast<int64_t>(v));
					else if (field == 53)
						r.start_rng = static_cast<u32>(v);
					else if (field == 54)
						r.load_frames.push_back(static_cast<int64_t>(v));
					else if (field == 55)
						r.load_rngs.push_back(static_cast<int64_t>(v));
					else if (field == 57)
						r.play_starts.push_back(static_cast<int64_t>(v));
					else if (field == 58)
						r.game_ends.push_back(static_cast<int64_t>(v));
					else if (field == 59)
						r.round_ends.push_back(static_cast<int64_t>(v));
				}
				else if (wt == 2)
				{
					if (field == 3)
						r.code = bytes;
					else if (field == 18)
					{
						int win = 0;
						Pb::Reader round{b, b + n};
						if (!round.Fields([&win](uint32_t f, uint32_t w, uint64_t x, const uint8_t*, size_t) {
								if (f == 1 && w == 0)
									win = static_cast<int32_t>(x);
								return true;
							}))
							return false;
						r.rounds.push_back(win);
					}
					else if (field == 45)
						r.rx0.assign(b, b + n);
					else if (field == 46)
						return Pb::ReadInts(wt, v, b, n, r.hle0);
					else if (field == 49)
						r.inputs = bytes;
					else if (field == 51 && n >= 12)
						r.answers.emplace_back(b, b + n);
					else if (field == 52)
						r.hashes = bytes;
					else if (field == 54)
						return Pb::ReadInts(wt, v, b, n, r.load_frames);
					else if (field == 55)
						return Pb::ReadInts(wt, v, b, n, r.load_rngs);
					else if (field == 57)
						return Pb::ReadInts(wt, v, b, n, r.play_starts);
					else if (field == 58)
						return Pb::ReadInts(wt, v, b, n, r.game_ends);
					else if (field == 59)
						return Pb::ReadInts(wt, v, b, n, r.round_ends);
				}
				return true;
			});
		}

		std::optional<std::vector<u8>> HttpGet(const std::string& url)
		{
			std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Host::GetHTTPUserAgent());
			if (!http)
			{
				Console.Error("ZdxsvGgpo: replay %s: no HTTP client", url.c_str());
				return std::nullopt;
			}
			http->SetTimeout(120.0f);
			std::optional<std::vector<u8>> got;
			http->CreateRequest(url, [&](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
				if (status == HTTPDownloader::HTTP_STATUS_OK)
					got = std::move(data);
				else
					Console.Error("ZdxsvGgpo: replay %s: status %d", url.c_str(), status); // 204: no such battle
			});
			http->WaitForAllRequests();
			return got;
		}

		// The first "replay_url" string of a JSON text (Go's encoder: \" \\ \/ and \u00XX escapes), empty if none.
		std::string JsonReplayUrl(std::string_view json)
		{
			const size_t key = json.find("\"replay_url\"");
			if (key == std::string_view::npos)
				return {};
			size_t i = json.find_first_not_of(" \t\r\n", key + 12);
			if (i == std::string_view::npos || json[i] != ':' || (i = json.find_first_not_of(" \t\r\n", i + 1)) == std::string_view::npos ||
				json[i] != '"')
				return {};
			std::string url;
			for (i++; i < json.size() && json[i] != '"'; i++)
			{
				if (json[i] != '\\')
					url += json[i];
				else if (i + 1 < json.size() && json[i + 1] == 'u' && i + 5 < json.size())
				{
					const std::optional<u32> c = StringUtil::FromChars<u32>(json.substr(i + 2, 4), 16);
					if (!c || *c >= 0x80) // a URL is ASCII
						return {};
					url += static_cast<char>(*c);
					i += 5;
				}
				else if (i + 1 < json.size())
					url += json[++i];
			}
			return i < json.size() ? url : std::string();
		}

		// ZDXSV_REPLAY=http(s)://...: a replay file, or the lobby's /lbs/replay?battle_code=C answer (as gdxsv
		// lbsapi: a JSON list, newest first), whose first battle's replay_url is then fetched.
		std::optional<std::vector<u8>> HttpOpen(const std::string& url)
		{
			std::optional<std::vector<u8>> got = HttpGet(url);
			if (!got || got->empty() || got->front() != '[') // a .pb starts with field 1 (0x08)
				return got;
			const std::string pb = JsonReplayUrl(std::string_view(reinterpret_cast<const char*>(got->data()), got->size()));
			if (pb.empty())
			{
				Console.Error("ZdxsvGgpo: replay %s: no replay_url in the answer", url.c_str());
				return std::nullopt;
			}
			Console.WriteLn("ZdxsvGgpo: replay %s: replay_url %s", url.c_str(), pb.c_str());
			return HttpGet(pb);
		}

		// Reads the file; returns its recorder's position, -1 = not used. One file holds every position's inputs, so it
		// plays any point of view: each starts from the common state with its own lobby answers (PlayCommonStart).
		int PlayLoadFile(const std::string& path)
		{
			const bool live = path.starts_with("udp://");
			const bool http = path.starts_with("http://") || path.starts_with("https://");
			if (s_play_frames > 0 || s_live_down)
			{
				Console.Error("ZdxsvGgpo: replay %s: one file plays every position, a 2nd one is not used", path.c_str());
				return -1;
			}
			const std::optional<std::vector<u8>> file =
				live ? LiveOpen(path.substr(6)) : http ? HttpOpen(path) : FileSystem::ReadBinaryFile(Path::ToNativePath(path).c_str()); // '/' fails on Windows
			const std::string_view all = file ? std::string_view(reinterpret_cast<const char*>(file->data()), file->size()) : std::string_view();
			ReplayFile r;
			if (all.empty() || !ReplayParse(all, r) || r.version <= 0)
			{
				Console.Error("ZdxsvGgpo: replay %s: not a replay file", path.c_str());
				return -1;
			}
			const s64 players = r.players, me = r.me, frames = r.frames;
			// each bound before the next product: the values are any s64
			if (players < 1 || players > GGPO_MAX_PLAYERS || me < 0 || me >= players || frames < 1 || frames > INT_MAX ||
				r.input_size != static_cast<s64>(sizeof(NetInput)) ||
				r.inputs.size() != static_cast<u64>(frames) * players * sizeof(NetInput))
			{
				Console.Error("ZdxsvGgpo: replay %s: bad header or short file (players %lld position %lld frames %lld)", path.c_str(),
					players, me, frames);
				return -1;
			}
			if (r.answers.empty())
			{
				Console.Error("ZdxsvGgpo: replay %s: no lobby answers (an older file with a start state is not played)", path.c_str());
				return -1;
			}
			int h[9] = {};
			if (r.hle0.size() != std::size(h))
			{
				Console.Error("ZdxsvGgpo: replay %s: hle0 has %zu values, not %zu", path.c_str(), r.hle0.size(), std::size(h));
				return -1;
			}
			std::copy(r.hle0.begin(), r.hle0.end(), h);
			// The kind-3 counters index s_zds_k3 (NetApply) and the file holds no kind-3 bodies: only the empty barrier
			// the recorder writes (ReplayBegin, right after the session reset) can be played.
			if (std::any_of(h + 4, h + 9, [](int v) { return v != 0; }))
			{
				Console.Error("ZdxsvGgpo: replay %s: hle0 kind-3 counters %d,%d,%d,%d,%d not 0", path.c_str(), h[4], h[5], h[6], h[7], h[8]);
				return -1;
			}
			s_play_common = true;
			s_play_answers = std::move(r.answers);
			s_play_inputs.resize(static_cast<size_t>(frames * players));
			std::memcpy(s_play_inputs.data(), r.inputs.data(), r.inputs.size());
			s_play_frames = static_cast<int>(frames);
			s_play_hashes.resize(r.hashes.size() == static_cast<u64>(frames) * sizeof(u32) ? static_cast<size_t>(frames) : 0);
			std::memcpy(s_play_hashes.data(), r.hashes.data(), s_play_hashes.size() * sizeof(u32));
			s_play_rngs.clear();
			if (r.start_rng)
				s_play_rngs[0] = *r.start_rng;
			for (size_t i = 0; i < r.load_frames.size() && i < r.load_rngs.size(); i++)
				s_play_rngs[static_cast<int>(r.load_frames[i])] = static_cast<u32>(r.load_rngs[i]);
			s_play_cut.clear();
			for (const int64_t e : r.game_ends)
			{
				int64_t to = frames;
				for (const int64_t s : r.play_starts)
					if (s > e)
						to = std::min(to, s);
				s_play_cut.emplace_back(static_cast<int>(e), static_cast<int>(to));
			}
			for (size_t i = 0; i + 1 < r.round_ends.size(); i += 2)
				s_play_cut.emplace_back(static_cast<int>(r.round_ends[i]), static_cast<int>(r.round_ends[i + 1]));
			{
				std::lock_guard lock(s_battle_loads_mtx);
				s_play_file_rounds = r.rounds;
			}
			s_play_rng_pos = static_cast<int>(me);
			s_players = static_cast<int>(players);
			// key 0 of each position: its state is saved at its common start's arm (PlayCommonStart); the HLE state
			// here (the recorder's) is only compared there
			for (int p = 0; p < s_players; p++)
			{
				auto k = std::make_unique<PlayKey>();
				k->path = Path::Combine(EmuFolders::Cache, fmt::format("zdxsv-replay-p{}.p2s", p));
				s_play_keys[p].emplace(0, std::move(k));
				s_play_pov_ok[p] = true;
			}
			RollbackState& rb = s_play_keys[me].at(0)->rb;
			rb.net_rx.assign(r.rx0.begin(), r.rx0.end());
			rb.ps = {static_cast<u8>(h[0]), static_cast<u8>(h[1]), h[2] != 0, h[3] != 0};
			for (int p = 0; p < 4; p++)
				rb.zds_seen[p] = h[4 + p];
			rb.zds_rel = h[8];
			Console.WriteLn("ZdxsvGgpo: replay %s: recorded at position %lld of %d, %lld frames, rx0 %zu bytes, "
							"%zu lobby answers, state hashes %s, rngs %zu", path.c_str(), me, s_players, frames,
				rb.net_rx.size(), s_play_answers.size(), r.hashes.empty() ? "no" : "yes",
				(r.start_rng ? 1 : 0) + std::min(r.load_frames.size(), r.load_rngs.size()));
			if (s_net_trace) // battle start marker: pwcheck.py --battle splits a multi-battle trace (live auto-next)
				std::fprintf(s_net_trace, "%u B %s\n", g_FrameCount, r.code.empty() ? "-" : r.code.c_str());
			return static_cast<int>(me);
		}

		// The hosted post-entry state of a common start (as gdxsv's slot 99): ZDXSV_REPLAY_STATE, else the setting
		// DEV9/Eth ZdxsvReplayStateUrl (default: the hosted one); a URL is downloaded once into the cache (one file
		// per URL). Set empty = the booted state is used as it is.
		bool PlayCommonState()
		{
			const char* env = std::getenv("ZDXSV_REPLAY_STATE");
			const std::string src = env ? env : Host::GetStringSettingValue("DEV9/Eth", "ZdxsvReplayStateUrl", REPLAY_STATE_URL);
			std::string path = src;
			if (src.empty()) // the booted state, saved at the first start for a switch's start
			{
				path = Path::Combine(EmuFolders::Cache, "zdxsv-common-booted.p2s");
				if (!std::exchange(s_play_booted_saved, true))
				{
					Error error;
					std::unique_ptr<ArchiveEntryList> list = SaveState_DownloadState(&error);
					if (!list || !SaveState_ZipToDisk(std::move(list), nullptr, path.c_str(), &error))
					{
						Console.Error("ZdxsvGgpo: replay: booted state save failed: %s", error.GetDescription().c_str());
						s_play_booted_saved = false;
					}
					return true;
				}
			}
			if (src.starts_with("http://") || src.starts_with("https://"))
			{
				path = Path::Combine(EmuFolders::Cache, fmt::format("zdxsv-common-{:016x}.p2s", std::hash<std::string>{}(src)));
				if (!FileSystem::FileExists(path.c_str()))
				{
					const std::optional<std::vector<u8>> got = HttpGet(src);
					if (!got || !FileSystem::WriteBinaryFile(path.c_str(), got->data(), got->size()))
					{
						Console.Error("ZdxsvGgpo: replay: common state %s: download to %s failed", src.c_str(), path.c_str());
						return false;
					}
					Console.WriteLn("ZdxsvGgpo: replay: common state %s: %zu bytes into %s", src.c_str(), got->size(), path.c_str());
				}
			}
			Error error;
			if (!VMManager::LoadState(path.c_str(), &error))
			{
				Console.Error("ZdxsvGgpo: replay: common state %s: load failed: %s", path.c_str(), error.GetDescription().c_str());
				return false;
			}
			Console.WriteLn("ZdxsvGgpo: replay: common state %s loaded", path.c_str());
			return true;
		}

		// Returns the frame to run next.
		int PlaySwitch(int pov, int target)
		{
			if (pov < 0 || pov >= s_players || !s_play_pov_ok[pov])
			{
				Console.Error("ZdxsvGgpo: replay: no point of view %d in the battle", pov);
				return s_net_frame + 1;
			}
			const int old = s_net_me;
			const auto swap_sent = [](int p) {
				std::swap(s_net_sent, s_play_sent[p].sent);
				std::swap(s_net_sent_at, s_play_sent[p].at);
				std::swap(s_net_out, s_play_sent[p].out);
			};
			swap_sent(old); // park the old position's sent msgs
			swap_sent(pov);
			s_net_me = pov;
			Console.WriteLn("ZdxsvGgpo: replay: point of view %d -> %d at frame %d, vsync %u", old, pov, target, g_FrameCount);
			if (!s_play_keys[pov].at(0)->ok) // never played: its own battle start first
			{
				if (PlayCommonArm(pov, std::clamp(target, 0, s_play_frames - 1)))
					return -2;
				Console.Error("ZdxsvGgpo: replay: point of view %d: no common state", pov);
				swap_sent(pov);
				swap_sent(old);
				s_net_me = old;
				return s_net_frame + 1;
			}
			const int next = PlaySeek(target, true);
			if (next >= 0)
				return next;
			swap_sent(pov);
			swap_sent(old);
			s_net_me = old;
			return s_net_frame + 1;
		}

		// Takeover requests at the frame end before `next` runs (may set it). Returns true: pause after it.
		bool PlayTakeover(int& next)
		{
			int req = s_to_req.exchange(TO_REQ_NONE);
			if (next == s_to_test_at)
				s_to_test_at = -1, req = TO_REQ_TAKE;
			if (next == s_to_test_retry && s_to_phase == TO_ON)
				s_to_test_retry = -1, req = TO_REQ_RETRY;
			if (s_to_phase == TO_ON && !s_to_test)
			{
				const bool start = s_host[PadDualshock2::Inputs::PAD_START] >= 0.5f;
				if (start && !s_to_start_held)
					req = TO_REQ_RETRY;
				s_to_start_held = start;
			}
			const int phase = s_to_phase;
			const bool aligning = phase == TO_ALIGN || phase == TO_COUNT;
			const auto align = [] {
				s_to_phase = TO_ALIGN;
				s_to_skip = false;
				s_to_start_held = s_host[PadDualshock2::Inputs::PAD_START] >= 0.5f; // skip needs a new press
				Console.WriteLn("ZdxsvGgpo: replay takeover: hold the replay's input %04x at frame %d", s_to_target.load(), s_to_frame);
			};
			const auto load = [&next] {
				const int t = TakeoverLoad();
				if (t < 0)
					TakeoverOff("failed");
				else
					next = t;
				return t >= 0;
			};
			if (req == TO_REQ_TAKE && phase == TO_OFF)
			{
				if (s_live_down)
					Console.Error("ZdxsvGgpo: replay takeover: not while spectating live");
				else if (TakeoverBegin(next))
				{
					PlayRunEnd("cancelled by a takeover", s_net_frame);
					if (!s_to_test)
					{
						align();
						return true;
					}
					TakeoverStart(); // the running state is the one before T
				}
			}
			else if (req == TO_REQ_START && aligning)
			{
				if (load())
					TakeoverStart();
			}
			else if (req == TO_REQ_RETRY && phase == TO_ON)
			{
				Console.WriteLn("ZdxsvGgpo: replay takeover: retry at frame %d", s_net_frame);
				if (load())
				{
					if (s_to_test)
						TakeoverStart();
					else
					{
						align();
						return true;
					}
				}
			}
			else if (req == TO_REQ_RETURN && phase != TO_OFF)
			{
				const bool ok = load();
				TakeoverOff("back to the replay");
				return ok;
			}
			else if (req == TO_REQ_CANCEL && aligning)
				TakeoverOff("cancelled");
			return false;
		}
	} // namespace


	// CPU thread, queued at the first vsync.
	void PlayLoad()
	{
		int me = -1;
		const std::string paths = s_live_next_url.empty() ? s_play_env : s_live_next_url; // the split's views point into it
		for (const std::string_view path : StringUtil::SplitString(paths, ';'))
			if (const int p = PlayLoadFile(std::string(path)); me < 0)
				me = p;
		if (me < 0)
			return;
		if (const char* e = std::getenv("ZDXSV_REPLAY_POV"))
		{
			const int p = std::atoi(e);
			if (p >= 0 && p < s_players && s_play_pov_ok[p])
				me = p;
			else
				Console.Error("ZdxsvGgpo: replay: ZDXSV_REPLAY_POV=%s is not a position of the battle, playing position %d", e, me);
		}
		PlayCommonArm(me);
	}

	// The battle start from the common state, the lobby answering 0x6912 (own position) with pov: the game plays that
	// position from its arm on. PlayLoad, and a switch to a position with no key yet (PlaySwitch), which then seeks.
	bool PlayCommonArm(int pov, int seek)
	{
		if (!PlayCommonState())
			return false;
		s_play_common = true;
		s_play_rearm_seek = seek;
		if (seek >= 0)
		{
			s_play_rearm_limiter = s_play_target >= 0 ? s_play_limiter : VMManager::GetLimiterMode();
			s_play_target = -1;
			VMManager::SetLimiterMode(LimiterModeType::Unlimited); // the lobby phase up to the arm
		}
		s_started = s_net_armed = s_frame_ended = false;
		g_ggpo_active = false;
		// the battle socket's HLE state as at the first start (the arm adds the lobby frames to net_rx)
		s_rb = {};
		s_rings = {};
		s_net_frame = 0;
		s_net_end = -1;
		s_zd_steps = s_zd_changed = s_zds_echo = s_zds_skip = s_zds_k3rel = 0;
		for (auto& k3 : s_zds_k3)
			k3.clear();
		s_net = true;
		s_net_me = pov;
		s_rbk = true;
		s_rbk_me = pov;
		s_rbk_n = s_players;
		RbkReset();
		Console.WriteLn("ZdxsvGgpo: replay: common start as position %d of %d, %zu lobby answers%s", pov, s_players,
			s_play_answers.size(), seek >= 0 ? fmt::format(", then seek to {}", seek).c_str() : "");
		return true;
	}

	// Returned at the arm of a common start = GGPO frame 0 as ReplayBegin sees it live: this state becomes key 0
	// of the point of view, then the replay starts from it as from a file's state.
	void PlayCommonStart()
	{
		s_play_common = false;
		const int me = s_net_me;
		PlayKey& k0 = *s_play_keys[me].at(0);
		const bool hle_same = k0.rb.net_rx == s_rb.net_rx && k0.rb.ps.n == s_rb.ps.n && k0.rb.ps.rel == s_rb.ps.rel &&
							  k0.rb.ps.hold == s_rb.ps.hold && k0.rb.ps.go == s_rb.ps.go;
		Error error;
		std::unique_ptr<ArchiveEntryList> list = SaveState_DownloadState(&error);
		if (!list || !SaveState_ZipToDisk(std::move(list), nullptr, k0.path.c_str(), &error))
		{
			Console.Error("ZdxsvGgpo: replay: common start: state save failed: %s", error.GetDescription().c_str());
			return;
		}
		k0.rb = s_rb;
		k0.ok = true;
		// the file's HLE state is its recorder's position's
		Console.WriteLn("ZdxsvGgpo: replay: common start armed at vsync %u as position %d (asked %d), frame 0 HLE state %s",
			g_FrameCount, me, s_rbk_me, me != s_play_rng_pos ? "not compared" : hle_same ? "equals the file's" : "differs from the file's");
		// PlayBegin loads that state as PlayLoad does
		s_started = false;
		g_ggpo_active = false;
		Host::RunOnCPUThread([me] { PlayBegin(me); });
	}

	void PlayBegin(int me)
	{
		const PlayKey& k0 = *s_play_keys[me].at(0);
		Error error;
		if (!VMManager::LoadState(k0.path.c_str(), &error))
		{
			Console.Error("ZdxsvGgpo: replay: state load failed: %s", error.GetDescription().c_str());
			return;
		}
		s_net = true;
		s_net_me = me;
		PlayKeyApply(k0);
		s_net_armed = true;
		s_started = true;
		g_ggpo_active = true;
		if (const int seek = std::exchange(s_play_rearm_seek, -1); seek >= 0)
		{
			// a switch: the rest of the first start (options, four-screen) stays
			VMManager::SetLimiterMode(s_play_rearm_limiter);
			Console.WriteLn("ZdxsvGgpo: replay: point of view %d from its key 0, seeking to %d", me, seek);
			s_play_req = seek;
			PlayFrame(0);
			return;
		}
		if (const char* e = std::getenv("ZDXSV_REPLAY_TURBO"); e && e[0] == '1')
			VMManager::SetLimiterMode(LimiterModeType::Turbo);
		if (s_play_skip_ms)
			PlayRunBegin(1);
		if (const char* e = std::getenv("ZDXSV_REPLAY_ROUND_AT"))
			for (const std::string_view one : StringUtil::SplitString(e, ','))
				if (const size_t c = one.find(':'); c != std::string_view::npos)
					s_play_round_at.emplace_back(StringUtil::FromChars<int>(one.substr(0, c)).value_or(-1),
						StringUtil::FromChars<int>(one.substr(c + 1)).value_or(-1));
		if (const char* e = std::getenv("ZDXSV_REPLAY_SEEK"))
			for (const std::string_view one : StringUtil::SplitString(e, ','))
				if (const size_t c = one.find(':'); c != std::string_view::npos)
					s_play_seeks.emplace_back(StringUtil::FromChars<int>(one.substr(0, c)).value_or(-1),
						StringUtil::FromChars<int>(one.substr(c + 1)).value_or(0));
		if (const char* e = std::getenv("ZDXSV_REPLAY_POV_AT"))
			for (const std::string_view one : StringUtil::SplitString(e, ','))
				if (const size_t c = one.find(':'); c != std::string_view::npos)
					s_play_pov_at.emplace_back(StringUtil::FromChars<int>(one.substr(0, c)).value_or(-1),
						StringUtil::FromChars<int>(one.substr(c + 1)).value_or(-1));
		std::string povs;
		for (int p = 0; p < s_players; p++)
			if (s_play_pov_ok[p])
				povs += fmt::format("{}{}", povs.empty() ? "" : ",", p);
		Console.WriteLn("ZdxsvGgpo: replay: point of view %d (positions with a file: %s), %d frames", s_net_me, povs.c_str(), s_play_frames);
		SyncStart(me);
		PlayFrame(0);
	}

	void PlayNext()
	{
		s_session_frames++;
		int next = s_net_frame + 1;
		if (!s_play_seeks.empty() && s_play_seeks.front().first == next)
		{
			s_play_req = s_play_seeks.front().second;
			s_play_seeks.pop_front();
		}
		if (!s_play_pov_at.empty() && s_play_pov_at.front().first == next)
		{
			s_play_pov_req = s_play_pov_at.front().second;
			s_play_pov_at.pop_front();
		}
		if (!s_play_round_at.empty() && s_play_round_at.front().first == next)
		{
			s_play_round_req = s_play_round_at.front().second;
			s_play_round_at.pop_front();
		}
		const bool pause = PlayTakeover(next);
		if (s_to_phase != TO_OFF) // no seek, round jump or point of view switch
		{
			s_play_req = INT_MIN;
			s_play_pov_req = -1;
			s_play_round_req = INT_MIN;
		}
		SyncFollow(next);
		int req = s_play_req.exchange(INT_MIN);
		const int pov = s_play_pov_req.exchange(-1);
		int run = -1; // load to run to after the seek
		if (const int round = s_play_round_req.exchange(INT_MIN); round != INT_MIN)
		{
			const int load = 1 + std::max(0, round);
			if (load < static_cast<int>(s_battle_loads.size()))
			{
				req = s_battle_loads[load];
				Console.WriteLn("ZdxsvGgpo: replay round %d: starts at frame %d", load - 1, req);
			}
			else
			{
				req = s_play_hi > s_net_frame ? s_play_hi : INT_MIN;
				run = load;
			}
		}
		if (const bool chase = std::exchange(s_sync_chase, false); req != INT_MIN && !chase && Zdxsv::SpectateSync::IsHost())
			Zdxsv::SpectateSync::HostSeek(req);
		if (req != INT_MIN || run >= 0 || (pov >= 0 && pov != s_net_me))
		{
			PlayRunEnd("cancelled by a seek", s_net_frame);
			LiveCatchupEnd(s_net_frame);
		}
		if (req == 0 && s_battle_loads.size() > 1)
			req = s_battle_loads[1]; // from the start = from the briefing
		if (pov >= 0 && pov != s_net_me)
		{
			if (next = PlaySwitch(pov, req != INT_MIN ? req : next); next == -2)
				return; // PlayCommonStart plays on
		}
		else if (req != INT_MIN)
			next = PlaySeek(req);
		if (req == 0 && next == 0 && s_play_skip_ms)
			run = 1; // briefing not reached yet
		if (run >= 0)
			PlayRunBegin(run);
		if (s_live_down)
			LiveNext(next);
		if (next >= s_play_frames)
			PlayRunEnd("replay ended first", s_net_frame);
		const char* exit_env = std::getenv("ZDXSV_REPLAY_EXIT");
		if (next < s_play_frames || (s_to_phase == TO_ON && !(exit_env && exit_env[0] == '1')))
		{
			s_play_at_end = false;
			s_live_wait.reset(); // played on from the end: no move to another battle
			SyncFrame(next);
			PlayFrame(next);
			if (pause)
				VMManager::SetPaused(true);
		}
		else if (s_live_down && !s_play_at_end && LiveAutoNext())
		{
			s_play_at_end = true;
			PlayReport("done, auto-next"); // the next battle's VM reset drops this one's stats and pw hashes
		}
		else if (const char* e = std::getenv("ZDXSV_REPLAY_EXIT"); s_play_at_end || (e && e[0] == '1'))
			PlayStop("end");
		else
		{
			// a seek requested while paused here plays on; resuming without one stops the replay
			s_play_at_end = true;
			Console.WriteLn("ZdxsvGgpo: replay at its end (frame %d of %d, vsync %u), paused", s_net_frame, s_play_frames, g_FrameCount);
			VMManager::SetPaused(true); // now: a queued pause lets one more frame run, which ends the replay
		}
	}
} // namespace Zdxsv
