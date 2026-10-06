// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// zdxsv lobby side of a GGPO battle (ZdxsvGgpo lobby=1).
// The lobby sends the custom notice 0x9951 (battle info) to emulators that
// announce "udp=1" in their platform info. LobbyFilter strips it from the
// game's stream and hands it to the listener (GGPO peers). The game itself
// always talks TCP to the battle server (no zproxy UDP bridge).
// Self-contained (sockets + std only) so it builds outside pcsx2 for tests.

#pragma once

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

	struct BattleInfo
	{
		std::string sessionId;
		std::string userId;
		std::string battleCode; // "battle_code=" (zdxsv since inada-s/zdxsv ai/ggpo-report), for the report
		uint32_t serverIP = 0; // network byte order
		uint16_t serverPort = 0;
		std::vector<std::string> users; // every player, self included
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
		// "ggpo_ping_ms=": ping test length (zdxsv sends 7500, gdxsv's P2PMatching value).
		uint32_t ggpoSession = 0;
		int ggpoPingMs = 0;
	};
	// Asks the lobby's UDP STUN (zdxsv ServeUDPStunServer) at stunIP:stunPort for
	// our public address, from a UDP socket on bindPort (0 = any) that is closed after.
	// Once per process: later calls return the first answer (same address after a battle).
	// Returns platform info lines "udp_addr=..\nudp_local=..\nudp_addr6=[..]:..\n" (udp_addr only if
	// STUN answered; udp_addr6 = our global IPv6 address, only if we have one: IPv6 has no NAT, so
	// the source address of a route to the internet is the public one); "" if the socket can't be opened.
	std::string OpenUdp(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort = 0);

	// Log sink (pcsx2: Console). Default: none.
	void SetLogger(std::function<void(const std::string&)> log);

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
	// another player has no GGPO port or no p2p address: the battle stays on the battle server (TCP).
	bool GgpoPeers(const BattleInfo& info, const std::string& ownPublicIP, std::vector<std::vector<PeerAddr>>& byPosition);

	// GGPO ping test before a lobby GGPO battle, as flycast's UdpPingPong (same packet: magic,
	// session id, from / to peer = battle position, candidate, timestamps): binds the GGPO port on
	// IPv4 and IPv6, pings every candidate of every peer of byPosition (own position empty) every
	// 100 ms and answers its pings, for durationMs (pings stop 500 ms before the end). Packets with
	// another magic or session, not to us, or not from one of that position's candidates are dropped.
	void StartPingTest(uint32_t session, const std::vector<std::vector<PeerAddr>>& byPosition, uint16_t port, int durationMs);
	// Per battle position: the candidate picked as flycast's UdpPingPong::GetAvailableAddress
	// (lowest rtt; loopback, private, IPv6 get a bonus) and its mean rtt in ms; rtt -1 = no
	// candidate answered (and own position), addr empty.
	struct PingResult
	{
		int rtt = -1;
		PeerAddr addr;
	};
	// Waits for the running test to end (the GGPO port is free after); empty if no test ran.
	std::vector<PingResult> FinishPingTest();

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

	// Lobby save states, a debugging feature, off unless ZDXSV_LOBBY_STATE=1:
	// DEV9 state is saved/loaded with save states, and after a load DEV9 adopts
	// TCP connections the PS2 opened before the save. Off = upstream behaviour.
	bool LobbyStateEnabled();
	// A save state was loaded: from now on DEV9 adopts TCP connections the PS2
	// opened before the save (only if LobbyStateEnabled()).
	void OnStateLoaded();
	bool AdoptConnections();
	// ip:port is the battle server of the last battle info. Its connection is never
	// adopted: the PS2's late packets get a RST instead of a new connection to a
	// closed room (s612).
	bool IsBattleServer(uint32_t ip, uint16_t port);

	// STUN codec (zdxsv/pkg/proto/zdxsv.proto Ping / Pong), exposed for tests.
	namespace Proto
	{
		enum MessageType : uint32_t
		{
			Ping = 2,
			Pong = 3,
		};

		struct Packet
		{
			uint32_t type = 0;
			// Ping: timestamp + user_id; Pong: + public_addr (STUN answer).
			int64_t timestamp = 0;
			std::string pingUserId;
			std::string publicAddr;
		};

		std::vector<uint8_t> Encode(const Packet& p);
		bool Decode(const uint8_t* data, size_t len, Packet& p);
	} // namespace Proto
} // namespace Zdxsv
