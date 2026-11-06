// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Live spectating over the lobby HTTP API (Lobby.h LiveUp / LiveDown): the uplink streams one
// POST /lbs/live_stream body for the whole battle, each spectator reads one GET of the same
// bytes from their start. Stream format: docs/zdxsv/replay.md "Live stream".

#include "Zdxsv/Lobby.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace Zdxsv
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto LIVE_TICK = std::chrono::milliseconds(333);
		constexpr int LIVE_HTTP_TIMEOUT_MS = 10000; // the uplink sends at least every LIVE_TICK
		constexpr int LIVE_SLEEP_STEP_MS = 10;
		constexpr size_t LIVE_MAX_RECORD = 1 << 20;
		constexpr std::string_view LIVE_SCHEME = "live:";
		constexpr std::string_view LIVE_MAGIC = "ZLV1";

		enum : uint8_t
		{
			REC_KEEPALIVE = 0,
			REC_HEADER = 1,
			REC_INPUTS = 2,
			REC_ROUND = 3,
			REC_END = 4,
		};

		void PutU32(std::string& out, uint32_t v)
		{
			for (int i = 0; i < 4; i++)
				out.push_back(static_cast<char>(v >> (8 * i)));
		}

		uint32_t GetU32(const char* p)
		{
			uint32_t v = 0;
			for (int i = 0; i < 4; i++)
				v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
			return v;
		}

		void PutRecord(std::string& out, uint8_t type, const void* data, size_t len)
		{
			out.push_back(static_cast<char>(type));
			PutU32(out, static_cast<uint32_t>(len));
			out.append(static_cast<const char*>(data), len);
		}

		std::string StreamUrl(const std::string& api, const std::string& query)
		{
			return api + "/lbs/live_stream?" + query;
		}

		using NextChunk = std::function<bool(std::string& out)>; // false: the body ends
		using OnHeaders = std::function<bool(int status, const std::string& battleCode)>; // false: stop
		using OnData = std::function<bool(const char* p, size_t n)>; // false: stop

