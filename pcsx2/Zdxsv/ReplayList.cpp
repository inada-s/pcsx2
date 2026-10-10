// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/ReplayList.h"
#include "Zdxsv/Proto.h"
#include "Config.h"
#include "Host.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <cstdlib>
#include <mutex>

namespace Zdxsv
{
	namespace
	{
		std::mutex s_next_mutex;
		std::string s_next_src;
		int s_next_pov = -1;
	} // namespace

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
		return Path::Combine(EmuFolders::DataRoot, "replays");
	}

	bool ReadReplayInfo(const std::string& path, ReplayFileInfo* info)
	{
		const std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(path.c_str());
		if (!data)
			return false;
		*info = {};
		Pb::Reader rd{data->data(), data->data() + data->size()};
		return rd.Fields([info](uint32_t field, uint32_t wt, uint64_t v, const uint8_t* b, size_t n) {
			if (wt == 0)
			{
				if (field == 20)
					info->start_at = static_cast<s64>(v);
				else if (field == 40)
					info->players = static_cast<int>(v);
				else if (field == 48)
					info->frames = static_cast<int>(v);
			}
			else if (wt == 2)
			{
				if (field == 3)
					info->battle_code.assign(reinterpret_cast<const char*>(b), n);
				else if (field == 18)
					info->rounds++;
				else if (field == 11)
				{
					ReplayFileUser u;
					Pb::Reader user{b, b + n};
					if (!user.Fields([&u](uint32_t f, uint32_t uwt, uint64_t uv, const uint8_t* ub, size_t un) {
							if (uwt == 2 && (f == 2 || f == 3))
								(f == 2 ? u.name : u.pilot).assign(reinterpret_cast<const char*>(ub), un);
							else if (uwt == 0 && f == 12)
								u.pos = static_cast<int>(uv);
							return true;
						}))
						return false;
					info->users.push_back(std::move(u));
				}
			}
			return true;
		});
	}

	std::string LobbyApiUrl()
	{
		std::string url = Host::GetStringSettingValue("DEV9/Eth", "ZdxsvLobbyApiUrl", "https://zdxsv.net");
		while (!url.empty() && (url.back() == '/' || url.back() == ' '))
			url.pop_back();
		return url;
	}

	std::string LiveReplaySource(const std::string& battle_code)
	{
		std::string_view host = LobbyApiUrl();
		if (const size_t s = host.find("://"); s != std::string_view::npos)
			host.remove_prefix(s + 3);
		host = host.substr(0, host.find('/'));
		// drop the API port: the live stream is on the lobby's STUN port, as LobbyConnection's
		const size_t colon = host.rfind(':');
		if (colon != std::string_view::npos && host.find(']', colon) == std::string_view::npos)
			host = host.substr(0, colon);
		if (host.empty())
			return {};
		const char* port = std::getenv("ZDXSV_STUN_PORT");
		return fmt::format("udp://{}:{}/{}", host, port ? std::atoi(port) : 8201, battle_code);
	}
} // namespace Zdxsv
