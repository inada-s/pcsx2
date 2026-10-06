// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the DEV9 network code (DEV9.cpp, TCP_Session). Only flags and declarations (AGENTS.md,
// Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

namespace Zdxsv::Lobby
{
	// A save state was loaded: from now on DEV9 adopts TCP connections the PS2
	// opened before the save (only if LobbyStateEnabled(), ZDXSV_LOBBY_STATE=1).
	void OnStateLoaded();
	bool AdoptConnections();
	// ip:port is the battle server of the last battle info. Its connection is never
	// adopted: the PS2's late packets get a RST instead of a new connection to a
	// closed room.
	bool IsBattleServer(u32 ip, u16 port);

	// Server-to-game lobby stream filter (Zdxsv/Lobby.h), held by TCP_Session through
	// std::unique_ptr<LobbyFilter, LobbyFilterDeleter>, so its header needs no definition.
	class LobbyFilter;
	struct LobbyFilterDeleter
	{
		void operator()(LobbyFilter* filter) const;
	};
} // namespace Zdxsv::Lobby
