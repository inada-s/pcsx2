// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/Dev9Hooks.h"
#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/Ggpo.h"
#include "Zdxsv/Lobby.h"
#include "Zdxsv/TestOptions.h"

#include "BuildVersion.h"
#include "Host.h"
#include "IconsFontAwesome.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include <bit>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // std::min/std::max; the CMake build doesn't define it, the MSBuild one does
#endif
#include <winsock2.h>
using sock_t = SOCKET;
#else
#include <errno.h>
#include <sys/socket.h>
using sock_t = int;
#define SOCKET_ERROR -1
#endif

namespace Zdxsv
{
	namespace
	{
		// ZDXSV_GGPO net=1,lobby=1: battle infos go to Zdxsv/Ggpo.cpp as GGPO peers. Returns our GGPO port, 0 = off.
		int ListenGgpo()
		{
			const int port = GgpoLobbyPort();
			if (port > 0)
				SetBattleInfoListener([](const BattleInfo& info) {
					std::vector<std::vector<PeerAddr>> byPosition;
					const bool ok = GgpoPeers(info, PublicIP(), byPosition);
					std::vector<std::pair<std::string, std::string>> players;
					for (const std::string& u : info.users)
					{
						const auto name = info.names.find(u);
						players.emplace_back(u, name == info.names.end() ? std::string() : name->second);
					}
					std::string ids = "battle_code=" + info.battleCode + "\nuser_id=" + info.userId + "\n";
					if (info.liveUplink && !LobbyUdpAddr().empty())
						ids += "live_uplink=" + LobbyUdpAddr() + "\n"; // Ggpo.cpp ReplayBegin streams the battle there
					SetLobbyPeers(ok, std::move(byPosition), info.ggpoSession, info.ggpoPingMs, std::move(ids), std::move(players), info.relays);
				});
			return port;
		}

		// ZDXSV_GGPO net=1,lobby=1 on a lobby connection: battle infos go to GGPO (ListenGgpo), the
		// lobby's UDP STUN gives our public address. Returns platform info lines "udp_addr=..\nudp_local=..\n"
		// (+ "udp_addr6=[..]:..\n" with a global IPv6 address) and our GGPO port (0 = off: nothing done, the
		// connection is plain TCP). filter: the connection's lobby filter when on.
		std::string OpenLobby(u32 serverIp, int& ggpoPort, LobbyFilter*& filter, bool natTest = true)
		{
			ggpoPort = ListenGgpo();
			if (ggpoPort <= 0)
				return "";
			LobbySetLogger([](const std::string& s) { Console.WriteLn("DEV9: %s", s.c_str()); });
			filter = new LobbyFilter();
			// The lobby's UDP STUN is on its host at 8201 (zdxsv docker-compose); ZDXSV_STUN_PORT overrides.
			const char* stunPortEnv = std::getenv("ZDXSV_STUN_PORT");
			const u16 stunPort = stunPortEnv ? static_cast<u16>(std::atoi(stunPortEnv)) : 8201;
			std::string lines = OpenUdp(serverIp, stunPort);
			// Connectivity test of the GGPO port (flycast's P2P feasibility test), once per run: the lobby
			// reconnects after every battle and the result does not change in between. It runs on its own
			// thread (up to ~2 s without answers; on this DEV9 rx thread it stalled the network), so its
			// nat= line goes out from the first lobby connection after it ended. Not on an adopted
			// connection (3 of 3 adoptions after a state load never reached the lobby while it ran
			// there; no platform info goes out there anyway).
			static std::mutex natMutex;
			static std::string natLine;
			static bool natStarted = false;
			std::lock_guard natLock(natMutex);
			if (natTest && !natStarted)
				natStarted = StartUdpTest(serverIp, stunPort, static_cast<u16>(ggpoPort),
					[](const std::string& line, const std::string& summary) {
						Host::AddIconOSDMessage("ZdxsvUdpTest", ICON_FA_NETWORK_WIRED, "P2P connectivity: " + summary, 10.0f);
						std::lock_guard lock(natMutex);
						natLine = line;
					});
			return lines + natLine;
		}
	} // namespace

	// Adopted after a state load: the server side starts mid-session (no key
	// pair question, no platform info), but battle info and STUN still work as
	// on a fresh lobby connection.
	LobbyFilter* LobbyOnAdopted(u32 serverIp)
	{
		int ggpoPort;
		LobbyFilter* filter = nullptr;
		OpenLobby(serverIp, ggpoPort, filter, false); // the lobby cannot know the port (no platform info): fake_lobby.py --ggpo
		return filter;
	}

