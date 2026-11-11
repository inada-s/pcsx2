// Unit test of the waits in pcsx2/Zdxsv/Lobby.cpp (no PCSX2, no game): a fake zdxsv STUN on
// 127.0.0.1 (silent, or answering after a delay) and held UDP ports. Build and run: lobbytest.sh.
// Exit 0 = every check passed; a failed check prints FAIL.
// Uses 127.0.0.1 UDP ports 27110-27130 and 27150-27151, 27201-27205, 27211-27216, 27131-27132.
#include "Zdxsv/Lobby.h"

#include "rapidjson/document.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using Clock = std::chrono::steady_clock;
static long Ms(Clock::time_point t0) { return (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count(); }

static int g_fails = 0;
static void Check(bool ok, const char* what, long value)
{
	std::printf("%s %s (%ld)\n", ok ? "PASS" : "FAIL", what, value);
	g_fails += !ok;
}

// a zdxsv wire fixture (tests/zdxsv/testdata/wire, run from the repository root) without its final newline
static std::string Fixture(const char* name)
{
	std::string s;
	if (FILE* f = std::fopen((std::string("tests/zdxsv/testdata/wire/") + name).c_str(), "rb"))
	{
		char buf[4096];
		for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;)
			s.append(buf, n);
		std::fclose(f);
	}
	else
		std::printf("FAIL fixture %s missing\n", name), g_fails++;
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
		s.pop_back();
	return s;
}

// equal JSON values, key order ignored
static bool SameJson(const std::string& a, const std::string& b)
{
	rapidjson::Document da, db;
	da.Parse(a.data(), a.size());
	db.Parse(b.data(), b.size());
	if (da.HasParseError() || db.HasParseError() || da != db)
	{
		std::printf("  got  %s\n  want %s\n", a.c_str(), b.c_str());
		return false;
	}
	return true;
}

