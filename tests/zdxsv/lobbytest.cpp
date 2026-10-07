// Unit test of the waits in pcsx2/Zdxsv/Lobby.cpp (no PCSX2, no game): a fake zdxsv STUN on
// 127.0.0.1 (silent, or answering after a delay) and held UDP ports. Build and run: lobbytest.sh.
// Exit 0 = every check passed; a failed check prints FAIL.
// Uses 127.0.0.1 UDP ports 27110-27130 and 27201-27203.
#include "Zdxsv/Lobby.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using Clock = std::chrono::steady_clock;
static long Ms(Clock::time_point t0) { return (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count(); }

static int g_fails = 0;
static void Check(bool ok, const char* what, long value)
{
	std::printf("%s %s (%ld)\n", ok ? "PASS" : "FAIL", what, value);
	g_fails += !ok;
}

// fake zdxsv STUN on 127.0.0.1:port: mode 0 silent, else answers Pong{1.2.3.4:5555} after mode ms
struct FakeStun
{
	SOCKET s;
	std::atomic<int> mode{0};
	std::atomic<bool> stop{false};
	std::thread th;
	explicit FakeStun(uint16_t port)
	{
		s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(port);
		a.sin_addr.s_addr = inet_addr("127.0.0.1");
		if (bind(s, (sockaddr*)&a, sizeof(a)) != 0)
			std::printf("FAIL fake STUN bind :%u\n", port), g_fails++;
		th = std::thread([this] {
			while (!stop)
			{
				fd_set rd;
				FD_ZERO(&rd);
				FD_SET(s, &rd);
				timeval tv{0, 20000};
				if (select(0, &rd, nullptr, nullptr, &tv) <= 0)
					continue;
				char buf[512];
				sockaddr_in from{};
				int fl = sizeof(from);
				const int n = recvfrom(s, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
				const int m = mode;
				if (n <= 0 || m == 0)
					continue;
				Zdxsv::ProtoPacket pong;
				pong.type = Zdxsv::ProtoPong;
				pong.publicAddr = "1.2.3.4:5555";
				const std::vector<uint8_t> d = Zdxsv::ProtoEncode(pong);
				std::thread([this, d, from, m] {
					std::this_thread::sleep_for(std::chrono::milliseconds(m));
					sendto(s, (const char*)d.data(), (int)d.size(), 0, (const sockaddr*)&from, sizeof(from));
				}).detach();
			}
		});
	}
	~FakeStun()
	{
		stop = true;
		th.join();
		std::this_thread::sleep_for(std::chrono::milliseconds(300)); // delayed answers still pending
		closesocket(s);
	}
};

int main()
{
	WSADATA w;
	WSAStartup(MAKEWORD(2, 2), &w);
	Zdxsv::LobbySetLogger([](const std::string& s) { std::printf("  log: %s\n", s.c_str()); });
	const uint32_t lo = inet_addr("127.0.0.1");

	// ggpo_ping_ms has an upper bound (the CPU thread waits for the whole ping test)
	Zdxsv::BattleInfo info;
	const bool ok = Zdxsv::ParseBattleInfo("session_id=1\nuser_id=A\nbattle_server=1.2.3.4:5\nusers=A,B\nggpo_ping_ms=600000\n", info);
	Check(ok && info.ggpoPingMs == Zdxsv::MAX_PING_MS, "ggpo_ping_ms=600000 parsed as MAX_PING_MS", info.ggpoPingMs);

	// replay_upload: http(s) URLs only; ReplayUploadTarget = the last battle info's
	{
		const char* base = "session_id=1\nuser_id=A\nbattle_code=123\nbattle_server=1.2.3.4:5\nusers=A,B\n";
		Zdxsv::BattleInfo a, b;
		Zdxsv::ParseBattleInfo(std::string(base) + "replay_upload=http://192.168.1.8:8204/replay\n", a);
		Zdxsv::ParseBattleInfo(std::string(base) + "replay_upload=file:///c:/x\n", b);
		Check(a.replayUpload == "http://192.168.1.8:8204/replay" && b.replayUpload.empty(), "replay_upload: http kept, file: dropped", 0);
		Zdxsv::SetBattleInfo(a);
		const auto t = Zdxsv::ReplayUploadTarget();
		Check(t.first == a.replayUpload && t.second == "123", "ReplayUploadTarget = last battle info", 0);
		Zdxsv::SetBattleInfo(b);
		Check(Zdxsv::ReplayUploadTarget().first.empty(), "a battle info without replay_upload clears it", 0);
	}

	// OpenUdp: 3 tries of 200 ms each; no answer is not cached, an answer is
	{
		FakeStun stun(27201);
		auto t0 = Clock::now();
		std::string l = Zdxsv::OpenUdp(lo, 27201);
		const long silent = Ms(t0);
		Check(l.find("udp_addr=") == std::string::npos && silent >= 550, "silent STUN: no udp_addr after 3 full tries (ms)", silent);
		stun.mode = 160;
		t0 = Clock::now();
		l = Zdxsv::OpenUdp(lo, 27201);
		Check(l.find("udp_addr=1.2.3.4:5555") != std::string::npos, "next call asks again, STUN answering after 160 ms: udp_addr (ms)", Ms(t0));
		stun.mode = 0;
		t0 = Clock::now();
		l = Zdxsv::OpenUdp(lo, 27201);
		const long cached = Ms(t0);
		Check(l.find("udp_addr=1.2.3.4:5555") != std::string::npos && cached < 50, "after an answer: cached udp_addr (ms)", cached);
	}

	// a 2nd StartPingTest cancels the running one instead of waiting for it
	const std::vector<std::vector<Zdxsv::PeerAddr>> byPos = {{}, {Zdxsv::PeerAddr{"127.0.0.1", 9}}};
	{
		Zdxsv::StartPingTest(77, byPos, 27110, 3000);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const auto t0 = Clock::now();
		Zdxsv::StartPingTest(78, byPos, 27110, 600);
		const long ms = Ms(t0);
		Check(ms < 300, "2nd StartPingTest during a 3000 ms test returns (ms)", ms);
		std::string err;
		const auto r = Zdxsv::FinishPingTest(nullptr, &err);
		Check(r.size() == 2 && err.empty(), "2nd test ran (results)", (long)r.size());
	}

	// GGPO port held by another socket (IPv4 and IPv6): the ping test reports why it did not run
	{
		SOCKET h4 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), h6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
		BOOL excl = TRUE;
		setsockopt(h4, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl));
		setsockopt(h6, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl));
		DWORD v6only = 1;
		setsockopt(h6, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
		sockaddr_in a4{};
		a4.sin_family = AF_INET;
		a4.sin_port = htons(27120);
		sockaddr_in6 a6{};
		a6.sin6_family = AF_INET6;
		a6.sin6_port = htons(27120);
		const bool held = bind(h4, (sockaddr*)&a4, sizeof(a4)) == 0 && bind(h6, (sockaddr*)&a6, sizeof(a6)) == 0;
		Zdxsv::StartPingTest(79, byPos, 27120, 600);
		std::string err;
		const auto r = Zdxsv::FinishPingTest(nullptr, &err);
		Check(held && r.empty() && err.find("bind :27120 failed") != std::string::npos, "port held: no results, bind error", (long)r.size());
		closesocket(h4);
		closesocket(h6);
	}

	// the connectivity test runs on its own thread; a ping test on its port cancels it
	{
		FakeStun stun(27203);
		auto t0 = Clock::now();
		std::atomic<int> doneMs{-1};
		std::string line;
		const bool started = Zdxsv::StartUdpTest(lo, 27203, 27130, [&](const std::string& l, const std::string&) { line = l; doneMs = Ms(t0); });
		const long ret = Ms(t0);
		Check(started && ret < 50, "StartUdpTest returns at once (ms)", ret);
		Check(!Zdxsv::StartUdpTest(lo, 27203, 27130, nullptr), "2nd StartUdpTest while one runs: refused", 0);
		while (doneMs < 0 && Ms(t0) < 5000)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		Check(doneMs >= 0 && line == "nat=unknown\n", "silent STUN: done with nat=unknown (ms)", doneMs);
		std::atomic<bool> called{false};
		Zdxsv::StartUdpTest(lo, 27203, 27130, [&](const std::string&, const std::string&) { called = true; });
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		t0 = Clock::now();
		Zdxsv::StartPingTest(80, byPos, 27130, 600);
		const long ms = Ms(t0);
		std::string err;
		Zdxsv::FinishPingTest(nullptr, &err);
		Check(ms < 200 && err.empty() && !called, "ping test on the port cancels the connectivity test, done not called (ms)", ms);
	}
	std::printf("%d failed\n", g_fails);
	return g_fails ? 1 : 0;
}
