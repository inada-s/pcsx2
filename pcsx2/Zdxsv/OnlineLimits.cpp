// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// PCSX2 features limited while the Z game is online (connected to the zdxsv lobby or in a net battle): PINE
// (every request), macro buttons (turbo included), input recording, manual hardware renderer fixes, dumps and
// debugger breakpoints (cleared when the game goes online: a hit stops the emulator).
// Switching online or offline reloads the settings, so the limits come and go with the connection.

#include "Zdxsv/OnlineLimits.h"
#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/InputHooks.h"
#include "Zdxsv/PineHooks.h"
#include "Zdxsv/UiHooks.h"
#include "Zdxsv/TestOptions.h"

#include "Config.h"
#include "DebugTools/Breakpoints.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace Zdxsv
{
	bool g_online_limits = false;

	namespace
	{
		struct OnlineLimitsState
		{
			std::atomic<bool> online{false}; // g_online_limits for other threads
			std::atomic<bool> pine_refused{false}; // read on the PINE thread
			std::atomic<bool> pine_logged{false}; // one log line per online period
			std::atomic<bool> battle{false};
			bool reset_allowed = false;
			bool test_breakpoint_set = false;
		};
		OnlineLimitsState s_online;

		// ZDXSV_PINE_ONLINE=1 (test): PINE answers while online; the rigs drive the lobby and battles through it
		bool PineExempt()
		{
			static const bool exempt = [] {
				const char* e = TestEnv("ZDXSV_PINE_ONLINE");
				return e && std::strcmp(e, "1") == 0;
			}();
			return exempt;
		}

		void SetOnline(bool online)
		{
			g_online_limits = online;
			s_online.online.store(online, std::memory_order_relaxed);
			s_online.pine_refused.store(online && !PineExempt(), std::memory_order_relaxed);
			s_online.pine_logged.store(false, std::memory_order_relaxed);
		}

		void ClearBreakpoints()
		{
			if (CBreakPoints::GetNumBreakpoints() == 0 && CBreakPoints::GetNumMemchecks() == 0)
				return;
			CBreakPoints::ClearAllBreakPoints();
			CBreakPoints::ClearAllMemChecks();
			Console.WriteLn("ZdxsvOnline: debugger breakpoints cleared");
		}

		// ZDXSV_TEST_BREAKPOINT=<hex EE address> (test): one breakpoint there at the first vsync, as if set in the
		// debugger before going online; the address must never run
		void SetTestBreakpoint()
		{
			s_online.test_breakpoint_set = true;
			const char* e = TestEnv("ZDXSV_TEST_BREAKPOINT");
			if (!e || !*e)
				return;
			const u32 addr = static_cast<u32>(std::strtoul(e, nullptr, 16));
			Host::RunOnCPUThread([addr] {
				CBreakPoints::AddBreakPoint(BREAKPOINT_EE, addr);
				Console.WriteLn("ZdxsvOnline: test breakpoint at %08x", addr);
			});
		}
	} // namespace

	void OnlineLimitsOnVsync()
	{
		if (g_ggpo_in_rollback)
			return;
		if (!s_online.test_breakpoint_set)
			SetTestBreakpoint();
		s_online.battle.store(OnlineBattleNow(), std::memory_order_relaxed);
		const bool online = OnlineNow();
		if (online == g_online_limits)
			return;
		SetOnline(online);
		Console.WriteLn("ZdxsvOnline: %s: settings reloaded", online ? "online" : "offline");
		Host::RunOnCPUThread([online] {
			if (!VMManager::HasValidVM())
				return;
			VMManager::ApplySettings();
			if (online)
				ClearBreakpoints();
		});
	}

	bool OnlineLimitsActive()
	{
		return g_online_limits;
	}

	void OnlineLimitsForce(Pcsx2Config& c, std::string* changed)
	{
		const auto note = [changed](bool on, const char* name) {
			if (on && changed)
				*changed += (changed->empty() ? "" : ", ") + std::string(name);
		};
		note(c.EnableRecordingTools, "input recording");
		c.EnableRecordingTools = false;
		note(c.GS.ManualUserHacks, "manual hardware renderer fixes");
		c.GS.ManualUserHacks = false;
		note(c.GS.DumpReplaceableTextures, "texture dumping");
		c.GS.DumpReplaceableTextures = false;
		note(c.GS.DumpGSData, "draw data dumping");
		c.GS.DumpGSData = false;
	}

	void OnlineLimitsReset()
	{
		SetOnline(false);
		s_online.battle.store(false, std::memory_order_relaxed);
		s_online.reset_allowed = false;
		s_online.test_breakpoint_set = false;
	}

	bool OnlineRefuses(Error* error)
	{
		if (!s_online.online.load(std::memory_order_relaxed))
			return false;
		const std::string msg = TRANSLATE_STR("Zdxsv", "Not available while online.");
		Console.WriteLn("ZdxsvOnline: refused while online");
		if (error)
			Error::SetString(error, msg);
		else
			Host::AddIconOSDMessage("ZdxsvOnlineRefuses", ICON_FA_TRIANGLE_EXCLAMATION, msg, Host::OSD_WARNING_DURATION);
		return true;
	}

	bool OnlineNowAnyThread()
	{
		return s_online.online.load(std::memory_order_relaxed);
	}

	bool OnlineBattleNowAnyThread()
	{
		return s_online.battle.load(std::memory_order_relaxed);
	}

	void OnlineAllowReset()
	{
		s_online.reset_allowed = true;
	}

	bool OnlineRefusesReset()
	{
		return !s_online.reset_allowed && OnlineRefuses();
	}

	bool PineRefuses()
	{
		if (!s_online.pine_refused.load(std::memory_order_relaxed))
			return false;
		if (!s_online.pine_logged.exchange(true, std::memory_order_relaxed))
			Console.WriteLn("ZdxsvOnline: PINE requests refused while online");
		return true;
	}
} // namespace Zdxsv
