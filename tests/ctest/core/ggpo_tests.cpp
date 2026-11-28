// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#ifdef _WIN32
// GGPO's platform header calls timeGetTime; WIN32_LEAN_AND_MEAN drops mmsystem.h from windows.h.
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#endif

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

	bool HostHasIpv6()
	{
		const SOCKET s = socket(AF_INET6, SOCK_DGRAM, 0);
		if (s == INVALID_SOCKET)
			return false;
		sockaddr_in6 a{};
		a.sin6_family = AF_INET6;
		a.sin6_addr = in6addr_loopback;
		const bool ok = bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
		closesocket(s);
		return ok;
	}
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
	if (!HostHasIpv6())
		GTEST_SKIP() << "no IPv6 on this host";
	Poll poll;
	MsgCount cb;
	TestUdp udp;
	udp.Init(0, &poll, &cb);
	ASSERT_NE(udp.Socket(true), INVALID_SOCKET);

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

namespace
{
	SOCKET BoundLoopbackSocket(unsigned short* port)
	{
		SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
		socklen_t len = sizeof(a);
		getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
		*port = ntohs(a.sin_port);
#ifdef _WIN32
		u_long nonblocking = 1;
		ioctlsocket(s, FIONBIO, &nonblocking);
#else
		fcntl(s, F_SETFL, O_NONBLOCK);
#endif
		return s;
	}

	unsigned short FreeLoopbackPort()
	{
		unsigned short port;
		closesocket(BoundLoopbackSocket(&port));
		return port;
	}

	constexpr uint16 kForgedMagic = 0x5555;
	constexpr uint16 kForgedSeq = 0x4321;

	void SendRelay(SOCKET s, unsigned short to_port, int relay_to)
	{
		UdpMsg m(UdpMsg::KeepAlive);
		m.hdr.const_magic = CONST_MAGIC;
		m.hdr.magic = kForgedMagic;
		m.hdr.sequence_number = kForgedSeq;
		m.hdr.remote_endpoint = 0;
		m.hdr.type = UdpMsg::Relay;
		m.hdr.relay_magic = RELAY_MAGIC;
		m.hdr.relay_to_endpoint = static_cast<uint8>(relay_to);
		m.hdr.org_type = UdpMsg::KeepAlive;
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = htons(to_port);
		sendto(s, reinterpret_cast<const char*>(&m), sizeof(m.hdr), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
	}

	// remote_endpoint of the forwarded copy of a SendRelay packet read from s, or -1 if none came
	int ForwardedSender(GGPOSession* session, SOCKET s)
	{
		int sender = -1;
		for (int k = 0; k < 30; k++)
		{
			ggpo_idle(session, 0);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			char buf[2048];
			int len;
			while ((len = recv(s, buf, sizeof(buf), 0)) >= static_cast<int>(sizeof(UdpMsg::hdr)))
			{
				const UdpMsg* m = reinterpret_cast<const UdpMsg*>(buf);
				if (m->hdr.type == UdpMsg::KeepAlive && m->hdr.magic == kForgedMagic && m->hdr.sequence_number == kForgedSeq)
					sender = m->hdr.remote_endpoint;
			}
		}
		return sender;
	}
} // namespace

// A relay packet is forwarded only for a player reached directly, and carries that player's index
// whatever index the packet names.
TEST(GGPO, RelayForwardsOnlyFromPlayers)
{
	unsigned short port_b, port_c, port_x;
	SOCKET b = BoundLoopbackSocket(&port_b);
	SOCKET c = BoundLoopbackSocket(&port_c);
	SOCKET x = BoundLoopbackSocket(&port_x);
	const unsigned short local_port = FreeLoopbackPort();

	GGPOSessionCallbacks cb{};
	cb.begin_game = BeginGame;
	cb.save_game_state = SaveGameState;
	cb.load_game_state = LoadGameState;
	cb.log_game_state = LogGameState;
	cb.free_buffer = FreeBuffer;
	cb.advance_frame = AdvanceFrame;
	cb.on_event = OnEvent;
	GGPOSession* session = nullptr;
	ASSERT_EQ(ggpo_start_session(&session, &cb, "ggpo_test", 3, sizeof(uint16_t), local_port, nullptr, 0), GGPO_OK);

	const unsigned short ports[] = {0, port_b, port_c};
	for (int i = 0; i < 3; i++)
	{
		GGPOPlayer player{};
		player.size = sizeof(player);
		player.player_num = i + 1;
		player.type = i == 0 ? GGPO_PLAYERTYPE_LOCAL : GGPO_PLAYERTYPE_REMOTE;
		if (i != 0)
		{
			std::strcpy(player.u.remote.ip_address, "127.0.0.1");
			player.u.remote.port = ports[i];
		}
		GGPOPlayerHandle handle;
		ASSERT_EQ(ggpo_add_player(session, &player, &handle), GGPO_OK);
	}

	SendRelay(x, local_port, 2);
	EXPECT_EQ(ForwardedSender(session, c), -1) << "forwarded for an unknown source";

	SendRelay(b, local_port, 2);
	EXPECT_EQ(ForwardedSender(session, c), 1) << "not forwarded for a player, or not named by its source";

	SendRelay(c, local_port, 2);
	EXPECT_EQ(ForwardedSender(session, c), -1) << "forwarded back to its sender";

	EXPECT_EQ(ggpo_close_session(session), GGPO_OK);
	closesocket(b);
	closesocket(c);
	closesocket(x);
}

namespace
{
	constexpr uint16 kPeerMagic = 0x2468;

