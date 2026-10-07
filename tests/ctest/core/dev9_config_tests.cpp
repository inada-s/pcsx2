// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "Zdxsv/Dev9Hooks.h"

#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

#include <gtest/gtest.h>

#include <string>

using DEV9Options = Pcsx2Config::DEV9Options;

// zdxsv: a fresh config has the network adapter on and internal DNS; the game's servers need no host entry.
TEST(DEV9Config, ZdxsvDefaults)
{
	const DEV9Options opts;
	EXPECT_TRUE(opts.EthEnable);
	EXPECT_EQ(opts.EthApi, DEV9Options::NetApi::Sockets);
	EXPECT_EQ(opts.EthDevice, "Auto");
	EXPECT_TRUE(opts.InterceptDHCP);
	EXPECT_EQ(opts.ModeDNS1, DEV9Options::DnsMode::Internal);
	EXPECT_EQ(opts.ModeDNS2, DEV9Options::DnsMode::Internal);
	EXPECT_TRUE(opts.EthHosts.empty());
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
	EXPECT_EQ(si.GetIntValue("DEV9/Eth/Hosts", "Count", -1), 0);
}

// Entries mapping a game host to the old default address are dropped on load; other entries stay.
TEST(DEV9Config, ZdxsvStaleHostEntries)
{
	const char* entries[][2] = {
		{"www01.kddi-mmbb.jp", "153.121.44.150"},
		{"gate1.jp.dnas.playstation.org", "153.121.44.150"},
		{"ca1202.mmcp6", "192.168.1.8"},
		{"CA1203.MMCP6", "153.121.44.150"},
		{"example.com", "153.121.44.150"},
	};
	MemorySettingsInterface si;
	si.SetIntValue("DEV9/Eth/Hosts", "Count", static_cast<int>(std::size(entries)));
	for (size_t i = 0; i < std::size(entries); i++)
	{
		const std::string section = "DEV9/Eth/Hosts/Host" + std::to_string(i);
		si.SetStringValue(section.c_str(), "Url", entries[i][0]);
		si.SetStringValue(section.c_str(), "Address", entries[i][1]);
		si.SetBoolValue(section.c_str(), "Enabled", true);
	}

	DEV9Options loaded;
	SettingsLoadWrapper slw(si);
	loaded.LoadSave(slw);
	ASSERT_EQ(loaded.EthHosts.size(), 2u);
	EXPECT_EQ(loaded.EthHosts[0].Url, "ca1202.mmcp6");
	EXPECT_EQ(loaded.EthHosts[0].Address[0], 192);
	EXPECT_EQ(loaded.EthHosts[1].Url, "example.com");
}

TEST(DEV9Config, ZdxsvDnsLookupName)
{
	EXPECT_STREQ(Zdxsv::DnsLookupName("www01.kddi-mmbb.jp"), "zdxsv.net");
	EXPECT_STREQ(Zdxsv::DnsLookupName("gate1.jp.dnas.playstation.org"), "zdxsv.net");
	EXPECT_STREQ(Zdxsv::DnsLookupName("ca1202.mmcp6"), "zdxsv.net");
	EXPECT_STREQ(Zdxsv::DnsLookupName("Ca1203.Mmcp6"), "zdxsv.net");
	EXPECT_STREQ(Zdxsv::DnsLookupName("example.com"), "example.com");
	EXPECT_STREQ(Zdxsv::DnsLookupName("ca1202.mmcp6.example.com"), "ca1202.mmcp6.example.com");
}
