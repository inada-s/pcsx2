// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Game patches from the lobby (notice 0x9953), written in EE RAM while a battle runs.

#include "Zdxsv/GgpoShared.h"
#include "Zdxsv/TestOptions.h"

#include <algorithm>
#include <iterator>

#include "Memory.h"
#include "VMManager.h"

namespace Zdxsv
{
	namespace
	{
		struct ServerPatchState
		{
			std::vector<GamePatch> applied;
		};
		ServerPatchState s_server_patches;

		// Teams change every battle of a series: the series counter step (0x307c10) jumps to a cave that, with 4
		// players, sets the slot team bytes (0xba4703 + 6 * slot; MS select and the battle read them) to slot 0 and
		// slot ((battle - 1) % 3) + 1 against the other two. The cave is free RAM after the single draw list's.
		GamePatch TeamRotatePatch()
		{
			static constexpr u32 kCave[] = {
				0x3c0500ba, 0x24a54703, 0x80a40001, 0x04800018, 0x80a40007, 0x04800016, 0x80a4000d, 0x04800014,
				0x80a40013, 0x04800012, 0x00000000, 0x28640004, 0x5080fffe, 0x2463fffd, 0x00031880, 0x34046ac6,
				0x00642006, 0x30860001, 0xa0a60000, 0x00043042, 0x30c60001, 0xa0a60006, 0x00043082, 0x30c60001,
				0xa0a6000c, 0x000430c2, 0x30c60001, 0xa0a60012, 0x03e00008, 0x00000000,
			};
			GamePatch p{"team_rotate", "SLPS-25419", true, {}};
			for (u32 i = 0; i < std::size(kCave); i++)
				p.codes.push_back({32, 0xf0100 + i * 4, 0, kCave[i]});
			p.codes.push_back({32, 0x307c38, 0x03e00008, 0x0803c040});
			return p;
		}

		const std::vector<GamePatch>& BuiltinPatches()
		{
			static const std::vector<GamePatch> patches = {
				{"rematch_max5", "SLPS-25419", true, {{32, 0x307ff0, 0x2842000a, 0x28420005}, {32, 0x308044, 0x0c0dccc4, 0x1000ffe7}}},
				TeamRotatePatch(),
			};
			return patches;
		}

		bool SamePatch(const GamePatch& a, const GamePatch& b)
		{
			if (a.game != b.game || a.codes.size() != b.codes.size())
				return false;
			for (size_t i = 0; i < a.codes.size(); i++)
			{
				const GamePatchCode& x = a.codes[i];
				const GamePatchCode& y = b.codes[i];
				if (x.size != y.size || x.address != y.address || x.original != y.original || x.changed != y.changed)
					return false;
			}
			return true;
		}

		u32 ReadCode(EEMemoryInterface& ee, const GamePatchCode& c)
		{
			switch (c.size)
			{
				case 8:
					return ee.Read8(c.address);
				case 16:
					return ee.Read16(c.address);
				default:
					return ee.Read32(c.address);
			}
		}

		// The write PCSX2's own patches use: it skips an equal value, and a write to recompiled code clears its blocks.
		void WriteCode(EEMemoryInterface& ee, const GamePatchCode& c, u32 value)
		{
			switch (c.size)
			{
				case 8:
					ee.IdempotentWrite8(c.address, static_cast<u8>(value));
					break;
				case 16:
					ee.IdempotentWrite16(c.address, static_cast<u16>(value));
					break;
				default:
					ee.IdempotentWrite32(c.address, value);
					break;
			}
		}
	} // namespace

	void ServerPatchesBegin(const std::vector<GamePatch>& patches)
	{
		ServerPatchesEnd();
		if (patches.empty())
			return;
		EEMemoryInterface ee;
		const std::string serial = VMManager::GetDiscSerial();
		for (const GamePatch& p : patches)
		{
			if (!p.game.empty() && p.game != serial)
			{
				Console.WriteLn("ZdxsvPatch: %s skipped: for %s, running %s", p.name.c_str(), p.game.c_str(), serial.c_str());
				continue;
			}
			const GamePatchCode* bad = nullptr;
			for (const GamePatchCode& c : p.codes)
				if (ReadCode(ee, c) != c.original)
				{
					bad = &c;
					break;
				}
			if (bad)
			{
				Console.WriteLn("ZdxsvPatch: %s skipped: %08x is %x, not %x", p.name.c_str(), bad->address, ReadCode(ee, *bad),
					bad->original);
				continue;
			}
			for (const GamePatchCode& c : p.codes)
				WriteCode(ee, c, c.changed);
			s_server_patches.applied.push_back(p);
			Console.WriteLn("ZdxsvPatch: %s applied, %zu codes, %s", p.name.c_str(), p.codes.size(),
				p.writeOnce ? "once" : "every frame");
		}
	}

