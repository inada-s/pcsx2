// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/Upnp.h"
#include "Zdxsv/TestOptions.h"

#include "Host.h"

#include "common/Console.h"

#include <miniupnpc.h>
#include <upnpcommands.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace Zdxsv
{
	namespace
	{
		struct Upnp
		{
			UPNPUrls urls{};
			IGDdatas data{};
			char lanAddress[64] = {};
			bool initialized = false;
			std::vector<std::string> mappedPorts;
			std::mutex mutex;

			~Upnp() { UnmapAll(); }

			bool Init(std::string& error)
			{
				if (initialized)
					return true;
				int err = 0;
				UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &err);
				if (!devlist)
				{
					error = "UPnP discover failed: error " + std::to_string(err);
					return false;
				}
				const int igd = UPNP_GetValidIGD(devlist, &urls, &data, lanAddress, sizeof(lanAddress));
				freeUPNPDevlist(devlist);
				// 2: an IGD that reports no routable WAN address (0.0.0.0 behind a shared IPv4): its mappings
				// are accepted but open nothing, so it counts as no gateway.
				const char* any = TestEnv("ZDXSV_UPNP_ANY_IGD");
				const bool anyIgd = any && std::string(any) == "1";
				if (igd != 1 && !(anyIgd && igd == 2))
				{
					if (igd != 0)
						FreeUPNPUrls(&urls);
					error = "Internet Gateway not found: error " + std::to_string(igd);
					return false;
				}
				initialized = true;
				return true;
			}

			void UnmapAll()
			{
				std::lock_guard lock(mutex);
				if (!initialized)
					return;
				for (const std::string& port : mappedPorts)
					UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "UDP", nullptr);
				mappedPorts.clear();
				FreeUPNPUrls(&urls);
				initialized = false;
			}
		};

		Upnp& Get()
		{
			static Upnp s_upnp;
			return s_upnp;
		}
	} // namespace

	bool UpnpEnabled()
	{
		return Host::GetBoolSettingValue("DEV9/Eth", "ZdxsvUPnP", true);
	}

	bool UpnpMapUdp(uint16_t port, std::string& error)
	{
		Upnp& u = Get();
		std::lock_guard lock(u.mutex);
		if (!u.Init(error))
		{
			Console.WriteLn("DEV9: zdxsv UPnP: %s", error.c_str());
			return false;
		}
		const std::string portStr = std::to_string(port);
		const int err = UPNP_AddPortMapping(u.urls.controlURL, u.data.first.servicetype, portStr.c_str(), portStr.c_str(),
			u.lanAddress, "PCSX2 zdxsv", "UDP", nullptr, "86400");
		if (err != 0)
		{
			error = "Port " + portStr + " redirection failed: error " + std::to_string(err);
			Console.WriteLn("DEV9: zdxsv UPnP: %s", error.c_str());
			return false;
		}
		if (std::find(u.mappedPorts.begin(), u.mappedPorts.end(), portStr) == u.mappedPorts.end())
			u.mappedPorts.push_back(portStr);
		Console.WriteLn("DEV9: zdxsv UPnP: UDP port %s mapped", portStr.c_str());
		return true;
	}

	std::string UpnpLocalAddress()
	{
		Upnp& u = Get();
		std::lock_guard lock(u.mutex);
		return u.initialized ? u.lanAddress : "";
	}

	void UpnpUnmapAll()
	{
		Get().UnmapAll();
	}
} // namespace Zdxsv
