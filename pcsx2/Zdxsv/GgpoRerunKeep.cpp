// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Rerun keep: EE RAM ranges a rollback does not roll back (docs/zdxsv/rollback.md).

#include "Zdxsv/GgpoShared.h"

#include <cmath>

namespace Zdxsv
{
	namespace
	{
		// Per-player bone matrix slots (4 x 0x65d0), written only by the render callbacks. A draw reads
		// the previous frame's slot, so rerun frames that skip the callbacks would leave a stale pose.
		u32 BoneSlots() { return GgpoAddrs().bone_slots; }
		u32 BoneSlotsEnd() { return GgpoAddrs().bone_slots + 4 * 0x65d0; }

		// Game object list (obj_list_gp_offset): each object holds its handler at +0xa8 and
		// the next object at +0xac.
		constexpr u32 OBJ_HANDLER = 0xa8, OBJ_NEXT = 0xac;
		constexpr u32 OBJ_MAX = 4096, OBJ_MIN_SIZE = 0x400;
		// Object types whose fields here are advanced only by the render callbacks.
		// An address is reused by other object types, so a field is kept only for an object with the
		// same address and handler before and after the load.
		struct KeptObjectType
		{
			u32 handler;
			std::array<std::pair<u32, u32>, 2> fields;
		};
		std::array<KeptObjectType, 2> KeptTypes()
		{
			const u32* h = GgpoAddrs().kept_types;
			return {{
				{h[0], {{{0x0, 0xa0}, {0x150, 0x1d0}}}}, // beam saber trail history
				{h[1], {{{0xb0, 0x1f0}, {0x0, 0x0}}}}, // an effect type
			}};
		}

		// Player objects (player_handlers) copy part of their work into their bone slot at the start of each draw.
		// That part follows the rerun game state, so it is written again after the load and each rerun frame.
		constexpr u32 PLAYER_POSE = 0x20, PLAYER_ANGLES = 0x50;
		constexpr u32 PLAYER_BLOCK_SIZE = 0x40;
		constexpr u32 SLOT_SIZE = 0x65d0, SLOT_POSE = 0x220, SLOT_ANGLES = 0x200, SLOT_BLOCKS = 0x240;
		constexpr u32 SLOT_BLOCK_STRIDE = 0x240, SLOT_BLOCK_OFFSET = 0xc0, SLOT_MODEL = 0x65bc, MODEL_BLOCK_COUNT = 7;
		constexpr u32 SLOT_MAX_BLOCKS = (SLOT_MODEL - SLOT_BLOCKS - SLOT_BLOCK_OFFSET) / SLOT_BLOCK_STRIDE;
		constexpr u32 ANGLE_UNIT = 0x38c90fda; // 2 pi / 65536

		// Parts attached to a player (part_handler) draw their matrix into a bone block of the player's slot past the blocks
		// RebuildPlayerSlot copies; the player's next draw reads it. Their draws also run in skipped rerun
		// frames, through the object draw walker alone.
		constexpr u32 DRAW_LIST_MAX = 64;

		// ZDXSV_RERUN_PARTS=0: no draws in skipped rerun frames.
		const bool s_keep_parts = [] {
			const char* e = TestEnv("ZDXSV_RERUN_PARTS");
			return !(e && e[0] == '0');
		}();

		struct RerunKeepConfig
		{
			std::vector<std::pair<u32, u32>> ranges;
			bool any_phase = false; // ranges from ZDXSV_RERUN_KEEP: kept outside battle play too
		};

