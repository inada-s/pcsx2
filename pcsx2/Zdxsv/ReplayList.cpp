// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/ReplayList.h"
#include "Zdxsv/Lobby.h"
#include "Zdxsv/Settings.h"
#include "Zdxsv/Proto.h"
#include "Zdxsv/TestOptions.h"
#include "Config.h"
#include "Host.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Zdxsv
{
	bool g_replay_child = false;
	bool g_replay_child_unfocused = false;

	namespace
	{
		std::mutex s_next_mutex;
		std::string s_next_src;
		int s_next_pov = -1;

		// Indexed by the battle MS id (player work +0x1f2a); the game's name table at 0x695d30 in the same order.
		constexpr const char* MS_NAMES[] = {
			TRANSLATE_NOOP("ZdxsvMs", "Gundam"),
			TRANSLATE_NOOP("ZdxsvMs", "Guncannon"),
			TRANSLATE_NOOP("ZdxsvMs", "GM"),
			TRANSLATE_NOOP("ZdxsvMs", "Old Zaku"),
			TRANSLATE_NOOP("ZdxsvMs", "Zaku"),
			TRANSLATE_NOOP("ZdxsvMs", "Char's Zaku"),
			TRANSLATE_NOOP("ZdxsvMs", "Gouf"),
			TRANSLATE_NOOP("ZdxsvMs", "Dom"),
			TRANSLATE_NOOP("ZdxsvMs", "Rick Dom"),
			TRANSLATE_NOOP("ZdxsvMs", "Gelgoog"),
			TRANSLATE_NOOP("ZdxsvMs", "Char's Gelgoog"),
			TRANSLATE_NOOP("ZdxsvMs", "Gyan"),
			TRANSLATE_NOOP("ZdxsvMs", "Gogg"),
			TRANSLATE_NOOP("ZdxsvMs", "Acguy"),
			TRANSLATE_NOOP("ZdxsvMs", "Z'Gok"),
			TRANSLATE_NOOP("ZdxsvMs", "Char's Z'Gok"),
			TRANSLATE_NOOP("ZdxsvMs", "Zock"),
			TRANSLATE_NOOP("ZdxsvMs", "Guntank"),
			TRANSLATE_NOOP("ZdxsvMs", "Zeong"),
			TRANSLATE_NOOP("ZdxsvMs", "Ground Type Gundam"),
			TRANSLATE_NOOP("ZdxsvMs", "Ground Type GM"),
			TRANSLATE_NOOP("ZdxsvMs", "Gundam Mk-II"),
			TRANSLATE_NOOP("ZdxsvMs", "GM II"),
			TRANSLATE_NOOP("ZdxsvMs", "Rick Dias"),
			TRANSLATE_NOOP("ZdxsvMs", "Hyaku Shiki"),
			TRANSLATE_NOOP("ZdxsvMs", "Nemo"),
			TRANSLATE_NOOP("ZdxsvMs", "Zeta Gundam"),
			TRANSLATE_NOOP("ZdxsvMs", "Methuss"),
			TRANSLATE_NOOP("ZdxsvMs", "Dijeh"),
			TRANSLATE_NOOP("ZdxsvMs", "Hi-Zack"),
			TRANSLATE_NOOP("ZdxsvMs", "Galbaldy Beta"),
			TRANSLATE_NOOP("ZdxsvMs", "Marasai"),
			TRANSLATE_NOOP("ZdxsvMs", "Messala"),
			TRANSLATE_NOOP("ZdxsvMs", "Asshimar"),
			TRANSLATE_NOOP("ZdxsvMs", "Gaplant"),
			TRANSLATE_NOOP("ZdxsvMs", "Psyco Gundam"),
			TRANSLATE_NOOP("ZdxsvMs", "Gabthley"),
			TRANSLATE_NOOP("ZdxsvMs", "Hambrabi"),
			TRANSLATE_NOOP("ZdxsvMs", "Palace Athene"),
			TRANSLATE_NOOP("ZdxsvMs", "Barzam"),
			TRANSLATE_NOOP("ZdxsvMs", "Psyco Gundam Mk-II"),
			TRANSLATE_NOOP("ZdxsvMs", "Byarlant"),
			TRANSLATE_NOOP("ZdxsvMs", "Bound Doc"),
			TRANSLATE_NOOP("ZdxsvMs", "Bolinoak Sammahn"),
			TRANSLATE_NOOP("ZdxsvMs", "The O"),
			TRANSLATE_NOOP("ZdxsvMs", "Gaza-C"),
			TRANSLATE_NOOP("ZdxsvMs", "Qubeley"),
		};
	} // namespace

	const char* MsName(u32 id)
	{
		return id < std::size(MS_NAMES) ? TRANSLATE("ZdxsvMs", MS_NAMES[id]) : nullptr;
	}

	void SetNextReplay(std::string src, int pov)
	{
		std::lock_guard lock(s_next_mutex);
		s_next_src = std::move(src);
		s_next_pov = pov;
	}

	bool TakeNextReplay(std::string& src, int& pov)
	{
		std::lock_guard lock(s_next_mutex);
		src = std::exchange(s_next_src, {});
		pov = std::exchange(s_next_pov, -1);
		return !src.empty();
	}

	std::string ReplayDir()
	{
		const std::string fallback = Path::Combine(EmuFolders::DataRoot, "replays");
		std::string dir = Host::GetStringSettingValue("DEV9/Eth", "ZdxsvReplayPath");
		if (dir.empty())
			return fallback;
		if (!Path::IsAbsolute(dir))
			dir = Path::Canonicalize(Path::Combine(EmuFolders::DataRoot, dir));
		if (FileSystem::DirectoryExists(dir.c_str()) || FileSystem::CreateDirectoryPath(dir.c_str(), true))
			return dir;
		Console.Warning("ZdxsvGgpo: replay folder %s not usable, saving to %s", dir.c_str(), fallback.c_str());
		return fallback;
	}

	bool ReadReplayInfo(const std::string& path, ReplayFileInfo* info)
	{
		const auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
		if (!fp)
			return false;
		const s64 size = FileSystem::FSize64(fp.get());
		if (size < 0)
			return false;
		const auto varint = [f = fp.get()](u64& v) {
			v = 0;
			for (int shift = 0; shift < 64; shift += 7)
			{
				const int c = std::fgetc(f);
				if (c == EOF)
					return false;
				v |= u64{static_cast<u8>(c) & 0x7Fu} << shift;
				if (!(c & 0x80))
					return true;
			}
			return false;
		};
		constexpr int max_fields = 4096;
		constexpr u64 max_header_bytes = 4096;
		constexpr size_t max_users = 4;
		*info = {};
		std::vector<u8> buf;
		for (int fields = 0;; fields++)
		{
			if (fields == max_fields)
				return false;
			const s64 at = FileSystem::FTell64(fp.get());
			if (at < 0 || at > size)
				return false;
			if (at == size)
				return true;
			u64 tag, v;
			if (!varint(tag))
				return false;
			const u32 field = static_cast<u32>(tag >> 3);
			const u32 wt = tag & 7;
			if (wt == 0)
			{
				if (!varint(v))
					return false;
				if (field == 20)
					info->start_at = static_cast<s64>(v);
				else if (field == 40)
					info->players = static_cast<int>(v);
				else if (field == 48)
					info->frames = static_cast<int>(v);
				continue;
			}
			if (wt == 1 || wt == 5)
				v = wt == 1 ? 8 : 4;
			else if (wt != 2 || !varint(v))
				return false;
			const s64 body = FileSystem::FTell64(fp.get());
			if (body < 0 || body > size || v > static_cast<u64>(size - body))
				return false;
			if (wt == 2 && (field == 3 || field == 11) && v <= max_header_bytes && (field == 3 || info->users.size() < max_users))
			{
				buf.resize(static_cast<size_t>(v));
				if (std::fread(buf.data(), 1, buf.size(), fp.get()) != buf.size())
					return false;
				if (field == 3)
				{
					info->battle_code.assign(reinterpret_cast<const char*>(buf.data()), buf.size());
					continue;
				}
				ReplayFileUser u;
				Pb::Reader user{buf.data(), buf.data() + buf.size()};
				if (!user.Fields([&u](uint32_t f, uint32_t uwt, uint64_t uv, const uint8_t* ub, size_t un) {
						if (uwt == 2 && (f == 2 || f == 3))
							(f == 2 ? u.name : u.pilot).assign(reinterpret_cast<const char*>(ub), un);
						else if (uwt == 0 && f == 12)
							u.pos = static_cast<int>(uv);
						return true;
					}))
					return false;
				info->users.push_back(std::move(u));
				continue;
			}
			if (wt == 2 && field == 18)
				info->rounds++;
			if (FileSystem::FSeek64(fp.get(), static_cast<s64>(v), SEEK_CUR) != 0)
				return false;
		}
	}

	std::string LobbyApiUrl()
	{
		std::string url = Host::GetStringSettingValue("DEV9/Eth", "ZdxsvLobbyApiUrl", ServerApiUrl("").c_str());
		while (!url.empty() && (url.back() == '/' || url.back() == ' '))
			url.pop_back();
		return url;
	}

	std::string LiveReplaySource(const std::string& battle_code)
	{
		return LiveSource(LobbyApiUrl(), battle_code);
	}
} // namespace Zdxsv
