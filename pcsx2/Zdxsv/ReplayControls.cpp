// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Replay playback controls: the rounds shown, the host pad's controls, the operations refused during a replay.

#include "Zdxsv/GgpoShared.h"
#include "IconsFontAwesome.h"

namespace Zdxsv
{
	int PlaySpanAt(int f)
	{
		if (!s_play_skip_ms || s_play.spans.empty())
			return -1;
		for (size_t i = 0; i < s_play.spans.size(); i++)
			if (f < s_play.spans[i].second)
				return static_cast<int>(i);
		return static_cast<int>(s_play.spans.size()) - 1;
	}

	bool PlayInBattle(int f)
	{
		const int i = PlaySpanAt(f);
		if (i < 0)
			return true;
		return f >= s_play.plays[i] && f < s_play.spans[i].second && eeMem->Main[TICK_STATE] == TICK_PLAY;
	}

	int PlaySkipGap(int next)
	{
		const int i = PlaySpanAt(next);
		if (i < 0 || next < 0 || s_to_phase != TO_OFF || s_play.target >= 0 || s_play.run_load >= 0)
			return next;
		const auto [begin, end] = s_play.spans[i];
		if (next >= end)
		{
			if (next < s_play.frames)
				Console.WriteLn("ZdxsvGgpo: replay: past the last round at frame %d, the end", next);
			return s_play.frames;
		}
		if (next >= begin)
			return next;
		Console.WriteLn("ZdxsvGgpo: replay: frame %d is outside the rounds, skipping to round %d at frame %d", next, i + 1, begin);
		return PlaySeek(begin);
	}

	namespace
	{
		struct ReplayPadState
		{
			u32 held = 0;
			Common::Timer::Value seek_since = 0;
		} s_pad;

		enum : u32
		{
			PAD_BACK = 1,
			PAD_FORWARD = 2,
			PAD_FASTER = 4,
			PAD_SLOWER = 8,
			PAD_PAUSE = 16
		};
		constexpr int PAD_SEEK_FRAMES = 60;
		constexpr double PAD_REPEAT_MS = 500.0;

		std::atomic<u32> s_pad_pokes{0};

		constexpr int SPEED_PERCENT[] = {33, 50, 100, 200, 300};

		// read by the overlays (GS thread)
		struct ReplayControlsShown
		{
			std::atomic<int> speed_percent{100};
			std::atomic<u16> to_current{0};
			std::atomic<float> to_left{0.0f};
		} s_ctl;
	} // namespace

	void ReplayControlsReset()
	{
		s_pad = {};
		s_ctl.speed_percent = 100;
		s_ctl.to_current = 0;
		s_ctl.to_left = 0.0f;
	}


	void ReplayStep(int dir)
	{
		if (!s_play_env || s_play.frames <= 0 || !g_ggpo_active || dir == 0)
			return;
		// a step (or a seek from a pause) still runs, unpaused, until it pauses at its target: not playing
		if (s_play.step_pause)
			return;
		if (VMManager::GetState() != VMState::Paused || s_play.at_end)
		{
			ReplaySeekBy(dir * PAD_SEEK_FRAMES);
			return;
		}
		s_play_req = s_battle.frame + 1 + (dir < 0 ? -2 : 0);
		s_play.step_pause = true;
		PlayStepResume();
	}

	u32 ReplayPadPokes()
	{
		return s_pad_pokes;
	}

	void ReplayPadPoll()
	{
		if (!s_play_env || s_play.frames <= 0 || !g_ggpo_active || s_to_phase != TO_OFF)
		{
			s_pad = {};
			return;
		}
		using I = PadDualshock2::Inputs;
		const auto on = [](I a, I b) { return s_host[a] >= 0.5f || s_host[b] >= 0.5f; };
		const u32 now = (on(I::PAD_LEFT, I::PAD_L_LEFT) ? PAD_BACK : 0) | (on(I::PAD_RIGHT, I::PAD_L_RIGHT) ? PAD_FORWARD : 0) |
		                (on(I::PAD_UP, I::PAD_L_UP) ? PAD_FASTER : 0) | (on(I::PAD_DOWN, I::PAD_L_DOWN) ? PAD_SLOWER : 0) |
		                (s_host[I::PAD_CIRCLE] >= 0.5f ? PAD_PAUSE : 0);
		const u32 pressed = now & ~s_pad.held;
		if (now != s_pad.held)
			s_pad_pokes++;
		s_pad.held = now;
		const Common::Timer::Value t = Common::Timer::GetCurrentValue();
		const int dir = (now & PAD_BACK) ? -1 : (now & PAD_FORWARD) ? 1 :
		                                                              0;
		if (pressed & (PAD_BACK | PAD_FORWARD))
		{
			s_pad.seek_since = t;
			ReplayStep(dir);
		}
		else if (dir != 0 && Common::Timer::ConvertValueToMilliseconds(t - s_pad.seek_since) >= PAD_REPEAT_MS &&
				 s_play_req.load() == INT_MIN && s_play.target < 0 && !s_play.step_pause)
			ReplayStep(dir);
		if (pressed & PAD_FASTER)
			ReplaySpeedBy(1);
		if (pressed & PAD_SLOWER)
			ReplaySpeedBy(-1);
		if (pressed & PAD_PAUSE)
			ReplayTogglePause();
	}

