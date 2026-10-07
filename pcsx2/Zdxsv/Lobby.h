// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
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
#include <string>
#include <utility>
#include <vector>

namespace Zdxsv
{
	// A peer's UDP address: numeric IPv4 ("1.2.3.4") or IPv6 ("2001:db8::1") ip, host order port.
	struct PeerAddr
	{
		std::string ip;
		uint16_t port = 0;
		bool V6() const { return ip.find(':') != std::string::npos; }
		std::string String() const { return V6() ? "[" + ip + "]:" + std::to_string(port) : ip + ":" + std::to_string(port); }
		bool operator==(const PeerAddr& o) const { return ip == o.ip && port == o.port; }
	};

	// Longest ping test a battle info can ask for (zdxsv sends 7500; flycast has no bound).
	constexpr int MAX_PING_MS = 10000;

	struct BattleInfo
	{
		std::string sessionId;
		std::string userId;
		std::string battleCode; // "battle_code=" (zdxsv since inada-s/zdxsv ai/ggpo-report), for the report
		uint32_t serverIP = 0; // network byte order
		uint16_t serverPort = 0;
		std::vector<std::string> users; // every player, self included
		std::map<std::string, std::string> names; // "name_<user>=" (UTF-8, zdxsv since ai/ggpo-osd): network status OSD
		// "p2p_<user>=ip:port,ip:port,[ip6]:port": UDP addresses of peers that reported them
		// (udp_addr public, udp_local, udp_addr6); GgpoPeers takes the IPs.
		struct Peer
		{
			std::string userId;
			std::vector<PeerAddr> addrs;
		};
		std::vector<Peer> p2p;
		// "ggpo_<user>=port": GGPO UDP port of peers that announced one (platform info ggpo=).
		std::map<std::string, uint16_t> ggpo;
		// "ggpo_session=": the battle's id in GGPO ping test packets (0 = none: no GGPO);
		// "ggpo_ping_ms=": ping test length (zdxsv sends 7500, gdxsv's P2PMatching value), clamped to
		// [0, MAX_PING_MS]: the CPU thread waits for the whole test (LobbyArm).
		uint32_t ggpoSession = 0;
		int ggpoPingMs = 0;
		// "relay_<k>=<token hex>,<ip:port>[,<[ip6]:port>]" (k = 0..3, zdxsv since ai/ggpo-relay): relay servers
		// of the battle (gdxsv P2PMatching.relays), in the lobby's order.
		struct Relay
		{
			uint64_t token = 0;
			PeerAddr addr, addr6; // addr6 empty when none
		};
		std::vector<Relay> relays;
	};
	// Asks the lobby's UDP STUN (zdxsv ServeUDPStunServer) at stunIP:stunPort for
	// our public address, from a UDP socket on bindPort (0 = any) that is closed after.
	// Waits up to 200 ms for each of 3 tries. Once per process after STUN answered: later calls return
	// that answer (same address after a battle); without an answer the next call asks again.
	// Returns platform info lines "udp_addr=..\nudp_local=..\nudp_addr6=[..]:..\n" (udp_addr only if
	// STUN answered; udp_addr6 = our global IPv6 address, only if we have one: IPv6 has no NAT, so
	// the source address of a route to the internet is the public one); "" if the socket can't be opened.
	std::string OpenUdp(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort = 0);

	// Connectivity test of bindPort against zdxsv's STUN (stunPort) and its test socket (stunPort + 1), as
	// flycast's P2P feasibility test: returns the platform info line "nat=open|cone|symmetric|unknown\n";
	// summary = the result for people (OSD, log). Takes up to ~2 s without answers; stop (optional) set
	// ends it within 50 ms with nat=unknown.
	std::string UdpTest(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort, std::string& summary,
		const std::atomic<bool>* stop = nullptr);
	// UdpTest on its own thread: returns at once; done(natLine, summary) is called from that thread
	// when it ends, unless cancelled (StopUdpTest). One test at a time: a call while one runs does
	// nothing and returns false.
	bool StartUdpTest(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort,
		std::function<void(const std::string& natLine, const std::string& summary)> done);
	// Cancels a running StartUdpTest and waits for its thread (< 100 ms): it frees the GGPO port.
	void StopUdpTest();

	// Log sink (pcsx2: Console). Default: none.
	void LobbySetLogger(std::function<void(const std::string&)> log);

