// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Replay playback of a live stream and the spectator sync of several viewers.

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{
	namespace
	{

		// Live: at the newest frame, wait until LIVE_BUFFER more are there or the stream is closed. More than
		// LIVE_BUFFER + LIVE_CATCHUP frames behind, run unlimited until LIVE_BUFFER + LIVE_EDGE.
		constexpr int LIVE_BUFFER = 30, LIVE_CATCHUP = 270, LIVE_EDGE = 60, LIVE_STALL_MS = 30000, LIVE_OPEN_MS = 15000;
		LimiterModeType s_live_limiter = LimiterModeType::Nominal;
		std::vector<std::string> s_live_seen; // battle codes watched, oldest first, kept across the resets
		int s_live_next_left = [] {
			const char* e = std::getenv("ZDXSV_LIVE_NEXT");
			return e ? std::atoi(e) : -1; // -1 = the setting decides (no limit)
		}();
		constexpr int LIVE_NEXT_POLL_S = 5, LIVE_NEWEST_MS = 2000, LIVE_NEXT_SKIP = 32; // the skip list fits one datagram

		void LiveSeen(const std::string& code)
		{
			if (std::ranges::find(s_live_seen, code) == s_live_seen.end())
				s_live_seen.push_back(code);
		}

		void LiveTake()
		{
			const bool ok = s_live_down->Take(s_live_got, LIVE_STALL_MS);
			const size_t fb = s_players * sizeof(NetInput);
			if (s_live_got.frameBytes > 0 && static_cast<size_t>(s_live_got.frameBytes) != fb && s_live_close.empty())
				s_live_close = fmt::format("frame size {}, not {}", s_live_got.frameBytes, fb);
			if (const size_t n = s_live_got.inputs.size() / fb; n > 0 && s_live_close.empty())
			{
				const size_t at = s_play_inputs.size();
				s_play_inputs.resize(at + n * s_players);
				std::memcpy(&s_play_inputs[at], s_live_got.inputs.data(), n * fb);
				s_live_got.inputs.erase(s_live_got.inputs.begin(), s_live_got.inputs.begin() + n * fb);
				s_play_frames += static_cast<int>(n);
			}
			if (s_live_got.closed && s_live_close.empty())
				s_live_close = s_live_got.close.empty() ? "end" : s_live_got.close;
			if (!ok && s_live_close.empty())
			{
				Console.Error("ZdxsvGgpo: live: nothing from the lobby for %d s, stream lost", LIVE_STALL_MS / 1000);
				s_live_close = "lost";
			}
		}

		// Pacing: while following the edge, g_frame_period_trim_us holds the unplayed frames at LIVE_BUFFER:
		// feedforward at the stream's rate plus PI terms on the buffer error (P outside a deadband).
		constexpr int PACE_DEADBAND_MAX = 2, PACE_US_PER_FRAME = 40, PACE_STALL = 5, PACE_LOG = 600;
		constexpr double PACE_I_GAIN = 0.25, PACE_I_LIMIT = 600, PACE_FLOOR_US = -4000, PACE_CEIL_US = 8000;
		constexpr double PACE_WINDOW_S = 1, PACE_ALPHA = 0.25, PACE_MIN_HZ = 30, PACE_MAX_HZ = 65, PACE_IDLE_S = 0.1;
		const bool s_pace_off = [] { const char* e = std::getenv("ZDXSV_LIVE_PACING"); return e && e[0] == '0'; }();
		double s_pace_i = 0, s_pace_hz = 0;
		Common::Timer s_pace_win, s_pace_call;
		int s_pace_win_recv = -1, s_pace_last_recv = 0, s_pace_stall = 0, s_pace_log = 0;

		void LivePace(int next)
		{
			if (++s_pace_log >= PACE_LOG)
			{
				s_pace_log = 0;
				Console.WriteLn("ZdxsvGgpo: live: pace frame %d gap %d trim %d us rate %.2f hz, %d waits %.0f ms", next,
					s_play_frames - next, g_frame_period_trim_us, s_pace_hz, s_live_waits, s_live_wait_ms);
			}
			const bool idle = s_pace_call.GetTimeSeconds() > PACE_IDLE_S; // paused, or a long wait at the edge
			s_pace_call.Reset();
			if (s_pace_off || s_live_catchup || s_run_load >= 0 || s_play_target >= 0 || s_to_phase != TO_OFF ||
				!s_live_close.empty())
			{
				LivePaceReset();
				return;
			}
			const int recv = s_play_frames;
			if (recv != s_pace_last_recv)
			{
				s_pace_last_recv = recv;
				s_pace_stall = 0;
			}
			else if (s_pace_stall < PACE_STALL)
				s_pace_stall++;
			if (s_pace_stall >= PACE_STALL || idle || s_pace_win_recv < 0)
			{
				// the silence is not the match's rate: restart the window, play at nominal
				g_frame_period_trim_us = 0;
				s_pace_win.Reset();
				s_pace_win_recv = recv;
				return;
			}
			const double nominal_hz = VMManager::GetFrameRate();
			if (s_pace_hz <= 0)
				s_pace_hz = nominal_hz;
			if (const double win = s_pace_win.GetTimeSeconds(); win >= PACE_WINDOW_S)
			{
				if (const double observed = (recv - s_pace_win_recv) / win; observed > 1)
					s_pace_hz = std::clamp((1 - PACE_ALPHA) * s_pace_hz + PACE_ALPHA * observed, PACE_MIN_HZ, PACE_MAX_HZ);
				s_pace_win.Reset();
				s_pace_win_recv = recv;
			}
			// positive error: further behind the edge than wanted, so a shorter period (negative trim)
			const int error = recv - next - LIVE_BUFFER;
			const double feedforward = 1e6 / s_pace_hz - 1e6 / nominal_hz;
			const int deadband = std::clamp(LIVE_BUFFER / 4, 1, PACE_DEADBAND_MAX);
			const double p = std::abs(error) > deadband ? -error * PACE_US_PER_FRAME : 0.0;
			const double unsaturated = feedforward + p + s_pace_i;
			const double step = -error * PACE_I_GAIN;
			if (!(unsaturated >= PACE_CEIL_US && step > 0) && !(unsaturated <= PACE_FLOOR_US && step < 0))
				s_pace_i = std::clamp(s_pace_i + step, -PACE_I_LIMIT, PACE_I_LIMIT);
			g_frame_period_trim_us = static_cast<s32>(std::clamp(feedforward + p + s_pace_i, PACE_FLOOR_US, PACE_CEIL_US));
		}

		// GgpoOnVmShutdown: the key files go; a new VM (or the reset one) loads the files again and plays from the start.
		// Four-screen: the host spawns one guest per other position; all hold each other on the same frame
		// (SpectateSync). A member more than SYNC_CHASE frames behind the newest seeks to it.
		constexpr int SYNC_WAIT_MS = 200, SYNC_CHASE = 30;
		std::thread s_sync_thread;
		std::atomic<bool> s_sync_quit{false};
		u32 s_sync_seek_gen = 0;
		int s_sync_pids[Zdxsv::SpectateSync::GRID] = {};
		// ZDXSV_REPLAY_SYNC=0 (test control): members publish their frame but neither wait nor chase
		const bool s_sync_test_off = [] { const char* e = std::getenv("ZDXSV_REPLAY_SYNC"); return e && e[0] == '0'; }();

		bool PlayCatchingUp() { return s_run_load >= 0 || s_play_target >= 0 || s_live_catchup; }

		// Not the CPU thread. Host: publishes its pause, tiles the windows; guest: follows the pause, quits with the host.
		void SyncWatch(bool host)
		{
			int want = 0;
			for (const int pid : s_sync_pids)
				want += pid != 0;
			bool tiled = false, quit = false;
			std::optional<bool> paused;
			for (int i = 0; !s_sync_quit; i++)
			{
				if (host)
				{
					Zdxsv::SpectateSync::HostPaused(VMManager::GetState() == VMState::Paused);
					if (!tiled && i % 25 == 0 && i < 3000) // 60 s
						tiled = Zdxsv::SpectateSync::TileWindows(s_sync_pids) == want;
				}
				else
				{
					Zdxsv::SpectateSync::Control c;
					if (!quit && Zdxsv::SpectateSync::HostGone())
					{
						quit = true;
						Console.WriteLn("ZdxsvGgpo: replay four-screen: the host is gone, quitting");
						Host::RequestVMShutdown(false, false, false);
					}
					if (Zdxsv::SpectateSync::ReadControl(c) && paused != c.paused)
					{
						paused = c.paused;
						Host::RunOnCPUThread([p = c.paused] {
							if (VMManager::HasValidVM())
								VMManager::SetPaused(p);
						});
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
		}
	} // namespace


	// Point of view: a switch at frame f loads the new position's newest key <= f and runs to f unlimited; a
	// position never played runs its own battle start first. Keys and sent msgs are kept per position.
	std::atomic<int> s_play_pov_req{-1}; // requested position, -1 = none
	std::deque<std::pair<int, int>> s_play_pov_at; // ZDXSV_REPLAY_POV_AT=frame:position,...
	PlaySent s_play_sent[GGPO_MAX_PLAYERS];
	std::unique_ptr<Zdxsv::LiveDown> s_live_down;
	Zdxsv::LiveStreams s_live_got; // inputs not in s_play_inputs yet (part of a frame)
	std::string s_live_close; // "" = running
	bool s_live_catchup = false, s_live_close_logged = false;
	int s_live_waits = 0;
	double s_live_wait_ms = 0;
	std::unique_ptr<LiveWait> s_live_wait;
	std::string s_live_next_url; // the battle auto-next moved on to, "" = ZDXSV_REPLAY

	// The live stream as a replay file's bytes (header, frames so far, start state).
	std::optional<std::vector<u8>> LiveOpen(const std::string& url)
	{
		std::string error;
		s_live_down = Zdxsv::LiveDown::Open(url, LIVE_OPEN_MS, error);
		if (!s_live_down)
		{
			Console.Error("ZdxsvGgpo: live %s: %s", url.c_str(), error.c_str());
			return std::nullopt;
		}
		s_live_got = {};
		s_live_down->Take(s_live_got, LIVE_STALL_MS);
		const size_t fb = std::max(s_live_got.frameBytes, 1);
		const size_t frames = s_live_got.inputs.size() / fb;
		using namespace Pb;
		std::vector<u8> pb = std::move(s_live_got.header);
		PutInt(pb, 48, static_cast<int64_t>(frames));
		PutBytes(pb, 49, s_live_got.inputs.data(), frames * fb);
		s_live_got.inputs.erase(s_live_got.inputs.begin(), s_live_got.inputs.begin() + frames * fb);
		const std::string code = s_live_down->Code();
		LiveSeen(code);
		Console.WriteLn("ZdxsvGgpo: live: battle %s, %zu frames so far%s", code.c_str(), frames,
			s_live_got.closed ? ", closed" : "");
		return pb;
	}

	// PlayNext at the end of a closed stream: true = auto-next waits for the next battle (VM paused).
	bool LiveAutoNext()
	{
		if (s_live_next_left < 0 ? !Host::GetBoolSettingValue("DEV9/Eth", "ZdxsvLiveAutoNext", false) : s_live_next_left == 0)
			return false;
		if (s_live_next_left > 0)
			s_live_next_left--;
		const std::string_view src = s_live_next_url.empty() ? std::string_view(s_play_env) : std::string_view(s_live_next_url);
		const std::string_view rest = src.substr(std::min<size_t>(6, src.size())); // after udp://
		std::string host(rest.substr(0, rest.find('/')));
		Console.WriteLn("ZdxsvGgpo: live: auto-next: waiting for a new battle at %s (%zu watched)", host.c_str(), s_live_seen.size());
		s_live_wait = std::make_unique<LiveWait>();
		const auto skip_from = s_live_seen.end() - std::min<ptrdiff_t>(s_live_seen.size(), LIVE_NEXT_SKIP);
		s_live_wait->t = std::thread([w = s_live_wait.get(), host = std::move(host), seen = s_live_seen,
										 skip = std::vector<std::string>(skip_from, s_live_seen.end())]() {
			Common::Timer since;
			while (!w->quit)
			{
				// the seen check holds the pick against a lobby that ignores skip (it answers its newest battle)
				if (const std::string code = Zdxsv::LiveDown::Newest(host, skip, LIVE_NEWEST_MS);
					!code.empty() && std::ranges::find(seen, code) == seen.end())
				{
					Console.WriteLn("ZdxsvGgpo: live: auto-next: moving on to %s after %.0f s", code.c_str(), since.GetTimeSeconds());
					Host::RunOnCPUThread([code, url = fmt::format("udp://{}/{}", host, code)] {
						if (!VMManager::HasValidVM())
							return;
						LiveSeen(code);
						s_live_next_url = url;
						VMManager::Reset();
						VMManager::SetPaused(false);
					});
					return;
				}
				for (int i = 0; i < LIVE_NEXT_POLL_S * 10 && !w->quit; i++)
					Threading::Sleep(100);
			}
		});
		VMManager::SetPaused(true);
		return true;
	}

	void LiveCatchupEnd(int f)
	{
		if (!std::exchange(s_live_catchup, false))
			return;
		VMManager::SetLimiterMode(s_live_limiter);
		Console.WriteLn("ZdxsvGgpo: live: caught up at frame %d, vsync %u", f, g_FrameCount);
	}

	void LivePaceReset()
	{
		g_frame_period_trim_us = 0;
		s_pace_i = 0;
		s_pace_hz = 0;
		s_pace_win_recv = -1;
		s_pace_stall = 0;
	}

	// PlayNext, before frame next: the received frames taken in, the wait at the newest frame, catch-up.
	void LiveNext(int next)
	{
		LiveTake();
		if (next >= s_play_frames && s_live_close.empty())
		{
			const int want = next + (s_run_load >= 0 || s_play_target >= 0 || s_live_catchup ? 1 : LIVE_BUFFER);
			Common::Timer wait;
			while (s_play_frames < want && s_live_close.empty())
			{
				Threading::Sleep(2);
				LiveTake();
			}
			s_live_waits++;
			s_live_wait_ms += wait.GetTimeMilliseconds();
		}
		if (!s_live_close.empty() && next >= s_play_frames && !std::exchange(s_live_close_logged, true))
			Console.WriteLn("ZdxsvGgpo: live: stream closed (%s) at frame %d, %d waits %.0f ms", s_live_close.c_str(), s_play_frames,
				s_live_waits, s_live_wait_ms);
		const int ahead = s_play_frames - next;
		if (!s_live_catchup && ahead > LIVE_BUFFER + LIVE_CATCHUP && s_run_load < 0 && s_play_target < 0)
		{
			s_live_catchup = true;
			s_live_limiter = VMManager::GetLimiterMode();
			VMManager::SetLimiterMode(LimiterModeType::Unlimited);
			Console.WriteLn("ZdxsvGgpo: live: %d frames behind at frame %d: catching up", ahead, next);
		}
		else if (s_live_catchup && ahead <= LIVE_BUFFER + LIVE_EDGE)
			LiveCatchupEnd(next);
		LivePace(next);
	}
	bool s_sync_chase = false; // s_play_req is a chase, not a host seek for the guests

	void SyncStop()
	{
		s_sync_quit = true;
		if (s_sync_thread.joinable())
			s_sync_thread.join();
		Zdxsv::SpectateSync::Leave();
		std::fill(std::begin(s_sync_pids), std::end(s_sync_pids), 0);
	}

	// CPU thread, after the replay loaded as position `me`.
	void SyncStart(int me)
	{
		const char* group = std::getenv("ZDXSV_REPLAY_GROUP");
		const char* four = std::getenv("ZDXSV_REPLAY_FOUR");
		const bool host = !group && four && four[0] == '1';
		if (!group && !host)
			return;
		if (s_live_down)
		{
			Console.Error("ZdxsvGgpo: replay four-screen: replays only, not live spectating");
			return;
		}
		std::random_device rd;
		const std::string g = group ? group : fmt::format("{:08x}{:08x}", rd(), rd());
		if (!Zdxsv::SpectateSync::Join(g, host))
			return;
		Zdxsv::SpectateSync::Control c;
		Zdxsv::SpectateSync::ReadControl(c);
		s_sync_seek_gen = c.seek_gen;
		if (host)
		{
			Zdxsv::SpectateSync::HostLimiter(static_cast<int>(VMManager::GetLimiterMode()));
			s_sync_pids[me] = Zdxsv::SpectateSync::SelfPid();
			const std::string trace = std::getenv("ZDXSV_NET_TRACE") ? std::getenv("ZDXSV_NET_TRACE") : "";
			for (int p = 0; p < s_players && p < Zdxsv::SpectateSync::GRID; p++)
			{
				if (p == me || !s_play_pov_ok[p])
					continue;
				std::vector<std::pair<std::string, std::string>> env = {
					{"ZDXSV_REPLAY_POV", std::to_string(p)}, {"ZDXSV_REPLAY_GROUP", g}, {"ZDXSV_REPLAY_FOUR", "0"}};
				if (!trace.empty())
				{
					const size_t dot = trace.find_last_of("./\\");
					const bool ext = dot != std::string::npos && trace[dot] == '.';
					env.emplace_back("ZDXSV_NET_TRACE", ext ? fmt::format("{}-pov{}{}", trace.substr(0, dot), p, trace.substr(dot)) :
					                                          fmt::format("{}-pov{}", trace, p));
				}
				s_sync_pids[p] = Zdxsv::SpectateSync::SpawnGuest(env, {"-logfile", Path::Combine(EmuFolders::Logs, fmt::format("emulog-pov{}.txt", p))});
				Console.WriteLn("ZdxsvGgpo: replay four-screen: group %s, point of view %d in pid %d", g.c_str(), p, s_sync_pids[p]);
			}
		}
		s_sync_quit = false;
		s_sync_thread = std::thread(SyncWatch, host);
	}

	// CPU thread, PlayNext: the host's seek and speed for a guest; a seek to the newest member when far behind.
	void SyncFollow(int next)
	{
		if (!Zdxsv::SpectateSync::Active())
			return;
		Zdxsv::SpectateSync::Control c;
		if (!Zdxsv::SpectateSync::IsHost() && Zdxsv::SpectateSync::ReadControl(c))
		{
			if (c.seek_gen != s_sync_seek_gen)
			{
				s_sync_seek_gen = c.seek_gen;
				s_play_req = c.seek_target;
				Console.WriteLn("ZdxsvGgpo: replay four-screen: the host seeked to frame %d", c.seek_target);
			}
			if (!PlayCatchingUp() && c.limiter >= 0 && c.limiter != static_cast<int>(VMManager::GetLimiterMode()))
				VMManager::SetLimiterMode(static_cast<LimiterModeType>(c.limiter));
		}
		if (const int lead = Zdxsv::SpectateSync::Leader();
			!s_sync_test_off && !PlayCatchingUp() && s_to_phase == TO_OFF && s_play_req == INT_MIN && lead - next > SYNC_CHASE)
		{
			Console.WriteLn("ZdxsvGgpo: replay four-screen: frame %d, %d behind: seeking to %d", next, lead - next, lead + SYNC_CHASE / 3);
			s_sync_chase = true;
			s_play_req = lead + SYNC_CHASE / 3; // the newest goes on during the seek
		}
	}

	// CPU thread, before frame `next` plays: holds it for a member behind, publishes the host's speed.
	void SyncFrame(int next)
	{
		if (!Zdxsv::SpectateSync::Active())
			return;
		if (PlayCatchingUp())
		{
			Zdxsv::SpectateSync::Publish(next, true);
			return;
		}
		if (Zdxsv::SpectateSync::IsHost())
			Zdxsv::SpectateSync::HostLimiter(static_cast<int>(VMManager::GetLimiterMode()));
		if (!s_sync_test_off)
			Zdxsv::SpectateSync::WaitForPeers(next, SYNC_WAIT_MS);
		else
			Zdxsv::SpectateSync::Publish(next, false);
		if (next % 300 == 0)
		{
			int slowest, newest;
			const int n = Zdxsv::SpectateSync::Members(slowest, newest);
			Console.WriteLn("ZdxsvGgpo: replay four-screen: frame %d, %d members, spread %d (%d..%d)", next, n,
				newest >= 0 ? newest - slowest : -1, slowest, newest);
		}
	}
} // namespace Zdxsv