		// ZDXSV_RERUN_KEEP=0: off; =lo-hi,... (hex, end exclusive): these ranges instead of the bone slots.
		const RerunKeepConfig s_keep_config = [] {
			RerunKeepConfig c;
			const char* e = TestEnv("ZDXSV_RERUN_KEEP");
			if (!e)
			{
				c.ranges.emplace_back(BoneSlots(), BoneSlotsEnd());
				return c;
			}
			c.any_phase = true;
			while (*e)
			{
				char* end;
				const unsigned long lo = std::strtoul(e, &end, 16);
				if (end == e || *end != '-')
					break;
				const unsigned long hi = std::strtoul(end + 1, &end, 16);
				if (lo < hi && hi <= Ps2MemSize::MainRam)
					c.ranges.emplace_back(static_cast<u32>(lo), static_cast<u32>(hi));
				if (*end != ',')
					break;
				e = end + 1;
			}
			return c;
		}();

		struct KeptObject
		{
			u32 addr;
			u32 handler;
			std::vector<u8> data;
		};

		struct RerunKeep
		{
			std::vector<u8> saved;
			std::vector<KeptObject> objects;
			bool armed = false;
			bool rebuild_slots = false;
			bool logged = false;
			bool parts_logged = false;
			bool walked = false;
		};
		RerunKeep s_keep;

		bool RerunKeepPhase()
		{
			return s_keep_config.any_phase || eeMem->Main[GgpoAddrs().tick_state] == TICK_PLAY;
		}

		u32 ReadU32(u32 addr)
		{
			u32 v;
			std::memcpy(&v, &eeMem->Main[addr], sizeof(v));
			return v;
		}

		template <typename F>
		void ForEachObject(F&& fn)
		{
			// gp is read now: the game keeps one gp, needs proper testing outside battle play.
			const u32 head = (cpuRegs.GPR.n.gp.UL[0] - GgpoAddrs().obj_list_gp_offset) & (Ps2MemSize::MainRam - 4);
			u32 obj = ReadU32(head);
			for (u32 i = 0; obj && i < OBJ_MAX; i++)
			{
				obj &= Ps2MemSize::MainRam - 1;
				if (obj % 4 || obj > Ps2MemSize::MainRam - OBJ_MIN_SIZE)
					break;
				fn(obj, ReadU32(obj + OBJ_HANDLER));
				obj = ReadU32(obj + OBJ_NEXT);
			}
		}

		template <typename F>
		void ForEachKeptObject(F&& fn)
		{
			ForEachObject([&fn](u32 obj, u32 handler) {
				for (const KeptObjectType& type : KeptTypes())
				{
					if (handler == type.handler)
						fn(obj, type);
				}
			});
		}

		// The game's s16 angle to radians, with the EE FPU's round-toward-zero product.
		float AngleToRadians(u32 v)
		{
			float unit;
			std::memcpy(&unit, &ANGLE_UNIT, sizeof(unit));
			const double exact = static_cast<double>(static_cast<s16>(v)) * unit;
			float r = static_cast<float>(exact);
			if (std::fabs(static_cast<double>(r)) > std::fabs(exact))
				r = std::nextafter(r, 0.0f);
			return r;
		}

		bool IsPlayerHandler(u32 handler)
		{
			const u32* h = GgpoAddrs().player_handlers;
			return std::find(h, h + 3, handler) != h + 3;
		}

		u32 PlayerSlot(u32 obj)
		{
			const s8 index = static_cast<s8>(eeMem->Main[obj + GgpoAddrs().player_slot_index]);
			if (index < 0 || index >= 4 || obj + GgpoAddrs().pw_size > Ps2MemSize::MainRam)
				return 0;
			return BoneSlots() + index * SLOT_SIZE;
		}

		u32 CopiedBlocks(u32 slot)
		{
			const u32 model = ReadU32(slot + SLOT_MODEL) & (Ps2MemSize::MainRam - 1);
			if (model + MODEL_BLOCK_COUNT >= Ps2MemSize::MainRam)
				return 0;
			return std::min<u32>({eeMem->Main[model + MODEL_BLOCK_COUNT], SLOT_MAX_BLOCKS,
				(GgpoAddrs().pw_size - GgpoAddrs().player_blocks) / PLAYER_BLOCK_SIZE});
		}

