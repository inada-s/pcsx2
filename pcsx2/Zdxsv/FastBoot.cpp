// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/Settings.h"

#include "Memory.h"
#include "SIO/Pad/Pad.h"
#include "VMManager.h"

#include "common/Console.h"

#include <array>
#include <deque>

namespace Zdxsv
{
	namespace
	{
		// Guest addresses of SLPS-25419 (g_z_game checks serial and CRC).
		constexpr u32 kGp = 0x6dd770;
		constexpr u32 kSceneBlock = kGp - 0x5ce4; // pointer; byte 0 = scene: 1 title, 2 main menu, 7 network mode
		constexpr u32 kMainMenu = kGp - 0x5e1c; // pointer; +0x25c = cursor
		constexpr u32 kMainMenuCursor = 0x25c;
		constexpr u8 kMenuNetwork = 7;
		constexpr u32 kNetState = 0xcd1360;
		constexpr u32 kLobbyObject = kGp - 0x5a1c; // pointer; +0xa1f = 4 at the user_id selection
		constexpr u32 kLobbyMarker = 0xa1f;

		enum Button : u32
		{
			Up = 0,
			Down = 2,
			Circle = 5,
			Start = 9,
		};

		constexpr u32 kHoldFrames = 7;
		constexpr u32 kGapFrames = 24;
		constexpr u32 kSequenceGapFrames = 21;
		// A menu that ignored a sequence (still loading) gets it again after this.
		constexpr u32 kSequenceRetryFrames = 180;
		constexpr u32 kGiveUpFrames = 60 * 240;
		// Presses right after a menu opens can be dropped (the cursor misses an Up).
		constexpr u32 kMenuSettleFrames = 60;
		// The netconf list keeps dropping presses for longer after it opens.
		constexpr u32 kNetconfSettleFrames = 90;

		struct FastBoot
		{
			bool started = false;
			bool done = false;
			u32 frame = 0;
			u32 next_press = 0;
			u32 release_at = 0;
			int held = -1;
			int net_state = -1;
			u32 net_entry = 0;
			u32 sequence_after = 0;
			std::deque<Button> queue;
			LimiterModeType limiter = LimiterModeType::Nominal;
		};
		FastBoot s_boot;

		u32 Pointer(u32 addr)
		{
			const u32 p = *reinterpret_cast<const u32*>(&eeMem->Main[addr]);
			return (p > 0x100000 && p < Ps2MemSize::MainRam) ? p : 0;
		}

		void Set(Button b, bool down)
		{
			Pad::SetControllerState(0, b, down ? 1.0f : 0.0f);
		}

		void Finish(const char* why)
		{
			if (s_boot.held >= 0)
				Set(static_cast<Button>(s_boot.held), false);
			s_boot.held = -1;
			s_boot.queue.clear();
			s_boot.done = true;
			if (VMManager::GetLimiterMode() == LimiterModeType::Turbo)
				VMManager::SetLimiterMode(s_boot.limiter);
			Console.WriteLn("Zdxsv: fast boot: %s at frame %u", why, s_boot.frame);
		}

		void Sequence(std::initializer_list<Button> buttons)
		{
			s_boot.queue.assign(buttons);
			s_boot.sequence_after = s_boot.frame + kSequenceRetryFrames;
		}

		void Choose(u8 scene, int net)
		{
			if (!s_boot.queue.empty())
				return;
			if (scene == 1)
				s_boot.queue.push_back(Start);
			else if (scene == 2)
			{
				const u32 menu = Pointer(kMainMenu);
				if (menu)
				{
					eeMem->Main[menu + kMainMenuCursor] = kMenuNetwork;
					s_boot.queue.push_back(Circle);
				}
			}
			else if (scene == kMenuNetwork && (net == 1 || net == 2 || net == 4 || (net == 3 && eeMem->Main[kNetState + 1] == 2)))
				s_boot.queue.push_back(Circle);
			else if (scene == kMenuNetwork && s_boot.frame >= s_boot.sequence_after && s_boot.frame - s_boot.net_entry > (net == 6 ? kNetconfSettleFrames : kMenuSettleFrames))
			{
				if (net == 3)
					Sequence({Up, Up, Circle}); // broadband
				else if (net == 5)
					Sequence({Up, Circle}); // accept the terms
				else if (net == 6)
					Sequence({Down, Down, Circle}); // network setting 1
			}
		}
	} // namespace

	void FastBootOnVsync()
	{
		if (s_boot.done)
			return;
		if (!s_boot.started)
		{
			s_boot.started = true;
			if (!BoolSetting("ZdxsvFastBoot", false, "ZDXSV_FAST_BOOT"))
			{
				s_boot.done = true;
				return;
			}
			s_boot.limiter = VMManager::GetLimiterMode();
			VMManager::SetLimiterMode(LimiterModeType::Turbo);
			Console.WriteLn("Zdxsv: fast boot: on");
		}
		s_boot.frame++;
		if (s_boot.frame > kGiveUpFrames)
			return Finish("gave up");

		const u32 lobby = Pointer(kLobbyObject);
		if (lobby && eeMem->Main[lobby + kLobbyMarker] == 4)
			return Finish("user_id selection");

		const u32 scene_block = Pointer(kSceneBlock);
		const u8 scene = scene_block ? eeMem->Main[scene_block] : 0;
		const int net = eeMem->Main[kNetState];
		if (net != s_boot.net_state)
		{
			s_boot.net_state = net;
			s_boot.net_entry = s_boot.frame;
			s_boot.sequence_after = 0;
		}

		if (s_boot.held >= 0)
		{
			if (s_boot.frame < s_boot.release_at)
				return;
			Set(static_cast<Button>(s_boot.held), false);
			s_boot.held = -1;
		}
		if (s_boot.frame < s_boot.next_press)
			return;
		Choose(scene, net);
		if (s_boot.queue.empty())
			return;
		const Button b = s_boot.queue.front();
		s_boot.queue.pop_front();
		Set(b, true);
		s_boot.held = b;
		s_boot.release_at = s_boot.frame + kHoldFrames;
		s_boot.next_press = s_boot.release_at + (s_boot.queue.empty() ? kGapFrames : kSequenceGapFrames);
	}

	void FastBootReset()
	{
		s_boot = FastBoot{};
	}
} // namespace Zdxsv
