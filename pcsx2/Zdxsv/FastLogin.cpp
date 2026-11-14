// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/AuthSignIn.h"
#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/FastLogin.h"
#include "Zdxsv/ServerHosts.h"
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
#include <cstdio>
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
		// Guest addresses per game (g_z_game / g_aeug_game check serial and CRC).
		struct FastLoginGame
		{
			u32 net_state; // network menu machine: 3 = connection menu, 0x21 = DNAS, 0x27 = login browser; +1 = its sub state
			u32 lobby_phase; // 2 = lobby connect, 0x17 = login browser; -4 = its sub, -8 = its step
			u32 loader_queue; // address of the file loader queue; its first byte is 1 while a load runs
			u32 lbs_count;
			u32 result; // the browser's result area: zeroed, then only the fields the lobby connect reads
			u32 selected_lbs_id;
			const char* code; // the server's game code (/lbs/login form game)
		};
		constexpr FastLoginGame kGvszGame = {0xcd1360, 0x6dd770 - 0x59ec, 0x6dd770 - 0x5dfc, 0x6dd770 - 0x5cac, 0x900900, 0xc63560, "00000020"};
		constexpr FastLoginGame kAeugGame = {0xb898c0, 0x5b8e70 - 0x5aa4, 0x5b8e70 - 0x5e8c, 0x5b8e70 - 0x5df4, 0x6e1570, 0xb1ff70, "00000004"};
		const FastLoginGame& Game() { return g_z_game ? kGvszGame : kAeugGame; }
		u32 NetState() { return Game().net_state; }
		u32 NetSub() { return Game().net_state + 1; }
		u32 LobbyPhase() { return Game().lobby_phase; }
		u32 LobbySub() { return Game().lobby_phase - 4; }
		u32 LobbyStep() { return Game().lobby_phase - 8; } // in phase 0x17: 0 = browser load, 4 = browser closing, display restored
		constexpr u8 kNetConnectMenu = 3;
		constexpr u8 kNetLoginBrowser = 0x27;
		constexpr u8 kPhaseLoginBrowser = 0x17;
		constexpr u8 kPhaseUserSelect = 3; // the user_id selection and the new user registration pages
		constexpr u8 kStepBrowserLoad = 0;
		constexpr u8 kStepParked = 7; // no step: the phase does nothing
		constexpr u32 kParkedMaxFrames = 600;
		// Offsets in the result area; the same in both games.
		constexpr u32 kResultSize = 0x3700;
		constexpr u32 kResultLbsId = 0x20;
		constexpr u32 kResultLoginKey = 0xb40;
		constexpr u32 kResultSessionId = 0xb51;
		constexpr u32 kResultLobbyAddr = 0xd6f;
		constexpr u32 kResultLbsList = 0x1143; // "<lbs id>,<name>": the lobby address is looked up by this id
		constexpr u32 kResultStatus = 0x31ac; // "999" = login accepted
		constexpr size_t kLbsIdLen = 12;

		struct LoginResult
		{
			std::string login_key, session_id, lobby_addr, lbs_info;
			int account = 0; // the account's number among the Discord account's, 0 when not given
		};

		// ZdxsvAccount -1: an account added in the settings and not made yet; the next login makes it.
		constexpr int kNewAccount = -1;

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
			s32 status = 0;
			bool applied = false;
			bool went_back = false;
			bool in_browser = false;
			bool parked = false;
			u32 parked_frames = 0;
			u8 last_phase = 0;
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

		// The game connects to the lobby by this address without a name lookup, so the TLS tunnel only
		// knows it as a server address from here.
		void AddLobbyServerAddress(const std::string& lobby_addr)
		{
			unsigned a[4], port;
			char end;
			if (std::sscanf(lobby_addr.c_str(), "%u.%u.%u.%u:%u%c", &a[0], &a[1], &a[2], &a[3], &port, &end) != 5)
				return;
			u8 bytes[4];
			for (int i = 0; i < 4; i++)
			{
				if (a[i] > 255)
					return;
				bytes[i] = static_cast<u8>(a[i]);
			}
			u32 ip;
			std::memcpy(&ip, bytes, 4);
			ServerAddressAdd(ip);
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
			if (doc.HasMember("account") && doc["account"].IsInt() && doc["account"].GetInt() > 0)
				r.account = doc["account"].GetInt();
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

		// Called with s_fast.mutex held. The game's own DNAS fails and shows its error when no answer comes
		// in time (soon under fast boot's unthrottled speed): it must not run while the request is out.
		void Hold(const std::shared_ptr<std::atomic<bool>>& cancel)
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

		// The game is held at DNAS (paused) from the request until the answer. No answer or a server error:
		// retry until the server answers or the player resumes. A refusal (4xx) is an answer.
		void Request(std::string url, std::string form, std::shared_ptr<std::atomic<bool>> cancel)
		{
			std::optional<LoginResult> result;
			s32 status;
			for (int wait = 2;; wait = std::min(wait * 2, 30))
			{
				result.reset();
				status = Post(url, form, result, *cancel);
				if (cancel->load())
					return;
				if (status >= 200 && status < 500)
					break;
				{
					std::lock_guard lock(s_fast.mutex);
					if (s_fast.cancel != cancel)
						return;
					if (!s_fast.holding)
						Hold(cancel);
				}
				Console.WriteLn("Zdxsv: fast login: no answer from the login API (%d), game held, retry in %d s", status, wait);
				Host::AddIconOSDMessage("ZdxsvLoginHold", ICON_FA_NETWORK_WIRED,
					fmt::format("zdxsv login server not answering, retrying in {} s. Resume the game to go back to the network menu.", wait),
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
			s_fast.status = status;
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

		bool LoaderBusy()
		{
			u32 queue;
			std::memcpy(&queue, &eeMem->Main[Game().loader_queue], 4);
			return queue < Ps2MemSize::MainRam && eeMem->Main[queue] != 0;
		}

		void PutResultArea(const LoginResult& r)
		{
			std::memset(&eeMem->Main[Game().result], 0, kResultSize);
			Put(Game().result + kResultLbsId, r.lbs_info.substr(0, kLbsIdLen));
			Put(Game().result + kResultLoginKey, r.login_key);
			Put(Game().result + kResultSessionId, r.session_id);
			Put(Game().result + kResultLobbyAddr, r.lobby_addr);
			Put(Game().result + kResultLbsList, r.lbs_info);
			Put(Game().result + kResultStatus, "999");
		}

		void Apply(const LoginResult& r)
		{
			PutResultArea(r);
			std::memset(&eeMem->Main[Game().selected_lbs_id], 0, 16);
			Put(Game().selected_lbs_id, r.lbs_info.substr(0, kLbsIdLen));
			eeMem->Main[Game().lbs_count] = 1;
			eeMem->Main[NetState()] = 0x27;
			eeMem->Main[NetSub()] = 0;
			eeMem->Main[LobbyStep()] = 4;
			eeMem->Main[LobbySub()] = 0;
			eeMem->Main[LobbyPhase()] = 0x17;
		}

		// Without a session the game must not run DNAS: it goes back to the connection menu, from where the
		// player connects again (and the next DNAS start signs in or logs in again).
		void BackToNetMenu(const std::string& why,
			const char* what_to_do = "Sign in from Settings > zdxsv, then connect again.")
		{
			Host::AddIconOSDMessage("ZdxsvLoginError", ICON_FA_NETWORK_WIRED,
				fmt::format("zdxsv login failed: {}. {}", why, what_to_do), 15.0f);
			// Out of the browser the network menu does not run; the game stays there as it would without fast login.
			if (s_fast.in_browser)
			{
				Console.WriteLn("Zdxsv: fast login: %s, the game stays in its login browser", why.c_str());
				if (s_fast.parked)
					eeMem->Main[LobbyStep()] = kStepBrowserLoad;
				s_fast.parked = false;
				return;
			}
			Console.WriteLn("Zdxsv: fast login: %s, back to the network menu", why.c_str());
			eeMem->Main[NetSub()] = 0;
			eeMem->Main[NetState()] = kNetConnectMenu;
			s_fast.went_back = true;
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
				const std::string error = AuthSignIn(AuthOpenBrowser, *cancel);
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
		// The game's reconnect (after a failed battle connect) opens its login browser without DNAS. Fast login
		// skipped that browser's page, so the browser has no server (it connects to 0.0.0.0:80) and never ends:
		// a login there sends the game to the lobby as at DNAS. Apply enters the phase at step 4, the game's own
		// browser at step 0.
		const u8 phase = eeMem->Main[LobbyPhase()];
		const bool in_phase = eeMem->Main[NetState()] == kNetLoginBrowser && phase == kPhaseLoginBrowser;
		const bool browser = in_phase && (s_fast.in_browser ||
			(s_fast.last_phase != kPhaseLoginBrowser && eeMem->Main[LobbyStep()] == kStepBrowserLoad));
		s_fast.last_phase = phase;
		// The game clears the result area for a newly made user; the lobby connect after a battle reads it.
		if (s_fast.applied && eeMem->Main[Game().result + kResultStatus] == 0 && eeMem->Main[NetState()] == kNetLoginBrowser &&
			phase != kPhaseUserSelect && phase != kPhaseLoginBrowser)
		{
			std::lock_guard lock(s_fast.mutex);
			if (s_fast.result)
			{
				PutResultArea(*s_fast.result);
				Console.WriteLn("Zdxsv: fast login: result area cleared by the game, restored");
			}
		}
		const bool at_dnas =(eeMem->Main[NetState()] == 0x21 && eeMem->Main[LobbyPhase()] == 2) || browser;
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
			s_fast.in_browser = browser;
			const std::optional<std::string> token = AuthTokenRead();
			s_fast.parked = token && browser;
			s_fast.parked_frames = 0;
			if (s_fast.parked)
				eeMem->Main[LobbyStep()] = kStepParked;
			if (!token)
			{
				if (browser)
					return;
				if (!s_fast.sign_in_tried && SignInAtDnas())
					StartSignIn();
				else
					BackToNetMenu("not signed in");
				return;
			}
			std::string form = "token=" + *token + "&game=" + Game().code;
			if (const int account = Host::GetIntSettingValue("DEV9/Eth", "ZdxsvAccount", 0); account > 0)
				form += "&account=" + std::to_string(account);
			else if (account == kNewAccount)
				form += "&account=new";
			Console.WriteLn("Zdxsv: fast login: by token%s%s", browser ? ", from the game's login browser" : "", s_fast.parked ? " (held before it loads)" : "");
			std::lock_guard lock(s_fast.mutex);
			s_fast.answered = false;
			s_fast.hold_paused = false;
			s_fast.result.reset();
			s_fast.cancel = std::make_shared<std::atomic<bool>>(false);
			Hold(s_fast.cancel);
			std::thread(Request, Url(), std::move(form), s_fast.cancel).detach();
			s_fast.step = FastLogin::Step::Pending;
			return;
		}
		std::lock_guard lock(s_fast.mutex);
		if (!s_fast.answered)
		{
			if (!s_fast.hold_paused || VMManager::GetState() != VMState::Running)
				return;
			Host::RemoveKeyedOSDMessage("ZdxsvLoginHold");
			if (s_fast.cancel)
				s_fast.cancel->store(true);
			s_fast.holding = false;
			s_fast.hold_paused = false;
			s_fast.step = FastLogin::Step::Done;
			if (at_dnas)
				BackToNetMenu("login server not answering");
			return;
		}
		// A load the game queued before the reconnect still runs: the step-4 load started on top of it never ends.
		if (s_fast.parked && LoaderBusy() && s_fast.parked_frames < kParkedMaxFrames)
		{
			s_fast.parked_frames++;
			return;
		}
		if (s_fast.parked)
			Console.WriteLn("Zdxsv: fast login: file loader %s after %u frames", LoaderBusy() ? "still busy" : "idle", s_fast.parked_frames);
		s_fast.step = FastLogin::Step::Done;
		if (!at_dnas)
		{
			Console.WriteLn("Zdxsv: fast login: the game left DNAS before the answer");
			return;
		}
		if (!s_fast.result && s_fast.status == 409)
		{
			BackToNetMenu("this Discord account has made its zdxsv account already",
				"Pick an account in Settings > zdxsv, then connect again.");
			return;
		}
		if (!s_fast.result && s_fast.status == 404)
		{
			BackToNetMenu("no zdxsv account yet",
				"Add an account or carry one over from a PS2 in Settings > zdxsv, then connect again.");
			return;
		}
		if (!s_fast.result)
		{
			BackToNetMenu(fmt::format("login refused (HTTP {})", s_fast.status));
			return;
		}
		if (s_fast.result->account > 0 && Host::GetIntSettingValue("DEV9/Eth", "ZdxsvAccount", 0) == kNewAccount)
		{
			Host::SetBaseIntSettingValue("DEV9/Eth", "ZdxsvAccount", s_fast.result->account);
			Host::CommitBaseSettingChanges();
			Console.WriteLn("Zdxsv: fast login: new account %d made", s_fast.result->account);
		}
		Apply(*s_fast.result);
		s_fast.parked = false;
		s_fast.in_browser = false;
		AddLobbyServerAddress(s_fast.result->lobby_addr);
		s_fast.applied = true;
		Console.WriteLn("Zdxsv: fast login: session %s, lobby %s", s_fast.result->session_id.c_str(), s_fast.result->lobby_addr.c_str());
	}

	bool FastLoginApplied()
	{
		return s_fast.applied;
	}

	bool FastLoginWentBack()
	{
		return s_fast.went_back;
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
		s_fast.went_back = false;
		s_fast.in_browser = false;
		s_fast.last_phase = 0;
		s_fast.parked = false;
		s_fast.step = FastLogin::Step::Idle;
		s_fast.sign_in_tried = false;
		s_fast.sign_in_ended = false;
		s_fast.sign_in_paused = false;
		s_fast.holding = false;
		s_fast.hold_paused = false;
	}
} // namespace Zdxsv
