// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Synctest harness (ZDXSV_GGPO without net=1): state diffs of rerun frames, random pad input.

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{
	std::map<int, Sample> s_first;
	bool s_rerun = false; // the save is of a rerun frame
	int s_diff_logged = 0;

	// hash=pw / pos: the differing u32 words, as player work p + offset (pw) or PosRng byte offset (pos).
	void DiffRaw(int frame, const std::vector<u8>& first, const std::vector<u8>& raw)
	{
		for (size_t w = 0; w + 4 <= std::min(first.size(), raw.size()) && s_diff_logged < 60; w += 4)
		{
			u32 x, y;
			std::memcpy(&x, &first[w], 4);
			std::memcpy(&y, &raw[w], 4);
			if (x == y)
				continue;
			if (s_hash == Hash::Pw && w == 4 * PW_SIZE)
				Console.WriteLn("ZdxsvGgpo: DIFF frame %d rng: %08x -> %08x", frame, x, y);
			else if (s_hash == Hash::Pw)
				Console.WriteLn("ZdxsvGgpo: DIFF frame %d player %zu +0x%04zx: %08x -> %08x", frame, w / PW_SIZE, w % PW_SIZE, x, y);
			else
				Console.WriteLn("ZdxsvGgpo: DIFF frame %d posrng +%zu: %08x -> %08x", frame, w, x, y);
			s_diff_logged++;
		}
	}

	void Diff(int frame, const Sample& first, const std::vector<u64>& pages, const std::vector<u8>& state)
	{
		if (s_diff_logged >= 60)
			return;
		for (size_t i = 0; i < pages.size() && s_diff_logged < 60; i++)
		{
			if (first.pages[i] != pages[i])
			{
				Console.WriteLn("ZdxsvGgpo: DIFF frame %d EE page 0x%08zx", frame, i * PAGE_SIZE);
				s_diff_logged++;
			}
		}
		size_t last = 0;
		bool any = false;
		for (size_t i = 0; i < std::min(state.size(), first.state.size()) && s_diff_logged < 60; i++)
		{
			if (first.state[i] == state[i])
				continue;
			if (!any || i > last + 8)
			{
				Console.WriteLn("ZdxsvGgpo: DIFF frame %d state offset %zu = %s: %02x -> %02x", frame, i,
					SaveState_DeltaDescribe(first.state, i).c_str(), first.state[i], state[i]);
				// Words around it, first run / rerun (pointers name the code that wrote them).
				const size_t w0 = (i & ~size_t{3}) >= 16 ? (i & ~size_t{3}) - 16 : 0;
				std::string a, b;
				for (size_t w = w0; w + 4 <= std::min(state.size(), first.state.size()) && w < w0 + 36; w += 4)
				{
					u32 x, y;
					std::memcpy(&x, &first.state[w], 4);
					std::memcpy(&y, &state[w], 4);
					a += fmt::format(" {:08x}", x);
					b += fmt::format(" {:08x}", y);
				}
				Console.WriteLn("ZdxsvGgpo: DIFF words from offset %zu first:%s", w0, a.c_str());
				Console.WriteLn("ZdxsvGgpo: DIFF words from offset %zu rerun:%s", w0, b.c_str());
				s_diff_logged++;
			}
			any = true;
			last = i;
		}
		if (state.size() != first.state.size())
			Console.WriteLn("ZdxsvGgpo: DIFF frame %d state size %zu -> %zu", frame, first.state.size(), state.size());
	}

	// ZDXSV_RAND_INPUT=seed: every 5 frames new buttons (each 1/4, never START / SELECT) and left
	// stick, from a generator seeded by (seed, position).
	Input RandInput()
	{
		static std::mt19937 rng;
		static Input in = {};
		static int calls = 0;
		if (calls == 0)
		{
			std::seed_seq seq{static_cast<u32>(std::atoi(s_rand_env)), static_cast<u32>(s_net_me)};
			rng.seed(seq);
		}
		if (calls++ % 5 == 0)
		{
			using I = PadDualshock2::Inputs;
			in.buttons = static_cast<u16>(rng() & rng() & ~((1u << I::PAD_START) | (1u << I::PAD_SELECT)));
			in.lx = static_cast<u8>(rng());
			in.ly = static_cast<u8>(rng());
			in.rx = in.ry = Pad::ANALOG_NEUTRAL_POSITION;
		}
		return in;
	}
} // namespace Zdxsv
