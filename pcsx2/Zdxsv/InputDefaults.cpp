// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// zdxsv default input settings: no upstream hotkeys (a stray key changed interlacing, speed or states mid-game)
// and the host keyboard on USB port 1 as a USB keyboard.

#include "Zdxsv/DefaultsHooks.h"

#include "SIO/Pad/Pad.h"
#include "USB/USB.h"

#include "common/MemorySettingsInterface.h"

#include <string_view>

namespace Zdxsv
{
	namespace
	{
		constexpr const char* MIGRATE_SECTION = "Zdxsv";
		constexpr const char* MIGRATE_KEY = "InputDefaultsVersion";
		constexpr u32 MIGRATE_VERSION = 1;
		constexpr u32 KEYBOARD_PORT = 0;
		constexpr const char* KEYBOARD_TYPE = "hidkbd";
	} // namespace

	void InputDefaultsHotkeys(SettingsInterface& si)
	{
		MemorySettingsInterface upstream;
		Pad::SetDefaultHotkeyConfig(upstream);
		for (const auto& [name, value] : upstream.GetKeyValueList("Hotkeys"))
		{
			if (std::string_view(name).starts_with("Zdxsv"))
				continue;
			if (si.GetStringValue("Hotkeys", name.c_str()) == value)
				si.DeleteValue("Hotkeys", name.c_str());
		}
	}

	void InputDefaultsUsb(SettingsInterface& si)
	{
		si.SetStringValue(USB::GetConfigSection(KEYBOARD_PORT).c_str(), "Type", KEYBOARD_TYPE);
	}

	bool InputDefaultsMigrate(SettingsInterface& si)
	{
		if (si.GetUIntValue(MIGRATE_SECTION, MIGRATE_KEY, 0u) >= MIGRATE_VERSION)
			return false;

		InputDefaultsHotkeys(si);
		const std::string section = USB::GetConfigSection(KEYBOARD_PORT);
		const std::string type = si.GetStringValue(section.c_str(), "Type", "None");
		if (type.empty() || type == "None")
			InputDefaultsUsb(si);
		si.SetUIntValue(MIGRATE_SECTION, MIGRATE_KEY, MIGRATE_VERSION);
		return true;
	}
} // namespace Zdxsv
