// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Game patches from the lobby (notice 0x9953), written in EE RAM while a battle runs.

#include "Zdxsv/GgpoShared.h"

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
			Pb::PutString(m, 1, p.game);
			Pb::PutString(m, 2, p.name);
			Pb::PutUint(m, 4, p.writeOnce);
			for (const GamePatchCode& c : p.codes)
			{
				std::vector<uint8_t> code;
				Pb::PutUint(code, 1, c.size);
				Pb::PutUint(code, 2, c.address);
				Pb::PutUint(code, 3, c.original);
				Pb::PutUint(code, 4, c.changed);
				Pb::PutBytes(m, 10, code.data(), code.size());
			}
			Pb::PutBytes(pb, field, m.data(), m.size());
		}
	}

	bool ServerPatchRead(const uint8_t* b, size_t n, GamePatch& out)
	{
		constexpr size_t MAX_CODES = 256;
		Pb::Reader rd{b, b + n};
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
				Pb::Reader code{fb, fb + fn};
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
