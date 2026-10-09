// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Arming a GGPO battle from the zdxsv lobby: battle info, ping test result, frame delay, the cut
// connection of a battle that cannot run (LobbyCutCall).

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{
	namespace
	{
		bool s_lobby_info = false, s_lobby_ok = false, s_lobby_logged = false, s_lobby_unreachable = false;
		std::vector<std::vector<Zdxsv::PeerAddr>> s_lobby_peers; // candidates per position
		std::vector<Zdxsv::BattleInfo::Relay> s_lobby_relays; // relay servers of the last battle info
		u32 s_lobby_gen = 0, s_armed_gen = 0; // battle infos received; the one the last GGPO battle armed with
	} // namespace

	// lobby=1: peers of the last battle info (SetLobbyPeers, DEV9 thread)
	std::mutex s_lobby_mtx;
	std::vector<Zdxsv::PeerAddr> s_net_peers; // per position, picked when armed
	std::vector<int> s_net_via; // per position: PingResult.via (0 direct, 1 peer relay, 2 relay server)
	std::vector<Zdxsv::RelayServerAddr> s_net_servers; // relay servers, registered with GGPO in order
	u32 s_lobby_session = 0; // ggpo_session
	std::string s_report_ids; // battle info lines naming the battle (SetLobbyPeers)
	std::string s_report; // P2PMatchingReport of the last lobby battle (TakeLobbyReport)

	// lobby=1: a battle info came after the one the last GGPO battle armed with
	bool LobbyNewBattle()
	{
		std::lock_guard lock(s_lobby_mtx);
		return s_lobby_gen != s_armed_gen;
	}
	// lobby=1, at the battle's first key msg: GGPO only when the battle info has every peer and our
	// position; else the battle connection is cut (LobbyCutCall, logged once per battle info). Without a
	// battle info it is not a lobby battle: the call goes to the IOP.
	bool LobbyArm(int me)
	{
		std::lock_guard lock(s_lobby_mtx);
		const int n = static_cast<int>(s_lobby_peers.size());
		const char* why = !s_lobby_info ? "no battle info" :
		                  !s_lobby_ok ? "a peer has no GGPO address" :
		                  s_lobby_unreachable ? "a peer did not answer the ping test" :
		                  !s_lobby_session ? "no ggpo_session" :
		                  (n < 2 || n > GGPO_MAX_PLAYERS) ? "player count" :
		                  (me < 0 || me >= n || !s_lobby_peers[me].empty()) ? "own position not in the battle info" :
		                                                                         nullptr;
		auto report = [&](const char* result) {
			s_report = s_report_ids + "result=" + result + "\nposition=" + std::to_string(me) + "\nplayers=" + std::to_string(n) + "\n";
		};
		if (why)
		{
			if (!s_lobby_logged && s_lobby_info) // else not a lobby battle (key msgs can come before the login)
			{
				Console.WriteLn("ZdxsvGgpo: lobby battle connection cut: %s (position %d, %d players, vsync %u)",
					why, me, n, g_FrameCount);
				s_lobby_cut = true;
				s_cut_sends = 0;
				report("cut");
				s_report += std::string("reason=") + why + "\n";
			}
			s_lobby_logged = true;
			return false;
		}
		std::vector<Zdxsv::PeerAddr> peers(n);
		for (int p = 0; p < n; p++)
			if (!s_lobby_peers[p].empty())
				peers[p] = s_lobby_peers[p].front();
		if (!s_delay_set)
		{
			// as flycast's rollback backend: one-way time to the slowest peer in 16 ms frames, rounded up
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
							pings[p].rtt, pings[p].addr.String().c_str());
				}
			rtts += "relays=" + std::to_string(servers.size()) + "\n";
			if (!pingError.empty())
				rtts += "ping_error=" + pingError + "\n";
			s_net_via = std::move(via);
			s_net_servers = std::move(servers);
			rtts += "ping_wait_ms=" + std::to_string(static_cast<int>(wait.GetTimeMilliseconds())) + "\n";
			// as flycast ("Peer%d unreachable"): no GGPO unless every peer answered the ping test (same
			// session, position and address); a peer from another battle never gets our inputs
			if (up < n - 1)
			{
				Console.WriteLn("ZdxsvGgpo: lobby battle connection cut: %d of %d peers answered the ping test (position %d, waited %.1f s, vsync %u)",
					up, n - 1, me, wait.GetTimeSeconds(), g_FrameCount);
				s_lobby_unreachable = s_lobby_logged = s_lobby_cut = true;
				s_cut_sends = 0;
				report("cut");
				s_report += "answered=" + std::to_string(up) + "\n" + rtts;
				return false;
			}
			s_delay = std::max(s_min_delay, rtt > 0 ? (rtt + 31) / 32 : 0);
			Console.WriteLn("ZdxsvGgpo: lobby delay %d: slowest peer rtt %d ms (%d of %d peers measured), min %d, waited %.1f s for the ping test",
				s_delay, rtt, up, n - 1, s_min_delay, wait.GetTimeSeconds());
			report("ggpo");
			s_report += "delay=" + std::to_string(s_delay) + "\nmin_delay=" + std::to_string(s_min_delay) + "\n" + rtts;
		}
		else
		{
			report("ggpo");
			s_report += "delay=" + std::to_string(s_delay) + "\nfixed_delay=1\n";
			s_net_via.clear();
			s_net_servers.clear();
		}
		s_players = n;
		s_net_peers = std::move(peers);
		s_net_players = s_lobby_players;
		s_armed_gen = s_lobby_gen;
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
			result = std::clamp<s32>(len, 0, 0x3ca), s_cut_sends++;
		else if (sock == NET_BATTLE_SOCK && fno == NET_FNO_POLL)
			result = -1;
		else if (!(sock == NET_BATTLE_SOCK && fno == NET_FNO_RECV))
		{
			if (fno == 7 || fno == 0xd)
			{
				Console.WriteLn("ZdxsvGgpo: lobby battle connection cut ended at vsync %u: game RPC 0x%x after %u dropped sends",
					g_FrameCount, fno, s_cut_sends);
				s_lobby_cut = false;
				std::lock_guard lock(s_lobby_mtx);
				if (!s_report.empty())
					s_report += "cut_sends=" + std::to_string(s_cut_sends) + "\n";
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
		// s_options only changes in GgpoOnVmInitialize, before the DEV9 thread that calls this runs
		if (s_options.find("net=1") == std::string::npos || s_options.find("lobby=1") == std::string::npos)
			return 0;
		int p = 7001;
		for (const std::string_view item : StringUtil::SplitString(s_options, ','))
			if (item.starts_with("port="))
				p = StringUtil::FromChars<int>(item.substr(5)).value_or(0);
		return (p > 0 && p <= 0xFFFF) ? p : 0;
	}

	int GgpoLobbyAdvertisePort()
	{
		if (GgpoLobbyPort() <= 0)
			return 0;
		int p = 0;
		for (const std::string_view item : StringUtil::SplitString(s_options, ','))
			if (item.starts_with("advertise="))
				p = StringUtil::FromChars<int>(item.substr(10)).value_or(0);
		return (p > 0 && p <= 0xFFFF) ? p : 0;
	}

	void SetLobbyPeers(bool ok, std::vector<std::vector<Zdxsv::PeerAddr>> byPosition, u32 session, int pingMs, std::string ids,
		std::vector<LobbyPlayer> players, std::vector<Zdxsv::BattleInfo::Relay> relays)
	{
		std::lock_guard lock(s_lobby_mtx);
		s_report_ids = std::move(ids);
		s_lobby_players = std::move(players);
		s_lobby_info = true;
		s_lobby_gen++;
		s_lobby_ok = ok;
		s_lobby_logged = s_lobby_unreachable = false;
		if (s_bad_session && session)
		{
			session ^= 0x5a5a5a5a;
			Console.WriteLn("ZdxsvGgpo: badsession=1: ping test session %08x", session);
		}
		s_lobby_session = session;
		s_lobby_peers = std::move(byPosition);
		s_lobby_relays = std::move(relays);
		if (ok && session && !s_delay_set && pingMs > 0)
			Zdxsv::StartPingTest(session, s_lobby_peers, static_cast<u16>(GgpoLobbyPort()), pingMs, s_lobby_relays);
	}

	std::string TakeLobbyReport()
	{
		std::lock_guard lock(s_lobby_mtx);
		return std::exchange(s_report, {});
	}
} // namespace Zdxsv
