// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/QtHooks.h"

#include "QtHost.h"

#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "IconsFontAwesome.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/SPU2/spu2.h"
#include "pcsx2/VMManager.h"

#include "fmt/format.h"

#include <algorithm>

namespace Zdxsv
{
	namespace
	{
		constexpr int WheelVolumeNotch = 120;
		constexpr int WheelVolumeStep = 5;

		struct WheelVolumeState
		{
			// High-resolution wheels and touchpads send fractions of a notch.
			int pending = 0;
		};

		WheelVolumeState s_wheel_volume;

		void WheelVolumeAdjust(s32 delta)
		{
			if (!VMManager::HasValidVM())
				return;

			if (!SPU2::SetOutputMuted(false))
			{
				Host::AddIconOSDMessage("VolumeChanged", ICON_FA_VOLUME_XMARK, TRANSLATE_STR("Hotkeys_Volume", "Volume: Muted in Settings"));
				return;
			}

			const s32 current_volume = static_cast<s32>(SPU2::GetOutputVolume());
			const s32 maximum_volume = static_cast<s32>(Pcsx2Config::SPU2Options::MAX_VOLUME);
			const s32 new_volume = std::clamp(current_volume + delta, 0, maximum_volume);
			if (current_volume != new_volume)
				SPU2::SetOutputVolume(static_cast<u32>(new_volume));

			const char* icon = new_volume == 0 ? ICON_FA_VOLUME_OFF : (new_volume < 100 ? ICON_FA_VOLUME_LOW : ICON_FA_VOLUME_HIGH);
			Host::AddIconOSDMessage("VolumeChanged", icon, fmt::format(TRANSLATE_FS("Hotkeys_Volume", "Volume: {}%"), new_volume),
				Host::OSD_QUICK_DURATION);
		}
	} // namespace

	bool QtWheelVolumeTakes(int angle_delta_y)
	{
		if (angle_delta_y == 0 || !QtHost::IsVMValid() || FullscreenUI::HasActiveWindow() || ImGuiManager::WantsMouseInput())
			return false;
		if (!Host::GetBoolSettingValue("UI", "ZdxsvWheelVolume", true))
			return false;
		if (InputManager::HasAnyBindingsForKey(InputManager::MakePointerAxisKey(0, InputPointerAxis::WheelY)))
			return false;

		WheelVolumeState& s = s_wheel_volume;
		if ((s.pending < 0) != (angle_delta_y < 0))
			s.pending = 0;
		s.pending += angle_delta_y;
		const int notches = s.pending / WheelVolumeNotch;
		if (notches != 0)
		{
			s.pending -= notches * WheelVolumeNotch;
			Host::RunOnCPUThread([delta = notches * WheelVolumeStep]() { WheelVolumeAdjust(delta); });
		}
		return true;
	}
} // namespace Zdxsv
