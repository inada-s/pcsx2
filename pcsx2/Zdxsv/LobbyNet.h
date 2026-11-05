// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Sockets shared by the lobby files (Lobby.cpp, LobbyLive.cpp, LobbyPing.cpp); included by no other component.

#include "Zdxsv/Lobby.h"

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

	// numeric PeerAddr -> sockaddr_in / sockaddr_in6; false if the ip doesn't parse
	inline bool LobbyToSockaddr(const PeerAddr& a, sockaddr_storage& sa, socklen_t& len)
	{
		sa = {};
		if (a.V6())
		{
			auto* s6 = reinterpret_cast<sockaddr_in6*>(&sa);
			s6->sin6_family = AF_INET6;
			s6->sin6_port = htons(a.port);
			len = sizeof(sockaddr_in6);
			return inet_pton(AF_INET6, a.ip.c_str(), &s6->sin6_addr) == 1;
		}
		auto* s4 = reinterpret_cast<sockaddr_in*>(&sa);
		s4->sin_family = AF_INET;
		s4->sin_port = htons(a.port);
		len = sizeof(sockaddr_in);
		return inet_pton(AF_INET, a.ip.c_str(), &s4->sin_addr) == 1;
	}

	// sockaddr_in / sockaddr_in6 -> numeric PeerAddr; ip "" for another family
	inline PeerAddr LobbyFromSockaddr(const sockaddr_storage& sa)
	{
		char buf[INET6_ADDRSTRLEN] = {};
		if (sa.ss_family == AF_INET6)
		{
			const auto& s6 = reinterpret_cast<const sockaddr_in6&>(sa);
			if (inet_ntop(AF_INET6, &s6.sin6_addr, buf, sizeof(buf)))
				return {buf, ntohs(s6.sin6_port)};
		}
		else if (sa.ss_family == AF_INET)
		{
			const auto& s4 = reinterpret_cast<const sockaddr_in&>(sa);
			if (inet_ntop(AF_INET, &s4.sin_addr, buf, sizeof(buf)))
				return {buf, ntohs(s4.sin_port)};
		}
		return {};
	}

	// UDP socket of family af bound to port on every address; IPv6 only on AF_INET6 (one socket per
	// family). INVALID_SOCKET if it can't be opened or bound.
	inline sock_t LobbyBindUdp(int af, uint16_t port)
	{
		sock_t s = socket(af, SOCK_DGRAM, IPPROTO_UDP);
		if (s == INVALID_SOCKET)
			return s;
		sockaddr_storage sa{};
		socklen_t len = 0;
		const int on = 1;
		if (af == AF_INET6)
		{
			setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&on), sizeof(on));
			LobbyToSockaddr({"::", port}, sa, len);
		}
		else
			LobbyToSockaddr({"0.0.0.0", port}, sa, len);
		if (bind(s, reinterpret_cast<const sockaddr*>(&sa), len) != 0)
		{
			closesocket(s);
			return INVALID_SOCKET;
		}
		return s;
	}
} // namespace Zdxsv
