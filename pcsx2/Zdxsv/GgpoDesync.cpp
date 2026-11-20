// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Online desync check: each peer sends the state hash of a sampled frame in its input trailer and compares the
// peers' hashes with its own. A mismatch is logged and kept as a suspected desync; the battle goes on.

#include "Zdxsv/GgpoShared.h"

#include "common/Console.h"

namespace Zdxsv
{
	DesyncState s_desync;
	// ZDXSV_DESYNC_TEST=1: this peer's samples are altered, so its peers and it report every check as a mismatch.
	const u32 s_desync_test_xor = [] {
		const char* e = Zdxsv::TestEnv("ZDXSV_DESYNC_TEST");
		return e && e[0] == '1' ? 0x5a5a5au : 0u;
	}();

	namespace
	{
		bool DesyncOn()
		{
			return s_session && !s_opt.sync && s_opt.players > 1;
		}
		int Confirmed()
		{
			int c = -1;
			if (ggpo_get_last_confirmed_frame(s_session, &c) != GGPO_OK)
				return -1;
			return std::min(c, s_battle.last_real);
		}
		// The ring entry of sampled frame h, null if it was not sampled or is overwritten.
		const u32* SampleAt(int h)
		{
			const int i = (h / DESYNC_PERIOD) % static_cast<int>(std::size(s_desync.frame));
			return h >= 0 && s_desync.frame[i] == h + 1 ? &s_desync.hash[i] : nullptr;
		}
		// The trailer's sampled frame: the last multiple of the period at or before f with the trailer's tag.
		int TagFrame(int f, u8 tag)
		{
			const int n = f / DESYNC_PERIOD;
			const int back = ((n % 255) + 1 - tag + 255) % 255;
			return (n - back) * DESYNC_PERIOD;
		}
	} // namespace

	// The state at the start of frame f, before its inputs apply (the replay's state_hashes value). A rollback
	// samples a frame again, so an entry is final once every input before it is confirmed.
	void DesyncSample(int f)
	{
		if (!DesyncOn() || f < 0 || f % DESYNC_PERIOD != 0)
			return;
		const int i = (f / DESYNC_PERIOD) % static_cast<int>(std::size(s_desync.frame));
		s_desync.frame[i] = f + 1;
		s_desync.hash[i] = ReplayStateHash() ^ s_desync_test_xor;
	}

	// Writes the newest final sample into the trailer. Called only for an input that differs from the previous one
	// anyway: a trailer change alone would make every peer's prediction of this input miss.
	void DesyncClaim(NetInput& in)
	{
		if (!DesyncOn() || in.len > DESYNC_TRAILER_AT)
			return;
		const int c = Confirmed();
		if (c < 0)
			return;
		const int h = (c + 1) / DESYNC_PERIOD * DESYNC_PERIOD;
		const u32* v = SampleAt(h);
		if (!v)
			return;
		u8* t = in.data + DESYNC_TRAILER_AT;
		t[0] = static_cast<u8>((h / DESYNC_PERIOD) % 255 + 1);
		t[1] = static_cast<u8>(*v);
		t[2] = static_cast<u8>(*v >> 8);
		t[3] = static_cast<u8>(*v >> 16);
	}

	// Compares each peer's trailer with the own sample of that frame, once per peer and sample. A predicted input
	// repeats a real one, so its trailer is that peer's own claim too.
	void DesyncCheck(const NetInput* in, int f)
	{
		if (!DesyncOn())
			return;
		const int c = Confirmed();
		for (int p = 0; p < s_opt.players; p++)
		{
			const u8* t = in[p].data + DESYNC_TRAILER_AT;
			// A trailer keeps its claim until the peer's input changes; after 255 periods its tag names a newer frame.
			if (p == s_battle.me || in[p].len > DESYNC_TRAILER_AT || t[0] == 0 || t[0] == s_desync.checked_tag[p])
				continue;
			const int h = TagFrame(f, t[0]);
			if (h <= s_desync.checked[p] || h > c + 1)
				continue;
			const u32* v = SampleAt(h);
			if (!v)
				continue;
			s_desync.checked[p] = h;
			s_desync.checked_tag[p] = t[0];
			s_desync.checks++;
			const u32 theirs = t[1] | t[2] << 8 | t[3] << 16;
			if ((*v & 0xffffff) == theirs)
				continue;
			s_desync.mismatches++;
			if (s_desync.first[p] < 0)
			{
				s_desync.first[p] = h;
				Console.Error("ZdxsvGgpo: suspected desync with position %d at frame %d (state hash %06x, theirs %06x)", p + 1, h,
					*v & 0xffffff, theirs);
			}
		}
	}

	void DesyncReport()
	{
		if (s_desync.checks == 0)
			return;
		Console.WriteLn("ZdxsvGgpo: desync checks %u mismatches %u", s_desync.checks, s_desync.mismatches);
	}

	// Battle report lines: the checks done and the first sampled frame any peer's hash differed at (-1 = none).
	std::string DesyncReportLines()
	{
		int first = -1;
		for (const int h : s_desync.first)
			if (h >= 0 && (first < 0 || h < first))
				first = h;
		return "desync_checks=" + std::to_string(s_desync.checks) + "\ndesync_frame=" + std::to_string(first) + "\n";
	}
} // namespace Zdxsv
