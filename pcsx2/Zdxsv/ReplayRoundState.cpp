// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Replay round states (replay.proto round_states): the state a later battle load end needs on top of the first one's.

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{
	namespace
	{
		constexpr u32 PW_BASE = 0x839330; // player work, one per position
		constexpr u32 PW_SIZE = 0x2200;
		// EE RAM the game carries from one round to the next: RNG B, the MS cache, the MS slot table, the battle block
		// before the player work, the per-position previous-MS table, and per player work its MS id, team and round.
		constexpr std::pair<u32, u32> ROUND_STATE_EE[] = {
			{0x6d793c, 4},
			{0x6f2b6c, 6},
			{0x6f3010, 0x20},
			{0x838fb0, PW_BASE - 0x838fb0},
			{0xba4703, 0x15},
			{PW_BASE + 0x21d0, 8},
			{PW_BASE + PW_SIZE + 0x21d0, 8},
			{PW_BASE + PW_SIZE * 2 + 0x21d0, 8},
			{PW_BASE + PW_SIZE * 3 + 0x21d0, 8},
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
				rb.ms.go ? 1 : 0};
		}
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
		using namespace Pb;
		std::vector<uint8_t> pb;
		PutInt(pb, 1, s.frame);
		PutBytes(pb, 2, s.ee.data(), s.ee.size());
		PutPackedInts(pb, 3, RoundStateHle(s.rb));
		PutBytes(pb, 4, s.rb.net_rx.data(), s.rb.net_rx.size());
		return pb;
	}

	bool RoundStateRead(const uint8_t* b, size_t n, RoundState& out)
	{
		std::vector<int64_t> hle;
		int64_t frame = -1;
		Pb::Reader rd{b, b + n};
		if (!rd.Fields([&](uint32_t f, uint32_t w, uint64_t x, const uint8_t* rb, size_t rn) {
				if (f == 1 && w == 0)
					frame = static_cast<int64_t>(x);
				else if (f == 2 && w == 2)
					out.ee.assign(rb, rb + rn);
				else if (f == 3)
					return Pb::ReadInts(w, x, rb, rn, hle);
				else if (f == 4 && w == 2 && rn <= 0x10000)
					out.rb.net_rx.assign(rb, rb + rn);
				return true;
			}))
			return false;
		if (frame < 1 || frame > INT_MAX || out.ee.size() != RoundStateEeSize() || hle.size() != RoundStateHle({}).size())
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
