// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Breakpoints set before going online are cleared: a hit would stop the emulator, and every peer with it.

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
			std::atomic<bool> online{false};
			std::atomic<bool> pine_refused{false};
			std::atomic<bool> pine_logged{false};
			std::atomic<bool> battle{false};
			bool reset_allowed = false;
			bool test_breakpoint_set = false;
		};
		OnlineLimitsState s_online;

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

		// The address must never run.
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
