// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The game's TCP connections on host sockets (NoDev9.cpp): lobby filter, TLS, battle server.

#include "common/Pcsx2Defs.h"

#include <string>

namespace Zdxsv
{
	// ip:port is the battle server of the last battle info. Its connection is never
	// re-opened after a state load: the room is closed.
	bool IsBattleServer(u32 ip, u16 port);

	// Server-to-game lobby stream filter (Zdxsv/Lobby.h), held through
	// std::unique_ptr<LobbyFilter, LobbyFilterDeleter>, so this header needs no definition.
	class LobbyFilter;
	struct LobbyFilterDeleter
	{
		void operator()(LobbyFilter* filter) const;
	};

	// Once per connection (Zdxsv/LobbyConnection.cpp). Both return the connection's lobby filter (the
	// caller owns it), nullptr = plain TCP. The first bytes from the server: on a zdxsv lobby connection
	// the platform info is sent on socket before the game answers.
	LobbyFilter* LobbyOnFirstData(uptr socket, u32 serverIp, const u8* data, int len);
	// A connection re-opened after a state load.
	LobbyFilter* LobbyOnAdopted(u32 serverIp);
	// Server bytes into the connection's filter. While the VM is paused the lobby's line checks are
	// answered on socket, so a long pause does not drop the player.
	void LobbyFeed(LobbyFilter& filter, uptr socket, const u8* data, int len);

	// Zdxsv/TlsTunnel.cpp. Before connect, endpoint is its sockaddr_in. A connection to a zdxsv
	// server port is redirected to a local tunnel that carries it through TLS to the server's TLS port.
	void TlsOnConnect(void* endpoint);

	// Zdxsv/HttpsLatency.cpp. HTTPS latency to the cloud regions, once per run, on its own
	// threads. Started on the first lobby connection.
	void HttpsLatencyStart();
	// "<region>=<ms>\n" platform info lines once every region ended, else "".
	std::string HttpsLatencyLines();
} // namespace Zdxsv
