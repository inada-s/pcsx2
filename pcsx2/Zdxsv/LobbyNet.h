// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Sockets shared by the lobby files (Lobby.cpp, LobbyLive.cpp); included by no other component.

#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // std::min/std::max; the CMake build doesn't define it, the MSBuild one does
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

namespace Zdxsv
{
	// "zdxsv: "-prefixed line to the LobbySetLogger logger.
	void LobbyLog(const std::string& s);

	inline bool LobbyWaitReadable(sock_t s, int ms)
	{
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(s, &rd);
		timeval tv{ms / 1000, (ms % 1000) * 1000};
		return select(static_cast<int>(s) + 1, &rd, nullptr, nullptr, &tv) > 0;
	}

	inline bool LobbySameAddr(const sockaddr_in& a, const sockaddr_in& b)
	{
		return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
	}
} // namespace Zdxsv
