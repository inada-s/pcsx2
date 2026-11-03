// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "Zdxsv/Settings.h"

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