	void SendToLocal(SOCKET s, unsigned short port, const UdpMsg& m, int len)
	{
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = htons(port);
		sendto(s, reinterpret_cast<const char*>(&m), len, 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
	}

	// Two players: the local one and a peer played by socket peer, which answers the sync requests.
	GGPOSession* StartSyncedWithPeer(SOCKET peer, unsigned short peer_port, unsigned short local_port)
	{
		GGPOSessionCallbacks cb{};
		cb.begin_game = BeginGame;
		cb.save_game_state = SaveGameState;
		cb.load_game_state = LoadGameState;
		cb.log_game_state = LogGameState;
		cb.free_buffer = FreeBuffer;
		cb.advance_frame = AdvanceFrame;
		cb.on_event = OnEvent;
		GGPOSession* session = nullptr;
		EXPECT_EQ(ggpo_start_session(&session, &cb, "ggpo_test", 2, sizeof(uint16_t), local_port, nullptr, 0), GGPO_OK);
		for (int i = 0; i < 2; i++)
		{
			GGPOPlayer player{};
			player.size = sizeof(player);
			player.player_num = i + 1;
			player.type = i == 0 ? GGPO_PLAYERTYPE_LOCAL : GGPO_PLAYERTYPE_REMOTE;
			if (i != 0)
			{
				std::strcpy(player.u.remote.ip_address, "127.0.0.1");
				player.u.remote.port = peer_port;
			}
			GGPOPlayerHandle handle;
			EXPECT_EQ(ggpo_add_player(session, &player, &handle), GGPO_OK);
		}
		// The peer socket sends nothing but the test's packets: on a slow runner the disconnect
		// timeout would drop the peer before the test reads what the session took.
		EXPECT_EQ(ggpo_set_disconnect_timeout(session, 0), GGPO_OK);
		int replies = 0;
		for (int k = 0; k < 200 && replies < 5; k++)
		{
			EXPECT_EQ(ggpo_idle(session, 0), GGPO_OK);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			char buf[2048];
			int len;
			while ((len = recv(peer, buf, sizeof(buf), 0)) >= static_cast<int>(sizeof(UdpMsg::hdr)))
			{
				const UdpMsg* req = reinterpret_cast<const UdpMsg*>(buf);
				if (req->hdr.type != UdpMsg::SyncRequest)
					continue;
				UdpMsg reply(UdpMsg::SyncReply);
				reply.hdr.const_magic = CONST_MAGIC;
				reply.hdr.magic = kPeerMagic;
				reply.hdr.sequence_number = 0;
				reply.hdr.remote_endpoint = 1;
				reply.u.sync_reply.random_reply = req->u.sync_request.random_request;
				reply.u.sync_reply.verification_failure = 0;
				SendToLocal(peer, local_port, reply, sizeof(reply.hdr) + sizeof(reply.u.sync_reply));
				replies++;
			}
		}
		EXPECT_GE(replies, 5) << "sync never finished";
		return session;
	}

	void SendInput(SOCKET peer, unsigned short local_port, uint16 seq, uint32 start_frame, int frames, int peer_last_frame)
	{
		UdpMsg m(UdpMsg::Input);
		m.hdr.const_magic = CONST_MAGIC;
		m.hdr.magic = kPeerMagic;
		m.hdr.sequence_number = seq;
		m.hdr.remote_endpoint = 1;
		for (auto& s : m.u.input.peer_connect_status)
		{
			s.disconnected = 0;
			s.last_frame = peer_last_frame;
		}
		m.u.input.start_frame = start_frame;
		m.u.input.disconnect_requested = 0;
		m.u.input.ack_frame = 0;
		m.u.input.num_bits = static_cast<uint16>(frames);
		m.u.input.input_size = sizeof(uint16_t);
		std::memset(m.u.input.bits, 0, sizeof(m.u.input.bits));
		const int len = static_cast<int>(reinterpret_cast<const char*>(&m.u.input.bits[(frames + 7) / 8]) - reinterpret_cast<const char*>(&m));
		SendToLocal(peer, local_port, m, len);
	}

	void IdleExpectOk(GGPOSession* session, const char* what)
	{
		for (int k = 0; k < 20; k++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			ASSERT_EQ(ggpo_idle(session, 0), GGPO_OK) << what;
		}
	}
} // namespace

// A synced peer's malformed inputs are dropped; none of them ends the session.
TEST(GGPO, PeerInputsDoNotEndSession)
{
	struct Case
	{
		const char* what;
		uint32 start_frame;
		int frames;
		int first_last_frame;
		int second_last_frame;
	};
	const Case cases[] = {
		{"first frame far ahead", 1000000, 1, -1, -1},
		{"peer last frame goes back", 0, 1, 10, 5},
		{"more frames than the event queue holds", 0, 200, -1, -1},
	};
	for (const Case& c : cases)
	{
		unsigned short peer_port;
		SOCKET peer = BoundLoopbackSocket(&peer_port);
		const unsigned short local_port = FreeLoopbackPort();
		GGPOSession* session = StartSyncedWithPeer(peer, peer_port, local_port);
		SendInput(peer, local_port, 1, c.start_frame, c.frames, c.first_last_frame);
		IdleExpectOk(session, c.what);
		SendInput(peer, local_port, 2, c.start_frame + c.frames, 1, c.second_last_frame);
		IdleExpectOk(session, c.what);
		ggpo_close_session(session);
		closesocket(peer);
	}
}

namespace
{
	// The newest frame of the peer that the session reports back in its own input packets.
	int ReportedPeerFrame(GGPOSession* session, SOCKET peer)
	{
		uint16_t input = 0;
		EXPECT_EQ(ggpo_add_local_input(session, 1, &input, sizeof(input)), GGPO_OK);
		int frame = -2;
		for (int k = 0; k < 20; k++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			EXPECT_EQ(ggpo_idle(session, 0), GGPO_OK);
			char buf[2048];
			int len;
			while ((len = recv(peer, buf, sizeof(buf), 0)) >= static_cast<int>(sizeof(UdpMsg::hdr)))
			{
				const UdpMsg* m = reinterpret_cast<const UdpMsg*>(buf);
				if (m->hdr.type == UdpMsg::Input)
					frame = m->u.input.peer_connect_status[1].last_frame;
			}
		}
		return frame;
	}
} // namespace

// The session keeps taking a peer's inputs after a forged far-ahead frame and stops at what its queue holds.
TEST(GGPO, PeerInputsBounded)
{
	struct Packet
	{
		uint32 start_frame;
		int frames;
		int last_frame;
	};
	struct Case
	{
		const char* what;
		std::vector<Packet> packets;
		int reported;
	};
	const Case cases[] = {
		{"forged far-ahead last frame", {{0, 1, 1000000}, {0, 2, 1}}, 1},
		{"more frames than the input queue holds", {{0, 50, -1}, {50, 50, -1}, {100, 50, -1}, {150, 50, -1}}, 127},
	};
	for (const Case& c : cases)
	{
		unsigned short peer_port;
		SOCKET peer = BoundLoopbackSocket(&peer_port);
		const unsigned short local_port = FreeLoopbackPort();
		GGPOSession* session = StartSyncedWithPeer(peer, peer_port, local_port);
		uint16 seq = 1;
		for (const Packet& p : c.packets)
		{
			SendInput(peer, local_port, seq++, p.start_frame, p.frames, p.last_frame);
			IdleExpectOk(session, c.what);
		}
		EXPECT_EQ(ReportedPeerFrame(session, peer), c.reported) << c.what;
		ggpo_close_session(session);
		closesocket(peer);
	}
}