	namespace
	{
		struct ReplayLimits
		{
			bool own_load = false;
		} s_limits;
	} // namespace

	bool ReplayRefuses(Error* error)
	{
		if (!s_play_env || s_limits.own_load)
			return false;
		const std::string msg = TRANSLATE_STR("Zdxsv", "Not available during a replay.");
		Console.WriteLn("ZdxsvGgpo: refused during a replay (state save, state load or disc change)");
		if (error)
			Error::SetString(error, msg);
		else
			Host::AddIconOSDMessage("ZdxsvReplayRefuses", ICON_FA_TRIANGLE_EXCLAMATION, msg, Host::OSD_WARNING_DURATION);
		return true;
	}

	bool ReplayLoadState(const char* path, Error* error)
	{
		s_limits.own_load = true;
		const bool ok = VMManager::LoadState(path, error);
		s_limits.own_load = false;
		PlayHideAfterLoad();
		return ok;
	}

	void ReplaySpeedBy(int delta)
	{
		if (!s_play_env || s_play.frames <= 0 || !g_ggpo_active || s_live_down)
			return;
		const int speed = std::clamp(s_play.speed + delta, -2, 2);
		if (speed == s_play.speed)
			return;
		s_play.speed = speed;
		s_ctl.speed_percent = SPEED_PERCENT[speed + 2];
		Console.WriteLn("ZdxsvGgpo: replay speed %d %%", SPEED_PERCENT[speed + 2]);
	}

	// Not the emulator's speed: a change of it switches SPU2 to the fast-forward volume and back.
	int PlayFastEvery()
	{
		return s_play.speed > 0 ? 1 + s_play.speed : 1;
	}

	void PlaySlowWait()
	{
		if (s_play.speed >= 0 || s_to_phase != TO_OFF)
			return;
		Threading::SleepUntil(GetCPUTicks() + static_cast<u64>(-s_play.speed) * GetTickFrequency() * 1001 / 60000);
	}

	int ReplaySpeedPercent()
	{
		return s_ctl.speed_percent;
	}

	void ReplayTakeover()
	{
		if (!s_play_env || s_play.frames <= 0 || !g_ggpo_active)
			return;
		if (s_to_phase == TO_OFF || s_to_phase == TO_ON)
		{
			s_to_req = s_to_phase == TO_OFF ? TO_REQ_TAKE : TO_REQ_RETRY;
			if (VMManager::GetState() == VMState::Paused)
				VMManager::SetPaused(false);
		}
	}

	void ReplayTakeoverCancel()
	{
		if (s_to_phase != TO_COUNT)
			return;
		s_to_req = TO_REQ_CANCEL;
		VMManager::SetPaused(false);
	}

	void ReplayTakeoverReturn()
	{
		if (s_to_phase == TO_OFF)
			return;
		s_to_req = TO_REQ_RETURN;
		VMManager::SetPaused(false);
	}

	bool ReplayTakeoverInfo(int& phase, u16& target, u16& current, float& countdown)
	{
		if (!s_play_env || !g_ggpo_active)
			return false;
		phase = s_to_phase;
		target = s_to_target;
		current = s_ctl.to_current;
		countdown = s_ctl.to_left;
		return true;
	}

	void ReplayTakeoverIdle()
	{
		ReplayPadPoll();
		if (s_to_phase != TO_COUNT)
			return;
		u16 a, b;
		ZdPadAB(TakeoverHostInput(), a, b);
		s_ctl.to_current = b;
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (s_to.count_t0 == 0)
			s_to.count_t0 = now;
		const double ms = Common::Timer::ConvertValueToMilliseconds(now - s_to.count_t0);
		s_ctl.to_left = static_cast<float>(std::max(0.0, 1.0 - ms / 1000.0));
		if (ms >= 1000.0)
		{
			Console.WriteLn("ZdxsvGgpo: replay takeover: countdown over, input %s, starting", b == s_to_target ? "matched" : "not matched");
			s_to_req = TO_REQ_START;
			VMManager::SetPaused(false);
		}
	}

} // namespace Zdxsv