#ifdef _WIN32
		struct WinHttp
		{
			HINTERNET session = nullptr, connect = nullptr, request = nullptr;
			~WinHttp()
			{
				for (HINTERNET h : {request, connect, session})
					if (h)
						WinHttpCloseHandle(h);
			}
			bool Open(const std::string& url, const wchar_t* method, int timeoutMs, std::string& error)
			{
				const std::wstring wurl = StringUtil::UTF8StringToWideString(url);
				URL_COMPONENTS uc = {};
				uc.dwStructSize = sizeof(uc);
				uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = static_cast<DWORD>(-1);
				if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
				{
					error = "bad URL " + url;
					return false;
				}
				const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
				const std::wstring path = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) + std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
				session = WinHttpOpen(L"PCSX2-zdxsv", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
				if (session)
				{
					WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
					connect = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
				}
				if (connect)
					request = WinHttpOpenRequest(connect, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
						uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
				if (!request)
					error = "WinHttp open failed: " + std::to_string(GetLastError());
				return request != nullptr;
			}
			int Status()
			{
				DWORD status = 0, size = sizeof(status);
				if (!WinHttpReceiveResponse(request, nullptr) ||
					!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
						&size, WINHTTP_NO_HEADER_INDEX))
					return -1;
				return static_cast<int>(status);
			}
		};

		int HttpPost(const std::string& url, const NextChunk& next, std::string& error)
		{
			WinHttp h;
			if (!h.Open(url, L"POST", LIVE_HTTP_TIMEOUT_MS, error))
				return -1;
			if (!WinHttpSendRequest(h.request, L"Transfer-Encoding: chunked\r\nContent-Type: application/octet-stream\r\n", static_cast<DWORD>(-1),
					WINHTTP_NO_REQUEST_DATA, 0, WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH, 0))
			{
				error = "send failed: " + std::to_string(GetLastError());
				return -1;
			}
			std::string chunk, wire;
			bool more = true;
			while (more)
			{
				chunk.clear();
				more = next(chunk);
				wire.clear();
				if (!chunk.empty())
					wire = fmt::format("{:x}\r\n", chunk.size()) + chunk + "\r\n";
				if (!more)
					wire += "0\r\n\r\n";
				DWORD written = 0;
				if (!wire.empty() && !WinHttpWriteData(h.request, wire.data(), static_cast<DWORD>(wire.size()), &written))
				{
					error = "write failed: " + std::to_string(GetLastError());
					return -1;
				}
			}
			const int status = h.Status();
			if (status < 0)
				error = "no response: " + std::to_string(GetLastError());
			return status;
		}

		int HttpGet(const std::string& url, int timeoutMs, const OnHeaders& headers, const OnData& data, std::string& error)
		{
			WinHttp h;
			if (!h.Open(url, L"GET", timeoutMs, error))
				return -1;
			if (!WinHttpSendRequest(h.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
			{
				error = "send failed: " + std::to_string(GetLastError());
				return -1;
			}
			const int status = h.Status();
			if (status < 0)
			{
				error = "no response: " + std::to_string(GetLastError());
				return status;
			}
			wchar_t code[128] = {};
			DWORD size = sizeof(code) - sizeof(wchar_t);
			if (!WinHttpQueryHeaders(h.request, WINHTTP_QUERY_CUSTOM, L"X-Battle-Code", code, &size, WINHTTP_NO_HEADER_INDEX))
				code[0] = 0;
			if (!headers(status, StringUtil::WideStringToUTF8String(code)) || status != 200)
				return status;
			char buf[16384];
			for (;;)
			{
				DWORD avail = 0, got = 0;
				if (!WinHttpQueryDataAvailable(h.request, &avail))
				{
					error = "read failed: " + std::to_string(GetLastError());
					break;
				}
				if (avail == 0)
					break;
				if (!WinHttpReadData(h.request, buf, std::min<DWORD>(avail, sizeof(buf)), &got) || got == 0)
				{
					error = "read failed: " + std::to_string(GetLastError());
					break;
				}
				if (!data(buf, got))
					break;
			}
			return status;
		}
#else
		struct Curl
		{
			CURL* c = curl_easy_init();
			curl_slist* headers = nullptr;
			~Curl()
			{
				if (headers)
					curl_slist_free_all(headers);
				if (c)
					curl_easy_cleanup(c);
			}
			void Common(const std::string& url, int timeoutMs)
			{
				curl_easy_setopt(c, CURLOPT_URL, url.c_str());
				curl_easy_setopt(c, CURLOPT_USERAGENT, "PCSX2-zdxsv");
				curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
				curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeoutMs));
				curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
				curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, static_cast<long>(std::max(timeoutMs / 1000, 1)));
			}
			int Status(CURLcode rc, std::string& error)
			{
				long status = 0;
				curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
				if (rc != CURLE_OK)
					error = curl_easy_strerror(rc);
				return status > 0 ? static_cast<int>(status) : -1;
			}
		};

		int HttpPost(const std::string& url, const NextChunk& next, std::string& error)
		{
			Curl h;
			if (!h.c)
			{
				error = "curl_easy_init failed";
				return -1;
			}
			struct Src
			{
				const NextChunk* next;
				std::string pending;
				size_t pos = 0;
				bool more = true;
			} src{&next};
			h.Common(url, LIVE_HTTP_TIMEOUT_MS);
			h.headers = curl_slist_append(h.headers, "Transfer-Encoding: chunked");
			h.headers = curl_slist_append(h.headers, "Content-Type: application/octet-stream");
			curl_easy_setopt(h.c, CURLOPT_HTTPHEADER, h.headers);
			curl_easy_setopt(h.c, CURLOPT_POST, 1L);
			curl_easy_setopt(h.c, CURLOPT_READDATA, &src);
			curl_easy_setopt(h.c, CURLOPT_READFUNCTION, +[](char* buf, size_t size, size_t n, void* p) -> size_t {
				Src& s = *static_cast<Src*>(p);
				while (s.pos == s.pending.size())
				{
					if (!s.more)
						return 0;
					s.pending.clear();
					s.pos = 0;
					s.more = (*s.next)(s.pending);
				}
				const size_t len = std::min(size * n, s.pending.size() - s.pos);
				std::memcpy(buf, s.pending.data() + s.pos, len);
				s.pos += len;
				return len;
			});
			return h.Status(curl_easy_perform(h.c), error);
		}

		int HttpGet(const std::string& url, int timeoutMs, const OnHeaders& headers, const OnData& data, std::string& error)
		{
			Curl h;
			if (!h.c)
			{
				error = "curl_easy_init failed";
				return -1;
			}
			struct Sink
			{
				CURL* c;
				const OnHeaders* headers;
				const OnData* data;
				std::string code;
				bool started = false, ok = false;
			} sink{h.c, &headers, &data};
			h.Common(url, timeoutMs);
			curl_easy_setopt(h.c, CURLOPT_HEADERDATA, &sink);
			curl_easy_setopt(h.c, CURLOPT_HEADERFUNCTION, +[](char* buf, size_t size, size_t n, void* p) -> size_t {
				Sink& s = *static_cast<Sink*>(p);
				const std::string_view line(buf, size * n);
				constexpr std::string_view name = "x-battle-code:";
				if (line.size() > name.size() && StringUtil::Strncasecmp(line.data(), name.data(), name.size()) == 0)
					s.code = std::string(StringUtil::StripWhitespace(line.substr(name.size())));
				return size * n;
			});
			curl_easy_setopt(h.c, CURLOPT_WRITEDATA, &sink);
			curl_easy_setopt(h.c, CURLOPT_WRITEFUNCTION, +[](char* buf, size_t size, size_t n, void* p) -> size_t {
				Sink& s = *static_cast<Sink*>(p);
				if (!s.started)
				{
					s.started = true;
					long status = 0;
					curl_easy_getinfo(s.c, CURLINFO_RESPONSE_CODE, &status);
					s.ok = (*s.headers)(static_cast<int>(status), s.code) && status == 200;
				}
				return s.ok && (*s.data)(buf, size * n) ? size * n : 0;
			});
			const CURLcode rc = curl_easy_perform(h.c);
			if (!sink.started)
			{
				long status = 0;
				curl_easy_getinfo(h.c, CURLINFO_RESPONSE_CODE, &status);
				if (status > 0)
					headers(static_cast<int>(status), sink.code);
			}
			return h.Status(rc == CURLE_WRITE_ERROR && sink.started ? CURLE_OK : rc, error);
		}
