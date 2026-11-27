// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Arming a GGPO battle from the zdxsv lobby: battle info, ping test result, frame delay, the cut
// connection of a battle that cannot run (LobbyCutCall).

#include "Zdxsv/GgpoShared.h"

#include "IconsFontAwesome.h"

namespace Zdxsv
{
	namespace
	{
		// a peer that armed GGPO starts it up to its ping test's end after us, then waits its start timeout
		constexpr int CUT_REFUSAL_MS = 20000;

		void LobbyCutOsd(std::string message)
		{
			Host::AddIconOSDMessage("ZdxsvBattleCut", ICON_FA_TRIANGLE_EXCLAMATION, std::move(message), Host::OSD_ERROR_DURATION);
		}
	} // namespace

	std::mutex s_lobby_mtx;
	LobbyBattle s_lobby;
	ArmedBattle s_net;
	std::string s_report; // P2PMatchingReport of the last lobby battle (TakeLobbyReport)

	// lobby=1: a battle info came after the one the last GGPO battle armed with
	bool LobbyNewBattle()
	{
		std::lock_guard lock(s_lobby_mtx);
		return s_lobby.gen != s_net.gen;
	}
	// lobby=1: a battle info not armed yet that lists our position; n = its player count
	bool LobbyPendingBattle(int& me, int& n)
	{
		std::lock_guard lock(s_lobby_mtx);
		me = s_lobby.me;
		n = static_cast<int>(s_lobby.peers.size());
		return s_lobby.received && s_lobby.gen != s_net.gen && me >= 0 && me < n && n <= GGPO_MAX_PLAYERS;
	}
	// lobby=1, at the battle's first key msg: GGPO only when the battle info has every peer and our
	// position; else the battle connection is cut (LobbyCutCall, logged once per battle info). Without a
	// battle info it is not a lobby battle: the call goes to the IOP.
	bool LobbyArm(int me)
	{
		std::lock_guard lock(s_lobby_mtx);
		const int n = static_cast<int>(s_lobby.peers.size());
		// a peer without a listed address is reached only at a path the ping test found
		bool unlisted = false;
		for (int p = 0; p < n; p++)
			unlisted |= p != s_lobby.me && s_lobby.peers[p].empty();
		static const bool test_cut = TestEnv("ZDXSV_LOBBY_CUT") != nullptr;
		const char* why = !s_lobby.received ? "no battle info" :
		                  test_cut ? "test cut (ZDXSV_LOBBY_CUT)" :
		                  s_lobby.unreachable ? "a peer did not answer the ping test" :
		                  !s_lobby.session ? "no ggpo_session" :
		                  (n < 1 || n > GGPO_MAX_PLAYERS) ? "player count" :
		                  (me < 0 || me >= n || me != s_lobby.me) ? "own position not in the battle info" :
		                  (unlisted && s_opt.delay_set) ? "a peer has no GGPO address" :
		                                              nullptr;
		auto report = [&](const char* result) {
			s_report = s_lobby.ids + "result=" + result + "\nposition=" + std::to_string(me) + "\nplayers=" + std::to_string(n) + "\n";
		};
		if (why)
		{
			if (!s_lobby.logged && s_lobby.received) // else not a lobby battle (key msgs can come before the login)
			{
				Console.WriteLn("ZdxsvGgpo: lobby battle connection cut: %s (position %d, %d players, vsync %u)",
					why, me, n, g_FrameCount);
				s_battle.lobby_cut = true;
				s_battle.cut_sends = 0;
				report("cut");
				s_report += std::string("reason=") + why + "\n";
				if (s_lobby.session && s_lobby.me >= 0)
					Zdxsv::StartCutRefusal(s_lobby.me, static_cast<u16>(GgpoLobbyPort()), CUT_REFUSAL_MS);
				if (unlisted && s_opt.delay_set)
					LobbyCutOsd(TRANSLATE_STR("Zdxsv", "No battle: an opponent could not be reached (no network address)."));
				else
					LobbyCutOsd(TRANSLATE_STR("Zdxsv", "No battle: the battle information from the lobby is incomplete."));
			}
			s_lobby.logged = true;
			return false;
		}
		std::vector<Zdxsv::PeerAddr> peers(n);
		for (int p = 0; p < n; p++)
			if (!s_lobby.peers[p].empty())
				peers[p] = s_lobby.peers[p].front();
		if (!s_opt.delay_set)
		{
			// one-way time to the slowest peer in 16 ms frames, rounded up
			Common::Timer wait;
			std::vector<Zdxsv::RelayServerAddr> servers;
			std::string pingError;
			const std::vector<Zdxsv::PingResult> pings = Zdxsv::FinishPingTest(&servers, &pingError);
			if (!pingError.empty())
				Console.WriteLn("ZdxsvGgpo: lobby ping test failed: %s", pingError.c_str());
			std::vector<int> via(n);
			int rtt = -1, up = 0;
			for (size_t p = 0; p < pings.size() && p < peers.size(); p++)
			{
				if (pings[p].rtt <= 0)
					continue;
				rtt = std::max(rtt, pings[p].rtt), up++;
				peers[p] = pings[p].addr; // the address the ping test picked (IPv4 or IPv6), or the relay's
				via[p] = pings[p].via;
			}
			std::string rtts;
			for (size_t p = 0; p < pings.size(); p++)
				if (static_cast<int>(p) != me)
				{
					const std::string path = pings[p].via == 1 ? "peer " + std::to_string(pings[p].relay) :
					                         pings[p].via == 2 ? "relay " + std::to_string(pings[p].relay) : "direct";
					rtts += "rtt_" + std::to_string(p) + "=" + std::to_string(pings[p].rtt) +
					        (pings[p].rtt > 0 ? "\naddr_" + std::to_string(p) + "=" + pings[p].addr.String() + "\npath_" +
					                                std::to_string(p) + "=" + path : "") + "\n";
					if (pings[p].rtt > 0)
						Console.WriteLn("ZdxsvGgpo: lobby path to position %zu: %s, rtt %d ms, at %s", p, path.c_str(),
							pings[p].rtt, (pings[p].via == 2 ? pings[p].addr.String() : pings[p].addr.Masked()).c_str());
				}
			rtts += "relays=" + std::to_string(servers.size()) + "\n";
			if (!pingError.empty())
				rtts += "ping_error=" + pingError + "\n";
			s_net.via = std::move(via);
			s_net.sources.assign(n, {});
			for (size_t p = 0; p < pings.size() && p < s_net.sources.size(); p++)
				s_net.sources[p] = pings[p].sources;
			s_net.servers = std::move(servers);
			rtts += "ping_wait_ms=" + std::to_string(static_cast<int>(wait.GetTimeMilliseconds())) + "\n";
			// no GGPO unless every peer answered the ping test (same
			// session, position and address); a peer from another battle never gets our inputs
			if (up < n - 1)
			{
				Console.WriteLn("ZdxsvGgpo: lobby battle connection cut: %d of %d peers answered the ping test (position %d, waited %.1f s, vsync %u)",
					up, n - 1, me, wait.GetTimeSeconds(), g_FrameCount);
				s_lobby.unreachable = s_lobby.logged = s_battle.lobby_cut = true;
				s_battle.cut_sends = 0;
				report("cut");
				s_report += "answered=" + std::to_string(up) + "\n" + rtts;
				Zdxsv::StartCutRefusal(me, static_cast<u16>(GgpoLobbyPort()), CUT_REFUSAL_MS);
				LobbyCutOsd(!pingError.empty() ?
								TRANSLATE_STR("Zdxsv", "No battle: the network port is in use by another program.") :
								TRANSLATE_STR("Zdxsv", "No battle: an opponent did not answer. Check your router and firewall (UDP)."));
				return false;
			}
			const float learned = GgpoBudgetLearned(), used = GgpoBudgetUsed();
			s_opt.delay = GgpoBudgetDelay(rtt, s_config.min_delay, used);
			Console.WriteLn("ZdxsvGgpo: lobby delay %d: slowest peer rtt %d ms (%d of %d peers measured), min %d, rollback budget %.2f (learned %.2f), waited %.1f s for the ping test",
				s_opt.delay, rtt, up, n - 1, s_config.min_delay, used, learned, wait.GetTimeSeconds());
			report("ggpo");
			s_report += "delay=" + std::to_string(s_opt.delay) + "\nmin_delay=" + std::to_string(s_config.min_delay) + "\nrollback_budget=" +
			            fmt::format("{:.2f}", learned) + "\nrollback_budget_used=" + fmt::format("{:.2f}", used) + "\n" + rtts;
		}
		else
		{
			report("ggpo");
			s_report += "delay=" + std::to_string(s_opt.delay) + "\nfixed_delay=1\n";
			s_net.via.clear();
			s_net.sources.clear();
			s_net.servers.clear();
		}
		s_opt.players = n;
		s_net.peers = std::move(peers);
		s_net.players = s_lobby.players;
		s_net.patches = s_lobby.patches;
		s_net.gen = s_lobby.gen;
		return true;
	}
	// lobby=1, no GGPO session (LobbyArm): a connection failure, no fallback to the battle server.
	// Sends are dropped, nothing to recv, and the poll fails, so the game closes the sock and goes back to
	// the lobby at once instead of after its timeout. That close goes to the IOP and ends the cut.
	bool LobbyCutCall(u32 fno, s16 sock, s16 len)
	{
		u8* ram = eeMem->Main;
		s32 result = 0;
		if (sock == NET_BATTLE_SOCK && fno == NET_FNO_SEND)
			result = std::clamp<s32>(len, 0, 0x3ca), s_battle.cut_sends++;
		else if (sock == NET_BATTLE_SOCK && fno == NET_FNO_POLL)
			result = -1;
		else if (!(sock == NET_BATTLE_SOCK && fno == NET_FNO_RECV))
		{
			if (fno == 7 || fno == 0xd)
			{
				Console.WriteLn("ZdxsvGgpo: lobby battle connection cut ended at vsync %u: game RPC 0x%x after %u dropped sends",
					g_FrameCount, fno, s_battle.cut_sends);
				s_battle.lobby_cut = false;
				std::lock_guard lock(s_lobby_mtx);
				if (!s_report.empty())
					s_report += "cut_sends=" + std::to_string(s_battle.cut_sends) + "\n";
			}
			return false;
		}
		*reinterpret_cast<s32*>(ram + NET_RES_LEN) = result;
		cpuRegs.GPR.n.v0.SD[0] = result;
		cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
		return true;
	}

