// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Rerun keep: EE RAM ranges a rollback does not roll back (docs/zdxsv/rollback.md).

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{
	namespace
	{
		// Per-player bone matrix slots (4 x 0x65d0), written only by the render callbacks. A draw reads
		// the previous frame's slot, so rerun frames that skip the callbacks would leave a stale pose.
		constexpr u32 BONE_SLOTS = 0x8afda0, BONE_SLOTS_END = BONE_SLOTS + 4 * 0x65d0;

		// Game object list: head pointer at gp - 0x5b80; each object holds its handler at +0xa8 and
		// the next object at +0xac.
		constexpr u32 OBJ_LIST_GP_OFFSET = 0x5b80, OBJ_HANDLER = 0xa8, OBJ_NEXT = 0xac;
		constexpr u32 OBJ_MAX = 4096, OBJ_MIN_SIZE = 0x400;
		// Object types whose fields here are advanced only by the render callbacks.
		// An address is reused by other object types, so a field is kept only for an object with the
		// same address and handler before and after the load.
		struct KeptObjectType
		{
			u32 handler;
			std::array<std::pair<u32, u32>, 2> fields;
		};
		constexpr KeptObjectType KEPT_TYPES[] = {
			{0x167810, {{{0x0, 0xa0}, {0x150, 0x1d0}}}}, // beam saber trail history
			{0x167f50, {{{0x0, 0xa0}, {0xb0, 0x1f0}}}}, // an effect type, needs proper testing
		};

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
				c.ranges.emplace_back(BONE_SLOTS, BONE_SLOTS_END);
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
			bool logged = false;
		};
		RerunKeep s_keep;

		bool RerunKeepPhase()
		{
			return s_keep_config.any_phase || eeMem->Main[TICK_STATE] == TICK_PLAY;
		}

		u32 ReadU32(u32 addr)
		{
			u32 v;
			std::memcpy(&v, &eeMem->Main[addr], sizeof(v));
			return v;
		}

		template <typename F>
		void ForEachKeptObject(F&& fn)
		{
			// gp is read at the load: the game keeps one gp, needs proper testing outside battle play.
			const u32 head = (cpuRegs.GPR.n.gp.UL[0] - OBJ_LIST_GP_OFFSET) & (Ps2MemSize::MainRam - 4);
			u32 obj = ReadU32(head);
			for (u32 i = 0; obj && i < OBJ_MAX; i++)
			{
				obj &= Ps2MemSize::MainRam - 1;
				if (obj % 4 || obj > Ps2MemSize::MainRam - OBJ_MIN_SIZE)
					break;
				const u32 handler = ReadU32(obj + OBJ_HANDLER);
				for (const KeptObjectType& type : KEPT_TYPES)
				{
					if (handler == type.handler)
						fn(obj, type);
				}
				obj = ReadU32(obj + OBJ_NEXT);
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
		size_t off = 0;
		for (const auto& [lo, hi] : s_keep_config.ranges)
		{
			std::memcpy(&eeMem->Main[lo], &s_keep.saved[off], hi - lo);
			off += hi - lo;
		}
		RestoreObjects();
		if (!s_keep.logged)
		{
			s_keep.logged = true;
			Console.WriteLn("ZdxsvGgpo: rerun keep %zu ranges", s_keep_config.ranges.size());
		}
	}
} // namespace Zdxsv