	// The zdxsv lobby server opens every connection with a key pair question
	// (dir 0x18, category 0x01, command 0x6101). On such a connection, send the
	// server a custom message (dir 0x81, category 0xFF, command 0x9950) with
	// "key=value" lines, before the game answers. Real PS2 never sends it, so the
	// server can keep emulator and console players apart (as gdxsv does).
	// The PS2 side never sees this message.
	LobbyFilter* LobbyOnFirstData(uptr socket, u32 serverIp, const u8* data, int len)
	{
		const sock_t client = static_cast<sock_t>(socket);
		if (len < 12 || data[0] != 0x18 || data[1] != 0x01 || data[2] != 0x61 || data[3] != 0x01)
			return nullptr;
		// Only the Z game talks to the zdxsv lobby: any other game's server that happens to open the same way
		// gets nothing (the platform info carries our addresses).
		if (!g_z_game)
		{
			Console.WriteLn("DEV9: TCP: zdxsv lobby question, but not the Z game: no platform info");
			return nullptr;
		}
		// ZDXSV_PLATFORM_INFO=0: behave like a real PS2 (for testing the console side)
		const char* env = TestEnv("ZDXSV_PLATFORM_INFO");
		if (env && std::string(env) == "0")
		{
			Console.WriteLn("DEV9: TCP: zdxsv lobby detected, platform info off (ZDXSV_PLATFORM_INFO=0)");
			return nullptr;
		}

		std::string body = "emulator=pcsx2\n";
		body += std::string("version=") + BuildVersion::GitRev + "\n";
#if defined(_WIN32)
		body += "os=windows\n";
#elif defined(__APPLE__)
		body += "os=macos\n";
#else
		body += "os=linux\n";
#endif
#if defined(_M_X86)
		body += "cpu=x86/64\n";
#elif defined(_M_ARM64)
		body += "cpu=arm64\n";
#else
		body += "cpu=unknown\n";
#endif
		// ZDXSV_GGPO net=1,lobby=1: udp=1 makes the lobby send battle info (0x9951) with every player's
		// address (udp_addr/udp_local/udp_addr6) and ggpo=port; when every other player has one the battle runs over
		// GGPO (Zdxsv/Ggpo.cpp), else on the battle server. The game always connects to the battle server by TCP.
		// relay_server=1: we can route GGPO through the lobby's relay servers (battle info relay_<k>), as flycast.
		int ggpoPort;
		LobbyFilter* filter = nullptr;
		const std::string udpLines = OpenLobby(serverIp, ggpoPort, filter);
		const int advertise = GgpoLobbyAdvertisePort();
		if (ggpoPort > 0 && advertise > 0)
		{
			const std::string port = std::to_string(advertise);
			body += "udp=1\nudp_addr=127.0.0.1:" + port + "\nggpo=" + port + "\nrelay_server=1\n";
			Console.WriteLn("DEV9: TCP: zdxsv advertise=%d: platform info announces 127.0.0.1:%d only", advertise, advertise);
		}
		else if (ggpoPort > 0)
			body += "udp=1\n" + udpLines + "ggpo=" + std::to_string(ggpoPort) + "\nrelay_server=1\n";

		std::vector<u8> msg;
		const auto append = [&msg](u8 command, const std::string& b) {
			const u8 header[12] = {0x81, 0xFF, 0x99, command, static_cast<u8>(b.size() >> 8), static_cast<u8>(b.size()),
				0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
			msg.insert(msg.end(), header, header + 12);
			msg.insert(msg.end(), b.begin(), b.end());
		};
		append(0x50, body);
		// The last lobby battle's report (0x9952 P2PMatchingReport, as flycast's lbsP2PMatchingReport).
		const std::string report = ggpoPort > 0 ? TakeLobbyReport() : "";
		if (!report.empty() && report.size() < 0x8000)
		{
			append(0x52, report);
			Console.WriteLn("DEV9: TCP: zdxsv matching report: %s", StringUtil::ReplaceAll(report, "\n", " ").c_str());
		}

		size_t sent = 0;
		while (sent < msg.size())
		{
			const int ret = send(client, reinterpret_cast<const char*>(&msg[sent]), static_cast<int>(msg.size() - sent), 0);
			if (ret == SOCKET_ERROR)
			{
#ifdef _WIN32
				const int err = WSAGetLastError();
				if (err == WSAEWOULDBLOCK)
#else
				const int err = errno;
				if (err == EWOULDBLOCK)
#endif
					std::this_thread::yield();
				else
				{
					Console.Error("DEV9: TCP: zdxsv platform info send error: %d", err);
					return filter;
				}
			}
			else
				sent += ret;
		}
		Console.WriteLn("DEV9: TCP: zdxsv lobby detected, sent platform info (%zu bytes)", msg.size());
		return filter;
	}
} // namespace Zdxsv
