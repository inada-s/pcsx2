// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/Dev9Hooks.h"
#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/Ggpo.h"
#include "Zdxsv/Lobby.h"
#include "Zdxsv/Settings.h"
#include "Zdxsv/SyncSettings.h"
#include "Zdxsv/TestOptions.h"
#include "Zdxsv/Upnp.h"

#include "BuildVersion.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/HTTPDownloader.h"
#include "common/StringUtil.h"

#include "fmt/format.h"
#define XXH_INLINE_ALL 1
#include "xxhash.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
#ifdef __APPLE__
#include <IOKit/IOKitLib.h>
#endif
#define SOCKET_ERROR -1
#endif

namespace Zdxsv
{
	namespace
	{
		// The host id hashed, never sent raw: Windows MachineGuid, the macOS serial number; empty elsewhere.
		std::string MachineId()
		{
			std::string id;
#if defined(_WIN32)
			char value[256];
			DWORD size = sizeof(value);
			if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", RRF_RT_REG_SZ, nullptr, value, &size) ==
				ERROR_SUCCESS)
				id = value;
#elif defined(__APPLE__)
			const io_service_t expert = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("IOPlatformExpertDevice"));
			if (expert)
			{
				if (const CFTypeRef serial = IORegistryEntryCreateCFProperty(expert, CFSTR(kIOPlatformSerialNumberKey), kCFAllocatorDefault, 0))
				{
					char value[256];
					if (CFGetTypeID(serial) == CFStringGetTypeID() &&
						CFStringGetCString(static_cast<CFStringRef>(serial), value, sizeof(value), kCFStringEncodingUTF8))
						id = value;
					CFRelease(serial);
				}
				IOObjectRelease(expert);
			}
#endif
			return id.empty() ? id : fmt::format("{:016x}", XXH64(id.data(), id.size(), 37));
		}

		// The match report on one line for the log, addr_<pos> values masked (the lobby gets them whole).
		std::string ReportLogLine(const std::string& report)
		{
			std::string out;
			size_t start = 0;
			while (start < report.size())
			{
				size_t end = report.find('\n', start);
				if (end == std::string::npos)
					end = report.size();
				std::string line = report.substr(start, end - start);
				if (const size_t eq = line.find('='); line.starts_with("addr_") && eq != std::string::npos)
					line = line.substr(0, eq + 1) + MaskIpAddress(line.substr(eq + 1));
				out += (out.empty() ? "" : " ") + line;
				start = end + 1;
			}
			return out;
		}

		// ZDXSV_GGPO net=1,lobby=1: battle infos go to Zdxsv/Ggpo.cpp as GGPO peers. Returns our GGPO port, 0 = off.
		int ListenGgpo()
		{
			const int port = GgpoLobbyPort();
			if (port > 0)
				SetBattleInfoListener([](const BattleInfo& info) {
					std::vector<std::vector<PeerAddr>> byPosition;
					const int me = GgpoPeers(info, PublicIP(), byPosition);
					std::vector<LobbyPlayer> players;
					for (const std::string& u : info.users)
					{
						const auto name = info.names.find(u);
						const auto pilot = info.pilots.find(u);
						const auto sync = info.syncs.find(u);
						players.push_back({u, name == info.names.end() ? std::string() : name->second,
							pilot == info.pilots.end() ? std::string() : pilot->second,
							sync == info.syncs.end() ? std::string() : sync->second});
					}
					std::string ids = "battle_code=" + info.battleCode + "\nuser_id=" + info.userId + "\n";
					if (info.liveUplink && !LobbyUdpAddr().empty())
						ids += "live_uplink=" + LobbyUdpAddr() + "\n"; // Ggpo.cpp ReplayBegin streams the battle there
					SetLobbyPeers(me, std::move(byPosition), info.ggpoSession, info.ggpoPingMs, std::move(ids), std::move(players), info.relays);
				});
			return port;
		}

		std::string UdpTestUrl()
		{
			if (const char* env = std::getenv("ZDXSV_UDPTEST_URL"))
				return env;
			return Host::GetStringSettingValue("DEV9/Eth", "ZdxsvUdpTestUrl", UDPTEST_URL);
		}

		bool AskUdpReach(const std::string& url, const std::string& addr, const std::atomic<bool>* stop)
		{
			std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Host::GetHTTPUserAgent());
			if (!http)
				return false;
			http->SetTimeout(5.0f);
			s32 status = HTTPDownloader::HTTP_STATUS_ERROR;
			http->CreatePostRequest(url, "addr=" + addr, [&status](s32 code, const std::string&, HTTPDownloader::Request::Data) { status = code; },
				nullptr, "application/x-www-form-urlencoded");
			while (http->HasAnyRequests() && !(stop && stop->load()))
			{
				http->PollRequests();
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			if (status != HTTPDownloader::HTTP_STATUS_OK)
				Console.Warning("DEV9: udp test: %s answered %d", url.c_str(), status);
			return status == HTTPDownloader::HTTP_STATUS_OK;
		}

		// The texts must match the summaries UdpTest sets; any other summary is shown untranslated.
		std::string UdpSummaryText(const std::string& summary)
		{
			static constexpr const char* texts[] = {
				TRANSLATE_NOOP("Zdxsv", "open"),
				TRANSLATE_NOOP("Zdxsv", "cone NAT (port closed)"),
				TRANSLATE_NOOP("Zdxsv", "cone NAT (open to the server only)"),
				TRANSLATE_NOOP("Zdxsv", "symmetric NAT (port closed)"),
				TRANSLATE_NOOP("Zdxsv", "unknown"),
				TRANSLATE_NOOP("Zdxsv", "unknown (port busy)"),
				TRANSLATE_NOOP("Zdxsv", "unknown (test cancelled)"),
				TRANSLATE_NOOP("Zdxsv", "unknown (no answer from the server)"),
				TRANSLATE_NOOP("Zdxsv", "unknown (no answer from the server's test socket)"),
			};
			for (const char* text : texts)
			{
				if (summary == text)
					return Host::TranslateToString("Zdxsv", text);
			}
			return summary;
		}

		// lobby=1: battle infos go to GGPO. Returns the platform info lines of OpenUdp and our GGPO port
		// (0 = off: the connection is plain TCP). filter: the connection's lobby filter when on.
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
			std::string lines = OpenUdp(serverIp, stunPort, static_cast<u16>(ggpoPort));
			// Connectivity test of the GGPO port, once per run, on its own thread so the DEV9 rx thread does
			// not stall: its nat= line goes out from the first lobby connection after it ended. Not on an
			// adopted connection: it sends no platform info.
			static std::mutex natMutex;
			static std::string natLine;
			static bool natStarted = false;
			std::lock_guard natLock(natMutex);
			if (natTest && !natStarted)
				natStarted = StartUdpTest(serverIp, stunPort, static_cast<u16>(ggpoPort),
					UdpTestUrl().empty() ? UdpReachAsk{} : [url = UdpTestUrl()](const std::string& addr, const std::atomic<bool>* stop) {
						return AskUdpReach(url, addr, stop);
					},
					[](const std::string& line, const std::string& summary) {
						Host::AddIconOSDMessage("ZdxsvUdpTest", ICON_FA_NETWORK_WIRED,
							TRANSLATE_STR("Zdxsv", "P2P connectivity:") + " " + UdpSummaryText(summary), 10.0f);
						std::lock_guard lock(natMutex);
						natLine = line;
					},
					[ggpoPort](std::string& lines) {
						if (!UpnpEnabled())
							return false;
						std::string error;
						const bool mapped = UpnpMapUdp(static_cast<u16>(ggpoPort), error);
						lines = "upnp_result=" + (mapped ? std::string("Success") : error) + "\n";
						if (const std::string local = UpnpLocalAddress(); !local.empty())
							lines += "upnp_local_ip=" + local + "\n";
						return mapped;
					});
			return lines + natLine;
		}

		// Release builds: their tag zdxsv-X.Y.Z. Builds N commits after one: zdxsv-X.Y.(Z+1)-dev.N, which sorts
		// above X.Y.Z and below the next release. Empty without a zdxsv release tag (local and upstream builds).
		std::string ClientVersion()
		{
			int major, minor, patch, ahead;
			char end;
			if (std::sscanf(BuildVersion::GitTag, "zdxsv-%d.%d.%d%c", &major, &minor, &patch, &end) == 3)
				return fmt::format("zdxsv-{}.{}.{}", major, minor, patch);
			if (std::sscanf(BuildVersion::GitRev, "zdxsv-%d.%d.%d-%d-g%c", &major, &minor, &patch, &ahead, &end) == 5)
				return fmt::format("zdxsv-{}.{}.{}-dev.{}", major, minor, patch + 1, ahead);
			return "";
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

	// On a connection the lobby opens with the key pair question (0x6101), send the platform info
	// (custom command 0x9950, "key=value" lines) before the game answers, so the lobby can tell
	// emulator players apart. The game never sees it.
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
		if (const std::string version = ClientVersion(); !version.empty())
			body += "pcsx2=" + version + "\n";
		// the lobby passes it to the battle's peers (sync_<user>); no battle unless all agree (LobbyArm)
		if (const std::string sync = SyncFingerprint(); !sync.empty())
			body += "sync=" + sync + "\n";
#if defined(_WIN32)
		body += "os=windows\n";
#elif defined(__APPLE__)
		body += "os=macos\n";
#else
		body += "os=linux\n";
#endif
#if defined(_M_X86)
		body += "cpu=x86/64\n";
#elif defined(ARCH_ARM64)
		body += "cpu=arm64\n";
#else
		body += "cpu=unknown\n";
#endif
		const std::string machineId = MachineId();
		if (!machineId.empty())
			body += "machine_id=" + machineId + "\n";
		// lobby=1: udp=1 asks the lobby for battle info (0x9951); relay_server=1: GGPO may go through its relay servers.
		int ggpoPort;
		LobbyFilter* filter = nullptr;
		const std::string udpLines = OpenLobby(serverIp, ggpoPort, filter);
		if (!filter)
			filter = new LobbyFilter();
		const int advertise = GgpoLobbyAdvertisePort();
		if (ggpoPort > 0 && advertise > 0)
		{
			const std::string port = std::to_string(advertise);
			body += "udp=1\nudp_addr=127.0.0.1:" + port + "\nggpo=" + port + "\nrelay_server=1\n";
			Console.WriteLn("DEV9: TCP: zdxsv advertise=%d: platform info announces 127.0.0.1:%d only", advertise, advertise);
		}
		else if (ggpoPort > 0)
			body += "udp=1\n" + udpLines + "ggpo=" + std::to_string(ggpoPort) + "\nrelay_server=1\n";
		// From the first lobby connection after the test ended (a reconnect follows every battle).
		HttpsLatencyStart();
		body += HttpsLatencyLines();

		std::vector<u8> msg;
		const auto append = [&msg](u8 command, const std::string& b) {
			const u8 header[12] = {0x81, 0xFF, 0x99, command, static_cast<u8>(b.size() >> 8), static_cast<u8>(b.size()),
				0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
			msg.insert(msg.end(), header, header + 12);
			msg.insert(msg.end(), b.begin(), b.end());
		};
		append(0x50, body);
		// The last lobby battle's report (0x9952 P2PMatchingReport).
		const std::string report = ggpoPort > 0 ? TakeLobbyReport() : "";
		const std::vector<uint8_t> encoded = EncodeMatchingReport(report);
		if (!report.empty() && encoded.size() < 0x8000)
		{
			append(0x52, std::string(encoded.begin(), encoded.end()));
			Console.WriteLn("DEV9: TCP: zdxsv matching report: %s", ReportLogLine(report).c_str());
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
		Console.WriteLn("DEV9: TCP: zdxsv lobby detected, sent platform info (%zu bytes, machine_id %s)", msg.size(),
			machineId.empty() ? "none" : machineId.c_str());
		return filter;
	}

	// A paused game reads nothing: the lobby would close the connection after its receive timeout.
	void LobbyFeed(LobbyFilter& filter, uptr socket, const u8* data, int len)
	{
		static const bool off = [] {
			const char* env = TestEnv("ZDXSV_PAUSE_LINE_CHECK");
			return env && std::string(env) == "0";
		}();
		filter.Feed(data, static_cast<size_t>(len), !off && VMManager::GetState() == VMState::Paused);
		if (filter.TakePleaseUpdate())
		{
			Console.Warning("DEV9: TCP: zdxsv lobby refused this PCSX2 as outdated");
			Host::AddIconOSDMessage("ZdxsvPleaseUpdate", ICON_FA_TRIANGLE_EXCLAMATION,
				TRANSLATE_STR("Zdxsv", "The lobby refused this PCSX2 as outdated. Please update it."), Host::OSD_ERROR_DURATION);
		}
		const std::vector<u8> answers = filter.TakeAnswers();
		if (answers.empty())
			return;
		const int ret = send(static_cast<sock_t>(socket), reinterpret_cast<const char*>(answers.data()), static_cast<int>(answers.size()), 0);
		if (ret == static_cast<int>(answers.size()))
			Console.WriteLn("DEV9: TCP: zdxsv answered the lobby line check (paused)");
		else
			Console.Error("DEV9: TCP: zdxsv paused line check answer: sent %d of %zu bytes", ret, answers.size());
	}
} // namespace Zdxsv
