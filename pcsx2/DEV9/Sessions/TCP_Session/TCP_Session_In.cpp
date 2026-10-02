// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef __POSIX__
#define SOCKET_ERROR -1
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#define SD_RECEIVE SHUT_RD
#endif

#include "TCP_Session.h"
#include "BuildVersion.h"

using namespace PacketReader;
using namespace PacketReader::IP;
using namespace PacketReader::IP::TCP;

namespace Sessions
{
	std::optional<ReceivedPayload> TCP_Session::Recv()
	{
		std::optional<ReceivedPayload> ret = PopRecvBuff();
		if (ret.has_value())
			return ret;

		switch (state)
		{
			case TCP_State::SendingSYN_ACK:
			{
				fd_set writeSet;
				fd_set exceptSet;

				FD_ZERO(&writeSet);
				FD_ZERO(&exceptSet);

				FD_SET(client, &writeSet);
				FD_SET(client, &exceptSet);

				timeval nowait{0};
				select(client + 1, nullptr, &writeSet, &exceptSet, &nowait);

				if (FD_ISSET(client, &writeSet))
					return ConnectTCPComplete(true);
				if (FD_ISSET(client, &exceptSet))
					return ConnectTCPComplete(false);

				return std::nullopt;
			}
			case TCP_State::SentSYN_ACK:
				// Don't read data untill PS2 ACKs connection
				return std::nullopt;
			case TCP_State::CloseCompletedFlushBuffer:
				/*
				 * When TCP connection is closed by the server
				 * the server is the last to send a packet
				 * so the event must be raised here
				 */
				state = TCP_State::CloseCompleted;
				RaiseEventConnectionClosed();
				return std::nullopt;
			case TCP_State::Connected:
			case TCP_State::Closing_ClosedByPS2:
				// Only accept data in above two states
				break;
			default:
				return std::nullopt;
		}

		if (ShouldWaitForAck())
			return std::nullopt;

		// Note, windowSize will be updated before _ReceivedAckNumber, potential race condition
		// in practice, we just get a smaller or -ve maxSize
		const u32 outstanding = GetOutstandingSequenceLength();

		int maxSize = 0;
		if (sendTimeStamps)
			maxSize = std::min<int>(maxSegmentSize - 12, windowSize.load() - outstanding);
		else
			maxSize = std::min<int>(maxSegmentSize, windowSize.load() - outstanding);

		if (maxSize > 0 && zdxsvLobbyFilter && zdxsvLobbyFilter->Ready() > 0)
		{
			// Lobby bytes the PS2's window could not take last time go first.
			PayloadData* heldData = new PayloadData(static_cast<int>(std::min<size_t>(maxSize, zdxsvLobbyFilter->Ready())));
			const u32 held = static_cast<u32>(zdxsvLobbyFilter->Take(heldData->data.get(), heldData->GetLength()));

			std::unique_ptr<TCP_Packet> iRet = CreateBasePacket(heldData);
			IncrementMyNumber(held);
			iRet->SetACK(true);
			iRet->SetPSH(true);
			myNumberACKed.store(false);
			return ReceivedPayload{destIP, std::move(iRet)};
		}

		if (maxSize > 0)
		{
			std::unique_ptr<u8[]> buffer;
			int err = 0;
			int recived;

			// FIONREAD uses unsigned long on windows and int on linux
			// Zero init so we don't have bad data on any unused bytes
			unsigned long available = 0;
#ifdef _WIN32
			err = ioctlsocket(client, FIONREAD, &available);
#elif defined(__POSIX__)
			err = ioctl(client, FIONREAD, &available);
#endif
			if (err != SOCKET_ERROR)
			{
				if (available > static_cast<uint>(maxSize))
					Console.WriteLn("DEV9: TCP: Got a lot of data: %lu using: %d", available, maxSize);

				buffer = std::make_unique<u8[]>(maxSize);
				recived = recv(client, reinterpret_cast<char*>(buffer.get()), maxSize, 0);
				if (recived == -1)
#ifdef _WIN32
					err = WSAGetLastError();
#elif defined(__POSIX__)
					err = errno;
#endif

				switch (err)
				{
#ifdef _WIN32
					case WSAEINVAL:
					case WSAESHUTDOWN:
						// In theory, this should only occur when the PS2 has RST the connection
						// and the call to TCPSession.Recv() occurs at just the right time.
						//Console.WriteLn("DEV9: TCP: Recv() on shutdown socket");
						return std::nullopt;
					case WSAEWOULDBLOCK:
						return std::nullopt;
#elif defined(__POSIX__)
					case EINVAL:
					case ESHUTDOWN:
						// See WSAESHUTDOWN
						//Console.WriteLn("DEV9: TCP: Recv() on shutdown socket");
						return std::nullopt;
					case EWOULDBLOCK:
						return std::nullopt;
#endif
					case 0:
						break;
					default:
						CloseByRemoteRST();
						Console.Error("DEV9: TCP: Recv error: %d", err);
						return std::nullopt;
				}

				// Server closed the Socket
				if (recived == 0)
				{
					const int result = shutdown(client, SD_RECEIVE);
					if (result == SOCKET_ERROR)
						Console.Error("DEV9: TCP: Shutdown SD_RECEIVE error: %d",
#ifdef _WIN32
							WSAGetLastError());
#elif defined(__POSIX__)
							errno);
#endif

					switch (state)
					{
						case TCP_State::Connected:
							return CloseByRemoteStage1();
						case TCP_State::Closing_ClosedByPS2:
							return CloseByPS2Stage3();
						default:
							CloseByRemoteRST();
							Console.Error("DEV9: TCP: Remote close occured with invalid TCP state");
							break;
					}
					return std::nullopt;
				}
				DevCon.WriteLn("DEV9: TCP: [SRV] Sending %d bytes", recived);
				ZdxsvSendPlatformInfo(buffer.get(), recived);
				if (zdxsvLobbyFilter)
				{
					// Strips the lobby's battle info notice; may hold back a partial frame.
					zdxsvLobbyFilter->Feed(buffer.get(), recived);
					recived = static_cast<int>(zdxsvLobbyFilter->Take(buffer.get(), maxSize));
					if (recived == 0)
						return std::nullopt;
				}

				PayloadData* recivedData = new PayloadData(recived);
				memcpy(recivedData->data.get(), buffer.get(), recived);

				std::unique_ptr<TCP_Packet> iRet = CreateBasePacket(recivedData);
				IncrementMyNumber(static_cast<u32>(recived));

				iRet->SetACK(true);
				iRet->SetPSH(true);

				myNumberACKed.store(false);
				//DevCon.WriteLn("DEV9: TCP: myNumberACKed reset");
				return ReceivedPayload{destIP, std::move(iRet)};
			}
		}

