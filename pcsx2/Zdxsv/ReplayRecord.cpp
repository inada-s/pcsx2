// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Replay recording of a GGPO battle (.pb), its upload and the live stream.

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{
	namespace
	{
		std::string s_replay_rec_dir; // where it is saved, "" = not saved
		std::vector<NetInput> s_replay_inputs; // [frame * s_players + position]
		// per frame, before its inputs: ReplayStateHash, GameRng, the tick state (load ends -> load_frames), the play
		// starts passed (-> play_start_frames, game_end_frames), RoundRecord (-> round_data)
		std::vector<u32> s_replay_hashes, s_replay_rngs, s_replay_records;
		std::vector<u8> s_replay_ticks, s_replay_ps;
		s64 s_replay_start_at = 0; // unix seconds
		int s_replay_confirmed = -1; // GGPO's last confirmed frame: the frames after it (predicted inputs) are not written
		constexpr int REPLAY_FILE_VERSION = 20261008; // BattleLogFile.log_file_version of the files written here
		// HLE state outside the save state at frame 0 (ReplayBegin; replay.proto net_rx0, hle0)
		struct ReplayHle0
		{
			std::vector<u8> rx;
			std::vector<int64_t> hle;
		} s_replay_hle0;
		std::vector<std::vector<u8>> s_replay_answers; // the battle start's lobby answers (Zdxsv::LobbyStartAnswers) at frame 0
		// The battle's live uplink (battle info live_uplink=, the lobby's UDP address): fed the confirmed frames;
		// left running after Stop until the lobby acked the close, replaced by the next battle's.
		std::shared_ptr<Zdxsv::LiveUp> s_live;

		// The BattleLogFile fields known at frame 0 (the live header; ReplayWrite adds the end, inputs and state).
		std::vector<uint8_t> ReplayHeader(const std::string& code, const std::string& ids)
		{
			using namespace Pb;
			std::vector<uint8_t> pb;
			PutString(pb, 3, code);
			PutInt(pb, 4, REPLAY_FILE_VERSION);
			PutString(pb, 5, "zdxsv-ps2");
			for (size_t p = 0; p < s_net_players.size(); p++)
			{
				std::vector<uint8_t> user;
				PutString(user, 1, s_net_players[p].id);
				PutString(user, 2, s_net_players[p].name);
				PutString(user, 3, s_net_players[p].pilot);
				PutInt(user, 12, static_cast<int64_t>(p));
				PutBytes(pb, 11, user.data(), user.size());
			}
			PutInt(pb, 20, s_replay_start_at);
			PutInt(pb, 40, s_players);
			PutInt(pb, 41, s_net_me);
			PutInt(pb, 42, s_delay);
			PutString(pb, 43, ids);
			PutBytes(pb, 45, s_replay_hle0.rx.data(), s_replay_hle0.rx.size());
			PutPackedInts(pb, 46, s_replay_hle0.hle);
			PutInt(pb, 47, sizeof(NetInput));
			for (const std::vector<u8>& a : s_replay_answers)
				PutBytes(pb, 51, a.data(), a.size());
			return pb;
		}

		// The confirmed frames not yet given to the live uplink.
		void LiveFeed(size_t frames)
		{
			if (!s_live)
				return;
			frames = std::min(frames, s_replay_inputs.size() / s_players);
			if (const size_t have = s_live->Frames(); frames > have)
				s_live->AddFrames(s_replay_inputs.data() + have * s_players, frames - have);
		}

		std::string ReplayDir()
		{
			if (s_replay_off || !s_net)
				return {};
			if (!s_replay_dir.empty())
				return s_replay_dir;
			return s_lobby ? Path::Combine(EmuFolders::DataRoot, "replays") : std::string();
		}

		std::string ReplayUploadUrl()
		{
			return !s_upload_url.empty() ? s_upload_url : Host::GetStringSettingValue("DEV9/Eth", "ZdxsvReplayUploadUrl", "");
		}

		// As flycast's GdxsvBackendRollback::SaveReplay: every player posts <battle_code>.pb (multipart field "file")
		// to the uploader, which keeps the first one (409 for the others). Detached: a slow upload blocks neither
		// the next battle nor the exit.
		void ReplayUpload(const std::string& name, const std::vector<uint8_t>& pb)
		{
			const std::string url = ReplayUploadUrl();
			if (url.empty())
				return;
			const std::string boundary = fmt::format("zdxsv{:016x}", XXH64(pb.data(), pb.size(), 0));
			std::string body = fmt::format("--{}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{}.pb\"\r\n"
										   "Content-Type: application/octet-stream\r\n\r\n",
				boundary, name);
			body.append(reinterpret_cast<const char*>(pb.data()), pb.size());
			body += fmt::format("\r\n--{}--\r\n", boundary);
			std::thread([url, body = std::move(body), type = "multipart/form-data; boundary=" + boundary]() mutable {
				std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Host::GetHTTPUserAgent());
				if (!http)
				{
					Console.Error("ZdxsvGgpo: replay upload: no HTTP client");
					return;
				}
				http->SetTimeout(300.0f);
				Common::Timer timer;
				const size_t size = body.size();
				http->CreatePostRequest(url, std::move(body), [&](s32 status, const std::string&, HTTPDownloader::Request::Data) {
					if (status == HTTPDownloader::HTTP_STATUS_OK || status == 409)
						Console.WriteLn("ZdxsvGgpo: replay upload %s: %s, %zu bytes, %.0f ms", url.c_str(),
							status == 409 ? "already there" : "ok", size, timer.GetTimeMilliseconds());
					else
						Console.Error("ZdxsvGgpo: replay upload %s failed: status %d", url.c_str(), status);
				}, nullptr, std::move(type));
				http->WaitForAllRequests();
			}).detach();
		}

		// replay.proto play_start_frames / game_end_frames of the first `frames` frames: a play start = the frame the
		// play-start barrier passes; a game end = the last tick state 6 -> 7 before the next play start (or the file end).
		// The peers enter the game end on different frames (the end phase waits on the HLE'd battle socket), so sync
		// checks stop there. Earlier 6 -> 7 after the same play start are round ends (replay.proto round_end_frames):
		// pairs (6 -> 7, the tick's next 6), the peers enter the load between them on different frames.
		void ReplayGameEnds(size_t frames, std::vector<int64_t>& starts, std::vector<int64_t>& ends, std::vector<int64_t>& rounds)
		{
			frames = std::min({frames, s_replay_ticks.size(), s_replay_ps.size()});
			int64_t end = -1, round_to = -1;
			for (size_t f = 1; f < frames; f++)
			{
				if (s_replay_ps[f] != s_replay_ps[f - 1])
				{
					if (end >= 0)
						ends.push_back(end);
					end = -1;
					starts.push_back(static_cast<int64_t>(f - 1)); // passed while applying frame f - 1's inputs
				}
				// f - 1: the frame whose step enters 7 (the trace's `L` frame); the peers' state hashes differ from there
				if (!starts.empty() && s_replay_ticks[f - 1] == TICK_PLAY && s_replay_ticks[f] == TICK_END)
				{
					if (end >= 0 && round_to >= 0)
					{
						rounds.push_back(end);
						rounds.push_back(round_to);
					}
					end = static_cast<int64_t>(f - 1);
					round_to = -1;
				}
				else if (end >= 0 && round_to < 0 && s_replay_ticks[f - 1] != TICK_PLAY && s_replay_ticks[f] == TICK_PLAY)
					round_to = static_cast<int64_t>(f - 1);
			}
			if (end >= 0)
				ends.push_back(end);
		}
	} // namespace


	// replay= (net): the lobby answers and HLE state at GGPO frame 0 + the synced inputs of every player per
	// frame; playback starts from the hosted common state (PlayCommonState). File format: see ReplayWrite.
	bool s_replay_rec = false; // a battle is being recorded

	std::string ReplayIds()
	{
		std::lock_guard lock(s_lobby_mtx);
		return s_lobby ? s_report_ids : std::string();
	}

	std::string IdValue(const std::string& ids, std::string_view key)
	{
		const std::string k = std::string(key) + "=";
		const size_t at = ids.starts_with(k) ? 0 : ids.find("\n" + k);
		if (at == std::string::npos)
			return {};
		const size_t from = at + (at ? 1 : 0) + k.size();
		return ids.substr(from, ids.find('\n', from) - from);
	}

	// At GGPO frame 0: the session started, its first input not added yet (Zdxsv::DeltaStateSave(0) saves this point).
	// Records when the battle is saved, uploaded or streamed live: each one goes without the others (as gdxsv).
	void ReplayBegin()
	{
		if (!s_net)
			return;
		std::string dir = ReplayDir();
		const std::string ids = ReplayIds(), code = IdValue(ids, "battle_code"), to = IdValue(ids, "live_uplink");
		Error error;
		if (!dir.empty() && !FileSystem::DirectoryExists(dir.c_str()) && !FileSystem::CreateDirectoryPath(dir.c_str(), true, &error))
		{
			Console.Error("ZdxsvGgpo: replay: cannot create %s: %s", dir.c_str(), error.GetDescription().c_str());
			dir.clear();
		}
		const bool upload = !code.empty() && !ReplayUploadUrl().empty();
		if (dir.empty() && !upload && to.empty())
			return;
		s_replay_rec = true;
		s_replay_start_at = static_cast<s64>(std::time(nullptr));
		s_replay_rec_dir = dir;
		s_replay_inputs.clear();
		s_replay_hashes.clear();
		s_replay_rngs.clear();
		s_replay_records.clear();
		s_replay_ticks.clear();
		s_replay_ps.clear();
		s_replay_confirmed = -1;
		// HLE state outside the save state at frame 0 (PlayLoad restores it): msgs waiting for the game's recv,
		// play-start barrier, kind-3 barrier
		s_replay_hle0.rx.assign(s_rb.net_rx.begin(), s_rb.net_rx.end());
		s_replay_hle0.hle = {s_rb.ps.n, s_rb.ps.rel, s_rb.ps.hold ? 1 : 0, s_rb.ps.go ? 1 : 0, s_rb.zds_seen[0], s_rb.zds_seen[1],
			s_rb.zds_seen[2], s_rb.zds_seen[3], s_rb.zds_rel};
		s_replay_answers = Zdxsv::LobbyStartAnswers();
		s_live.reset();
		if (!to.empty())
			s_live = std::make_shared<Zdxsv::LiveUp>(to, code, s_lobby_session, ReplayHeader(code, ids), s_players * sizeof(NetInput));
		Console.WriteLn("ZdxsvGgpo: replay: recording (save %s, upload %d, live %d)", dir.empty() ? "off" : dir.c_str(), upload ? 1 : 0,
			to.empty() ? 0 : 1);
	}

	// The synced inputs of frame f (also rerun frames: a rollback replaces the frames from f on).
	void ReplayLog(int f, const NetInput* in)
	{
		if (!s_replay_rec || f < 0)
			return;
		s_replay_inputs.resize(static_cast<size_t>(f) * s_players);
		s_replay_inputs.insert(s_replay_inputs.end(), in, in + s_players);
		s_replay_hashes.resize(f);
		s_replay_hashes.push_back(ReplayStateHash());
		s_replay_rngs.resize(f);
		s_replay_rngs.push_back(GameRng());
		s_replay_records.resize(f);
		s_replay_records.push_back(RoundRecord());
		s_replay_ticks.resize(f);
		s_replay_ticks.push_back(eeMem->Main[TICK_STATE]);
		s_replay_ps.resize(f);
		s_replay_ps.push_back(s_rb.ps.rel);
		int confirmed = -1;
		if (s_session && ggpo_get_last_confirmed_frame(s_session, &confirmed) == GGPO_OK)
			s_replay_confirmed = std::max(s_replay_confirmed, confirmed);
		LiveFeed(static_cast<size_t>(s_replay_confirmed + 1));
	}

	// <dir>/<battle_code>.pb (without a battle code: rbk-<start_at>-p<position>.pb): a BattleLogFile protobuf,
	// schema in Zdxsv/replay.proto.
	void ReplayWrite(const char* what)
	{
		if (!std::exchange(s_replay_rec, false))
			return;
		Common::Timer timer;
		const std::string dir = std::exchange(s_replay_rec_dir, {});
		const size_t frames = std::min<size_t>(s_replay_inputs.size() / s_players, s_replay_confirmed + 1);
		if (s_live)
		{
			LiveFeed(frames);
			s_live->Close(what);
		}
		const std::string ids = ReplayIds();
		const std::string code = IdValue(ids, "battle_code");
		std::string name;
		for (const char c : code)
			name += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
		if (name.empty())
			name = fmt::format("rbk-{}-p{}", s_replay_start_at, s_net_me);
		using namespace Pb;
		std::vector<uint8_t> pb = ReplayHeader(code, ids);
		pb.reserve(frames * s_players * sizeof(NetInput) + pb.size() + 1024);
		PutInt(pb, 21, static_cast<s64>(std::time(nullptr)));
		PutString(pb, 24, what);
		PutInt(pb, 48, static_cast<int64_t>(frames));
		PutBytes(pb, 49, s_replay_inputs.data(), frames * s_players * sizeof(NetInput));
		PutBytes(pb, 52, s_replay_hashes.data(), frames * sizeof(u32));
		if (frames > 0)
			PutUint(pb, 53, s_replay_rngs[0]);
		std::vector<int64_t> load_frames, load_rngs;
		for (size_t f = 1; f < frames; f++)
		{
			if (s_replay_ticks[f - 1] == TICK_LOAD && s_replay_ticks[f] != TICK_LOAD)
			{
				load_frames.push_back(static_cast<int64_t>(f));
				load_rngs.push_back(s_replay_rngs[f]);
			}
		}
		PutPackedInts(pb, 54, load_frames);
		PutPackedInts(pb, 55, load_rngs);
		std::vector<int64_t> starts, ends, rounds;
		ReplayGameEnds(frames, starts, ends, rounds);
		PutPackedInts(pb, 57, starts);
		PutPackedInts(pb, 58, ends);
		PutPackedInts(pb, 59, rounds);
		for (size_t f = 1; f < std::min(frames, s_replay_records.size()); f++)
		{
			if (const int win = RoundResult(s_replay_records[f - 1], s_replay_records[f]))
			{
				std::vector<uint8_t> round;
				PutInt(round, 1, win);
				PutBytes(pb, 18, round.data(), round.size());
				Console.WriteLn("ZdxsvGgpo: replay round result: win_team %d at frame %zu", win, f - 1);
			}
		}
		if (!dir.empty())
		{
			const std::string path = Path::Combine(dir, name + ".pb");
			if (!FileSystem::WriteBinaryFile(path.c_str(), pb.data(), pb.size()))
				Console.Error("ZdxsvGgpo: replay: write %s failed", path.c_str());
			else
				Console.WriteLn("ZdxsvGgpo: replay saved %s frames=%zu, file=%zu bytes, %.1f ms", path.c_str(), frames,
					pb.size(), timer.GetTimeMilliseconds());
		}
		if (!code.empty())
			ReplayUpload(name, pb);
	}
} // namespace Zdxsv
