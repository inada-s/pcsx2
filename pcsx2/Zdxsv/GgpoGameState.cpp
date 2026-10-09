// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Reads of the game's RAM for sync checks and replays: player work hash, RNG, tick state, round records.

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{
	namespace
	{
		// Player work words left out of the hash: machine-local scratch and the own position's HUD gauge.
		constexpr u32 PW_MASK[] = {0x274, 0x2b4, 0x1e64, 0x1e74, 0x1e84, 0x1e94, 0x214c};
		// Bits left out of the hash: viewer-team, in-view and own-player-only fields, which differ in sync.
		constexpr std::pair<u32, u32> PW_MASK_BITS[] = {{0x58, 0x100}, {0x68, 0x300}, {0x9c, 0x110000}, {0x2004, ~0u}, {0x2068, 1}, {0x2074, 0x101}, {0x2088, 0xff},
			{0xcc, 0xffff}, {0x90, 0xffff0000}};
		// XXH3 of player p's work with the fields above masked (synctest hash=pw).
		u64 PwHash(u32 p)
		{
			std::array<u8, PW_SIZE> w;
			std::memcpy(w.data(), &eeMem->Main[PW_BASE + PW_SIZE * p], PW_SIZE);
			for (u32 o : PW_MASK)
				std::memset(&w[o], 0, 4);
			for (const auto& [o, bits] : PW_MASK_BITS)
			{
				u32 v;
				std::memcpy(&v, &w[o], 4);
				v &= ~bits;
				std::memcpy(&w[o], &v, 4);
			}
			return XXH3_64bits(w.data(), PW_SIZE);
		}
		std::array<u8, 4 * 12 + 4> PosRng()
		{
			std::array<u8, 4 * 12 + 4> b;
			for (u32 p = 0; p < 4; p++)
				std::memcpy(&b[12 * p], &eeMem->Main[PW_BASE + PW_SIZE * p + PW_POS], 12);
			std::memcpy(&b[48], &eeMem->Main[RNG_A], 2);
			std::memcpy(&b[50], &eeMem->Main[RNG_B], 2);
			return b;
		}
		// Team result records, equal on all peers (team 1 = position 0's side): u8 wins at +0, u8 losses at +1. A round
		// adds a win to the winner and a loss to the loser, a time-up a loss to both (a draw), in one frame.
		static constexpr u32 TEAM_RECORD[2] = {0xc227f0, 0xc22804};
	} // namespace

	// ZDXSV_PW_HASH=1: per GGPO frame, each player's position hash and the game RNGs, written as
	// `H frame h0 h1 h2 h3 rng` to NET_TRACE. RNG A also takes machine-local draws, so it may differ in sync.
	const bool s_pw_hash = [] {
		const char* e = Zdxsv::TestEnv("ZDXSV_PW_HASH");
		return e && e[0] == '1';
	}();
	std::map<int, std::array<u64, 5>> s_pw;
	// The replay file's per-frame state hash (replay.proto state_hashes): the 4 masked player works (PwHash) and
	// RNG B. RNG A is left out: its machine-local draws would make the files of one battle's positions differ.
	u32 ReplayStateHash()
	{
		u64 h[5];
		for (u32 p = 0; p < 4; p++)
			h[p] = PwHash(p);
		u16 b;
		std::memcpy(&b, &eeMem->Main[RNG_B], 2);
		h[4] = b;
		const u64 x = XXH3_64bits(h, sizeof(h));
		return static_cast<u32>(x ^ (x >> 32));
	}
	// u16 RNG A << 16 | u16 RNG B (replay.proto start_rng, load_rngs)
	u32 GameRng()
	{
		u16 a, b;
		std::memcpy(&a, &eeMem->Main[RNG_A], 2);
		std::memcpy(&b, &eeMem->Main[RNG_B], 2);
		return (static_cast<u32>(a) << 16) | b;
	}
	u32 RoundRecord()
	{
		u32 v = 0;
		for (int t = 0; t < 2; t++)
			v |= static_cast<u32>(eeMem->Main[TEAM_RECORD[t]] | eeMem->Main[TEAM_RECORD[t] + 1] << 8) << (t * 16);
		return v;
	}
	// u8 MS id per player work (= position; 0 = Gundam), set from the battle setup at each round start (0x2bc6d0)
	static constexpr u32 PW_MS_ID = 0x1f2a;
	u32 MsIds()
	{
		u32 v = 0;
		for (int p = 0; p < 4; p++)
			v |= static_cast<u32>(eeMem->Main[PW_BASE + PW_SIZE * p + PW_MS_ID]) << (p * 8);
		return v;
	}
	// The round ended between records a and b: the winning team 1 / 2, -1 = a draw (losses only); 0 = none
	int RoundResult(u32 a, u32 b)
	{
		int up = 0, win = 0;
		for (int i = 0; i < 4; i++)
		{
			const int d = static_cast<int>((b >> (i * 8)) & 0xff) - static_cast<int>((a >> (i * 8)) & 0xff);
			up += d;
			if (d > 0 && i % 2 == 0)
				win = i / 2 + 1;
		}
		return up <= 0 ? 0 : win ? win : -1;
	}
	// ZDXSV_PW_DUMP=file: every save appends (s32 frame, 4 * PW_SIZE bytes of player work); rollback
	// re-saves a frame, the last record wins (`tests/zdxsv/pwdiff.py` finds the fields behind H mismatches).
	std::FILE* s_pw_dump = [] {
		const char* p = Zdxsv::TestEnv("ZDXSV_PW_DUMP");
		return p ? std::fopen(p, "wb") : nullptr;
	}();

	// sync=1 part of SaveGameState: checksum, synctest diff samples.
	void HashSave(int frame, int* checksum)
	{
		if (s_hash != Hash::Full)
		{
			// hash=pw / pos: the sample keeps the raw bytes (4 player works / PosRng) to name what differed.
			std::vector<u8> raw;
			u64 hash;
			if (s_hash == Hash::Pw)
			{
				raw.assign(&eeMem->Main[PW_BASE], &eeMem->Main[PW_BASE + 4 * PW_SIZE]);
				raw.insert(raw.end(), &eeMem->Main[RNG_A], &eeMem->Main[RNG_A + 2]);
				raw.insert(raw.end(), &eeMem->Main[RNG_B], &eeMem->Main[RNG_B + 2]);
				u64 h[5];
				for (u32 p = 0; p < 4; p++)
					h[p] = PwHash(p);
				h[4] = XXH3_64bits(&raw[4 * PW_SIZE], 4);
				hash = XXH3_64bits(h, sizeof(h));
			}
			else
			{
				const auto b = PosRng();
				raw.assign(b.begin(), b.end());
				hash = XXH3_64bits(b.data(), b.size());
			}
			*checksum = static_cast<int>(hash ^ (hash >> 32));
			if (s_rerun)
			{
				const auto first = s_first.find(frame);
				if (first != s_first.end())
					DiffRaw(frame, first->second.state, raw);
			}
			else
			{
				s_first[frame].state = std::move(raw);
				while (!s_first.empty() && s_first.begin()->first < frame - 16)
					s_first.erase(s_first.begin());
			}
			return;
		}
		const std::vector<u8>* state = Zdxsv::DeltaStateGetState(frame);
		Sample sample;
		Zdxsv::DeltaStateHashRam(sample.pages);
		const u64 ram_hash = XXH3_64bits(sample.pages.data(), sample.pages.size() * sizeof(u64));
		const u64 state_hash = Zdxsv::DeltaStateHash(*state);
		const u64 hash = ram_hash ^ state_hash;
		*checksum = static_cast<int>(hash ^ (hash >> 32));
		std::vector<u8> masked = *state;
		Zdxsv::DeltaStateMaskScratch(masked);
		if (s_rerun)
		{
			const auto first = s_first.find(frame);
			if (first != s_first.end())
				Diff(frame, first->second, sample.pages, masked);
		}
		else
		{
			sample.state = std::move(masked);
			s_first[frame] = std::move(sample);
			while (!s_first.empty() && s_first.begin()->first < frame - 16)
				s_first.erase(s_first.begin());
		}
	}
} // namespace Zdxsv
