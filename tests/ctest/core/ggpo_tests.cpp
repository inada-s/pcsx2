// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ggpo_log.h"
#include "ggponet.h"
#include "ggpo_types.h"
#include "network/udp.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// zdxsv: GGPO (from inada-s/flycast) builds and links into PCSX2. A synctest session
// rolls back every frame and compares state checksums, so a toy game must pass and a
// game whose state depends on something outside the saved state must be caught.

namespace
{
	struct ToyState
	{
		uint32_t frame;
		uint32_t acc[2];
	};

	ToyState s_state;
	uint32_t s_hidden; // not saved: makes the game nondeterministic when used
	bool s_use_hidden;
	GGPOSession* s_session;
	int s_loads;
	std::vector<std::string> s_errors;

	int Checksum(const unsigned char* buf, int len)
	{
		uint32_t h = 2166136261u;
		for (int i = 0; i < len; i++)
			h = (h ^ buf[i]) * 16777619u;
		return static_cast<int>(h);
	}

	void Step(const uint16_t inputs[2])
	{
		s_state.frame++;
		for (int i = 0; i < 2; i++)
			s_state.acc[i] = s_state.acc[i] * 31 + inputs[i] + (s_use_hidden ? s_hidden++ : 0);
	}

	bool BeginGame(const char*) { return true; }

	bool SaveGameState(unsigned char** buffer, int* len, int* checksum, int)
	{
		*len = sizeof(s_state);
		*buffer = static_cast<unsigned char*>(std::malloc(*len));
		std::memcpy(*buffer, &s_state, *len);
		*checksum = Checksum(*buffer, *len);
		return true;
	}

	bool LoadGameState(unsigned char* buffer, int len)
	{
		EXPECT_EQ(len, static_cast<int>(sizeof(s_state)));
		std::memcpy(&s_state, buffer, sizeof(s_state));
		s_loads++;
		return true;
	}

	bool LogGameState(char*, unsigned char*, int) { return true; }
	void FreeBuffer(void* buffer) { std::free(buffer); }
	bool OnEvent(GGPOEvent*) { return true; }

	bool AdvanceFrame(int)
	{
		uint16_t inputs[2] = {};
		int disconnect_flags = 0;
		EXPECT_EQ(ggpo_synchronize_input(s_session, inputs, sizeof(inputs), &disconnect_flags), GGPO_OK);
		Step(inputs);
		EXPECT_EQ(ggpo_advance_frame(s_session), GGPO_OK);
		return true;
	}

	void OnLog(int level, const char* msg)
	{
		if (level == GGPO_LOG_ERROR)
			s_errors.emplace_back(msg);
	}

	// Runs a 2-player synctest session for the given frames; returns the final state.
	ToyState RunSyncTest(bool use_hidden, int frames)
	{
		s_state = {};
		s_hidden = 0;
		s_use_hidden = use_hidden;
		s_loads = 0;
		s_errors.clear();
		ggpo_set_log_function(OnLog);

		GGPOSessionCallbacks cb{};
		cb.begin_game = BeginGame;
		cb.save_game_state = SaveGameState;
		cb.load_game_state = LoadGameState;
		cb.log_game_state = LogGameState;
		cb.free_buffer = FreeBuffer;
		cb.advance_frame = AdvanceFrame;
		cb.on_event = OnEvent;

		EXPECT_EQ(ggpo_start_synctest(&s_session, &cb, "ggpo_test", 2, sizeof(uint16_t), 1), GGPO_OK);
		EXPECT_EQ(ggpo_idle(s_session, 0), GGPO_OK);

		GGPOPlayerHandle handles[2];
		for (int i = 0; i < 2; i++)
		{
			GGPOPlayer player{sizeof(GGPOPlayer), GGPO_PLAYERTYPE_LOCAL, i + 1};
			EXPECT_EQ(ggpo_add_player(s_session, &player, &handles[i]), GGPO_OK);
		}

		for (int f = 0; f < frames; f++)
		{
			for (int i = 0; i < 2; i++)
			{
				uint16_t input = static_cast<uint16_t>((f * 7 + i * 13) & 0xfff);
				EXPECT_EQ(ggpo_add_local_input(s_session, handles[i], &input, sizeof(input)), GGPO_OK);
			}
			AdvanceFrame(0);
		}

		EXPECT_EQ(ggpo_close_session(s_session), GGPO_OK);
		s_session = nullptr;
		ggpo_set_log_function(nullptr);
		return s_state;
	}
} // namespace

