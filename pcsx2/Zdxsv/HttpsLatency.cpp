// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// HTTPS latency to the cloud regions, as flycast's gcp_ping_test (gdxsv_https_latency_*): per region, a warm-up
// HEAD and then 3 HEADs on the same connection; the fastest is the region's latency. The lobby gets them as
// "<region>=<ms>" platform info lines (gdxsv's keys: its lobby picks the battle region from them).

#include "Zdxsv/Dev9Hooks.h"
#include "Zdxsv/TestOptions.h"

#include "Host.h"
#include "IconsFontAwesome.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <curl/curl.h>
#endif

namespace Zdxsv
{
	namespace
	{
		struct Region
		{
			std::string name; // platform info key
			std::string host;
			std::string label; // log and OSD
		};

		// flycast's list (powered by https://github.com/GoogleCloudPlatform/gcping)
		const char* const kPath = "/api/ping";
		const Region kRegions[] = {
			{"australia-southeast1", "australia-southeast1-5tkroniexa-ts.a.run.app", "Australia"},
			{"europe-west2", "europe-west2-5tkroniexa-nw.a.run.app", "London"},
			{"northamerica-northeast1", "northamerica-northeast1-5tkroniexa-nn.a.run.app", "Canada"},
			{"southamerica-east1", "southamerica-east1-5tkroniexa-rj.a.run.app", "Brazil"},
			{"us-central1", "us-central1-5tkroniexa-uc.a.run.app", "US Central - Iowa"},
			{"us-east4", "us-east4-5tkroniexa-uk.a.run.app", "US East - Virginia"},
			{"us-west2", "us-west2-5tkroniexa-wl.a.run.app", "US West - California"},
			{"asia-southeast1", "asia-southeast1-5tkroniexa-as.a.run.app", "Singapore"},
			{"asia-east1", "asia-east1-5tkroniexa-de.a.run.app", "Taiwan"},
			{"asia-east2", "asia-east2-5tkroniexa-df.a.run.app", "Hong Kong"},
			{"asia-northeast1", "asia-northeast1-5tkroniexa-an.a.run.app", "Tokyo"},
			{"asia-northeast2", "asia-northeast2-5tkroniexa-dt.a.run.app", "Osaka"},
			{"asia-northeast3", "asia-northeast3-5tkroniexa-du.a.run.app", "Seoul"},
		};
		constexpr int kAttempts = 3;
		constexpr int kWorkers = 6;

		struct Result
		{
			int minMs = -1;
			std::vector<int> attemptsMs;
			std::string error;
		};

#ifdef _WIN32
		class Connection
		{
		public:
			Connection(const std::string& host)
			{
				m_session = WinHttpOpen(L"PCSX2-zdxsv", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
				if (!m_session)
					return;
				WinHttpSetTimeouts(m_session, 5000, 5000, 10000, 10000);
				m_connect = WinHttpConnect(m_session, StringUtil::UTF8StringToWideString(host).c_str(),
					INTERNET_DEFAULT_HTTPS_PORT, 0);
			}
			~Connection()
			{
				if (m_connect)
					WinHttpCloseHandle(m_connect);
				if (m_session)
					WinHttpCloseHandle(m_session);
			}

			// HTTP status, or 0 with error set
			int Head(const std::string& path, std::string& error)
			{
				if (!m_connect)
				{
					error = "WinHttpConnect failed: " + std::to_string(GetLastError());
					return 0;
				}
				HINTERNET request = WinHttpOpenRequest(m_connect, L"HEAD", StringUtil::UTF8StringToWideString(path).c_str(),
					nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
				if (!request)
				{
					error = "WinHttpOpenRequest failed: " + std::to_string(GetLastError());
					return 0;
				}
				DWORD status = 0;
				DWORD size = sizeof(status);
				if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
					!WinHttpReceiveResponse(request, nullptr) ||
					!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
						WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
				{
					error = "WinHTTP error " + std::to_string(GetLastError());
					status = 0;
				}
				WinHttpCloseHandle(request);
				return static_cast<int>(status);
			}

		private:
			HINTERNET m_session = nullptr;
			HINTERNET m_connect = nullptr;
		};
#else
		class Connection
		{
		public:
			Connection(const std::string& host)
				: m_host(host)
			{
				m_curl = curl_easy_init();
			}
			~Connection()
			{
				if (m_curl)
					curl_easy_cleanup(m_curl);
			}

			int Head(const std::string& path, std::string& error)
			{
				if (!m_curl)
				{
					error = "curl_easy_init failed";
					return 0;
				}
				const std::string url = "https://" + m_host + path;
				curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
				curl_easy_setopt(m_curl, CURLOPT_USERAGENT, "PCSX2-zdxsv");
				curl_easy_setopt(m_curl, CURLOPT_NOBODY, 1L);
				curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
				curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
				curl_easy_setopt(m_curl, CURLOPT_TIMEOUT_MS, 10000L);
				const CURLcode code = curl_easy_perform(m_curl);
				long status = 0;
				if (code != CURLE_OK)
				{
					error = curl_easy_strerror(code);
					return 0;
				}
				curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &status);
				return static_cast<int>(status);
			}

		private:
			std::string m_host;
			CURL* m_curl = nullptr;
		};
#endif

