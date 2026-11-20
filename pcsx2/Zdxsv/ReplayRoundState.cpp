// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Replay round states (replay.proto round_states): the state a later battle load end needs on top of the first one's.

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{
	namespace
	{
		constexpr u32 PW_ROUND = 0x1f28; // per player work: MS id, team, round number
		// MS-select menu state (cursor, entry count, selected MS) of the select work at 0x117f424: a cursor left from
		// round 1 on the random entry makes the pick draw RNG B
		constexpr u32 MS_SELECT_WORK = 0x117f480;
		// slot of each MS id (0xff = none) in the two blocks the game clears with MS_SLOTS (13 bytes at 0x6f3000, 0x37
		// at 0x6f3010); a battle load reads it by the MS id
		constexpr u32 MS_SLOT_OF = 0x6f3000, MS_SLOT_OF_N = 0x47;
		// rounds started in this game (+1 at each MS select's end)
		constexpr u32 ROUND_COUNT = 0xba473a;
		// four 0x14-byte result records (the two team records first: wins, losses, ...)
		constexpr u32 RESULT_RECORDS = 0xc227f0, RESULT_RECORDS_N = 0x50;
		// EE RAM the game carries from one round to the next: RNG B, the MS cache, the MS slot table, the battle block
		// before the player work, the per-position previous-MS table, the MS-select menu state, the round count, the
		// result records, and per player work its MS id, team and round.
		constexpr std::pair<u32, u32> ROUND_STATE_EE[] = {
			{RNG_B, 4},
			{MS_SLOTS, MS_SLOTS_N},
			{MS_SLOT_OF, MS_SLOT_OF_N},
			{0x838fb0, PW_BASE - PW_POS - 0x838fb0},
			{0xba4703, 0x16},
			{MS_SELECT_WORK, 8},
			{ROUND_COUNT, 1},
			{RESULT_RECORDS, RESULT_RECORDS_N},
			{PW_BASE + PW_ROUND, 8},
			{PW_BASE + PW_SIZE + PW_ROUND, 8},
			{PW_BASE + PW_SIZE * 2 + PW_ROUND, 8},
			{PW_BASE + PW_SIZE * 3 + PW_ROUND, 8},
		};

		constexpr size_t RoundStateEeSize()
		{
			size_t n = 0;
			for (const auto& [a, len] : ROUND_STATE_EE)
				n += len;
			return n;
		}

		std::vector<int64_t> RoundStateHle(const RollbackState& rb)
		{
			return {static_cast<int64_t>(rb.net_pos), rb.end_frame, rb.ps.n, rb.ps.rel, rb.ps.hold ? 1 : 0, rb.ps.go ? 1 : 0,
				rb.zds_seen[0], rb.zds_seen[1], rb.zds_seen[2], rb.zds_seen[3], rb.zds_rel, rb.ms.n, rb.ms.rel, rb.ms.hold ? 1 : 0,
				rb.ms.go ? 1 : 0, rb.re.n, rb.re.rel, rb.re.hold ? 1 : 0, rb.re.go ? 1 : 0};
		}

		// Recorded without the round-end barrier: its fields are absent.
		constexpr size_t ROUND_STATE_HLE_NO_RE = 15;
	} // namespace

	RoundState RoundStateTake(int f)
	{
		RoundState s;
		s.frame = f;
		s.ee.reserve(RoundStateEeSize());
		for (const auto& [a, len] : ROUND_STATE_EE)
			s.ee.insert(s.ee.end(), eeMem->Main + a, eeMem->Main + a + len);
		s.rb = s_rb;
		return s;
	}

	std::vector<uint8_t> RoundStatePb(const RoundState& s)
	{
		std::vector<uint8_t> pb;
		PbPutInt(pb, 1, s.frame);
		PbPutBytes(pb, 2, s.ee.data(), s.ee.size());
		PbPutPackedInts(pb, 3, RoundStateHle(s.rb));
		PbPutBytes(pb, 4, s.rb.net_rx.data(), s.rb.net_rx.size());
		return pb;
	}

	bool RoundStateRead(const uint8_t* b, size_t n, RoundState& out)
	{
		std::vector<int64_t> hle;
		int64_t frame = -1;
		PbReader rd{b, b + n};
		if (!rd.Fields([&](uint32_t f, uint32_t w, uint64_t x, const uint8_t* rb, size_t rn) {
				if (f == 1 && w == 0)
					frame = static_cast<int64_t>(x);
				else if (f == 2 && w == 2)
					out.ee.assign(rb, rb + rn);
				else if (f == 3)
					return PbReadInts(w, x, rb, rn, hle, RoundStateHle({}).size());
				else if (f == 4 && w == 2 && rn <= 0x10000)
					out.rb.net_rx.assign(rb, rb + rn);
				return true;
			}))
			return false;
		if (frame < 1 || frame > INT_MAX || out.ee.size() != RoundStateEeSize() ||
			(hle.size() != RoundStateHle({}).size() && hle.size() != ROUND_STATE_HLE_NO_RE))
			return false;
		const auto u8in = [](int64_t v) { return v >= 0 && v <= 0xff; };
		if (hle[0] < 0 || hle[0] > INT_MAX || hle[1] < -1 || hle[1] > INT_MAX || !std::all_of(hle.begin() + 2, hle.end(), u8in))
			return false;
		RollbackState& rb = out.rb;
		out.frame = static_cast<int>(frame);
		rb.net_pos = static_cast<size_t>(hle[0]);
		rb.end_frame = static_cast<int>(hle[1]);
		rb.ps = {static_cast<u8>(hle[2]), static_cast<u8>(hle[3]), hle[4] != 0, hle[5] != 0};
		for (int p = 0; p < 4; p++)
			rb.zds_seen[p] = static_cast<int>(hle[6 + p]);
		rb.zds_rel = static_cast<int>(hle[10]);
		rb.ms = {static_cast<u8>(hle[11]), static_cast<u8>(hle[12]), hle[13] != 0, hle[14] != 0};
		if (hle.size() > ROUND_STATE_HLE_NO_RE)
			rb.re = {static_cast<u8>(hle[15]), static_cast<u8>(hle[16]), hle[17] != 0, hle[18] != 0};
		return true;
	}

	bool RoundStateApply(const RoundState& s, const std::vector<NetInput>& inputs)
	{
		const int players = s_opt.players;
		if (players <= 0 || players > GGPO_MAX_PLAYERS || s.ee.size() != RoundStateEeSize() ||
			inputs.size() < static_cast<size_t>(s.frame) * players)
			return false;
		size_t at = 0;
		for (const auto& [a, len] : ROUND_STATE_EE)
		{
			std::memcpy(eeMem->Main + a, s.ee.data() + at, len);
			at += len;
		}
		// the key holds another round's MS assets (sounds too): the battle load reloads every MS
		MsSlotsClear();
		s_rb = s.rb;
		s_rings = {};
		for (int p = 0; p < players; p++)
			s_battle.zds_k3[p].clear();
		int seen[GGPO_MAX_PLAYERS] = {};
		for (int f = 0; f < s.frame; f++)
		{
			for (int p = 0; p < players; p++)
			{
				const NetInput& in = inputs[static_cast<size_t>(f) * players + p];
				std::memcpy(s_rings.zd_hist[f & 127][p], &in.pad, sizeof(s_rings.zd_hist[f & 127][p]));
				const bool fresh = in.seq != (f == 0 ? decltype(in.seq){} : inputs[static_cast<size_t>(f - 1) * players + p].seq);
				const u8* d = in.data;
				const u32 len = std::min<u32>(in.len, sizeof(in.data));
				if (fresh)
					for (u32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
						if ((d[i + 1] >> 4) == 3)
							s_battle.zds_k3[p].emplace_back(d + i, d + i + d[i]);
				s_rings.net_seq_at[f & 63][p] = in.seq;
			}
		}
		for (int p = 0; p < players; p++)
			if (s_battle.zds_k3[p].size() < static_cast<size_t>(s.rb.zds_seen[p]))
				s_battle.zds_k3[p].resize(s.rb.zds_seen[p]);
		return true;
	}
} // namespace Zdxsv
