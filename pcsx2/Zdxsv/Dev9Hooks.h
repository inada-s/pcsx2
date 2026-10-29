// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the DEV9 network code (DEV9.cpp, TCP_Session, DNS_Server, the DEV9 config). Only flags and declarations (AGENTS.md,
// Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

#include <string>

namespace Zdxsv
{
	// A save state was loaded: from now on DEV9 adopts TCP connections the PS2
	// opened before the save (only if LobbyStateEnabled(), ZDXSV_LOBBY_STATE=1).
	void LobbyOnStateLoaded();
	bool AdoptConnections();
	// ip:port is the battle server of the last battle info. Its connection is never
	// adopted: the PS2's late packets get a RST instead of a new connection to a
	// closed room.
	bool IsBattleServer(u32 ip, u16 port);

	// Delta states (Zdxsv/DeltaState.h) restore DEV9, so the frames SMAP received after a save
	// are received again after its load. All three with rx_mutex held. rx_process logs each frame
	// while delta states are kept,
	void DeltaStateOnRx(const void* data, int size);
	// the DEV9 part of a delta state saves the log position,
	u64 DeltaStateRxSeq();
	// and its load passes the frames logged from that position on to deliver, oldest first.
	void DeltaStateRedeliverRx(u64 seq, void (*deliver)(const void* data, int size));

	// Server-to-game lobby stream filter (Zdxsv/Lobby.h), held by TCP_Session through
	// std::unique_ptr<LobbyFilter, LobbyFilterDeleter>, so its header needs no definition.
	class LobbyFilter;
	struct LobbyFilterDeleter
	{
		void operator()(LobbyFilter* filter) const;
	};

	// TCP_Session, once per connection (Zdxsv/LobbyConnection.cpp). Both return the connection's lobby
	// filter (the caller owns it), nullptr = plain TCP. The first bytes from the server: on a zdxsv lobby
	// connection the platform info is sent on socket before the game answers.
	LobbyFilter* LobbyOnFirstData(uptr socket, u32 serverIp, const u8* data, int len);
	// A connection adopted after a state load.
	LobbyFilter* LobbyOnAdopted(u32 serverIp);
	// Server bytes into the connection's filter. While the VM is paused the lobby's line checks are
	// answered on socket, so a long pause does not drop the player.
	void LobbyFeed(LobbyFilter& filter, uptr socket, const u8* data, int len);

	// Zdxsv/ServerHosts.cpp. The internal DNS server looks up the game's server hosts (login, DNAS,
	// lobby) as the zdxsv server's hostname. A host entry for the same name is checked first and wins.
	// Returns the name to look up for url: the zdxsv server's hostname or url itself.
	const char* DnsLookupName(const char* url);
	// url -> address is a game host mapped to the fixed address the defaults once held. The config drops
	// such entries on load, so the lookup above applies.
	bool DnsIsStaleHostEntry(const char* url, const u8* address);
	// The DNS server answers name with address (4 bytes); a game host's address is a zdxsv server.
	void DnsOnAnswer(const char* name, const u8* address);

	// Zdxsv/TlsTunnel.cpp. TCP_Session before connect, endpoint is its sockaddr_in. A connection to a zdxsv
	// server port is redirected to a local tunnel that carries it through TLS to the server's TLS port.
	void TlsOnConnect(void* endpoint);

	// Zdxsv/HttpsLatency.cpp. HTTPS latency to the cloud regions, once per run, on its own
	// threads. Started when the game goes online (a game host lookup or the first lobby connection).
	void HttpsLatencyStart();
	// "<region>=<ms>\n" platform info lines once every region ended, else "".
	std::string HttpsLatencyLines();
} // namespace Zdxsv