	void ServerPatchesFrame()
	{
		if (s_server_patches.applied.empty())
			return;
		EEMemoryInterface ee;
		for (const GamePatch& p : s_server_patches.applied)
			if (!p.writeOnce)
				for (const GamePatchCode& c : p.codes)
					WriteCode(ee, c, c.changed);
	}

	void ServerPatchesEnd()
	{
		if (s_server_patches.applied.empty())
			return;
		EEMemoryInterface ee;
		size_t codes = 0, held = 0, restored = 0;
		for (const GamePatch& p : s_server_patches.applied)
			for (const GamePatchCode& c : p.codes)
			{
				codes++;
				held += ReadCode(ee, c) == c.changed;
				WriteCode(ee, c, c.original);
				restored += ReadCode(ee, c) == c.original;
			}
		Console.WriteLn("ZdxsvPatch: %zu patches restored, codes %zu, held changed %zu, original back %zu",
			s_server_patches.applied.size(), codes, held, restored);
		s_server_patches.applied.clear();
	}

	void ServerPatchesPut(std::vector<uint8_t>& pb, uint32_t field, const std::vector<GamePatch>& patches)
	{
		for (const GamePatch& p : patches)
		{
			std::vector<uint8_t> m;
			PbPutString(m, 1, p.game);
			PbPutString(m, 2, p.name);
			PbPutUint(m, 4, p.writeOnce);
			for (const GamePatchCode& c : p.codes)
			{
				std::vector<uint8_t> code;
				PbPutUint(code, 1, c.size);
				PbPutUint(code, 2, c.address);
				PbPutUint(code, 3, c.original);
				PbPutUint(code, 4, c.changed);
				PbPutBytes(m, 10, code.data(), code.size());
			}
			PbPutBytes(pb, field, m.data(), m.size());
		}
	}

	const GamePatch* ServerPatchNotBuiltin(const std::vector<GamePatch>& patches)
	{
		static const bool any = TestEnv("ZDXSV_PATCHES_ANY") != nullptr;
		if (any)
			return nullptr;
		for (const GamePatch& p : patches)
			if (std::none_of(BuiltinPatches().begin(), BuiltinPatches().end(), [&p](const GamePatch& b) { return SamePatch(p, b); }))
				return &p;
		return nullptr;
	}

	bool ServerPatchRead(const uint8_t* b, size_t n, GamePatch& out)
	{
		constexpr size_t MAX_CODES = 256;
		PbReader rd{b, b + n};
		return rd.Fields([&out](uint32_t f, uint32_t wt, uint64_t v, const uint8_t* fb, size_t fn) {
			if (f == 1 && wt == 2)
				out.game.assign(reinterpret_cast<const char*>(fb), fn);
			else if (f == 2 && wt == 2)
				out.name.assign(reinterpret_cast<const char*>(fb), fn);
			else if (f == 4 && wt == 0)
				out.writeOnce = v != 0;
			else if (f == 10 && wt == 2)
			{
				uint64_t c[5] = {};
				PbReader code{fb, fb + fn};
				if (out.codes.size() >= MAX_CODES || !code.Fields([&c](uint32_t cf, uint32_t cw, uint64_t cv, const uint8_t*, size_t) {
						if (cf >= 1 && cf <= 4 && cw == 0)
							c[cf] = cv;
						return true;
					}) ||
					!GamePatchCodeOk(c[1], c[2], c[3], c[4]))
					return false;
				out.codes.push_back({static_cast<uint8_t>(c[1]), static_cast<uint32_t>(c[2]), static_cast<uint32_t>(c[3]),
					static_cast<uint32_t>(c[4])});
			}
			return true;
		});
	}
} // namespace Zdxsv