#endif

		template <typename T>
		void SleepUntil(Clock::time_point t, const T& quit)
		{
			while (!quit && Clock::now() < t)
				std::this_thread::sleep_for(std::chrono::milliseconds(LIVE_SLEEP_STEP_MS));
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

	// The thread owns a reference: the LiveUp may go before the upload ends.
	struct LiveUp::Impl
	{
		struct State
		{
			std::string url, code;
			std::vector<uint8_t> header;
			int frameBytes = 0;
			std::mutex mtx;
			struct Item
			{
				bool round = false;
				std::vector<uint8_t> data;
			};
			std::vector<Item> queue;
			size_t frames = 0;
			bool closing = false;
			std::string reason;
			std::atomic<bool> quit{false}, done{false};

			void Run()
			{
				bool started = false, ended = false;
				auto tick = Clock::now();
				size_t sent = 0;
				const NextChunk next = [&](std::string& out) {
					if (!started)
					{
						started = true;
						out.append(LIVE_MAGIC);
						PutRecord(out, REC_HEADER, header.data(), header.size());
						return true;
					}
					if (ended || quit)
						return false;
					tick += LIVE_TICK;
					SleepUntil(tick, quit);
					std::vector<Item> items;
					bool close;
					std::string why;
					{
						std::lock_guard lock(mtx);
						items.swap(queue);
						close = closing;
						why = reason;
					}
					for (const Item& it : items)
					{
						if (it.round)
						{
							PutRecord(out, REC_ROUND, it.data.data(), it.data.size());
							continue;
						}
						std::string rec;
						PutU32(rec, static_cast<uint32_t>(frameBytes));
						rec.append(reinterpret_cast<const char*>(it.data.data()), it.data.size());
						PutRecord(out, REC_INPUTS, rec.data(), rec.size());
						sent += it.data.size() / frameBytes;
					}
					if (close)
					{
						PutRecord(out, REC_END, why.data(), why.size());
						ended = true;
					}
					if (out.empty())
						PutRecord(out, REC_KEEPALIVE, nullptr, 0);
					return true;
				};
				std::string error;
				const int status = HttpPost(url, next, error);
				Console.WriteLn("Zdxsv live: upload %s ended, %zu frames%s, HTTP %d%s", code.c_str(), sent, ended ? " and the close" : "", status,
					error.empty() ? "" : (" (" + error + ")").c_str());
				done = true;
			}
		};
		std::shared_ptr<State> s = std::make_shared<State>();
	};

	LiveUp::LiveUp(const std::string& api, std::string code, uint32_t session, std::vector<uint8_t> header, int frameBytes)
		: m(std::make_unique<Impl>())
	{
		Impl::State& s = *m->s;
		s.url = StreamUrl(api, "battle_code=" + code + "&session_id=" + std::to_string(static_cast<int32_t>(session)));
		s.code = std::move(code);
		s.header = std::move(header);
		s.frameBytes = frameBytes;
		std::thread([s = m->s] { s->Run(); }).detach();
	}

	LiveUp::~LiveUp()
	{
		m->s->quit = true;
	}

	void LiveUp::AddFrames(const void* data, size_t frames)
	{
		Impl::State& s = *m->s;
		const auto* p = static_cast<const uint8_t*>(data);
		std::lock_guard lock(s.mtx);
		if (s.closing)
			return;
		if (s.queue.empty() || s.queue.back().round)
			s.queue.emplace_back();
		s.queue.back().data.insert(s.queue.back().data.end(), p, p + frames * s.frameBytes);
		s.frames += frames;
	}

	void LiveUp::AddRound(const std::vector<uint8_t>& entry)
	{
		Impl::State& s = *m->s;
		std::lock_guard lock(s.mtx);
		if (!s.closing)
			s.queue.push_back({true, entry});
	}

	size_t LiveUp::Frames()
	{
		std::lock_guard lock(m->s->mtx);
		return m->s->frames;
	}

	void LiveUp::Close(const std::string& reason)
	{
		std::lock_guard lock(m->s->mtx);
		if (!m->s->closing)
		{
			m->s->closing = true;
			m->s->reason = reason;
		}
	}

	bool LiveUp::Done()
	{
		return m->s->done;
	}

	struct LiveDown::Impl
	{
		struct State
		{
			std::string api, code;
			std::mutex mtx;
			LiveStreams got; // inputs not taken yet
			bool headerTaken = false;
			size_t frames = 0;
			Clock::time_point rx = Clock::now();
			std::string error;
			std::string buf; // bytes not parsed yet
			bool magic = false;
			std::atomic<bool> quit{false};

			// false: a malformed stream.
			bool Parse()
			{
				if (!magic)
				{
					if (buf.size() < LIVE_MAGIC.size())
						return true;
					if (std::string_view(buf).substr(0, LIVE_MAGIC.size()) != LIVE_MAGIC)
						return false;
					magic = true;
					buf.erase(0, LIVE_MAGIC.size());
				}
				size_t pos = 0;
				while (buf.size() - pos >= 5)
				{
					const uint8_t type = static_cast<uint8_t>(buf[pos]);
					const uint32_t len = GetU32(buf.data() + pos + 1);
					if (len > LIVE_MAX_RECORD)
						return false;
					if (buf.size() - pos - 5 < len)
						break;
					const char* p = buf.data() + pos + 5;
					const auto* u = reinterpret_cast<const uint8_t*>(p);
					switch (type)
					{
						case REC_HEADER:
							got.header.assign(u, u + len);
							break;
						case REC_INPUTS:
						{
							if (len < 4)
								return false;
							const int fb = static_cast<int>(GetU32(p));
							if (fb <= 0 || (len - 4) % fb != 0 || (got.frameBytes != 0 && got.frameBytes != fb))
								return false;
							got.frameBytes = fb;
							got.inputs.insert(got.inputs.end(), u + 4, u + len);
							frames += (len - 4) / fb;
							break;
						}
						case REC_ROUND:
							got.rounds.insert(got.rounds.end(), u, u + len);
							break;
						case REC_END:
							got.closed = true;
							got.close.assign(p, len);
							break;
						default:
							break;
					}
					pos += 5 + len;
				}
				buf.erase(0, pos);
				return true;
			}

			void Run()
			{
				std::string err;
				const std::string url = StreamUrl(api, code.empty() ? std::string() : "battle_code=" + code);
				const int status = HttpGet(
					url, LIVE_HTTP_TIMEOUT_MS,
					[&](int, const std::string& c) {
						std::lock_guard lock(mtx);
						if (code.empty())
							code = c;
						return !quit;
					},
					[&](const char* p, size_t n) {
						std::lock_guard lock(mtx);
						rx = Clock::now();
						buf.append(p, n);
						if (!Parse())
						{
							err = "malformed stream";
							return false;
						}
						return !quit && !got.closed;
					},
					err);
				std::lock_guard lock(mtx);
				error = status == 200 ? err : "HTTP " + std::to_string(status) + (err.empty() ? "" : ", " + err);
				if (!got.closed)
				{
					got.closed = true;
					got.close = "stream lost" + (error.empty() ? "" : " (" + error + ")");
				}
			}
		};
		std::shared_ptr<State> s = std::make_shared<State>();
	};

	LiveDown::LiveDown()
		: m(std::make_unique<Impl>())
	{
	}

	LiveDown::~LiveDown()
	{
		m->s->quit = true;
	}

	std::unique_ptr<LiveDown> LiveDown::Open(const std::string& url, int timeoutMs, std::string& error)
	{
		std::unique_ptr<LiveDown> d(new LiveDown());
		const std::shared_ptr<Impl::State> s = d->m->s;
		if (!LiveSourceSplit(url, s->api, s->code) || s->api.empty())
		{
			error = "not a live source (live:<lobby API URL>[#battle code])";
			return nullptr;
		}
		std::thread([s] { s->Run(); }).detach();
		const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
		while (Clock::now() < until)
		{
			{
				std::lock_guard lock(s->mtx);
				if (!s->got.header.empty() && (s->frames > 0 || s->got.closed))
					return d;
				if (s->got.closed)
					break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(LIVE_SLEEP_STEP_MS));
		}
		std::lock_guard lock(s->mtx);
		error = s->code.empty() ? "no live battle there" + (s->error.empty() ? "" : " (" + s->error + ")") :
								  "battle " + s->code + ": header " + std::to_string(s->got.header.size()) + " bytes, " +
									  std::to_string(s->frames) + " frames after " + std::to_string(timeoutMs) + " ms" +
									  (s->error.empty() ? "" : ", " + s->error);
		return nullptr;
	}

	std::string LiveDown::Code()
	{
		std::lock_guard lock(m->s->mtx);
		return m->s->code;
	}

	std::string LiveDown::Newest(const std::string& api, const std::vector<std::string>& skip, int timeoutMs)
	{
		std::string list;
		for (const std::string& code : skip)
			list += (list.empty() ? "" : ",") + code;
		std::string code, error;
		HttpGet(
			StreamUrl(api, "skip=" + list), std::max(timeoutMs, 1),
			[&](int status, const std::string& c) {
				if (status == 200)
					code = c;
				return false;
			},
			[](const char*, size_t) { return false; }, error);
		return code;
	}

	bool LiveDown::Take(LiveStreams& s, int stallMs)
	{
		Impl::State& st = *m->s;
		std::lock_guard lock(st.mtx);
		LiveStreams& g = st.got;
		if (!st.headerTaken && !g.header.empty())
		{
			st.headerTaken = true;
			s.header = std::move(g.header);
		}
		s.inputs.insert(s.inputs.end(), g.inputs.begin(), g.inputs.end());
		g.inputs.clear();
		s.rounds = g.rounds;
		s.frameBytes = g.frameBytes;
		s.closed = g.closed;
		s.close = g.close;
		return g.closed || Clock::now() - st.rx < std::chrono::milliseconds(stallMs);
	}
} // namespace Zdxsv
