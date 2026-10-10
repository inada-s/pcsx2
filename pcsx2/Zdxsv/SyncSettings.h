// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Sync-safe settings of the Z game (SyncSettings.cpp). SyncSettingsEnforce is in CpuHooks.h.

#include <string>

namespace Zdxsv
{
	// GgpoOnExecuteReturned (start) and after Stop (end), CPU thread outside Execute: battle settings on or off,
	// the settings reloaded. Both peers switch at the same session frame.
	void SyncSettingsOnBattle(bool battle);
	// VM shutdown and reset: no battle
	void SyncSettingsReset();
	// The settings page: a forced setting is greyed out (battle_item: forced only during battles).
	bool SyncSettingsForced(bool battle_item);
	// 16 hex digits: the build and the settings forced in a battle; "" until the Z game ran. Any thread.
	std::string SyncFingerprint();
} // namespace Zdxsv