		void RebuildPlayerSlot(u32 obj)
		{
			const u32 slot = PlayerSlot(obj);
			if (!slot)
				return;
			std::memcpy(&eeMem->Main[slot + SLOT_POSE], &eeMem->Main[obj + PLAYER_POSE], 12);
			const float w = 1.0f; // vmove.w from vf0
			std::memcpy(&eeMem->Main[slot + SLOT_POSE + 12], &w, sizeof(w));
			for (u32 i = 0; i < 3; i++)
			{
				const float a = AngleToRadians(ReadU32(obj + PLAYER_ANGLES + i * 4));
				std::memcpy(&eeMem->Main[slot + SLOT_ANGLES + i * 4], &a, sizeof(a));
			}
			const u32 n = CopiedBlocks(slot);
			for (u32 i = 0; i < n; i++)
				std::memcpy(&eeMem->Main[slot + SLOT_BLOCKS + i * SLOT_BLOCK_STRIDE + SLOT_BLOCK_OFFSET],
					&eeMem->Main[obj + GgpoAddrs().player_blocks + i * PLAYER_BLOCK_SIZE], PLAYER_BLOCK_SIZE);
		}

		// The next frame reads what the skipped draw would have copied.
		void RebuildPlayerSlots()
		{
			if (!s_keep.rebuild_slots)
				return;
			ForEachObject([](u32 obj, u32 handler) {
				if (IsPlayerHandler(handler))
					RebuildPlayerSlot(obj);
			});
		}

		// What the players' draws copied into their slots by the loaded frame. A culled player is not
		// drawn and its copy stays from an older frame; the next draw after the cull reads it.
		struct CopiedRange
		{
			u32 addr, len;
		};
		std::vector<CopiedRange> s_copied_ranges;
		std::vector<u8> s_copied;

		void SaveCopiedRanges()
		{
			s_copied_ranges.clear();
			s_copied.clear();
			ForEachObject([](u32 obj, u32 handler) {
				const u32 slot = IsPlayerHandler(handler) ? PlayerSlot(obj) : 0;
				if (!slot)
					return;
				s_copied_ranges.push_back({slot + SLOT_ANGLES, 12});
				s_copied_ranges.push_back({slot + SLOT_POSE, 16});
				const u32 n = CopiedBlocks(slot);
				for (u32 i = 0; i < n; i++)
					s_copied_ranges.push_back({slot + SLOT_BLOCKS + i * SLOT_BLOCK_STRIDE + SLOT_BLOCK_OFFSET, PLAYER_BLOCK_SIZE});
			});
			for (const CopiedRange& r : s_copied_ranges)
				s_copied.insert(s_copied.end(), &eeMem->Main[r.addr], &eeMem->Main[r.addr + r.len]);
		}

		void RestoreCopiedRanges()
		{
			size_t k = 0;
			for (const CopiedRange& r : s_copied_ranges)
			{
				std::memcpy(&eeMem->Main[r.addr], &s_copied[k], r.len);
				k += r.len;
			}
		}

		void SaveObjects()
		{
			s_keep.objects.clear();
			ForEachKeptObject([](u32 obj, const KeptObjectType& type) {
				auto& o = s_keep.objects.emplace_back();
				o.addr = obj;
				o.handler = type.handler;
				for (const auto& [lo, hi] : type.fields)
					o.data.insert(o.data.end(), &eeMem->Main[obj + lo], &eeMem->Main[obj + hi]);
			});
		}

		void RestoreObjects()
		{
			if (s_keep.objects.empty())
				return;
			ForEachKeptObject([](u32 obj, const KeptObjectType& type) {
				const auto it = std::find_if(s_keep.objects.begin(), s_keep.objects.end(),
					[obj, &type](const KeptObject& o) { return o.addr == obj && o.handler == type.handler; });
				if (it == s_keep.objects.end())
					return;
				size_t k = 0;
				for (const auto& [lo, hi] : type.fields)
				{
					std::memcpy(&eeMem->Main[obj + lo], &it->data[k], hi - lo);
					k += hi - lo;
				}
			});
		}
	} // namespace

