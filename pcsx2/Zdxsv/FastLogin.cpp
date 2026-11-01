// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/Settings.h"

#include "Memory.h"

#include "common/Console.h"
#include "common/HTTPDownloader.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace Zdxsv
{
	namespace
	{
		// Guest addresses of SLPS-25419 (g_z_game checks serial and CRC).
		constexpr u32 kGp = 0x6dd770;
		constexpr u32 kNetState = 0xcd1360; // network menu machine: 0x21 = DNAS, 0x27 = login browser
		constexpr u32 kNetSub = 0xcd1361;
		constexpr u32 kLobbyPhase = kGp - 0x59ec; // 2 = lobby connect, 0x17 = login browser
		constexpr u32 kLobbySub = kGp - 0x59f0;
		constexpr u32 kLobbyStep = kGp - 0x59f4; // in phase 0x17: 4 = browser closing, display restored
		constexpr u32 kLbsCount = kGp - 0x5cac;
		constexpr u32 kNetFileLoginKey = 0xf43960; // filled by the NET file load
		// The browser's result area: zeroed, then only the fields the lobby connect reads.
		constexpr u32 kResultBegin = 0x900900;
		constexpr u32 kResultEnd = 0x904000;
		constexpr u32 kResultLbsId = 0x900920;
		constexpr u32 kResultLoginKey = 0x901440;
		constexpr u32 kResultSessionId = 0x901451;
		constexpr u32 kResultLobbyAddr = 0x90166f;
		constexpr u32 kResultLbsList = 0x901a43; // "<lbs id>,<name>": the lobby address is looked up by this id
		constexpr u32 kResultStatus = 0x903aac; // "999" = login accepted
		constexpr u32 kSelectedLbsId = 0xc63560;
		constexpr size_t kLoginKeyLen = 10;
		constexpr size_t kLbsIdLen = 12;

		constexpr const char* kDefaultUrl = "https://lbs.zdxsv.net:8443/lbs/login";

		struct LoginResult
		{
			std::string login_key, session_id, lobby_addr, lbs_info;
		};

		struct FastLogin
		{
			enum class Step
			{
				Idle,
				Pending,
				Done,
			};
			Step step = Step::Idle;
			std::mutex mutex;
			bool answered = false;
			std::optional<LoginResult> result;
			std::shared_ptr<std::atomic<bool>> cancel;
		};
		FastLogin s_fast;

		bool Enabled()
		{
			return BoolSetting("ZdxsvFastLogin", true, "ZDXSV_FAST_LOGIN");
		}

		std::string Url()
		{
			if (const char* env = std::getenv("ZDXSV_LOGIN_API_URL"))
				return env;
			return Host::GetStringSettingValue("DEV9/Eth", "ZdxsvLoginApiUrl", kDefaultUrl);
		}

		// A flat JSON object of strings, as the login API writes it; anything else (escapes included) = no value.
		std::optional<std::string> JsonString(const std::string& body, const char* key)
		{
			const std::string k = std::string("\"") + key + "\":\"";
			const size_t at = body.find(k);
			if (at == std::string::npos)
				return std::nullopt;
			const size_t begin = at + k.size();
			const size_t end = body.find('"', begin);
			if (end == std::string::npos)
				return std::nullopt;
			std::string v = body.substr(begin, end - begin);
			if (v.find('\\') != std::string::npos)
				return std::nullopt;
			return v;
		}

		bool Printable(const std::string& s, size_t max_len)
		{
			if (s.empty() || s.size() > max_len)
				return false;
			for (unsigned char c : s)
				if (c < 0x20 || c > 0x7e)
					return false;
			return true;
		}

		std::optional<LoginResult> Parse(const std::string& body)
		{
			LoginResult r;
			std::optional<std::string> v;
			if (!(v = JsonString(body, "login_key")) || !Printable(*v, 16))
				return std::nullopt;
			r.login_key = *v;
			if (!(v = JsonString(body, "session_id")) || !Printable(*v, 10))
				return std::nullopt;
			r.session_id = *v;
			if (!(v = JsonString(body, "lobby_addr")) || !Printable(*v, 21))
				return std::nullopt;
			r.lobby_addr = *v;
			if (!(v = JsonString(body, "lbs_info")) || !Printable(*v, 64) || v->find(',') != kLbsIdLen)
				return std::nullopt;
			r.lbs_info = *v;
			return r;
		}

		void Request(std::string url, std::string key, std::shared_ptr<std::atomic<bool>> cancel)
		{
			std::optional<LoginResult> result;
			if (std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Host::GetHTTPUserAgent()))
			{
				http->SetTimeout(5.0f);
				http->CreatePostRequest(url, "login_key=" + key,
					[&result](s32 code, const std::string&, HTTPDownloader::Request::Data data) {
						if (code == HTTPDownloader::HTTP_STATUS_OK)
							result = Parse(std::string(data.begin(), data.end()));
					},
					nullptr, "application/x-www-form-urlencoded");
				while (http->HasAnyRequests() && !cancel->load())
				{
					http->PollRequests();
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
			}
			if (cancel->load())
				return;
			std::lock_guard lock(s_fast.mutex);
			if (s_fast.cancel != cancel)
				return;
			s_fast.result = std::move(result);
			s_fast.answered = true;
		}

		void Put(u32 addr, const std::string& s)
		{
			std::memcpy(&eeMem->Main[addr], s.data(), s.size());
		}

		void Apply(const LoginResult& r)
		{
			std::memset(&eeMem->Main[kResultBegin], 0, kResultEnd - kResultBegin);
			const std::string lbs_id = r.lbs_info.substr(0, kLbsIdLen);
			Put(kResultLbsId, lbs_id);
			Put(kResultLoginKey, r.login_key);
			Put(kResultSessionId, r.session_id);
			Put(kResultLobbyAddr, r.lobby_addr);
			Put(kResultLbsList, r.lbs_info);
			Put(kResultStatus, "999");
			std::memset(&eeMem->Main[kSelectedLbsId], 0, 16);
			Put(kSelectedLbsId, lbs_id);
			eeMem->Main[kLbsCount] = 1;
			eeMem->Main[kNetState] = 0x27;
			eeMem->Main[kNetSub] = 0;
			eeMem->Main[kLobbyStep] = 4;
			eeMem->Main[kLobbySub] = 0;
			eeMem->Main[kLobbyPhase] = 0x17;
		}
	} // namespace

	void FastLoginOnVsync()
	{
		if (s_fast.step == FastLogin::Step::Done)
			return;
		if (s_fast.step == FastLogin::Step::Idle)
		{
			if (eeMem->Main[kNetState] != 0x21 || eeMem->Main[kLobbyPhase] != 2)
				return;
			s_fast.step = FastLogin::Step::Done;
			if (!Enabled())
				return;
			const char* key = reinterpret_cast<const char*>(&eeMem->Main[kNetFileLoginKey]);
			const std::string login_key(key, strnlen(key, kLoginKeyLen + 1));
			if (login_key.size() != kLoginKeyLen || !Printable(login_key, kLoginKeyLen))
			{
				Console.WriteLn("Zdxsv: fast login: no login key in the NET file");
				return;
			}
			std::lock_guard lock(s_fast.mutex);
			s_fast.answered = false;
			s_fast.result.reset();
			s_fast.cancel = std::make_shared<std::atomic<bool>>(false);
			std::thread(Request, Url(), login_key, s_fast.cancel).detach();
			s_fast.step = FastLogin::Step::Pending;
			return;
		}
		std::lock_guard lock(s_fast.mutex);
		if (!s_fast.answered)
			return;
		s_fast.step = FastLogin::Step::Done;
		// The game left DNAS meanwhile (failed or done): too late to jump.
		if (!s_fast.result || eeMem->Main[kNetState] != 0x21 || eeMem->Main[kLobbyPhase] != 2)
		{
			Console.WriteLn("Zdxsv: fast login: %s, normal login", s_fast.result ? "DNAS already over" : "no answer from the login API");
			return;
		}
		Apply(*s_fast.result);
		Console.WriteLn("Zdxsv: fast login: session %s, lobby %s", s_fast.result->session_id.c_str(), s_fast.result->lobby_addr.c_str());
	}

	void FastLoginReset()
	{
		std::lock_guard lock(s_fast.mutex);
		if (s_fast.cancel)
			s_fast.cancel->store(true);
		s_fast.cancel.reset();
		s_fast.answered = false;
		s_fast.result.reset();
		s_fast.step = FastLogin::Step::Idle;
	}
} // namespace Zdxsv
