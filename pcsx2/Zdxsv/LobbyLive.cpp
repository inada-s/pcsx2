// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Live spectating over the lobby HTTP API (Lobby.h LiveUp / LiveDown): the uplink POSTs
// /lbs/live_push, spectators long-poll /lbs/live_pull.

#include "Zdxsv/Lobby.h"
#include "Zdxsv/LobbyNet.h"
#include "Zdxsv/Proto.h"

#include "Host.h"

#include "common/HTTPDownloader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace Zdxsv
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr size_t LIVE_MAX_PUSH_FRAMES = 600;
		constexpr auto LIVE_PUSH_INTERVAL = std::chrono::milliseconds(100);
		constexpr auto LIVE_RETRY = std::chrono::milliseconds(500);
		constexpr auto LIVE_CLOSE_GIVEUP = std::chrono::seconds(30);
		constexpr float LIVE_HTTP_TIMEOUT_S = 10.0f; // above the lobby's 1 s long poll
		constexpr int LIVE_SLEEP_STEP_MS = 10;
		constexpr s32 LIVE_HTTP_NOT_FOUND = 404;

		constexpr std::string_view LIVE_SCHEME = "live:";

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

		// One spectator id per process: the lobby counts spectators by it, auto-next probes included.
		const std::string& LiveSpectatorId()
		{
			static const std::string id = [] {
				std::random_device rd;
				return std::to_string(rd()) + std::to_string(rd());
			}();
			return id;
		}

		struct LiveHttp
		{
			std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Host::GetHTTPUserAgent());

			LiveHttp()
			{
				if (http)
					http->SetTimeout(LIVE_HTTP_TIMEOUT_S);
			}

			// The status (HTTPDownloader::HTTP_STATUS_* below 0) and body of a GET, or a POST when post is set.
			s32 Call(const std::string& url, const std::vector<uint8_t>* post, std::vector<uint8_t>& body)
			{
				if (!http)
					return HTTPDownloader::HTTP_STATUS_ERROR;
				s32 status = HTTPDownloader::HTTP_STATUS_ERROR;
				auto done = [&](s32 code, const std::string&, HTTPDownloader::Request::Data data) {
					status = code;
					body = std::move(data);
				};
				if (post)
					http->CreatePostRequest(url, std::string(post->begin(), post->end()), done, nullptr, "application/x-protobuf");
				else
					http->CreateRequest(url, done);
				http->WaitForAllRequests();
				return status;
			}
		};

		void LiveSleep(const std::atomic<bool>& quit, std::chrono::milliseconds d)
		{
			const auto until = Clock::now() + d;
			while (!quit && Clock::now() < until)
				std::this_thread::sleep_for(std::chrono::milliseconds(LIVE_SLEEP_STEP_MS));
		}

		std::string LivePullUrl(const std::string& api, const std::string& query)
		{
			return api + "/lbs/live_pull?id=" + LiveSpectatorId() + query;
		}
	} // namespace

	std::string LiveSource(const std::string& api, const std::string& code)
	{
		if (api.empty())
			return {};
		return std::string(LIVE_SCHEME) + api + (code.empty() ? "" : "#" + code);
	}

	bool LiveSourceSplit(std::string_view src, std::string& api, std::string& code)
	{
		if (!src.starts_with(LIVE_SCHEME))
			return false;
		src.remove_prefix(LIVE_SCHEME.size());
		const size_t hash = src.find('#');
		api = std::string(src.substr(0, hash));
		while (!api.empty() && api.back() == '/')
			api.pop_back();
		code = hash == std::string_view::npos ? std::string() : std::string(src.substr(hash + 1));
		return true;
	}

	struct LiveUp::Impl
	{
		std::string url;
		std::string code;
		uint32_t session = 0;
		int frameBytes = 0;
		std::mutex mtx;
		std::vector<uint8_t> header, inputs, rounds;
		size_t roundAck = 0, frameAck = 0;
		bool closeSet = false, headerAck = false, closeAck = false;
		std::string close;
		Clock::time_point closeAt{};
		std::atomic<bool> quit{false}, done{false};
		std::thread t;

		// The next push, empty when there is nothing to send.
		std::vector<uint8_t> Next()
		{
			std::lock_guard lock(mtx);
			std::vector<uint8_t> m;
			Pb::PutString(m, 1, code);
			Pb::PutInt(m, 2, static_cast<int32_t>(session));
			const size_t base = m.size();
			if (!headerAck)
			{
				Pb::PutBytes(m, 10, header.data(), header.size());
				return m;
			}
			if (roundAck < rounds.size())
				Pb::PutBytes(m, 45, rounds.data(), rounds.size());
			const size_t fb = static_cast<size_t>(frameBytes);
			const size_t frames = inputs.size() / fb;
			if (frameAck < frames)
			{
				const size_t n = std::min(LIVE_MAX_PUSH_FRAMES, frames - frameAck);
				Pb::PutInt(m, 3, static_cast<int64_t>(frameAck));
				Pb::PutBytes(m, 40, inputs.data() + frameAck * fb, n * fb);
				Pb::PutInt(m, 41, frameBytes);
			}
			else if (closeSet && roundAck >= rounds.size())
			{
				Pb::PutInt(m, 3, static_cast<int64_t>(frames));
				Pb::PutString(m, 8, close);
				Pb::PutInt(m, 41, frameBytes);
			}
			if (m.size() == base)
				m.clear();
			return m;
		}

		void Run()
		{
			LiveHttp http;
			size_t posts = 0, failures = 0;
			std::vector<uint8_t> body;
			while (!quit)
			{
				{
					std::lock_guard lock(mtx);
					if (closeAck || (closeSet && Clock::now() - closeAt > LIVE_CLOSE_GIVEUP))
					{
						LobbyLog("live: uplink " + code + (closeAck ? " closed: " : " gave up on the close: ") + std::to_string(frameAck) +
							" frames, " + std::to_string(posts) + " posts, " + std::to_string(failures) + " failed");
						break;
					}
				}
				const std::vector<uint8_t> m = Next();
				if (m.empty())
				{
					LiveSleep(quit, LIVE_PUSH_INTERVAL);
					continue;
				}
				const auto sent = Clock::now();
				const s32 status = http.Call(url, &m, body);
				posts++;
				std::map<uint32_t, uint64_t> nums;
				std::map<uint32_t, std::string_view> bytes;
				const std::string_view msg(reinterpret_cast<const char*>(body.data()), body.size());
				if (status == LIVE_HTTP_NOT_FOUND)
				{
					LobbyLog("live: uplink " + code + ": the lobby does not know the battle (404), stopped");
					break;
				}
				if (status != HTTPDownloader::HTTP_STATUS_OK || !LiveFields(msg, nums, bytes) || bytes[1] != code)
				{
					if (failures++ == 0)
						LobbyLog("live: uplink " + code + ": push failed, status " + std::to_string(status) + ", retrying");
					LiveSleep(quit, LIVE_RETRY);
					continue;
				}
				{
					std::lock_guard lock(mtx);
					headerAck = headerAck || nums[40] != 0;
					frameAck = std::max<size_t>(frameAck, nums[2]);
					roundAck = std::max<size_t>(roundAck, nums[43]);
					closeAck = closeAck || nums[42] != 0;
				}
				const auto took = Clock::now() - sent;
				if (took < LIVE_PUSH_INTERVAL)
					LiveSleep(quit, std::chrono::duration_cast<std::chrono::milliseconds>(LIVE_PUSH_INTERVAL - took));
			}
			done = true;
		}
	};

	LiveUp::LiveUp(const std::string& api, std::string code, uint32_t session, std::vector<uint8_t> header, int frameBytes)
		: m(std::make_unique<Impl>())
	{
		m->url = api + "/lbs/live_push";
		m->code = std::move(code);
		m->session = session;
		m->header = std::move(header);
		m->frameBytes = std::max(frameBytes, 1);
		LobbyLog("live: uplink " + m->code + " to " + m->url);
		m->t = std::thread([this] { m->Run(); });
	}

	LiveUp::~LiveUp()
	{
		m->quit = true;
		if (m->t.joinable())
			m->t.join();
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
		std::string api;
		std::mutex mtx;
		std::string code;
		LiveStreams got; // the header until the first Take, then the frames not taken yet
		bool headerTaken = false;
		size_t frames = 0; // received in a row
		std::string error;
		Clock::time_point rx = Clock::now();
		std::atomic<bool> quit{false};
		std::thread t;

		std::string Query()
		{
			std::lock_guard lock(mtx);
			return "&battle_code=" + code + "&header=" + (headerTaken || !got.header.empty() ? "1" : "0") +
				   "&frame=" + std::to_string(frames) + "&round=" + std::to_string(got.rounds.size());
		}

		void Run()
		{
			LiveHttp http;
			std::vector<uint8_t> body;
			while (!quit)
			{
				const s32 status = http.Call(LivePullUrl(api, Query()), nullptr, body);
				std::map<uint32_t, uint64_t> nums;
				std::map<uint32_t, std::string_view> bytes;
				const std::string_view msg(reinterpret_cast<const char*>(body.data()), body.size());
				if (status != HTTPDownloader::HTTP_STATUS_OK || !LiveFields(msg, nums, bytes))
				{
					std::lock_guard lock(mtx);
					error = status == LIVE_HTTP_NOT_FOUND ? "no such live battle (404)" : "status " + std::to_string(status);
					LiveSleep(quit, LIVE_RETRY);
					continue;
				}
				std::lock_guard lock(mtx);
				if (code.empty())
					code = bytes[1];
				else if (bytes[1] != code)
					continue;
				rx = Clock::now();
				error.clear();
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
				// the lobby's close carries no frame_bytes
				if (bytes.count(8) && nums[3] == frames && !g.closed)
				{
					g.closed = true;
					g.close = bytes[8];
				}
				if (g.closed)
					break;
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
	}

	std::unique_ptr<LiveDown> LiveDown::Open(const std::string& url, int timeoutMs, std::string& error)
	{
		std::unique_ptr<LiveDown> d(new LiveDown());
		if (!LiveSourceSplit(url, d->m->api, d->m->code) || d->m->api.empty())
		{
			error = "not a live source (live:<lobby API URL>[#battle code])";
			return nullptr;
		}
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
			std::this_thread::sleep_for(std::chrono::milliseconds(LIVE_SLEEP_STEP_MS));
		}
		std::lock_guard lock(d->m->mtx);
		const LiveStreams& g = d->m->got;
		error = d->m->code.empty() ? "no live battle there" + (d->m->error.empty() ? "" : " (" + d->m->error + ")") :
									 "battle " + d->m->code + ": header " + std::to_string(g.header.size()) + " bytes, " +
										 std::to_string(d->m->frames) + " frames after " + std::to_string(timeoutMs) + " ms" +
										 (d->m->error.empty() ? "" : ", " + d->m->error);
		return nullptr;
	}

	std::string LiveDown::Code()
	{
		std::lock_guard lock(m->mtx);
		return m->code;
	}

	std::string LiveDown::Newest(const std::string& api, const std::vector<std::string>& skip, int timeoutMs)
	{
		std::string list;
		for (const std::string& code : skip)
			list += (list.empty() ? "" : ",") + code;
		LiveHttp http;
		if (http.http)
			http.http->SetTimeout(std::max(timeoutMs, 1) / 1000.0f);
		std::vector<uint8_t> body;
		std::map<uint32_t, uint64_t> nums;
		std::map<uint32_t, std::string_view> bytes;
		if (http.Call(LivePullUrl(api, "&header=1&frame=0&skip=" + list), nullptr, body) != HTTPDownloader::HTTP_STATUS_OK ||
			!LiveFields(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), nums, bytes))
			return {};
		return std::string(bytes[1]);
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