		return std::nullopt;
	}

	// The zdxsv lobby server opens every connection with a key pair question
	// (dir 0x18, category 0x01, command 0x6101). On such a connection, send the
	// server a custom message (dir 0x81, category 0xFF, command 0x9950) with
	// "key=value" lines, before the game answers. Real PS2 never sends it, so the
	// server can keep emulator and console players apart (as gdxsv does).
	// The PS2 side never sees this message.
	void TCP_Session::ZdxsvSendPlatformInfo(const u8* data, int len)
	{
		if (zdxsvChecked)
			return;
		zdxsvChecked = true;
		if (len < 12 || data[0] != 0x18 || data[1] != 0x01 || data[2] != 0x61 || data[3] != 0x01)
			return;
		// ZDXSV_PLATFORM_INFO=0: behave like a real PS2 (for testing the console side)
		const char* env = std::getenv("ZDXSV_PLATFORM_INFO");
		if (env && std::string(env) == "0")
		{
			Console.WriteLn("DEV9: TCP: zdxsv lobby detected, platform info off (ZDXSV_PLATFORM_INFO=0)");
			return;
		}

		std::string body = "emulator=pcsx2\n";
		body += std::string("version=") + BuildVersion::GitRev + "\n";
#if defined(_WIN32)
		body += "os=windows\n";
#elif defined(__APPLE__)
		body += "os=macos\n";
#else
		body += "os=linux\n";
#endif
#if defined(_M_X86)
		body += "cpu=x86/64\n";
#elif defined(_M_ARM64)
		body += "cpu=arm64\n";
#else
		body += "cpu=unknown\n";
#endif
		// udp=1: this emulator bridges the game's battle TCP to the battle server
		// over UDP (DEV9/Zdxsv), so the lobby sends it battle info (0x9951).
		if (Zdxsv::Enabled())
		{
			body += "udp=1\n";
			Zdxsv::SetLogger([](const std::string& s) { Console.WriteLn("DEV9: %s", s.c_str()); });
			// udp_addr/udp_local: where peers reach this emulator (P2P). The lobby's
			// UDP STUN is on its host at 8201 (zdxsv docker-compose); ZDXSV_STUN_PORT overrides.
			const char* stunPort = std::getenv("ZDXSV_STUN_PORT");
			body += Zdxsv::OpenUdp(std::bit_cast<u32>(destIP), stunPort ? static_cast<u16>(std::atoi(stunPort)) : 8201);
			zdxsvLobbyFilter = std::make_unique<Zdxsv::LobbyFilter>();
		}

		std::vector<u8> msg = {0x81, 0xFF, 0x99, 0x50,
			static_cast<u8>(body.size() >> 8), static_cast<u8>(body.size()),
			0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
		msg.insert(msg.end(), body.begin(), body.end());

		size_t sent = 0;
		while (sent < msg.size())
		{
			const int ret = send(client, reinterpret_cast<const char*>(&msg[sent]), static_cast<int>(msg.size() - sent), 0);
			if (ret == SOCKET_ERROR)
			{
#ifdef _WIN32
				const int err = WSAGetLastError();
				if (err == WSAEWOULDBLOCK)
#elif defined(__POSIX__)
				const int err = errno;
				if (err == EWOULDBLOCK)
#endif
					std::this_thread::yield();
				else
				{
					Console.Error("DEV9: TCP: zdxsv platform info send error: %d", err);
					return;
				}
			}
			else
				sent += ret;
		}
		Console.WriteLn("DEV9: TCP: zdxsv lobby detected, sent platform info (%zu bytes)", msg.size());
	}

	std::optional<ReceivedPayload> TCP_Session::ConnectTCPComplete(bool success)
	{
		if (success)
		{
			state = TCP_State::SentSYN_ACK;

			std::unique_ptr<TCP_Packet> ret = std::make_unique<TCP_Packet>(new PayloadData(0));
			// Send packet to say we connected
			ret->sourcePort = destPort;
			ret->destinationPort = srcPort;

			ret->sequenceNumber = GetMyNumber();
			IncrementMyNumber(1);

			ret->acknowledgementNumber = expectedSeqNumber;

			ret->SetSYN(true);
			ret->SetACK(true);
			ret->windowSize = (2 * maxSegmentSize);
			ret->options.push_back(new TCPopMSS(maxSegmentSize));

			ret->options.push_back(new TCPopNOP());
			ret->options.push_back(new TCPopWS(0));

			if (sendTimeStamps)
			{
				ret->options.push_back(new TCPopNOP());
				ret->options.push_back(new TCPopNOP());

				const auto timestampChrono = std::chrono::steady_clock::now() - timeStampStart;
				const u32 timestampSeconds = std::chrono::duration_cast<std::chrono::seconds>(timestampChrono).count() % UINT_MAX;

				ret->options.push_back(new TCPopTS(timestampSeconds, lastRecivedTimeStamp));
			}
			return ReceivedPayload{destIP, std::move(ret)};
		}
		else
		{
			int error = 0;
#ifdef _WIN32
			int len = sizeof(error);
			if (getsockopt(client, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) < 0)
				Console.Error("DEV9: TCP: Unkown TCP connection error (getsockopt error: %d)", WSAGetLastError());
#elif defined(__POSIX__)
			socklen_t len = sizeof(error);
			if (getsockopt(client, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) < 0)
				Console.Error("DEV9: TCP: Unkown TCP connection error (getsockopt error: %d)", errno);
#endif
			else
				Console.Error("DEV9: TCP: Connect error: %d", error);

			state = TCP_State::CloseCompleted;
			RaiseEventConnectionClosed();
			return std::nullopt;
		}
	}

	ReceivedPayload TCP_Session::CloseByPS2Stage3()
	{
		//Console.WriteLn("DEV9: TCP: Remote has closed connection after PS2");

		std::unique_ptr ret = CreateBasePacket();
		IncrementMyNumber(1);

		ret->SetACK(true);
		ret->SetFIN(true);

		myNumberACKed.store(false);
		//DevCon.WriteLn("myNumberACKed reset");

		state = TCP_State::Closing_ClosedByPS2ThenRemote_WaitingForAck;
		return ReceivedPayload{destIP, std::move(ret)};
	}

	ReceivedPayload TCP_Session::CloseByRemoteStage1()
	{
		//Console.WriteLn("DEV9: TCP: Remote has closed connection");

		std::unique_ptr<TCP_Packet> ret = CreateBasePacket();
		IncrementMyNumber(1);

		ret->SetACK(true);
		ret->SetFIN(true);

		myNumberACKed.store(false);
		//DevCon.WriteLn("myNumberACKed reset");

		state = TCP_State::Closing_ClosedByRemote;
		return ReceivedPayload{destIP, std::move(ret)};
	}
} // namespace Sessions