TEST(GGPO, SyncTestDeterministicGamePasses)
{
	const int frames = 120;
	const ToyState end = RunSyncTest(false, frames);
	EXPECT_EQ(end.frame, static_cast<uint32_t>(frames));
	EXPECT_GE(s_loads, frames - 1); // check distance 1: one rollback per frame
	EXPECT_TRUE(s_errors.empty()) << (s_errors.empty() ? "" : s_errors.front());
}

TEST(GGPO, SyncTestCatchesStateOutsideSave)
{
	RunSyncTest(true, 30);
	ASSERT_FALSE(s_errors.empty());
	EXPECT_NE(s_errors.front().find("Checksum"), std::string::npos) << s_errors.front();
}

// zdxsv: GGPO's UDP socket (network/udp.cpp). Datagrams of a UdpMsg header's size are enough:
// OnLoopPoll hands every one of at least that size to the callbacks.
namespace
{
	struct TestUdp : Udp
	{
		SOCKET Socket(bool v6) const { return v6 ? _socket_v6 : _socket_v4; }
	};

	struct MsgCount : Udp::Callbacks
	{
		int msgs = 0;
		int family = 0;
		void OnMsg(sockaddr_storage& from, UdpMsg*, int) override
		{
			msgs++;
			family = from.ss_family;
		}
	};

	// loopback address of family af with the port of socket s (INVALID_SOCKET: a port nothing listens on)
	sockaddr_storage Loopback(int af, SOCKET s)
	{
		sockaddr_storage a{};
		socklen_t len = sizeof(a);
		if (s != INVALID_SOCKET)
			getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
		else
		{
			// a port that was free a moment ago
			const SOCKET tmp = socket(af, SOCK_DGRAM, 0);
			a.ss_family = static_cast<decltype(a.ss_family)>(af);
			bind(tmp, reinterpret_cast<sockaddr*>(&a), af == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
			getsockname(tmp, reinterpret_cast<sockaddr*>(&a), &len);
			closesocket(tmp);
		}
		if (af == AF_INET)
			reinterpret_cast<sockaddr_in*>(&a)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		else
			reinterpret_cast<sockaddr_in6*>(&a)->sin6_addr = in6addr_loopback;
		return a;
	}

	int AddrLen(const sockaddr_storage& a) { return a.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6); }

	void SendFromNewSocket(const sockaddr_storage& dst)
	{
		char buf[sizeof(UdpMsg::hdr)] = {};
		const SOCKET s = socket(dst.ss_family, SOCK_DGRAM, 0);
		ASSERT_NE(s, INVALID_SOCKET);
		EXPECT_EQ(sendto(s, buf, sizeof(buf), 0, reinterpret_cast<const sockaddr*>(&dst), AddrLen(dst)), static_cast<int>(sizeof(buf)));
		closesocket(s);
	}

	void Wait() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
} // namespace

// Windows reports the ICMP port unreachable of an earlier send (peer not started yet, or gone)
// as an error of the next recvfrom. The socket ignores it: the datagram queued behind it is read
// by the same poll. Other platforms never report it on an unconnected socket.
TEST(GGPO, UdpReadsPastPortUnreachable)
{
	Poll poll;
	MsgCount cb;
	TestUdp udp;
	udp.Init(0, &poll, &cb);
	char buf[sizeof(UdpMsg::hdr)] = {};
	sockaddr_storage closed = Loopback(AF_INET, INVALID_SOCKET);
	udp.SendTo(buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&closed), AddrLen(closed));
	Wait();
	SendFromNewSocket(Loopback(AF_INET, udp.Socket(false)));
	Wait();
	udp.OnLoopPoll(nullptr);
	EXPECT_EQ(cb.msgs, 1);
}

// A peer's IPv6 candidate: the socket sends and receives over IPv6 next to IPv4 on the same port.
TEST(GGPO, UdpDualStack)
{
	Poll poll;
	MsgCount cb;
	TestUdp udp;
	udp.Init(0, &poll, &cb);
	if (udp.Socket(true) == INVALID_SOCKET)
		GTEST_SKIP() << "no IPv6 on this host";

	SendFromNewSocket(Loopback(AF_INET6, udp.Socket(true)));
	Wait();
	udp.OnLoopPoll(nullptr);
	EXPECT_EQ(cb.msgs, 1);
	EXPECT_EQ(cb.family, AF_INET6);

	MsgCount peer_cb;
	TestUdp peer;
	peer.Init(0, &poll, &peer_cb);
	sockaddr_storage peer_addr = Loopback(AF_INET6, peer.Socket(true));
	char buf[sizeof(UdpMsg::hdr)] = {};
	udp.SendTo(buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&peer_addr), AddrLen(peer_addr));
	Wait();
	peer.OnLoopPoll(nullptr);
	EXPECT_EQ(peer_cb.msgs, 1);
	EXPECT_EQ(peer_cb.family, AF_INET6);
}
