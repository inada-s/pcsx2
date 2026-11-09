// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/InputHooks.h"

#include "Host.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"

#include "common/SettingsInterface.h"

#include <algorithm>
#include <array>
#include <string>

namespace Zdxsv
{
	bool g_stick_message = false;

	namespace
	{
		using I = PadDualshock2::Inputs;

		constexpr u32 kNone = I::LENGTH;

		// Same direction order in each.
		constexpr std::array<u32, 4> kRight = {I::PAD_R_UP, I::PAD_R_RIGHT, I::PAD_R_DOWN, I::PAD_R_LEFT};
		constexpr std::array<u32, 4> kDpad = {I::PAD_UP, I::PAD_RIGHT, I::PAD_DOWN, I::PAD_LEFT};
		constexpr std::array<u32, 4> kLeft = {I::PAD_L_UP, I::PAD_L_RIGHT, I::PAD_L_DOWN, I::PAD_L_LEFT};

		struct Button
		{
			const char* name;
			u32 bind;
		};
		// The values of ZdxsvStickMessageButton (pcsx2-qt ZdxsvSettingsWidget lists the same).
		constexpr Button kButtons[] = {
			{"None", kNone},
			{"Start", I::PAD_START},
			{"Select", I::PAD_SELECT},
			{"L1", I::PAD_L1},
			{"L2", I::PAD_L2},
			{"R1", I::PAD_R1},
			{"R2", I::PAD_R2},
			{"L3", I::PAD_L3},
			{"R3", I::PAD_R3},
			{"Triangle", I::PAD_TRIANGLE},
			{"Circle", I::PAD_CIRCLE},
			{"Cross", I::PAD_CROSS},
			{"Square", I::PAD_SQUARE},
		};

		struct State
		{
			u32 button = I::PAD_START;
			std::array<std::array<float, I::LENGTH>, Pad::NUM_CONTROLLER_PORTS> host = {};
			std::array<std::array<float, 4>, Pad::NUM_CONTROLLER_PORTS> right = {};
		} s_state;

		u32 ParseButton(const std::string& name)
		{
			for (const Button& b : kButtons)
				if (name == b.name)
					return b.bind;
			return I::PAD_START;
		}

		void Deliver(u32 controller, u32 bind, float value)
		{
			if (g_ggpo_active && GgpoCaptureHostInput(controller, bind, value))
				return;
			Pad::GetPad(controller)->Set(bind, value);
		}

		bool IsDualshock2(u32 controller)
		{
			const PadBase* pad = Pad::GetPad(controller);
			return pad && pad->GetType() == Pad::ControllerType::DualShock2;
		}

		void UpdateRight(u32 controller)
		{
			const auto& host = s_state.host[controller];
			const bool held = s_state.button != kNone && host[s_state.button] >= 0.5f;
			for (u32 d = 0; d < kRight.size(); d++)
			{
				float value = host[kRight[d]];
				if (held)
					value = std::max({value, host[kDpad[d]] >= 0.5f ? 1.0f : 0.0f, host[kLeft[d]]});
				if (value != s_state.right[controller][d])
				{
					s_state.right[controller][d] = value;
					Deliver(controller, kRight[d], value);
				}
			}
		}

		void Apply(const std::string& button)
		{
			s_state.button = ParseButton(button);
			g_stick_message = true;
			for (u32 c = 0; c < Pad::NUM_CONTROLLER_PORTS; c++)
				if (IsDualshock2(c))
					UpdateRight(c);
		}
	} // namespace

	bool StickMessageSetInput(u32 controller, u32 bind, float value)
	{
		if (bind >= I::LENGTH || !IsDualshock2(controller))
			return false;
		s_state.host[controller][bind] = value;
		if (std::find(kRight.begin(), kRight.end(), bind) == kRight.end())
			Deliver(controller, bind, value);
		UpdateRight(controller);
		return true;
	}

	void StickMessageLoadSettings(SettingsInterface& si)
	{
		Apply(si.GetStringValue("DEV9/Eth", "ZdxsvStickMessageButton", "Start"));
	}

	void StickMessageOnVmInitialize()
	{
		s_state = {};
		Apply(Host::GetStringSettingValue("DEV9/Eth", "ZdxsvStickMessageButton", "Start"));
	}

	void StickMessageReset()
	{
		s_state = {};
		g_stick_message = false;
	}
} // namespace Zdxsv
