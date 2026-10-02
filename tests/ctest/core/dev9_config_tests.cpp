// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"

#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

#include <gtest/gtest.h>

using DEV9Options = Pcsx2Config::DEV9Options;

// zdxsv: a fresh config has the network adapter on and the game's servers mapped to zdxsv.net.
TEST(DEV9Config, ZdxsvDefaults)
{
	const DEV9Options opts;
	EXPECT_TRUE(opts.EthEnable);
	EXPECT_EQ(opts.EthApi, DEV9Options::NetApi::Sockets);
	EXPECT_EQ(opts.EthDevice, "Auto");
	EXPECT_TRUE(opts.InterceptDHCP);
	EXPECT_EQ(opts.ModeDNS1, DEV9Options::DnsMode::Internal);
	EXPECT_EQ(opts.ModeDNS2, DEV9Options::DnsMode::Internal);

	const char* urls[] = {"www01.kddi-mmbb.jp", "gate1.jp.dnas.playstation.org", "ca1202.mmcp6", "ca1203.mmcp6"};
	ASSERT_EQ(opts.EthHosts.size(), std::size(urls));
	for (size_t i = 0; i < std::size(urls); i++)
	{
		const DEV9Options::HostEntry& host = opts.EthHosts[i];
		EXPECT_EQ(host.Url, urls[i]);
		EXPECT_TRUE(host.Enabled);
		EXPECT_EQ(host.Address[0], 153);
		EXPECT_EQ(host.Address[1], 121);
		EXPECT_EQ(host.Address[2], 44);
		EXPECT_EQ(host.Address[3], 150);
	}
}

// The defaults are what a new ini gets (VMManager::SetDefaultSettings saves a default Pcsx2Config).
TEST(DEV9Config, ZdxsvDefaultsSaveLoad)
{
	MemorySettingsInterface si;
	DEV9Options saved;
	SettingsSaveWrapper ssw(si);
	saved.LoadSave(ssw);

	EXPECT_TRUE(si.GetBoolValue("DEV9/Eth", "EthEnable", false));
	EXPECT_EQ(si.GetStringValue("DEV9/Eth", "EthApi"), "Sockets");
	EXPECT_EQ(si.GetStringValue("DEV9/Eth", "ModeDNS1"), "Internal");
	EXPECT_EQ(si.GetIntValue("DEV9/Eth/Hosts", "Count", 0), 4);
	EXPECT_EQ(si.GetStringValue("DEV9/Eth/Hosts/Host2", "Url"), "ca1202.mmcp6");
	EXPECT_EQ(si.GetStringValue("DEV9/Eth/Hosts/Host2", "Address"), "153.121.44.150");

	// A user's own entries replace the defaults.
	si.SetIntValue("DEV9/Eth/Hosts", "Count", 1);
	si.SetStringValue("DEV9/Eth/Hosts/Host0", "Address", "192.168.1.8");
	DEV9Options loaded;
	SettingsLoadWrapper slw(si);
	loaded.LoadSave(slw);
	ASSERT_EQ(loaded.EthHosts.size(), 1u);
	EXPECT_EQ(loaded.EthHosts[0].Url, "www01.kddi-mmbb.jp");
	EXPECT_EQ(loaded.EthHosts[0].Address[0], 192);
	EXPECT_EQ(loaded.EthHosts[0].Address[3], 8);
}
