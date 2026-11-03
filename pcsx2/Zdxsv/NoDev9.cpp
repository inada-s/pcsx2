// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// The Z game online with or without a DEV9 device. The game's net RPC wrapper is answered on the EE:
// network setup from fixed replies, every name resolved to the zdxsv server, TCP on host sockets.
// Experimental, needs proper testing: lobby and a 1v1 TCP battle tested, GGPO not.

#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/Dev9Hooks.h"
#include "Zdxsv/Lobby.h"
#include "Zdxsv/ServerHosts.h"
#include "Zdxsv/Settings.h"
#include "Zdxsv/TestOptions.h"

#include "Memory.h"
#include "R5900.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

namespace Zdxsv
{
	namespace
	{
		// Wrapper RPC block: +0x18 result, +0x1c reply (setup), +0x1e length, +0x20 data.
		constexpr u32 kRpc = 0xc22c80;
		constexpr u32 kNetState = 0xcd1360; // u8, 5 = the network device pick
		// The boot search for module "dev9" fills this entry; without it the pick shows the device as unavailable.
		constexpr u32 kDevEntry = 0xc634e8;
		constexpr u32 kDevEntryWords[] = {1, 5, 1, 2, 0x21, 0x6af5b8, 0x6af5c0};
		// The pick's SMAP.IRX load (jal); SMAP needs dev9, and the pick never checks a load result.
		constexpr u32 kSmapLoadPc = 0x336bd4;
		constexpr u32 kSmapLoad = 0x0c0cd8f4;

		struct Reply
		{
			u32 fno;
			s32 result;
			u8 data[16];
			u8 size;
		};
		// Captured from the IOP stack with DEV9 on.
		constexpr Reply kReplies[] = {
			{0x47, 1, {0x02, 0, 0, 0, 0x01, 0, 0, 0, 0x01, 0, 0, 0, 'S', 'C', 'E', 0}, 16},
			{0x37, 0, {0xc0, 0, 0x02, 0x64, 0xff, 0xff, 0xff, 0, 0xc0, 0, 0x02, 0xff, 'S', 'C', 'E', 0}, 16},
			{0x30, 0, {'P', 'C', 'S', 'X', '2', 0, 0xff, 0, 0xff, 0xff, 0xff, 0xff, 'S', 'C', 'E', 0}, 16},
			{0x31, 0, {0xc0, 0, 0x02, 0x01, '2', 0, 0xff, 0}, 8},
			{0x4a, 4, {0x08, 0, 0, 0, 0x04, 0, 0, 0, 0x01, 0, 0, 0, 'S', 'C', 'E'}, 15},
			{0x2c, 1, {}, 0}, {0x38, 3, {}, 0},
			{0x48, 0, {}, 0}, {0x05, 0, {}, 0}, {0x29, 0, {}, 0}, {0x2b, 0, {}, 0}, {0x2e, 0, {}, 0}, {0x36, 0, {}, 0},
			{0x01, 0, {}, 0}, {0x3b, 0, {}, 0}, {0x1e, 0, {}, 0}, {0x3d, 0, {}, 0}, {0x3e, 0, {}, 0},
			{0x03, 0, {}, 0}, {0x04, 4, {}, 0}, // battle socket options: set; get (result = value size, value 0 at +0x22)
		};
		constexpr u32 kFnoLinkUp = 0x46; // 1 on the first call, then 0
		constexpr u32 kFnoConnect = 0x07;
		constexpr u32 kFnoClose = 0x0d;
		constexpr u32 kFnoAfterClose = 0x16;
		constexpr u32 kFnoPoll = 0x0f;
		constexpr u32 kFnoSend = 0x10;
		constexpr u32 kFnoRecvLobby = 0x13;
		constexpr u32 kFnoRecv = 0x14;
		constexpr u32 kFnoResolve = 0x3f;
		constexpr u32 kRxMax = 0x3e0;
		constexpr int kSlots = 4; // the socket id is connect's result; later calls pass it at +0x1c

		struct State
		{
			bool on = false;
			u32 ip = 0; // every name resolves to it, network order
			std::shared_ptr<std::atomic<u32>> server_ip; // without a fixed ip: the host DNS answer for SERVER_HOST
			u32 link_calls = 0;
			u8 last_net = 0;
			sock_t socks[kSlots] = {INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET};
			u32 peer[kSlots] = {};
			bool checked[kSlots] = {};
			// A zdxsv lobby connection: the server's bytes reach the game through it (platform info, battle info notice).
			std::unique_ptr<LobbyFilter, LobbyFilterDeleter> lobby[kSlots];
			sock_t none = INVALID_SOCKET;
		} s_state;

		constexpr int kResolveWaitMs = 5000;