		// The warm-up opens the connection (TCP + TLS), so the timed HEADs measure one round trip each.
		Result Measure(const std::string& host)
		{
			Result r;
			Connection c(host);
			int status = c.Head(kPath, r.error);
			if (status < 200 || status >= 300)
			{
				if (r.error.empty())
					r.error = "warm-up status " + std::to_string(status);
				return r;
			}
			for (int i = 0; i < kAttempts; i++)
			{
				const auto t1 = std::chrono::steady_clock::now();
				status = c.Head(kPath, r.error);
				const auto t2 = std::chrono::steady_clock::now();
				if (status < 200 || status >= 300)
				{
					if (r.error.empty())
						r.error = "status " + std::to_string(status);
					r.minMs = -1;
					return r;
				}
				const int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count());
				r.attemptsMs.push_back(ms);
				if (r.minMs < 0 || ms < r.minMs)
					r.minMs = ms;
			}
			return r;
		}

		// ZDXSV_HTTPS_LATENCY=0: off. =name:host,...: these hosts instead of the regions (test).
		std::vector<Region> Regions(bool& off)
		{
			const char* env = TestEnv("ZDXSV_HTTPS_LATENCY");
			off = env && std::string(env) == "0";
			if (!env || off)
				return std::vector<Region>(std::begin(kRegions), std::end(kRegions));
			std::vector<Region> list;
			for (const std::string_view item : StringUtil::SplitString(env, ','))
			{
				const size_t colon = item.find(':');
				if (colon == std::string_view::npos)
					continue;
				const std::string name(item.substr(0, colon));
				list.push_back({name, std::string(item.substr(colon + 1)), name});
			}
			return list;
		}

		struct Run
		{
			std::vector<std::thread> workers;
			~Run()
			{
				for (std::thread& t : workers)
					t.join();
			}
		};

		// Declared before s_run: the workers use them until it joins them at exit.
		std::mutex s_mutex;
		bool s_started = false;
		std::vector<Region> s_regions;
		std::string s_lines; // set once every region ended
		std::vector<std::pair<std::string, int>> s_results; // label, ms
		std::atomic<size_t> s_next{0};
		std::atomic<size_t> s_left{0};
		Run s_run;
	} // namespace

	void HttpsLatencyStart()
	{
		std::lock_guard lock(s_mutex);
		if (s_started)
			return;
		s_started = true;
		bool off;
		s_regions = Regions(off);
		if (off || s_regions.empty())
		{
			Console.WriteLn("ZdxsvHttpsLatency: off");
			return;
		}
		s_left = s_regions.size();
		// Joined at exit (Run): a worker still waiting there takes up to the request timeouts.
		for (int i = 0; i < std::min<int>(kWorkers, static_cast<int>(s_regions.size())); i++)
			s_run.workers.emplace_back([] {
				for (size_t k; (k = s_next++) < s_regions.size();)
				{
					const Region& region = s_regions[k];
					const Result r = Measure(region.host);
					std::string attempts;
					for (const int ms : r.attemptsMs)
						attempts += " " + std::to_string(ms);
					if (r.minMs >= 0)
						Console.WriteLn("ZdxsvHttpsLatency: %s %s: %d ms (attempts%s)", region.name.c_str(), region.label.c_str(),
							r.minMs, attempts.c_str());
					else
						Console.Warning("ZdxsvHttpsLatency: %s %s: failed: %s", region.name.c_str(), region.label.c_str(),
							r.error.c_str());

					std::lock_guard lock(s_mutex);
					if (r.minMs >= 0)
					{
						s_results.emplace_back(region.label, r.minMs);
						s_lines += region.name + "=" + std::to_string(r.minMs) + "\n";
					}
					if (--s_left > 0)
						continue;
					std::string summary = "no region answered";
					const auto best = std::min_element(s_results.begin(), s_results.end(),
						[](const auto& a, const auto& b) { return a.second < b.second; });
					if (best != s_results.end())
						summary = best->first + " " + std::to_string(best->second) + " ms (best of " +
								  std::to_string(s_results.size()) + ")";
					Console.WriteLn("ZdxsvHttpsLatency: done: %s", summary.c_str());
					Host::AddIconOSDMessage("ZdxsvHttpsLatency", ICON_FA_NETWORK_WIRED, "Server latency: " + summary, 10.0f);
				}
			});
	}

	std::string HttpsLatencyLines()
	{
		std::lock_guard lock(s_mutex);
		return s_left == 0 ? s_lines : std::string();
	}
} // namespace Zdxsv
