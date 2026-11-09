// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// The USB keyboard sends the physical key, not the character the host layout gives it: the game reads
// a Japanese layout, so a key named by its character (@ : ^ on a Japanese keyboard) would be lost or moved.

#include "Zdxsv/KeyboardHooks.h"

namespace Zdxsv
{
	namespace
	{
		struct HostKeyState
		{
			u32 native_scancode = 0;
		};

		HostKeyState s_host_key;

		// Windows: bits 0-7 scan code, 0x100 the extended (E0) flag. Pause has no single code; NumLock carries the flag.
		constexpr u32 WIN_EXTENDED = 0x100;
		constexpr u32 WIN_PAUSE = 0x45;
		constexpr u32 WIN_NUMLOCK = 0x145;
	} // namespace

	void HostKeySetScancode(u32 native_scancode)
	{
		s_host_key.native_scancode = native_scancode;
	}

	int HostKeyNumber()
	{
#ifdef _WIN32
		const u32 sc = s_host_key.native_scancode;
		if (sc == 0 || sc == WIN_PAUSE || sc > 0x1ff)
			return 0;
		if (sc == WIN_NUMLOCK)
			return 0x45;
		const u32 code = sc & 0x7f;
		if (code == 0 || (sc & 0x80))
			return 0;
		return static_cast<int>((sc & WIN_EXTENDED) ? (code | 0x80) : code);
#else
		return 0;
#endif
	}
} // namespace Zdxsv