		void ResolveServer(std::shared_ptr<std::atomic<u32>> out)
		{
#ifdef _WIN32
			WSADATA wsa;
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
				return;
#endif
			addrinfo hints = {};
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_STREAM;
			addrinfo* res = nullptr;
			if (getaddrinfo(SERVER_HOST, nullptr, &hints, &res) == 0 && res)
				out->store(reinterpret_cast<const sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
			if (res)
				freeaddrinfo(res);
#ifdef _WIN32
			WSACleanup();
#endif
		}

		u32 ServerIp()
		{
			if (s_state.ip || !s_state.server_ip)
				return s_state.ip;
			for (int waited = 0; !s_state.server_ip->load() && waited < kResolveWaitMs; waited += 10)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			s_state.ip = s_state.server_ip->load();
			if (!s_state.ip)
				Console.Error("Zdxsv: no DEV9: no address for %s", SERVER_HOST);
			return s_state.ip;
		}

		void CloseSock(sock_t& s)
		{
			if (s != INVALID_SOCKET)
				closesocket(s);
			s = INVALID_SOCKET;
		}

		bool WouldBlock()
		{
#ifdef _WIN32
			return WSAGetLastError() == WSAEWOULDBLOCK;
#else
			return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
		}

		int Slot(const u8* b)
		{
			s16 id;
			std::memcpy(&id, b + 0x1c, 2);
			return id >= 0 && id < kSlots ? id : -1;
		}

		sock_t& Sock(const u8* b)
		{
			const int id = Slot(b);
			s_state.none = INVALID_SOCKET;
			return id >= 0 ? s_state.socks[id] : s_state.none;
		}

		void CloseSlot(int id)
		{
			CloseSock(s_state.socks[id]);
			s_state.lobby[id].reset();
			s_state.checked[id] = false;
		}

		// Peeked: the first server bytes decide whether it is a zdxsv lobby; a lobby's bytes then all go through its filter.
		void PumpLobby(int id)
		{
			const sock_t s = s_state.socks[id];
			u8 buf[0x2000];
			if (!s_state.checked[id])
			{
				const int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), MSG_PEEK);
				if (n <= 0)
					return;
				s_state.checked[id] = true;
				s_state.lobby[id].reset(LobbyOnFirstData(static_cast<uptr>(s), s_state.peer[id], buf, n));
			}
			if (!s_state.lobby[id])
				return;
			for (int n; (n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0)) > 0;)
				LobbyFeed(*s_state.lobby[id], static_cast<uptr>(s), buf, n);
		}

		s32 Connect(const u8* b)
		{
			const sock_t* free = std::find(std::begin(s_state.socks), std::end(s_state.socks), INVALID_SOCKET);
			if (free == std::end(s_state.socks))
				return -1;
			const s32 id = static_cast<s32>(free - std::begin(s_state.socks));
			sock_t& s = s_state.socks[id];
			s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (s == INVALID_SOCKET)
				return -1;
#ifdef _WIN32
			u_long nb = 1;
			ioctlsocket(s, FIONBIO, &nb);
#else
			fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
			sockaddr_in sa = {};
			sa.sin_family = AF_INET;
			std::memcpy(&sa.sin_addr, b + 0x1c, 4);
			std::memcpy(&sa.sin_port, b + 0x20, 2);
			std::memcpy(&s_state.peer[id], b + 0x1c, 4);
			TlsOnConnect(&sa);
			connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
			return id;
		}

		// Reply as the IOP stack does: state 4 = connected (2 = not yet), send space, readable bytes.
		void Poll(u8* b)
		{
			const sock_t s = Sock(b);
			u16 st = 0, rx = 0;
			if (s != INVALID_SOCKET)
			{
				fd_set w;
				FD_ZERO(&w);
				FD_SET(s, &w);
				timeval tv = {};
				st = select(static_cast<int>(s) + 1, nullptr, &w, nullptr, &tv) > 0 ? 4 : 2;
				const int id = Slot(b);
				if (st == 4)
					PumpLobby(id);
				if (st == 4 && s_state.lobby[id])
					rx = static_cast<u16>(std::min<size_t>(s_state.lobby[id]->Ready(), 0x2000));
#ifdef _WIN32
				u_long n = 0;
				const int r = ioctlsocket(s, FIONREAD, &n);
#else
				int n = 0;
				const int r = ioctl(s, FIONREAD, &n);
#endif
				if (st == 4 && r == 0 && !s_state.lobby[id])
					rx = static_cast<u16>(std::min<u32>(static_cast<u32>(n), 0x2000));
			}
			const u16 z = 0, tx = 0x2000;
			std::memcpy(b + 0x1e, &st, 2);
			std::memcpy(b + 0x20, &z, 2);
			std::memcpy(b + 0x22, &tx, 2);
			std::memcpy(b + 0x24, &rx, 2);
		}

		void Send(const u8* b, u16 len)
		{
			const sock_t s = Sock(b);
			for (int o = 0; s != INVALID_SOCKET && o < len;)
			{
				const int n = send(s, reinterpret_cast<const char*>(b + 0x20 + o), len - o, 0);
				if (n > 0)
					o += n;
				else if (!WouldBlock())
					break;
			}
		}

		s32 Recv(u8* b, u16 len)
		{
			const sock_t s = Sock(b);
			if (s == INVALID_SOCKET)
				return 0;
			const int id = Slot(b);
			PumpLobby(id);
			if (s_state.lobby[id])
				return static_cast<s32>(s_state.lobby[id]->Take(b + 0x20, std::min<u32>(len, kRxMax)));
			const int n = recv(s, reinterpret_cast<char*>(b + 0x20), std::min<int>(len, kRxMax), 0);
			return std::max(n, 0);
		}

		void Patch(u32 addr, u32 original, u32 patched)
		{
			if (memRead32(addr) == original)
				memWrite32(addr, patched);
		}
	} // namespace

	bool g_no_dev9 = false;

	void NoDev9OnVmInitialize()
	{
		NoDev9Reset();
		if (!g_z_game || !BoolSetting("ZdxsvNoDev9", true, "ZDXSV_NODEV9"))
			return;
		const char* e = TestEnv("ZDXSV_NODEV9");
		unsigned q[4] = {};
		if (!e || std::sscanf(e, "%u.%u.%u.%u", &q[0], &q[1], &q[2], &q[3]) != 4)
		{
			if (!SERVER_HOST[0])
			{
				Console.WriteLn("Zdxsv: no DEV9: off, no server host (ZDXSV_NODEV9=<ipv4> sets one)");
				return;
			}
			s_state.on = g_no_dev9 = true;
			s_state.server_ip = std::make_shared<std::atomic<u32>>(0);
			std::thread(ResolveServer, s_state.server_ip).detach();
			Console.WriteLn("Zdxsv: no DEV9: the net RPCs are answered on the EE, names resolve to %s", SERVER_HOST);
			return;
		}
		s_state.on = g_no_dev9 = true;
		s_state.ip = (q[0] & 0xff) | (q[1] & 0xff) << 8 | (q[2] & 0xff) << 16 | (q[3] & 0xff) << 24;
		Console.WriteLn("Zdxsv: no DEV9: the net RPCs are answered on the EE, names resolve to %s", e);
	}

	void NoDev9Reset()
	{
		for (sock_t& s : s_state.socks)
			CloseSock(s);
		s_state = {};
		g_no_dev9 = false;
	}

	void NoDev9OnVsync()
	{
		const u8 net = memRead8(kNetState);
		if (net == 5 && s_state.last_net != 5)
		{
			if (memRead32(kDevEntry) == 0xffffffff)
			{
				for (u32 i = 0; i < std::size(kDevEntryWords); i++)
					memWrite32(kDevEntry + i * 4, kDevEntryWords[i]);
				Console.WriteLn("Zdxsv: no DEV9: network device entry set");
			}
		}
		if (net == 5)
			Patch(kSmapLoadPc, kSmapLoad, 0);
		s_state.last_net = net;
	}

	bool NoDev9OnNetCall()
	{
		const u32 fno = cpuRegs.GPR.n.a0.UL[0];
		u8* b = eeMem->Main + kRpc;
		u16 len;
		std::memcpy(&len, b + 0x1e, 2);
		s32 res = 0;
		if (fno == kFnoLinkUp)
			res = s_state.link_calls++ == 0;
		else if (fno == kFnoConnect)
			res = Connect(b);
		else if (fno == kFnoPoll)
			Poll(b);
		else if (fno == kFnoSend)
			Send(b, len);
		else if (fno == kFnoRecvLobby || fno == kFnoRecv)
		{
			res = Recv(b, len);
			const u16 r16 = static_cast<u16>(res);
			std::memcpy(b + 0x1e, &r16, 2);
		}
		else if (fno == kFnoClose)
		{
			if (const int id = Slot(b); id >= 0)
				CloseSlot(id);
		}
		else if (fno == kFnoAfterClose)
			;
		else if (fno == kFnoResolve)
		{
			const u32 ip = ServerIp();
			res = ip ? 1 : 0;
			if (ip)
				ServerAddressAdd(ip);
			std::memcpy(b + 0x20, &ip, 4);
		}
		else
		{
			const Reply* r = std::find_if(std::begin(kReplies), std::end(kReplies), [fno](const Reply& r) { return r.fno == fno; });
			if (r == std::end(kReplies))
				return false;
			res = r->result;
			const s32 rs = cpuRegs.GPR.n.a2.SL[0];
			if (rs > 0x1c)
				std::memset(b + 0x1c, 0, std::min(rs, 0x400) - 0x1c);
			std::memcpy(b + 0x1c, r->data, r->size);
		}
		std::memcpy(b + 0x18, &res, 4);
		cpuRegs.GPR.n.v0.SD[0] = res;
		cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
		return true;
	}
} // namespace Zdxsv