// fake zdxsv STUN on 127.0.0.1:port: mode 0 silent, else answers a Pong with public_addr 1.2.3.4:5555 after mode ms
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
				Zdxsv::StunPacket pong;
				pong.type = Zdxsv::StunPong;
				pong.publicAddr = "1.2.3.4:5555";
				const std::string d = Zdxsv::StunEncode(pong);
				std::thread([this, d, from, m] {
					std::this_thread::sleep_for(std::chrono::milliseconds(m));
					sendto(s, d.data(), (int)d.size(), 0, (const sockaddr*)&from, sizeof(from));
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

// fake RFC 5389 STUN on 127.0.0.1:port: answers a Binding request with XOR-MAPPED-ADDRESS 1.2.3.4:mappedPort;
// badTid: with the request's transaction id changed
struct FakeRfcStun
{
	SOCKET s;
	std::atomic<bool> stop{false};
	std::thread th;
	FakeRfcStun(uint16_t port, uint16_t mappedPort, bool badTid = false)
	{
		s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(port);
		a.sin_addr.s_addr = inet_addr("127.0.0.1");
		if (bind(s, (sockaddr*)&a, sizeof(a)) != 0)
			std::printf("FAIL fake RFC STUN bind :%u\n", port), g_fails++;
		th = std::thread([this, mappedPort, badTid] {
			while (!stop)
			{
				fd_set rd;
				FD_ZERO(&rd);
				FD_SET(s, &rd);
				timeval tv{0, 20000};
				if (select(0, &rd, nullptr, nullptr, &tv) <= 0)
					continue;
				uint8_t req[512];
				sockaddr_in from{};
				int fl = sizeof(from);
				const int n = recvfrom(s, (char*)req, sizeof(req), 0, (sockaddr*)&from, &fl);
				if (n != 20 || req[0] != 0x00 || req[1] != 0x01)
					continue;
				const uint8_t ip[4] = {1, 2, 3, 4};
				uint8_t r[32] = {0x01, 0x01, 0x00, 0x0c};
				std::memcpy(r + 4, req + 4, 16);
				r[8] ^= badTid ? 1 : 0;
				r[20] = 0x00, r[21] = 0x20, r[22] = 0x00, r[23] = 0x08, r[24] = 0x00, r[25] = 0x01;
				r[26] = (uint8_t)((mappedPort >> 8) ^ 0x21), r[27] = (uint8_t)((mappedPort & 0xff) ^ 0x12);
				for (int k = 0; k < 4; k++)
					r[28 + k] = ip[k] ^ req[4 + k];
				sendto(s, (const char*)r, sizeof(r), 0, (const sockaddr*)&from, sizeof(from));
			}
		});
	}
	~FakeRfcStun()
	{
		stop = true;
		th.join();
		closesocket(s);
	}
};

int main()
{
	WSADATA w;
	WSAStartup(MAKEWORD(2, 2), &w);
	Zdxsv::LobbySetLogger([](const std::string& s) { std::printf("  log: %s\n", s.c_str()); });
	const uint32_t lo = inet_addr("127.0.0.1");

	// port closed (no test-socket Pong to "udptest"), the server's two sockets see one mapping:
	// the outside STUN servers decide between cone and symmetric
	{
		FakeStun stun(27211), test(27212);
		stun.mode = 1;
		test.mode = 1;
		FakeRfcStun same1(27213, 5555), same2(27214, 5555), split(27215, 6666), bad(27216, 6666, true);
		const auto run = [&](const char* servers) {
			_putenv_s("ZDXSV_NAT_STUN", servers);
			std::string summary;
			return Zdxsv::UdpTest(lo, 27211, 27132, summary);
		};
		Check(run("127.0.0.1:27213,127.0.0.1:27214") == "nat=cone\n", "outside STUN same mapping: cone", 0);
		Check(run("127.0.0.1:27213,127.0.0.1:27215") == "nat=symmetric\n", "outside STUN mappings differ: symmetric", 0);
		Check(run("none") == "nat=cone\n", "no outside STUN: cone from the server's sockets", 0);
		Check(run("127.0.0.1:27213,127.0.0.1:27299") == "nat=cone\n", "one outside STUN silent: cone from the server's sockets", 0);
		Check(run("127.0.0.1:27213,127.0.0.1:27216") == "nat=cone\n", "reply with another transaction id ignored: cone", 0);
		Check(run("127.0.0.1:27213,no-such-host.invalid:3478") == "nat=cone\n", "unresolved outside STUN skipped: cone", 0);
		_putenv_s("ZDXSV_NAT_STUN", "");
	}

	// battle info: zdxsv's battle info notice (testdata/wire/battle_info.json is its exact output)
	const std::string golden = Fixture("battle_info.json");
	Zdxsv::BattleInfo info;
	bool ok = Zdxsv::ParseBattleInfo(golden, info);
	Check(ok && info.userId == "BBBBBB" && info.sessionId == "SBBBBBB" && info.battleCode == "1696492800000" &&
			  info.serverIP == inet_addr("192.168.1.8") && info.serverPort == 8210 && info.users.size() == 4,
		"battle info: own user, session, code, server, users", (int)info.users.size());
	Check(info.names.size() == 2 && info.names["AAAAAA"] == "\xe3\x82\xa2\xe3\x83\xa0\xe3\x83\xad" && info.names["BBBBBB"] == "Bob" &&
			  info.pilots.size() == 1 && info.syncs.size() == 2 && info.syncs["BBBBBB"] == "0123456789abcdef",
		"battle info: names, pilots, syncs", (int)info.syncs.size());
	Check(info.ggpo.size() == 1 && info.ggpo["AAAAAA"] == 7001 && info.ggpoSession == 1462212142 && info.ggpoPingMs == 7500 &&
			  !info.liveUplink && info.relays.empty(),
		"battle info: ggpo ports, session, ping test", (int)info.ggpo.size());
	Check(info.p2p.size() == 2 && info.p2p[0].userId == "AAAAAA" && info.p2p[0].addrs.size() == 3 &&
			  info.p2p[0].addrs[2].ip == "2001:db8::5" && info.p2p[0].addrs[2].port == 40001 && info.p2p[1].userId == "CCCCCC" &&
			  info.p2p[1].addrs.size() == 3 && info.p2p[1].addrs[2].ip == "198.51.100.9",
		"battle info: candidates by user, IPv6", (int)info.p2p.size());
	int truncated = 0;
	for (size_t n = 0; n < golden.size(); n++)
		truncated += Zdxsv::ParseBattleInfo(golden.substr(0, n), info);
	Check(truncated == 0, "battle info: every truncation rejected", truncated);
	ok = Zdxsv::ParseBattleInfo(Fixture("battle_info_full.json"), info);
	Check(ok && info.teams.size() == 2 && info.teams["AAAAAA"] == 1 && info.teams["BBBBBB"] == 2 && info.liveUplink &&
			  info.relays.size() == 1 && info.relays[0].token == 18364758544493064720ull && info.relays[0].addr.port == 8203 &&
			  info.relays[0].addr6.ip == "2001:db8::8" && info.relays[0].addr6.port == 8204 && info.p2p.size() == 1 &&
			  info.p2p[0].addrs.size() == 2 && info.p2p[0].addrs[1].port == 7002,
		"battle info, every field: teams, spectator uplink, relay with a 64-bit token and IPv6", (int)info.relays.size());

	auto base = [](int peer, int users, const std::string& extra) {
		std::string m = "{\"peer_id\":" + std::to_string(peer) + ",\"users\":[";
		for (int i = 0; i < users; i++)
			m += std::string(i ? "," : "") + "{\"user_id\":\"U" + std::to_string(i) + "\"}";
		return m + "]," + extra + "\"lobby_session_id\":\"S\",\"battle_server\":\"1.2.3.4:5\"}";
	};
	auto parse = [&](const std::string& m) { return Zdxsv::ParseBattleInfo(m, info); };
	ok = parse(base(1, 2, "\"ping_test_duration\":600000,"));
	Check(ok && info.ggpoPingMs == Zdxsv::MAX_PING_MS && info.userId == "U1", "ping_test_duration 600000 as MAX_PING_MS", info.ggpoPingMs);
	Check(parse(base(1, 2, "\"ping_test_duration\":-5,")) && info.ggpoPingMs == 0, "negative ping_test_duration as 0", info.ggpoPingMs);
	Check(!parse(base(2, 2, "")) && !parse(base(-1, 2, "")), "peer_id out of users rejected", 0);
	Check(parse(base(0, 8, "")) && !parse(base(0, 9, "")), "8 users kept, 9 rejected", 0);
	std::string dup = base(0, 2, "");
	dup.replace(dup.find("\"U1\""), 4, "\"U0\"");
	Check(!parse(dup), "duplicate user rejected", 0);
	Check(!parse(base(0, 2, "\"battle_code\":\"" + std::string(257, 'x') + "\",")) &&
			  parse(base(0, 2, "\"battle_code\":\"" + std::string(256, 'x') + "\",")),
		"257-byte string rejected, 256 kept", 0);
	Check(!parse(base(0, 2, "\"session_id\":4294967296,")) && !parse(base(0, 2, "\"session_id\":-1,")) &&
			  parse(base(0, 2, "\"session_id\":4294967295,")),
		"session_id outside 32 bits rejected", 0);
	Check(!parse(base(0, 2, "\"ping_test_duration\":\"5\",")) && !parse(base(0, 2, "\"ping_test_duration\":1.5,")) &&
			  !parse(base(0, 2, "\"spectator_uplink\":1,")) && !parse(base(0, 2, "\"candidates\":{},")) && !parse("[]") && !parse(""),
		"wrong JSON types rejected", 0);
	Check(parse(base(0, 2, "\"candidates\":null,\"relays\":null,\"unknown_key\":[1,{\"x\":2}],")),
		"null arrays and unknown keys accepted", 0);
	std::string cands;
	for (int i = 0; i < 65; i++)
		cands += std::string(i ? "," : "") + "{\"user_id\":\"U1\",\"ip\":\"1.2.3." + std::to_string(i) + "\",\"port\":7000}";
	Check(!parse(base(0, 2, "\"candidates\":[" + cands + "],")), "65 candidates rejected", 0);
	ok = parse(base(0, 2,
		"\"candidates\":[{\"user_id\":\"U1\",\"ip\":\"1.2.3.4\",\"port\":7000},{\"user_id\":\"U1\",\"ip\":\"1.2.3.4\",\"port\":0},"
		"{\"user_id\":\"U1\",\"ip\":\"1.2.3.4\",\"port\":70000}],"));
	Check(ok && info.p2p.size() == 1 && info.p2p[0].addrs.size() == 1, "candidate ports 0 and 70000 dropped",
		info.p2p.empty() ? 0 : (int)info.p2p[0].addrs.size());
	std::string relays;
	for (const char* token : {"4660", "0", "22136"})
		relays += std::string(relays.empty() ? "" : ",") + "{\"ip\":\"1.2.3.4\",\"port\":8203,\"token\":" + token +
				  ",\"ip6\":\"2001:db8::8\",\"port6\":8204}";
	Check(parse(base(0, 2, "\"relays\":[" + relays + "],")) && info.relays.size() == 1 && info.relays[0].token == 0x1234 &&
			  info.relays[0].addr6.ip == "2001:db8::8" && info.relays[0].addr6.port == 8204,
		"relays up to the first malformed one, IPv6 port", (int)info.relays.size());
	Check(!parse(base(0, 2, "\"relays\":[" + relays + "," + relays + "],")), "5 relays rejected", 0);

	// match report: these lines encode to zdxsv's testdata/wire/matching_report.json (every field)
	const std::string report = Zdxsv::EncodeMatchingReport(
		"battle_code=1696492800000\nuser_id=BBBBBB\nlive_uplink=127.0.0.1:9100\nresult=ggpo\nreason=sync\nsync=0123456789abcdef\n"
		"sync_differ=0\nposition=1\nplayers=2\nanswered=1\nping_error=timeout\ndelay=2\nmin_delay=1\nfixed_delay=1\n"
		"rollback_budget=2.50\nrollback_budget_used=1.25\nrtt_0=12\naddr_0=203.0.113.5:7001\npath_0=direct\nrelays=1\n"
		"ping_wait_ms=1500\ncut_sends=4\nclose=net battle end\nframes=21563\nrounds=1:3,17;-1:3,0\nrollback_frames=812\n"
		"mismatches=2\ndisconnected=1\ndisconnected_positions=0\nping_0=201/215/250\nloss_0=3/5\ntimesync=17\n"
		"input_blocks=42\nfps_history=60,59,60\n");
	Check(SameJson(report, Fixture("matching_report.json")), "report: every field as in matching_report.json", (int)report.size());
	const std::string sparse =
		Zdxsv::EncodeMatchingReport("position=0\nrtt_1=30\ndisconnected=0\ndisconnected_positions=\nping_1=-1/-1/-1\nfps_history=\n");
	Check(SameJson(sparse, "{\"peer_id\":0,\"disconnected\":false,\"remote_peers\":[{\"peer_id\":1,\"rtt_ms\":30}]}"),
		"report: no ping sample and no disconnected position leave the fields unset", (int)sparse.size());

	// STUN Ping / Pong: zdxsv's testdata/wire/ping.json and its answer pong.json
	{
		Zdxsv::StunPacket ping;
		ping.type = Zdxsv::StunPing;
		ping.timestamp = 1234567890123456789;
		ping.userId = "ABC123";
		const std::string want = Fixture("ping.json");
		Check(Zdxsv::StunEncode(ping) == want, "stun: ping encodes as ping.json", (int)want.size());
		const std::string pongJson = Fixture("pong.json");
		Zdxsv::StunPacket pong;
		ok = Zdxsv::StunDecode(reinterpret_cast<const uint8_t*>(pongJson.data()), pongJson.size(), pong);
		Check(ok && pong.type == Zdxsv::StunPong && pong.timestamp == 1234567890123456789 && pong.userId == "SERVER" &&
				  pong.publicAddr == "203.0.113.1:8250",
			"stun: pong.json decodes", ok);
		const uint8_t protobufPong[] = {0x08, 0x03, 0x62, 0x02, 0x08, 0x01};
		const char* intType = "{\"type\":2}";
		Check(!Zdxsv::StunDecode(protobufPong, sizeof(protobufPong), pong) &&
				  !Zdxsv::StunDecode(reinterpret_cast<const uint8_t*>(intType), std::strlen(intType), pong),
			"stun: protobuf and a non-string type rejected", 0);
	}

	// OpenUdp: 3 tries of 200 ms each, asked again on every call
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
		const long again = Ms(t0);
		Check(l.find("udp_addr=") == std::string::npos && again >= 550, "after an answer: asks again, silent STUN: no udp_addr (ms)", again);
	}

	// a 2nd StartPingTest cancels the running one instead of waiting for it
	const std::vector<std::vector<Zdxsv::PeerAddr>> byPos = {{}, {Zdxsv::PeerAddr{"127.0.0.1", 9}}};
	{
		Zdxsv::StartPingTest(77, 0, byPos, 27110, 3000);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const auto t0 = Clock::now();
		Zdxsv::StartPingTest(78, 0, byPos, 27110, 600);
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
		Zdxsv::StartPingTest(79, 0, byPos, 27120, 600);
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
		const bool started = Zdxsv::StartUdpTest(lo, 27203, 27130, {}, [&](const std::string& l, const std::string&) { line = l; doneMs = Ms(t0); });
		const long ret = Ms(t0);
		Check(started && ret < 50, "StartUdpTest returns at once (ms)", ret);
		Check(!Zdxsv::StartUdpTest(lo, 27203, 27130, {}, nullptr), "2nd StartUdpTest while one runs: refused", 0);
		while (doneMs < 0 && Ms(t0) < 5000)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		Check(doneMs >= 0 && line == "nat=unknown\n", "silent STUN: done with nat=unknown (ms)", doneMs);
		std::atomic<bool> called{false};
		Zdxsv::StartUdpTest(lo, 27203, 27130, {}, [&](const std::string&, const std::string&) { called = true; });
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		t0 = Clock::now();
		Zdxsv::StartPingTest(80, 0, byPos, 27130, 600);
		const long ms = Ms(t0);
		std::string err;
		Zdxsv::FinishPingTest(nullptr, &err);
		Check(ms < 200 && err.empty() && !called, "ping test on the port cancels the connectivity test, done not called (ms)", ms);
	}
	// the server's test socket (:27205) reaches us; open then also needs the udptest function's Hello
	{
		FakeStun stun(27204);
		stun.mode = 1;
		SOCKET ts = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in ta{};
		ta.sin_family = AF_INET;
		ta.sin_port = htons(27205);
		ta.sin_addr.s_addr = inet_addr("127.0.0.1");
		bind(ts, (sockaddr*)&ta, sizeof(ta));
		sockaddr_in me = ta;
		me.sin_port = htons(27131);
		const auto run = [&](const Zdxsv::UdpReachAsk& ask, std::string& summary) {
			std::thread pongs([&] {
				Zdxsv::StunPacket pong;
				pong.type = Zdxsv::StunPong;
				pong.publicAddr = "1.2.3.4:5555";
				const std::string d = Zdxsv::StunEncode(pong);
				for (int i = 0; i < 3; i++)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					sendto(ts, d.data(), (int)d.size(), 0, (const sockaddr*)&me, sizeof(me));
				}
			});
			const std::string line = Zdxsv::UdpTest(lo, 27204, 27131, summary, nullptr, ask);
			pongs.join();
			return line;
		};
		std::string summary, asked;
		Check(run({}, summary) == "nat=open\n", "test socket answers, no udptest: nat=open", 0);
		auto t0 = Clock::now();
		std::string line = run([&](const std::string& addr, const std::atomic<bool>*) {
			asked = addr;
			std::thread([ts, me] {
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				sendto(ts, "Hello", 5, 0, (const sockaddr*)&me, sizeof(me));
			}).detach();
			return true;
		}, summary);
		Check(line == "nat=open\n" && asked == "1.2.3.4:5555", "udptest Hello: nat=open, asked for the STUN address (ms)", Ms(t0));
		t0 = Clock::now();
		line = run([](const std::string&, const std::atomic<bool>*) { return true; }, summary);
		Check(line == "nat=cone\n" && summary == "cone NAT (open to the server only)", "no udptest Hello: nat=cone (ms)", Ms(t0));
		line = run([](const std::string&, const std::atomic<bool>*) { return false; }, summary);
		Check(line == "nat=open\n", "udptest request failed: server result kept, nat=open", 0);
		closesocket(ts);
	}
	// a peer listed at :27141 answers from :27142 (its NAT mapping): the pongs count, :27142 is picked
	{
		SOCKET pa = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), pb = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = inet_addr("127.0.0.1");
		a.sin_port = htons(27141);
		bool bound = bind(pa, (sockaddr*)&a, sizeof(a)) == 0;
		a.sin_port = htons(27142);
		bound = bind(pb, (sockaddr*)&a, sizeof(a)) == 0 && bound;
		std::atomic<bool> stop{false};
		std::thread peer([&] {
			while (!stop)
			{
				fd_set rd;
				FD_ZERO(&rd);
				FD_SET(pa, &rd);
				FD_SET(pb, &rd);
				timeval tv{0, 20000};
				if (select(0, &rd, nullptr, nullptr, &tv) <= 0)
					continue;
				for (SOCKET s : {pa, pb})
				{
					if (!FD_ISSET(s, &rd))
						continue;
					unsigned char pk[44];
					sockaddr_in from{};
					int fl = sizeof(from);
					if (recvfrom(s, (char*)pk, sizeof(pk), 0, (sockaddr*)&from, &fl) != 44 || pk[8] != 1)
						continue;
					pk[8] = 2; // pong
					std::swap(pk[9], pk[10]);
					std::memcpy(pk + 20, pk + 12, 8);
					sendto(pb, (const char*)pk, sizeof(pk), 0, (sockaddr*)&from, fl);
				}
			}
		});
		const std::vector<std::vector<Zdxsv::PeerAddr>> listed = {{}, {Zdxsv::PeerAddr{"127.0.0.1", 27141}}};
		Zdxsv::StartPingTest(81, 0, listed, 27140, 1200);
		std::string err;
		const auto r = Zdxsv::FinishPingTest(nullptr, &err);
		stop = true;
		peer.join();
		closesocket(pa);
		closesocket(pb);
		Check(bound && r.size() == 2 && r[1].rtt > 0, "pong from another source: peer 1 answered (rtt)", r.size() == 2 ? r[1].rtt : -2);
		Check(r.size() == 2 && r[1].addr.port == 27142, "pong from another source: its source picked (port)", r.size() == 2 ? r[1].addr.port : -2);
	}
	// a peer whose every 4th pong is 400 ms late
	{
		SOCKET pa = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = inet_addr("127.0.0.1");
		a.sin_port = htons(27151);
		const bool bound = bind(pa, (sockaddr*)&a, sizeof(a)) == 0;
		std::atomic<bool> stop{false};
		std::atomic<int> n{0};
		std::thread peer([&] {
			while (!stop)
			{
				fd_set rd;
				FD_ZERO(&rd);
				FD_SET(pa, &rd);
				timeval tv{0, 20000};
				if (select(0, &rd, nullptr, nullptr, &tv) <= 0)
					continue;
				unsigned char pk[44];
				sockaddr_in from{};
				int fl = sizeof(from);
				if (recvfrom(pa, (char*)pk, sizeof(pk), 0, (sockaddr*)&from, &fl) != 44 || pk[8] != 1)
					continue;
				pk[8] = 2;
				std::swap(pk[9], pk[10]);
				uint64_t ts;
				std::memcpy(&ts, pk + 12, 8);
				if (n++ % 4 == 3)
					ts -= 400;
				std::memcpy(pk + 20, &ts, 8);
				sendto(pa, (const char*)pk, sizeof(pk), 0, (sockaddr*)&from, fl);
			}
		});
		const std::vector<std::vector<Zdxsv::PeerAddr>> listed = {{}, {Zdxsv::PeerAddr{"127.0.0.1", 27151}}};
		Zdxsv::StartPingTest(82, 0, listed, 27150, 1200);
		std::string err;
		const auto r = Zdxsv::FinishPingTest(nullptr, &err);
		stop = true;
		peer.join();
		closesocket(pa);
		Check(bound && r.size() == 2 && r[1].rtt > 0 && r[1].rtt < 50, "every 4th pong 400 ms late: rtt is the median (rtt)", r.size() == 2 ? r[1].rtt : -2);
	}
	// relay rtts of a peer behind a symmetric NAT reach us only through the peer that reaches it (peer 3)
	{
		uint8_t mine[4][4] = {{18, 67}};
		const uint8_t from3[4][4] = {{17, 66}, {4, 55}, {60, 5}, {53, 4}};
		Zdxsv::MergeRelayRtt(mine, from3, 0, 3);
		Check(mine[0][0] == 18 && mine[0][1] == 67, "relay rtt merge: own row kept", mine[0][0]);
		Check(mine[1][0] == 4 && mine[1][1] == 55 && mine[2][0] == 60 && mine[3][0] == 53, "relay rtt merge: rows of others taken", mine[1][0]);
		const uint8_t from2[4][4] = {{0}, {0}, {61}, {0}};
		Zdxsv::MergeRelayRtt(mine, from2, 0, 2);
		Check(mine[1][0] == 4 && mine[2][0] == 61 && mine[3][0] == 53, "relay rtt merge: unknown rows do not erase known ones", mine[1][0]);
		const uint8_t lost3[4][4] = {};
		Zdxsv::MergeRelayRtt(mine, lost3, 0, 3);
		Check(mine[3][0] == 0 && mine[1][0] == 4, "relay rtt merge: sender's own row taken as is", mine[3][0]);
	}
	std::printf("%d failed\n", g_fails);
	return g_fails ? 1 : 0;
}
