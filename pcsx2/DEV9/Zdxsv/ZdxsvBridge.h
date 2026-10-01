// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// zdxsv UDP bridge: zproxy (zdxsv/src/zproxy) inside the emulator.
// The lobby sends the custom notice 0x9951 (battle info) to emulators that
// announce "udp=1" in their platform info. LobbyFilter strips it from the
// game's stream; the game's next TCP connect to the battle server is
// redirected to a loopback listener whose thread speaks zdxsv's UDP protocol
// (HelloServer, then Battle packets with seq/ack) to the battle server.
// Self-contained (sockets + std only) so it builds outside pcsx2 for tests.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Zdxsv
{
	struct BattleInfo
	{
		std::string sessionId;
		std::string userId;
		uint32_t serverIP = 0; // network byte order
		uint16_t serverPort = 0;
		std::vector<std::string> users; // every player, self included
	};

	// Log sink (pcsx2: Console). Default: none.
	void SetLogger(std::function<void(const std::string&)> log);

	// ZDXSV_UDP=0 turns the bridge off (the game talks TCP to the battle server).
	bool Enabled();

	// "key=value" lines of notice 0x9951. False if a key is missing or malformed.
	bool ParseBattleInfo(const std::string& body, BattleInfo& out);

	// Arms the bridge for the next connect to info.serverIP:serverPort.
	void SetBattleInfo(const BattleInfo& info);

	// Server-to-game lobby stream: passes whole frames through, keeps partial
	// ones until complete, consumes notice 0x9951 (and arms the bridge).
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

	// Host-side connect hook: if the bridge is armed for ip:port (network byte
	// order ip, host order port), starts the bridge and returns its loopback port.
	bool RedirectConnect(uint32_t ip, uint16_t port, uint16_t& bridgePort);

	// Stops a running bridge (emulator shutdown).
	void Shutdown();

	// Protocol codec (zdxsv/pkg/proto/zdxsv.proto), exposed for tests.
	namespace Proto
	{
		enum MessageType : uint32_t
		{
			HelloServer = 1,
			Ping = 2,
			Pong = 3,
			Battle = 4,
		};

		struct BattleMessage
		{
			std::string userId;
			uint32_t seq = 0;
			std::vector<uint8_t> body;
		};

		struct Packet
		{
			uint32_t type = 0;
			uint32_t seq = 0;
			uint32_t ack = 0;
			std::string helloSessionId;
			bool helloOk = false;
			std::vector<BattleMessage> battle;
		};

		std::vector<uint8_t> Encode(const Packet& p);
		bool Decode(const uint8_t* data, size_t len, Packet& p);
	} // namespace Proto
} // namespace Zdxsv