	int GgpoLobbyPort()
	{
		// s_config.options only changes in GgpoOnVmInitialize, before the CPU thread calls this
		if (s_config.options.find("net=1") == std::string::npos || s_config.options.find("lobby=1") == std::string::npos)
			return 0;
		int p = s_config.lobby_port;
		for (const std::string_view item : StringUtil::SplitString(s_config.options, ','))
			if (item.starts_with("port="))
				p = StringUtil::FromChars<int>(item.substr(5)).value_or(0);
		return (p > 0 && p <= 0xFFFF) ? p : 0;
	}

	int GgpoLobbyAdvertisePort()
	{
		if (GgpoLobbyPort() <= 0)
			return 0;
		int p = 0;
		for (const std::string_view item : StringUtil::SplitString(s_config.options, ','))
			if (item.starts_with("advertise="))
				p = StringUtil::FromChars<int>(item.substr(10)).value_or(0);
		return (p > 0 && p <= 0xFFFF) ? p : 0;
	}

	void SetLobbyPeers(int me, std::vector<std::vector<Zdxsv::PeerAddr>> byPosition, u32 session, int pingMs, std::string ids,
		std::vector<LobbyPlayer> players, std::vector<Zdxsv::BattleInfo::Relay> relays, std::vector<Zdxsv::GamePatch> patches)
	{
		std::lock_guard lock(s_lobby_mtx);
		s_lobby.ids = std::move(ids);
		s_lobby.players = std::move(players);
		s_lobby.received = true;
		s_lobby.gen++;
		s_lobby.me = me;
		s_lobby.logged = s_lobby.unreachable = false;
		if (s_opt.bad_session && session)
		{
			session ^= 0x5a5a5a5a;
			Console.WriteLn("ZdxsvGgpo: badsession=1: ping test session %08x", session);
		}
		s_lobby.session = session;
		s_lobby.peers = std::move(byPosition);
		s_lobby.relays = std::move(relays);
		s_lobby.patches = std::move(patches);
		std::vector<int> teams;
		for (const LobbyPlayer& p : s_lobby.players)
			teams.push_back(p.team);
		if (me >= 0 && session && !s_opt.delay_set && pingMs > 0)
			Zdxsv::StartPingTest(session, me, s_lobby.peers, static_cast<u16>(GgpoLobbyPort()), pingMs, s_lobby.relays, teams);
	}

	std::string TakeLobbyReport()
	{
		std::lock_guard lock(s_lobby_mtx);
		return std::exchange(s_report, {});
	}

	// The report of a battle already played is kept: the next lobby login sends it.
	void LobbyReset()
	{
		std::lock_guard lock(s_lobby_mtx);
		if (s_lobby.received && s_lobby.gen != s_net.gen)
			Console.WriteLn("ZdxsvGgpo: battle info dropped before its battle started (position %d)", s_lobby.me);
		s_lobby.received = s_lobby.logged = s_lobby.unreachable = false;
		s_lobby.me = -1;
		s_lobby.peers.clear();
		s_lobby.relays.clear();
		s_lobby.players.clear();
		s_lobby.patches.clear();
		s_lobby.session = 0;
		s_lobby.ids.clear();
		s_net.peers.clear();
		s_net.via.clear();
		s_net.sources.clear();
		s_net.servers.clear();
		s_net.players.clear();
		s_net.patches.clear();
		s_net.gen = s_lobby.gen;
		StopPingTest();
	}
} // namespace Zdxsv
