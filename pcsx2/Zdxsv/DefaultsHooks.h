// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the default settings (VMManager, Big Picture, Qt host). Only declarations (AGENTS.md, Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

class SettingsInterface;

namespace Zdxsv
{
	// After Pad::SetDefaultHotkeyConfig: removes the upstream default hotkeys, keeps the zdxsv replay ones.
	void InputDefaultsHotkeys(SettingsInterface& si);
	// After USB::SetDefaultConfiguration: USB port 1 is a keyboard the game reads (chat).
	void InputDefaultsUsb(SettingsInterface& si);
	// Once per settings file, after it loads: applies both to settings written before them. True: si changed.
	bool InputDefaultsMigrate(SettingsInterface& si);
} // namespace Zdxsv
