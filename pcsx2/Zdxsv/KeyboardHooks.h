// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the host keyboard path (Qt display widget, USB HID keyboard). Only declarations (AGENTS.md, Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	// CPU thread, around InputManager::InvokeEvents of a host key: the event's native scan code, 0 after it.
	void HostKeySetScancode(u32 native_scancode);

	// In HIDKbdDevice::SetBindingValue: the PS/2 set 1 number of the physical key being dispatched, 0 when unknown.
	int HostKeyNumber();
} // namespace Zdxsv
