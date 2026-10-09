// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Debug: NET_TRACE lines of the pad and the battle msgs' key slots.

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{

	bool ParseKeySlots(const u8* m, u32 n, std::vector<KeySlot>& out)
	{
		for (u32 i = 2; i < n;)
		{
			if (m[i] & 0x80)
			{
				if (i + 2 > n)
					return false;
				out.push_back({static_cast<u8>(m[i] & 0x3f), 0, m[i + 1], false, 0, 0});
				i += 2;
			}
			else
			{
				if (i + 8 > n)
					return false;
				out.push_back({static_cast<u8>(m[i + 1] & 0x3f), m[i + 4], m[i + 5], true,
					static_cast<u16>(m[i + 2] << 8 | m[i + 3]), static_cast<u16>(m[i + 6] << 8 | m[i + 7])});
				i += 8;
			}
		}
		return true;
	}

	// NET_TRACE: per frame, the pad in EE RAM when it changed: `P` raw SIO buffer (8 B, buttons
	// active-low at +2) and the game's copy byte.
	void TracePad()
	{
		constexpr u32 PAD_RAW = 0x6f2460;
		constexpr u32 PAD_GAME = 0x117f4d9;
		// `A vsync frame A0..A3`: pad module A cur per position (0x6f2500 + 16p, the applied input)
		if (s_net_trace && !g_ggpo_in_rollback)
		{
			static u16 last_a[4];
			u16 a[4];
			for (int p = 0; p < 4; p++)
				std::memcpy(&a[p], eeMem->Main + 0x6f2500 + 16 * p, 2);
			if (std::memcmp(a, last_a, sizeof(a)) != 0)
			{
				std::memcpy(last_a, a, sizeof(a));
				std::fprintf(s_net_trace, "%u A %d %04x %04x %04x %04x\n", g_FrameCount, s_net_frame, a[0], a[1], a[2], a[3]);
			}
			// `L vsync frame lead maxlead slots state`: lockstep lead 0xc62be0 (queued - executed
			// counters), its cap 0xc62be1, slots per key msg 0xc62bdf, tick state 0xc627b4, when changed
			static u8 last_l[4];
			const u8* l = eeMem->Main + 0xc62bdf;
			const u8 cl[4] = {l[1], l[2], l[0], eeMem->Main[0xc627b4]};
			if (std::memcmp(cl, last_l, 4) != 0)
			{
				std::memcpy(last_l, cl, 4);
				std::fprintf(s_net_trace, "%u L %d %d %d %d %d\n", g_FrameCount, s_net_frame, cl[0], cl[1], cl[2], cl[3]);
			}
		}
		static u8 last[9];
		u8 cur[9];
		std::memcpy(cur, eeMem->Main + PAD_RAW, 8);
		cur[8] = eeMem->Main[PAD_GAME];
		if (!s_net_trace || g_ggpo_in_rollback || std::memcmp(cur, last, 9) == 0)
			return;
		std::memcpy(last, cur, 9);
		std::fprintf(s_net_trace, "%u P %02x%02x%02x%02x%02x%02x%02x%02x %02x\n", g_FrameCount,
			cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7], cur[8]);
	}

	// NET_TRACE `I`: the game's per-position input array (16 B entries at *(gp-0x5b28) + 0x4d8,
	// reader 0x2ba63c), bytes 0-7 of each + the base, when changed.
	void TraceInputs()
	{
		// ZDXSV_RAM_DUMP=dir,start,step,count: EE RAM (32 MB) to dir/<frame>.bin.
		static const auto dump = [] {
			std::tuple<std::string, u32, u32, u32> d{"", 0, 1, 0};
			if (const char* e = Zdxsv::TestEnv("ZDXSV_RAM_DUMP"))
			{
				char dir[512];
				u32 a, b, c;
				if (std::sscanf(e, "%511[^,],%u,%u,%u", dir, &a, &b, &c) == 4)
					d = {dir, a, b ? b : 1, c};
			}
			return d;
		}();
		const auto& [ddir, dstart, dstep, dcount] = dump;
		if (!g_ggpo_in_rollback && dcount && g_FrameCount >= dstart && g_FrameCount < dstart + dstep * dcount &&
			(g_FrameCount - dstart) % dstep == 0)
		{
			if (std::FILE* fp = std::fopen(fmt::format("{}/{}.bin", ddir, g_FrameCount).c_str(), "wb"))
			{
				std::fwrite(eeMem->Main, 1, Ps2MemSize::MainRam, fp);
				std::fclose(fp);
			}
		}
		// ZDXSV_EE_CLAMP=addr,max[,lo,hi][;addr,...]: u16 at addr set to max whenever above it (and in lo..hi);
		// a function of state, so rollback-safe. 0x117f566 = 出撃準備 frames left (3599 at
		// entry; 1800 and 0xffff in earlier phases; tools/zdxsv/ramcount.py).
		static const auto clamps = [] {
			std::vector<std::tuple<u32, u32, u32, u32>> v;
			for (const char* e = Zdxsv::TestEnv("ZDXSV_EE_CLAMP"); e && *e;)
			{
				u32 a, m, lo = 0, hi = 0xffff;
				if (std::sscanf(e, "%x,%u,%u,%u", &a, &m, &lo, &hi) >= 2 && a + 2 <= Ps2MemSize::MainRam)
					v.emplace_back(a, m, lo, hi);
				e = std::strchr(e, ';');
				e = e ? e + 1 : nullptr;
			}
			return v;
		}();
		for (const auto& [caddr, cmax, clo, chi] : clamps)
		{
			u16& v = *reinterpret_cast<u16*>(eeMem->Main + (caddr & ~1u));
			if (v > cmax && v >= clo && v <= chi)
			{
				static int logged = 0;
				if (!g_ggpo_in_rollback && logged++ < 6)
					Console.WriteLn("ZdxsvGgpo: EE clamp %x %u -> %u at vsync %u", caddr, v, cmax, g_FrameCount);
				v = static_cast<u16>(cmax);
			}
		}
		if (!s_net_trace || g_ggpo_in_rollback || !s_game_gp)
			return;
		const u32 base = *reinterpret_cast<const u32*>(eeMem->Main + ((s_game_gp - 0x5b28) & 0x1fffffc)) & 0x1ffffff;
		if (base == 0 || base + 0x4d8 + 64 > Ps2MemSize::MainRam)
			return;
		static u8 last[36];
		u8 cur[36];
		for (int p = 0; p < 4; p++)
			std::memcpy(cur + 8 * p, eeMem->Main + base + 0x4d8 + 16 * p, 8);
		std::memcpy(cur + 32, &base, 4);
		if (std::memcmp(cur, last, 36) == 0)
			return;
		std::memcpy(last, cur, 36);
		std::string hex;
		for (int i = 0; i < 32; i++)
			hex += (i % 8 == 0 ? " " : "") + fmt::format("{:02x}", cur[i]);
		std::fprintf(s_net_trace, "%u I%s %x\n", g_FrameCount, hex.c_str(), base);
	}
} // namespace Zdxsv
