// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// zdxsv lobby side of a GGPO battle (ZDXSV_GGPO lobby=1).
// The lobby sends the custom notice 0x9951 (battle info) to emulators that
// announce "udp=1" in their platform info. LobbyFilter strips it from the
// game's stream and hands it to the listener (GGPO peers). The game itself
// always talks TCP to the battle server (no zproxy UDP bridge).
// Self-contained (sockets + std only) so it builds outside pcsx2 for tests.

#pragma once

#include "Zdxsv/Dev9Hooks.h"
#include "Zdxsv/SaveStateHooks.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Zdxsv
{
	// A player's address for logs players paste (streams): IPv4 digits after the first octet and IPv6
	// digits after the third group become one 'x' per part, the port stays ("1.x.x.x:7001",
	// "[2001:db8:1:x::x]:7001").
	std::string MaskIpAddress(const std::string& addr);

	// A peer's UDP address: numeric IPv4 ("1.2.3.4") or IPv6 ("2001:db8::1") ip, host order port.
	struct PeerAddr
	{
		std::string ip;
		uint16_t port = 0;
		bool V6() const { return ip.find(':') != std::string::npos; }
		std::string String() const { return V6() ? "[" + ip + "]:" + std::to_string(port) : ip + ":" + std::to_string(port); }
		std::string Masked() const { return MaskIpAddress(String()); }
		bool operator==(const PeerAddr& o) const { return ip == o.ip && port == o.port; }
	};

	// Longest ping test a battle info can ask for.
	constexpr int MAX_PING_MS = 10000;

	struct BattleInfo
	{
		std::string sessionId; // lobby_session_id
		std::string userId; // users[peer_id]
		std::string battleCode; // for the report
		uint32_t serverIP = 0; // battle_server, network byte order
		uint16_t serverPort = 0;
		std::vector<std::string> users; // every player, self included
		std::map<std::string, std::string> names; // users[].user_name (UTF-8): network status OSD
		std::map<std::string, std::string> pilots; // users[].pilot_name (UTF-8): OSD
		std::map<std::string, std::string> syncs; // users[].sync: settings fingerprint (16 hex digits), GgpoLobby LobbyArm
		// candidates: UDP addresses of peers that reported them, in users order
		// (udp_addr public, udp_local, udp_addr6, TCP source); GgpoPeers takes the IPs.
		struct Peer
		{
			std::string userId;
			std::vector<PeerAddr> addrs;
		};
		std::vector<Peer> p2p;
		// users[].ggpo_port: GGPO UDP port of peers that announced one (platform info ggpo=).
		std::map<std::string, uint16_t> ggpo;
		// session_id: the battle's id in GGPO ping test packets (0 = none: no GGPO);
		// ping_test_duration: ping test length in ms, clamped to
		// [0, MAX_PING_MS]: the CPU thread waits for the whole test (LobbyArm).
		uint32_t ggpoSession = 0;
		int ggpoPingMs = 0;
		// spectator_uplink: this client streams the battle to live spectators (LiveUp)
		bool liveUplink = false;
		// relays (at most 4): relay servers of the battle, in the lobby's order.
		struct Relay
		{
			uint64_t token = 0;
			PeerAddr addr, addr6; // addr6 empty when none
		};
		std::vector<Relay> relays;
	};
	// Asks the lobby's UDP STUN at stunIP:stunPort for our public address, from a UDP socket on
	// bindPort (the GGPO port, so udp_addr is its NAT mapping; any port if it is taken or 0). Asks on every call.
	// Returns platform info lines "udp_addr=..\nudp_local=..\nudp_addr6=[..]:..\n" (udp_addr only if
	// STUN answered, udp_addr6 only with a global IPv6 address); "" if the socket can't be opened.
	std::string OpenUdp(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort = 0);

	// "ip:port" of the lobby UDP socket OpenUdp asked (live spectating goes there too), "" before.
	std::string LobbyUdpAddr();

	// Live spectating over the lobby's UDP socket: each stream (header, inputs) goes in 1000-byte
	// datagrams, go-back-N from the receiver's ack, on its own thread.
	struct LiveStreams
	{
		std::vector<uint8_t> header; // a replay BattleLogFile without inputs
		std::vector<uint8_t> inputs; // frames x frameBytes
		std::vector<uint8_t> rounds; // the round results so far: BattleLogFile round_data entries (field 18)
		int frameBytes = 0;
		bool closed = false;
		std::string close;
	};
	// The battle's uplink (battle info live_uplink=1): sends what it is given, until the lobby acked the close.
	class LiveUp
	{
	public:
		// to = LobbyUdpAddr(); session = the battle's ggpo_session.
		LiveUp(const std::string& to, std::string code, uint32_t session, std::vector<uint8_t> header, int frameBytes);
		~LiveUp(); // stops the thread
		void AddFrames(const void* data, size_t frames); // the next confirmed frames, in order
		void AddRound(const std::vector<uint8_t>& entry); // a BattleLogFile round_data entry (field 18), once confirmed
		size_t Frames();
		void Close(const std::string& reason);
		bool Done(); // the lobby acked the close, or the thread gave up
	private:
		struct Impl;
		std::unique_ptr<Impl> m;
	};
	// A spectator: url = "host:port[/battle code]" (no code: the newest live battle there).
	class LiveDown
	{
	public:
		~LiveDown(); // stops the thread
		// Subscribes and waits up to timeoutMs for the header and a frame (or the close).
		static std::unique_ptr<LiveDown> Open(const std::string& url, int timeoutMs, std::string& error);
		std::string Code();
		// The battle code a subscribe without one gets at hostPort: with skip empty the newest live battle
		// (running ones first), else the newest running one not in skip (auto-next); "" = none or no answer
		// in timeoutMs. A lobby older than skip answers as if it were empty.
		static std::string Newest(const std::string& hostPort, const std::vector<std::string>& skip, int timeoutMs);
		// Moves the frames received since the last call into s.inputs (with the header on the first call),
		// copies the round results to s.rounds, sets s.closed / s.close; false = nothing from the lobby for stallMs.
		bool Take(LiveStreams& s, int stallMs);
	private:
		LiveDown();
		struct Impl;
		std::unique_ptr<Impl> m;
	};

	// Asks a host on another network (the udptest function) to send "Hello" to publicAddr ("ip:port");
	// true once the request was accepted. stop set: give up at once.
	using UdpReachAsk = std::function<bool(const std::string& publicAddr, const std::atomic<bool>* stop)>;

	// Connectivity test of bindPort against the lobby's STUN (stunPort) and its test socket (stunPort + 1):
	// returns the platform info line "nat=open|cone|symmetric|unknown\n"; summary = the result for people.
	// When the server finds the port open and reachAsk is set, open also needs reachAsk's "Hello";
	// without it the port is open to the server's IP only (nat=cone).
	// A closed port is also asked from two STUN servers on other networks (ZDXSV_NAT_STUN): different
	// mappings there = symmetric. stop (optional) set ends it with nat=unknown.
	std::string UdpTest(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort, std::string& summary,
		const std::atomic<bool>* stop = nullptr, const UdpReachAsk& reachAsk = {});
	// summary: the result in words for the OSD, without addresses or ports (the log line has them).
	// UdpTest on its own thread: returns at once; done(natLine, summary) is called from that thread
	// when it ends, unless cancelled (StopUdpTest). One test at a time: a call while one runs does
	// nothing and returns false. openPort (optional) runs when the port is not open: it may open it
	// (UPnP) and add platform info lines; true runs the test again, whose result replaces the first
	// unless only the retest got no answer.
	bool StartUdpTest(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort, UdpReachAsk reachAsk,
		std::function<void(const std::string& natLine, const std::string& summary)> done,
		std::function<bool(std::string& lines)> openPort = nullptr);
	// Cancels a running StartUdpTest and waits for its thread (< 100 ms): it frees the GGPO port.
	void StopUdpTest();

	// Log sink (pcsx2: Console). Default: none.
	void LobbySetLogger(std::function<void(const std::string&)> log);

	// Notice 0x9951: a P2PMatching (zdxsv pkg/proto/zdxsv.proto). False if malformed, over the
	// limits (8 users, 64 candidates, 4 relays, 256-byte strings) or without its own user.
	bool ParseBattleInfo(const std::string& body, BattleInfo& out);

	// Message 0x9952: the report's "key=value" lines (TakeLobbyReport) as a P2PMatchingReport.
	std::vector<uint8_t> EncodeMatchingReport(const std::string& lines);

	// Keeps info (IsBattleServer) and passes it to the listener.
	void SetBattleInfo(const BattleInfo& info);

	// Called by SetBattleInfo (DEV9 thread) with every battle info (pcsx2: GGPO lobby peers).
	void SetBattleInfoListener(std::function<void(const BattleInfo&)> listener);

	// GGPO address candidates of every player by battle position (own position empty); the first one is
	// the default without a ping test. ownPublicIP "" = unknown. A player without a GGPO port or p2p
	// address gets none (the ping test may still find a path). A player whose candidates carry its GGPO
	// port (STUN asked from it) keeps their ports, else every candidate gets the GGPO port. Returns our position, -1 if not listed.
	int GgpoPeers(const BattleInfo& info, const std::string& ownPublicIP, std::vector<std::vector<PeerAddr>>& byPosition);

	// GGPO ping test before a lobby GGPO battle: pings every candidate
	// of every peer of byPosition and every relay for durationMs on the GGPO port, and answers their
	// pings. A ping also joins us to the relay's session. Cancels a running test or StartUdpTest first.
	// me = our position. A peer without candidates is reached at the sources of its pings.
	void StartPingTest(uint32_t session, int me, const std::vector<std::vector<PeerAddr>>& byPosition, uint16_t port, int durationMs,
		const std::vector<BattleInfo::Relay>& relays = {});
	// Per battle position, the path picked: direct, unless a relaying peer is 32 ms
	// or a relay server 16 ms faster. rtt in ms (the whole path), -1 = unreachable (and own position).
	struct PingResult
	{
		int rtt = -1;
		PeerAddr addr; // where GGPO sends: the peer, the relaying peer or the relay server
		int via = 0; // 0 direct, 1 through the peer at position relay, 2 through relay server relay
		int relay = -1;
	};
	// Relay server k as GGPO registers it (ggpo_add_relay_server): addr = the faster family that
	// answered (IPv4 if none did), alt = the other family's address, empty if none.
	// sources: addresses its pongs came from other than addr/alt (ggpo_add_relay_server_source).
	struct RelayServerAddr
	{
		PeerAddr addr, alt;
		std::vector<PeerAddr> sources;
	};
	// Waits for the running test to end (the GGPO port is free after); empty if no test ran.
	// servers (optional): every relay of the test, in order. error (optional): why the last
	// StartPingTest ran no test (bind failure), "" otherwise.
	std::vector<PingResult> FinishPingTest(std::vector<RelayServerAddr>* servers = nullptr, std::string* error = nullptr);
	// The running ping test, for the connection diagram (any thread): ms since its start, its length, the
	// player count and the rtt matrix ([i][j]: i's rtt to j in ms, 255 max, 0 = unknown). False when none runs.
	bool PingTestView(int& elapsedMs, int& durationMs, int& players, uint8_t rtt[4][4]);
	// Merges a ping packet's relay rtt matrix ([peer][relay], 0 = unknown) from position from into dst:
	// from's row as is, other rows when non-zero, own row kept.
	void MergeRelayRtt(uint8_t dst[4][4], const uint8_t src[4][4], int self, int from);

	// Our public IPv4 from the lobby's STUN (OpenUdp); "" if unknown.
	std::string PublicIP();

	// Server-to-game lobby stream: passes whole frames through, keeps partial
	// ones until complete, consumes notice 0x9951 (SetBattleInfo).
	// Output is queued: Take() hands out at most what the game's window allows.
	// answerLineChecks (the game is paused): line check questions 0x6001 are consumed and
	// their answers queued for the server (TakeAnswers), as the game would answer them.
	class LobbyFilter
	{
	public:
		void Feed(const uint8_t* data, size_t len, bool answerLineChecks = false);
		size_t Ready() const { return ready.size(); }
		size_t Take(uint8_t* dst, size_t max);
		std::vector<uint8_t> TakeAnswers() { return std::exchange(answers, {}); }

	private:
		std::vector<uint8_t> buf; // incomplete frame
		std::vector<uint8_t> ready; // frames for the game
		std::vector<uint8_t> answers; // frames for the server
	};

	// Battle-start answers 0x6911..0x6917 (whole lobby frames) the game got since the last battle start notice
	// 0x6910: a replay keeps them (replay.proto lobby_answers) to play the start again from a common state.
	void LobbyNoteFrame(const uint8_t* frame, size_t size);
	std::vector<std::vector<uint8_t>> LobbyStartAnswers();

	// Lobby save states (LobbyStateEnabled, LobbyOnStateLoaded, AdoptConnections, IsBattleServer):
	// Zdxsv/SaveStateHooks.h, Zdxsv/Dev9Hooks.h.

	// STUN codec (the lobby's Ping / Pong messages), exposed for tests.
	enum ProtoMessageType : uint32_t
	{
		ProtoPing = 2,
		ProtoPong = 3,
	};

	struct ProtoPacket
	{
		uint32_t type = 0;
		// ProtoPing: timestamp + user_id; ProtoPong: + public_addr (STUN answer).
		int64_t timestamp = 0;
		std::string pingUserId;
		std::string publicAddr;
	};

	std::vector<uint8_t> ProtoEncode(const ProtoPacket& p);
	bool ProtoDecode(const uint8_t* data, size_t len, ProtoPacket& p);
} // namespace Zdxsv