	void RerunKeepBeforeLoad(bool callbacks_skipped)
	{
		s_keep.rebuild_slots = false;
		s_keep.armed = callbacks_skipped && !s_keep_config.ranges.empty() && RerunKeepPhase();
		if (!s_keep.armed)
			return;
		s_keep.saved.clear();
		for (const auto& [lo, hi] : s_keep_config.ranges)
			s_keep.saved.insert(s_keep.saved.end(), &eeMem->Main[lo], &eeMem->Main[hi]);
		SaveObjects();
	}

	void RerunKeepAfterLoad()
	{
		if (!s_keep.armed || !RerunKeepPhase())
			return;
		s_keep.armed = false;
		s_keep.rebuild_slots = std::any_of(s_keep_config.ranges.begin(), s_keep_config.ranges.end(),
			[](const auto& r) { return r.first < BoneSlotsEnd() && BoneSlots() < r.second; });
		if (s_keep.rebuild_slots)
			SaveCopiedRanges();
		size_t off = 0;
		for (const auto& [lo, hi] : s_keep_config.ranges)
		{
			std::memcpy(&eeMem->Main[lo], &s_keep.saved[off], hi - lo);
			off += hi - lo;
		}
		RestoreObjects();
		if (s_keep.rebuild_slots)
			RestoreCopiedRanges();
		if (!s_keep.logged)
		{
			s_keep.logged = true;
			Console.WriteLn("ZdxsvGgpo: rerun keep %zu ranges", s_keep_config.ranges.size());
		}
	}

	void RerunKeepAfterRerunFrame(bool last)
	{
		// The walker copied the drawn players' parts itself.
		if (!s_keep.walked)
			RebuildPlayerSlots();
		s_keep.walked = false;
		if (last)
			s_keep.rebuild_slots = false;
	}

	bool RerunKeepWalksObjects()
	{
		if (!s_keep_parts || !s_keep.rebuild_slots)
			return false;
		const u32 count = ReadU32((cpuRegs.GPR.n.gp.UL[0] - GgpoAddrs().draw_list_count_gp_offset) & (Ps2MemSize::MainRam - 4));
		for (u32 i = 0; i < std::min(count, DRAW_LIST_MAX); i++)
		{
			if (ReadU32(GgpoAddrs().draw_list + i * 4) != GgpoPcs().obj_walk)
				continue;
			if (!s_keep.parts_logged)
			{
				s_keep.parts_logged = true;
				Console.WriteLn("ZdxsvGgpo: rerun keep draws attached parts");
			}
			s_keep.walked = true;
			return true;
		}
		return false;
	}

	bool RerunKeepDrawsHandler(u32 handler, u32 obj)
	{
		// The walker calls a handler only for an object not culled.
		if (IsPlayerHandler(handler))
			RebuildPlayerSlot(obj & (Ps2MemSize::MainRam - 1));
		return GgpoAddrs().part_handler && handler == GgpoAddrs().part_handler;
	}

	bool OnDrawRun()
	{
		if (!g_rerun_draw_skip)
			return false;
		// ra stays: the walker returns to the runner's caller.
		cpuRegs.pc = RerunKeepWalksObjects() ? GgpoPcs().obj_walk : cpuRegs.GPR.n.ra.UL[0];
		return true;
	}

	bool OnDrawRun2()
	{
		if (!g_rerun_draw_skip)
			return false;
		cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
		return true;
	}

	bool OnObjCall()
	{
		if (!g_rerun_draw_skip || RerunKeepDrawsHandler(cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.s0.UL[0]))
			return false;
		cpuRegs.pc = GgpoPcs().obj_call + 8;
		return true;
	}
} // namespace Zdxsv
