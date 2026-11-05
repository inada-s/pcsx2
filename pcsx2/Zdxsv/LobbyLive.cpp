// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Live spectating over the lobby's UDP socket (Lobby.h LiveUp / LiveDown).

#include "Zdxsv/Lobby.h"
#include "Zdxsv/LobbyNet.h"
#include "Zdxsv/Proto.h"


#include <array>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace Zdxsv
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// Packet.type and its message field
		constexpr uint32_t LIVE_PUSH = 20, LIVE_ACK = 21, LIVE_SUBSCRIBE = 24, LIVE_CHALLENGE = 25;
		constexpr size_t LIVE_CHUNK = 1000;
		constexpr size_t LIVE_WINDOW = 32; // datagrams past the ack
		constexpr auto LIVE_RESEND = std::chrono::milliseconds(200);
		constexpr size_t LIVE_MAX_FRAMES_PER_PUSH = 128;
		constexpr auto LIVE_KEEPALIVE = std::chrono::seconds(2);
		constexpr auto LIVE_CLOSE_GIVEUP = std::chrono::seconds(30);
		constexpr size_t LIVE_COOKIE = 16;

		std::vector<uint8_t> LivePacket(uint32_t type, const std::vector<uint8_t>& msg)
		{
			std::vector<uint8_t> o;
			Pb::PutUint(o, 1, type);
			Pb::PutBytes(o, type, msg.data(), msg.size());
			return o;
		}

		// A spectator packet's type (0 = none) and message.
		uint32_t LiveParse(const uint8_t* p, size_t n, std::string_view& msg)
		{
			uint64_t type = 0;
			std::map<uint32_t, std::string_view> msgs;
			Pb::Reader rd{p, p + n};
			if (!rd.Fields([&](uint32_t field, uint32_t wt, uint64_t v, const uint8_t* b, size_t len) {
					if (field == 1 && wt == 0)
						type = v;
					else if (wt == 2)
						msgs[field] = std::string_view(reinterpret_cast<const char*>(b), len);
					return true;
				}))
				return 0;
			const auto it = msgs.find(static_cast<uint32_t>(type));
			if (it == msgs.end())
				return 0;
			msg = it->second;
			return static_cast<uint32_t>(type);
		}

		bool LiveFields(std::string_view msg, std::map<uint32_t, uint64_t>& nums, std::map<uint32_t, std::string_view>& bytes)
		{
			Pb::Reader rd{reinterpret_cast<const uint8_t*>(msg.data()), reinterpret_cast<const uint8_t*>(msg.data() + msg.size())};
			return rd.Fields([&](uint32_t field, uint32_t wt, uint64_t v, const uint8_t* b, size_t len) {
				if (wt == 0)
					nums[field] = v;
				else if (wt == 2)
					bytes[field] = std::string_view(reinterpret_cast<const char*>(b), len);
				return true;
			});
		}

		// The sender's go-back-N position in one stream (bytes or frames).
		struct LiveWindow
		{
			size_t acked = 0, next = 0;
			Clock::time_point progress{};
			void Ack(size_t n, Clock::time_point now)
			{
				if (acked < n)
				{
					acked = n;
					progress = now;
				}
				next = std::max(next, acked);
			}
			// Chunks of up to per units from next while next < have and within LIVE_WINDOW chunks of acked;
			// back to acked after LIVE_RESEND without progress.
			template <typename F>
			void Send(Clock::time_point now, size_t have, size_t per, F send)
			{
				if (next > acked && now - progress >= LIVE_RESEND)
				{
					next = acked;
					progress = now;
				}
				while (next < have && next < acked + LIVE_WINDOW * per)
				{
					const size_t n = std::min(per, have - next);
					if (next == acked)
						progress = now;
					send(next, n);
					next += n;
				}
			}
		};

		// "host:port" -> an IPv4 address.
		bool LiveResolve(const std::string& hostPort, sockaddr_in& out)
		{
#ifdef _WIN32
			static const bool wsa = [] {
				WSADATA d;
				return WSAStartup(MAKEWORD(2, 2), &d) == 0;
			}();
			if (!wsa)
				return false;
#endif
			const size_t colon = hostPort.rfind(':');
			if (colon == std::string::npos)
				return false;
			addrinfo hints{};
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_DGRAM;
			addrinfo* res = nullptr;
			if (getaddrinfo(hostPort.substr(0, colon).c_str(), hostPort.substr(colon + 1).c_str(), &hints, &res) != 0 || !res)
				return false;
			out = *reinterpret_cast<const sockaddr_in*>(res->ai_addr);
			freeaddrinfo(res);
			return true;
		}

		sock_t LiveSocket()
		{
			sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			sockaddr_in any{};
			any.sin_family = AF_INET;
			if (s != INVALID_SOCKET && bind(s, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) != 0)
			{
				closesocket(s);
				return INVALID_SOCKET;
			}
			return s;
		}

		void LiveSend(sock_t s, const sockaddr_in& to, uint32_t type, const std::vector<uint8_t>& msg)
		{
			const std::vector<uint8_t> data = LivePacket(type, msg);
			sendto(s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
		}

		// Receives one packet from `from` within ms; returns its spectator type (0 = none).
		uint32_t LiveRecv(sock_t s, const sockaddr_in& from, int ms, std::vector<uint8_t>& buf, std::string_view& msg)
		{
			if (!LobbyWaitReadable(s, ms))
				return 0;
			buf.resize(64 * 1024);
			sockaddr_in src{};
			socklen_t len = sizeof(src);
			const int n = recvfrom(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr*>(&src), &len);
			if (n <= 0 || !LobbySameAddr(src, from))
				return 0;
			return LiveParse(buf.data(), static_cast<size_t>(n), msg);
		}
	} // namespace

	struct LiveUp::Impl
	{
		sock_t s = INVALID_SOCKET;
		sockaddr_in to{};
		std::string code;
		uint32_t session = 0;
		int frameBytes = 0;
		std::mutex mtx;
		std::vector<uint8_t> header, inputs, rounds;
		size_t roundAck = 0; // bytes of rounds the lobby holds
		bool closeSet = false, headerAck = false, closeAck = false;
		std::string close;
		LiveWindow in;
		Clock::time_point headerSent{}, roundSent{}, closeSent{}, closeAt{};
		std::atomic<bool> quit{false}, done{false};
		std::thread t;

		std::vector<uint8_t> Push() const
		{
			std::vector<uint8_t> m;
			Pb::PutString(m, 1, code);
			Pb::PutInt(m, 2, static_cast<int32_t>(session));
			return m;
		}

		void Run()
		{
			std::vector<uint8_t> buf;
			size_t sent = 0;
			while (!quit)
			{
				std::string_view msg;
				const uint32_t type = LiveRecv(s, to, 10, buf, msg);
				std::lock_guard lock(mtx);
				const Clock::time_point now = Clock::now();
				std::map<uint32_t, uint64_t> nums;
				std::map<uint32_t, std::string_view> bytes;
				if (type == LIVE_ACK && LiveFields(msg, nums, bytes) && bytes[1] == code)
				{
					headerAck = headerAck || nums[40] != 0;
					in.Ack(nums[2], now);
					roundAck = std::max<size_t>(roundAck, nums[43]);
					closeAck = closeAck || nums[42] != 0;
				}
				if (closeAck || (closeSet && now - closeAt > LIVE_CLOSE_GIVEUP))
				{
					LobbyLog("live: uplink " + code + (closeAck ? " closed: " : " gave up on the close: ") + std::to_string(in.acked) + " frames, " +
						std::to_string(sent) + " datagrams");
					break;
				}
				if (!headerAck && now - headerSent >= LIVE_RESEND)
				{
					std::vector<uint8_t> m = Push();
					Pb::PutBytes(m, 10, header.data(), header.size());
					LiveSend(s, to, LIVE_PUSH, m);
					headerSent = now;
					sent++;
				}
				const size_t fb = static_cast<size_t>(frameBytes);
				const size_t frames = inputs.size() / fb;
				in.Send(now, frames, std::min(LIVE_MAX_FRAMES_PER_PUSH, LIVE_CHUNK / fb), [&](size_t f, size_t n) {
					std::vector<uint8_t> m = Push();
					Pb::PutInt(m, 3, static_cast<int64_t>(f));
					Pb::PutBytes(m, 40, inputs.data() + f * fb, n * fb);
					Pb::PutInt(m, 41, frameBytes);
					LiveSend(s, to, LIVE_PUSH, m);
					sent++;
				});
				if (headerAck && roundAck < rounds.size() && now - roundSent >= LIVE_RESEND)
				{
					std::vector<uint8_t> m = Push();
					Pb::PutBytes(m, 45, rounds.data(), rounds.size());
					LiveSend(s, to, LIVE_PUSH, m);
					roundSent = now;
					sent++;
				}
				if (closeSet && headerAck && in.acked == frames && roundAck >= rounds.size() && now - closeSent >= LIVE_RESEND)
				{
					std::vector<uint8_t> m = Push();
					Pb::PutInt(m, 3, static_cast<int64_t>(frames));
					Pb::PutString(m, 8, close);
					Pb::PutInt(m, 41, frameBytes);
					LiveSend(s, to, LIVE_PUSH, m);
					closeSent = now;
					sent++;
				}
			}
			done = true;
		}
	};

	LiveUp::LiveUp(const std::string& to, std::string code, uint32_t session, std::vector<uint8_t> header, int frameBytes)
		: m(std::make_unique<Impl>())
	{
		m->code = std::move(code);
		m->session = session;
		m->header = std::move(header);
		m->frameBytes = std::max(frameBytes, 1);
		if (!LiveResolve(to, m->to) || (m->s = LiveSocket()) == INVALID_SOCKET)
		{
			LobbyLog("live: uplink to " + to + " failed: no socket");
			m->done = true;
			return;
		}
		LobbyLog("live: uplink " + m->code + " to " + to);
		m->t = std::thread([this] { m->Run(); });
	}

	LiveUp::~LiveUp()
	{
		m->quit = true;
		if (m->t.joinable())
			m->t.join();
		if (m->s != INVALID_SOCKET)
			closesocket(m->s);
	}

	void LiveUp::AddFrames(const void* data, size_t frames)
	{
		std::lock_guard lock(m->mtx);
		const uint8_t* p = static_cast<const uint8_t*>(data);
		m->inputs.insert(m->inputs.end(), p, p + frames * m->frameBytes);
	}

	void LiveUp::AddRound(const std::vector<uint8_t>& entry)
	{
		std::lock_guard lock(m->mtx);
		m->rounds.insert(m->rounds.end(), entry.begin(), entry.end());
		m->roundSent = {};
	}

	size_t LiveUp::Frames()
	{
		std::lock_guard lock(m->mtx);
		return m->inputs.size() / m->frameBytes;
	}

	void LiveUp::Close(const std::string& reason)
	{
		std::lock_guard lock(m->mtx);
		if (std::exchange(m->closeSet, true))
			return;
		m->close = reason.empty() ? "end" : reason;
		m->closeAt = Clock::now();
	}

	bool LiveUp::Done()
	{
		return m->done;
	}

	struct LiveDown::Impl
	{
		sock_t s = INVALID_SOCKET;
		sockaddr_in to{};
		std::mutex mtx;
		std::string code;
		std::vector<uint8_t> cookie = std::vector<uint8_t>(LIVE_COOKIE);
		LiveStreams got; // the header until the first Take, then the frames not taken yet
		bool headerTaken = false;
		size_t frames = 0; // received in a row
		Clock::time_point rx = Clock::now(), subscribed{};
		std::atomic<bool> quit{false};
		std::thread t;

		void Subscribe(Clock::time_point now)
		{
			std::vector<uint8_t> m;
			Pb::PutString(m, 1, code);
			Pb::PutInt(m, 3, static_cast<int64_t>(frames));
			Pb::PutBytes(m, 4, cookie.data(), cookie.size());
			LiveSend(s, to, LIVE_SUBSCRIBE, m);
			subscribed = now;
		}

		void Run()
		{
			std::vector<uint8_t> buf;
			while (!quit)
			{
				std::string_view msg;
				const uint32_t type = LiveRecv(s, to, 10, buf, msg);
				std::lock_guard lock(mtx);
				const Clock::time_point now = Clock::now();
				std::map<uint32_t, uint64_t> nums;
				std::map<uint32_t, std::string_view> bytes;
				if (type == LIVE_CHALLENGE && LiveFields(msg, nums, bytes) && bytes[2].size() == LIVE_COOKIE && (code.empty() || bytes[1] == code))
				{
					code = bytes[1];
					cookie.assign(bytes[2].begin(), bytes[2].end());
					Subscribe(now);
				}
				else if (type == LIVE_PUSH && LiveFields(msg, nums, bytes) && !code.empty() && bytes[1] == code)
				{
					rx = now;
					LiveStreams& g = got;
					if (const auto h = bytes.find(10); h != bytes.end() && !headerTaken && g.header.empty())
						g.header.assign(h->second.begin(), h->second.end());
					const size_t fb = nums[41];
					if (fb > 0 && (g.frameBytes == 0 || static_cast<size_t>(g.frameBytes) == fb))
					{
						g.frameBytes = static_cast<int>(fb);
						const std::string_view in = bytes[40];
						if (in.size() % fb == 0 && nums[3] == frames)
						{
							g.inputs.insert(g.inputs.end(), in.begin(), in.end());
							frames += in.size() / fb;
						}
					}
					if (const auto r = bytes.find(45); r != bytes.end() && r->second.size() > g.rounds.size())
						g.rounds.assign(r->second.begin(), r->second.end());
					// the lobby's close push carries no frame_bytes
					if (bytes.count(8) && nums[3] == frames && !g.closed)
					{
						g.closed = true;
						g.close = bytes[8];
					}
					std::vector<uint8_t> ack;
					Pb::PutString(ack, 1, code);
					Pb::PutInt(ack, 2, static_cast<int64_t>(frames));
					Pb::PutInt(ack, 40, headerTaken || !g.header.empty() ? 1 : 0);
					Pb::PutInt(ack, 42, g.closed ? 1 : 0);
					Pb::PutInt(ack, 43, static_cast<int64_t>(g.rounds.size()));
					LiveSend(s, to, LIVE_ACK, ack);
				}
				if (!got.closed && now - subscribed >= LIVE_KEEPALIVE)
					Subscribe(now);
			}
		}
	};

	LiveDown::LiveDown()
		: m(std::make_unique<Impl>())
	{
	}

	LiveDown::~LiveDown()
	{
		m->quit = true;
		if (m->t.joinable())
			m->t.join();
		if (m->s != INVALID_SOCKET)
			closesocket(m->s);
	}

	std::unique_ptr<LiveDown> LiveDown::Open(const std::string& url, int timeoutMs, std::string& error)
	{
		std::unique_ptr<LiveDown> d(new LiveDown());
		const size_t slash = url.find('/');
		const std::string hostPort = url.substr(0, slash);
		d->m->code = slash == std::string::npos ? std::string() : url.substr(slash + 1);
		if (!LiveResolve(hostPort, d->m->to) || (d->m->s = LiveSocket()) == INVALID_SOCKET)
		{
			error = "cannot resolve " + hostPort + " or open a socket";
			return nullptr;
		}
		d->m->Subscribe(Clock::now());
		d->m->t = std::thread([m = d->m.get()] { m->Run(); });
		const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
		while (Clock::now() < until)
		{
			{
				std::lock_guard lock(d->m->mtx);
				const LiveStreams& g = d->m->got;
				if (!g.header.empty() && (d->m->frames > 0 || g.closed))
					return d;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		std::lock_guard lock(d->m->mtx);
		const LiveStreams& g = d->m->got;
		error = d->m->code.empty() ? "no live battle there (no challenge)" :
									 "battle " + d->m->code + ": header " + std::to_string(g.header.size()) + " bytes, " +
										 std::to_string(d->m->frames) + " frames after " + std::to_string(timeoutMs) + " ms";
		return nullptr;
	}

	std::string LiveDown::Code()
	{
		std::lock_guard lock(m->mtx);
		return m->code;
	}

	std::string LiveDown::Newest(const std::string& hostPort, const std::vector<std::string>& skip, int timeoutMs)
	{
		sockaddr_in to{};
		sock_t s;
		if (!LiveResolve(hostPort, to) || (s = LiveSocket()) == INVALID_SOCKET)
			return {};
		// a subscribe without a cookie only gets the challenge, which names the battle
		std::vector<uint8_t> m;
		Pb::PutString(m, 1, std::string());
		for (const std::string& code : skip)
			Pb::PutString(m, 5, code);
		LiveSend(s, to, LIVE_SUBSCRIBE, m);
		std::string code;
		std::vector<uint8_t> buf;
		const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
		while (code.empty() && Clock::now() < until)
		{
			std::string_view msg;
			std::map<uint32_t, uint64_t> nums;
			std::map<uint32_t, std::string_view> bytes;
			if (LiveRecv(s, to, 10, buf, msg) == LIVE_CHALLENGE && LiveFields(msg, nums, bytes))
				code = bytes[1];
		}
		closesocket(s);
		return code;
	}

	bool LiveDown::Take(LiveStreams& s, int stallMs)
	{
		std::lock_guard lock(m->mtx);
		LiveStreams& g = m->got;
		if (!m->headerTaken)
		{
			m->headerTaken = true;
			s.header = std::move(g.header);
		}
		s.inputs.insert(s.inputs.end(), g.inputs.begin(), g.inputs.end());
		g.inputs.clear();
		s.rounds = g.rounds;
		s.frameBytes = g.frameBytes;
		s.closed = g.closed;
		s.close = g.close;
		return g.closed || Clock::now() - m->rx < std::chrono::milliseconds(stallMs);
	}
} // namespace Zdxsv
