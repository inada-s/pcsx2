// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/AuthSignIn.h"
#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/FastLogin.h"
#include "Zdxsv/Settings.h"
#include "Zdxsv/TestOptions.h"

#include "Config.h"
#include "Memory.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/HTTPDownloader.h"
#include "common/Path.h"

#include "fmt/format.h"
#include "rapidjson/document.h"

#include <algorithm>
#include <atomic>
#include <cctype>
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
		constexpr size_t kLbsIdLen = 12;

		struct LoginResult
		{
			std::string login_key, session_id, lobby_addr, lbs_info;
		};

		struct FastLogin
		{
			enum class Step
			{
				Idle,
				SigningIn,
				Pending,
				Done,
			};
			Step step = Step::Idle;
			std::mutex mutex;
			bool sign_in_tried = false;
			bool sign_in_ended = false;
			bool sign_in_paused = false;
			bool holding = false;
			bool hold_paused = false;
			bool answered = false;
			bool applied = false;
			std::optional<LoginResult> result;
			std::shared_ptr<std::atomic<bool>> cancel;
		};
		FastLogin s_fast;

		bool Enabled()
		{
			return BoolSetting("ZdxsvFastLogin", true, "ZDXSV_FAST_LOGIN");
		}

		bool SignInAtDnas()
		{
			const char* env = TestEnv("ZDXSV_AUTH_AT_DNAS");
			return !env || std::strcmp(env, "0") != 0;
		}

		std::string Url()
		{
			if (const char* env = TestEnv("ZDXSV_LOGIN_API_URL"))
				return env;
			return Host::GetStringSettingValue("DEV9/Eth", "ZdxsvLoginApiUrl", ServerApiUrl("/lbs/login").c_str());
		}

		std::optional<std::string> JsonString(const rapidjson::Document& doc, const char* key)
		{
			const auto it = doc.FindMember(key);
			if (it == doc.MemberEnd() || !it->value.IsString())
				return std::nullopt;
			return std::string(it->value.GetString(), it->value.GetStringLength());
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
			rapidjson::Document doc;
			if (doc.Parse(body.data(), body.size()).HasParseError() || !doc.IsObject())
				return std::nullopt;
			LoginResult r;
			std::optional<std::string> v;
			if (!(v = JsonString(doc, "login_key")) || !Printable(*v, 16))
				return std::nullopt;
			r.login_key = *v;
			if (!(v = JsonString(doc, "session_id")) || !Printable(*v, 10))
				return std::nullopt;
			r.session_id = *v;
			if (!(v = JsonString(doc, "lobby_addr")) || !Printable(*v, 21))
				return std::nullopt;
			r.lobby_addr = *v;
			if (!(v = JsonString(doc, "lbs_info")) || !Printable(*v, 64) || v->find(',') != kLbsIdLen)
				return std::nullopt;
			r.lbs_info = *v;
			return r;
		}

		s32 Post(const std::string& url, const std::string& form, std::optional<LoginResult>& result, const std::atomic<bool>& cancel)
		{
			s32 status = HTTPDownloader::HTTP_STATUS_ERROR;
			std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Host::GetHTTPUserAgent());
			if (!http)
				return status;
			http->SetTimeout(5.0f);
			http->CreatePostRequest(url, form,
				[&result, &status](s32 code, const std::string&, HTTPDownloader::Request::Data data) {
					status = code;
					if (code == HTTPDownloader::HTTP_STATUS_OK)
						result = Parse(std::string(data.begin(), data.end()));
				},
				nullptr, "application/x-www-form-urlencoded");
			while (http->HasAnyRequests() && !cancel.load())
			{
				http->PollRequests();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return status;
		}

		// No answer or a server error: hold the game at DNAS (paused) and retry until the server answers
		// or the player resumes. A refusal (4xx) is an answer.
		void Request(std::string url, std::string form, std::shared_ptr<std::atomic<bool>> cancel)
		{
			std::optional<LoginResult> result;
			for (int wait = 2;; wait = std::min(wait * 2, 30))
			{
				result.reset();
				const s32 status = Post(url, form, result, *cancel);
				if (cancel->load())
					return;
				if (status >= 200 && status < 500)
					break;
				{
					std::lock_guard lock(s_fast.mutex);
					if (s_fast.cancel != cancel)
						return;
					if (!s_fast.holding)
					{
						s_fast.holding = true;
						Host::RunOnCPUThread([cancel]() {
							if (cancel->load())
								return;
							VMManager::SetPaused(true);
							std::lock_guard lock(s_fast.mutex);
							if (s_fast.cancel == cancel && s_fast.holding)
								s_fast.hold_paused = true;
						});
					}
				}
				Console.WriteLn("Zdxsv: fast login: no answer from the login API (%d), game held, retry in %d s", status, wait);
				Host::AddIconOSDMessage("ZdxsvLoginHold", ICON_FA_NETWORK_WIRED,
					fmt::format("zdxsv login server not answering, retrying in {} s. Resume the game to log in without it.", wait),
					static_cast<float>(wait + 1));
				for (int i = 0; i < wait * 10 && !cancel->load(); i++)
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				if (cancel->load())
					return;
			}
			std::lock_guard lock(s_fast.mutex);
			if (s_fast.cancel != cancel)
				return;
			s_fast.result = std::move(result);
			s_fast.answered = true;
			if (!s_fast.holding)
				return;
			s_fast.holding = false;
			s_fast.hold_paused = false;
			Console.WriteLn("Zdxsv: fast login: login API answered, game resumed");
			Host::RemoveKeyedOSDMessage("ZdxsvLoginHold");
			Host::RunOnCPUThread([]() {
				if (VMManager::GetState() == VMState::Paused)
					VMManager::SetPaused(false);
			});
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
		void StartSignIn()
		{
			std::lock_guard lock(s_fast.mutex);
			s_fast.sign_in_tried = true;
			s_fast.sign_in_ended = false;
			s_fast.sign_in_paused = false;
			s_fast.cancel = std::make_shared<std::atomic<bool>>(false);
			s_fast.step = FastLogin::Step::SigningIn;
			Console.WriteLn("Zdxsv: fast login: no token, signing in with the VM paused");
			const std::shared_ptr<std::atomic<bool>> cancel = s_fast.cancel;
			Host::RunOnCPUThread([cancel]() {
				if (cancel->load())
					return;
				VMManager::SetPaused(true);
				std::lock_guard lock(s_fast.mutex);
				s_fast.sign_in_paused = true;
			});
			std::thread([cancel]() {
				const std::string error = AuthSignIn("", AuthOpenBrowser, *cancel);
				Console.WriteLn("Zdxsv: fast login: sign-in %s", error.empty() ? "done" : error.c_str());
				{
					std::lock_guard lock(s_fast.mutex);
					if (s_fast.cancel != cancel)
						return;
					s_fast.sign_in_ended = true;
				}
				if (!cancel->load())
					Host::RunOnCPUThread([]() {
						if (VMManager::GetState() == VMState::Paused)
							VMManager::SetPaused(false);
					});
			}).detach();
		}
	} // namespace

	void FastLoginOnVsync()
	{
		const bool at_dnas = eeMem->Main[kNetState] == 0x21 && eeMem->Main[kLobbyPhase] == 2;
		if (s_fast.step == FastLogin::Step::Done)
		{
			// Every DNAS start logs in again: after ネット終了 or a lost lobby the game comes back here.
			if (!at_dnas)
			{
				s_fast.step = FastLogin::Step::Idle;
				s_fast.sign_in_tried = false;
			}
			return;
		}
		if (s_fast.step == FastLogin::Step::SigningIn)
		{
			std::lock_guard lock(s_fast.mutex);
			if (!s_fast.sign_in_ended)
			{
				// Vsyncs still run until the pause takes effect; running after it means the player resumed: give up.
				if (!s_fast.sign_in_paused || VMManager::GetState() != VMState::Running)
					return;
				Console.WriteLn("Zdxsv: fast login: resumed during the sign-in, sign-in cancelled");
				if (s_fast.cancel)
					s_fast.cancel->store(true);
			}
			s_fast.step = FastLogin::Step::Idle;
			return;
		}
		if (s_fast.step == FastLogin::Step::Idle)
		{
			if (!at_dnas)
				return;
			s_fast.step = FastLogin::Step::Done;
			s_fast.applied = false;
			if (!Enabled() || Url().empty())
				return;
			const std::optional<std::string> token = AuthTokenRead();
			if (!token)
			{
				if (!s_fast.sign_in_tried && SignInAtDnas())
					StartSignIn();
				else
					Console.WriteLn("Zdxsv: fast login: no token, normal login");
				return;
			}
			std::string form = "token=" + *token;
			if (const int account = Host::GetIntSettingValue("DEV9/Eth", "ZdxsvAccount", 0); account > 0)
				form += "&account=" + std::to_string(account);
			Console.WriteLn("Zdxsv: fast login: by token");
			std::lock_guard lock(s_fast.mutex);
			s_fast.answered = false;
			s_fast.holding = false;
			s_fast.hold_paused = false;
			s_fast.result.reset();
			s_fast.cancel = std::make_shared<std::atomic<bool>>(false);
			std::thread(Request, Url(), std::move(form), s_fast.cancel).detach();
			s_fast.step = FastLogin::Step::Pending;
			return;
		}
		std::lock_guard lock(s_fast.mutex);
		if (!s_fast.answered)
		{
			if (!s_fast.hold_paused || VMManager::GetState() != VMState::Running)
				return;
			Console.WriteLn("Zdxsv: fast login: resumed while held, normal login");
			Host::RemoveKeyedOSDMessage("ZdxsvLoginHold");
			if (s_fast.cancel)
				s_fast.cancel->store(true);
			s_fast.holding = false;
			s_fast.hold_paused = false;
			s_fast.step = FastLogin::Step::Done;
			return;
		}
		s_fast.step = FastLogin::Step::Done;
		// The game left DNAS meanwhile (failed or done): too late to jump.
		if (!s_fast.result || eeMem->Main[kNetState] != 0x21 || eeMem->Main[kLobbyPhase] != 2)
		{
			Console.WriteLn("Zdxsv: fast login: %s, normal login", s_fast.result ? "DNAS already over" : "no answer from the login API");
			return;
		}
		Apply(*s_fast.result);
		s_fast.applied = true;
		Console.WriteLn("Zdxsv: fast login: session %s, lobby %s", s_fast.result->session_id.c_str(), s_fast.result->lobby_addr.c_str());
	}

	bool FastLoginApplied()
	{
		return s_fast.applied;
	}

	void FastLoginReset()
	{
		std::lock_guard lock(s_fast.mutex);
		if (s_fast.cancel)
			s_fast.cancel->store(true);
		s_fast.cancel.reset();
		s_fast.answered = false;
		s_fast.result.reset();
		s_fast.applied = false;
		s_fast.step = FastLogin::Step::Idle;
		s_fast.sign_in_tried = false;
		s_fast.sign_in_ended = false;
		s_fast.sign_in_paused = false;
		s_fast.holding = false;
		s_fast.hold_paused = false;
	}
} // namespace Zdxsv