	// "key=value" lines of notice 0x9951. False if a key is missing or malformed.
	bool ParseBattleInfo(const std::string& body, BattleInfo& out);

	// Keeps info (IsBattleServer) and passes it to the listener.
	void SetBattleInfo(const BattleInfo& info);

	// Called by SetBattleInfo (DEV9 thread) with every battle info (pcsx2: GGPO lobby peers).
	void SetBattleInfoListener(std::function<void(const BattleInfo&)> listener);

	// GGPO address candidates of every player by battle position (users order), every IP of the
	// peer's p2p addresses (IPv4 and IPv6) with its ggpo_ port; own position empty. The first one is
	// the default (no ping test): the public IPv4, or the local one when that IP is our own public
	// one (same NAT), or the IPv6 one when the peer has no IPv4. ownPublicIP "" = unknown. False when
	// another player has no GGPO port or no p2p address: the battle connection is cut (no GGPO session).
	bool GgpoPeers(const BattleInfo& info, const std::string& ownPublicIP, std::vector<std::vector<PeerAddr>>& byPosition);

	// GGPO ping test before a lobby GGPO battle, as flycast's UdpPingPong (same packet: magic,
	// session id, from / to peer = battle position, candidate, timestamps): binds the GGPO port on
	// IPv4 and IPv6, pings every candidate of every peer of byPosition (own position empty) every
	// 100 ms and answers its pings, for durationMs (pings stop 500 ms before the end). Packets with
	// another magic or session, not to us, or not from one of that position's candidates are dropped.
	// Every player shares its rtts to the others (and to the relays) in the packets' rtt matrix, as
	// flycast. relays (the battle info's): each is pinged over IPv4 and IPv6 (gdxsv relay.go ping:
	// session, position, token), which also joins us to its session. A running test is cancelled
	// first; so is a running StartUdpTest (same port).
	void StartPingTest(uint32_t session, const std::vector<std::vector<PeerAddr>>& byPosition, uint16_t port, int durationMs,
		const std::vector<BattleInfo::Relay>& relays = {});
	// Per battle position, the path picked as flycast's rollback backend: the direct candidate of
	// UdpPingPong::GetAvailableAddress (lowest rtt; loopback, private, IPv6 get a bonus), unless a
	// relaying peer is 32 ms faster or a relay server 16 ms faster (or direct got no answer); of those
	// two the lower rtt wins. rtt in ms (the whole path), -1 = unreachable (and own position), addr empty.
	struct PingResult
	{
		int rtt = -1;
		PeerAddr addr; // where GGPO sends: the peer, the relaying peer or the relay server
		int via = 0; // 0 direct, 1 through the peer at position relay, 2 through relay server relay
		int relay = -1;
	};
	// Relay server k as GGPO registers it (ggpo_add_relay_server): addr = the faster family that
	// answered (IPv4 if none did), alt = the other family's address, empty if none.
	struct RelayServerAddr
	{
		PeerAddr addr, alt;
	};
	// Waits for the running test to end (the GGPO port is free after); empty if no test ran.
	// servers (optional): every relay of the test, in order. error (optional): why the last
	// StartPingTest ran no test (bind failure), "" otherwise.
	std::vector<PingResult> FinishPingTest(std::vector<RelayServerAddr>* servers = nullptr, std::string* error = nullptr);

	// Our public IPv4 from the lobby's STUN (OpenUdp); "" if unknown.
	std::string PublicIP();

	// Server-to-game lobby stream: passes whole frames through, keeps partial
	// ones until complete, consumes notice 0x9951 (SetBattleInfo).
	// Output is queued: Take() hands out at most what the game's window allows.
	class LobbyFilter
	{
	public:
		void Feed(const uint8_t* data, size_t len);
		size_t Ready() const { return ready.size(); }
		size_t Take(uint8_t* dst, size_t max);

	private:
		std::vector<uint8_t> buf; // incomplete frame
		std::vector<uint8_t> ready; // frames for the game
	};

	// Lobby save states (LobbyStateEnabled, LobbyOnStateLoaded, AdoptConnections, IsBattleServer):
	// Zdxsv/SaveStateHooks.h, Zdxsv/Dev9Hooks.h.

	// STUN codec (zdxsv/pkg/proto/zdxsv.proto Ping / Pong), exposed for tests.
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
