// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Sync-safe settings of the Z game, as RetroAchievements hardcore mode enforces its own
// (VMManager::EnforceAchievementsChallengeModeSettings): the settings that change what the emulated
// machine computes are forced to PCSX2's defaults while the game runs, its speedhacks only during
// battles (GGPO sessions: online battles, replay playback, live spectating). Peers compare a
// fingerprint of these settings and the build before a lobby battle (GgpoLobby.cpp LobbyArm).

#include "Zdxsv/SyncSettings.h"
#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/TestOptions.h"

#include "BuildVersion.h"
#include "Config.h"
#include "GameDatabase.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "VMManager.h"

#include "common/Console.h"

#include <cstring>
#include <mutex>

namespace Zdxsv
{
	namespace
	{
		struct SyncState
		{
			bool battle = false; // a GGPO session runs and its settings were applied
			std::mutex mtx;
			std::string fingerprint; // SyncFingerprint(), set on the CPU thread
		};
		SyncState s_sync;

		// ZDXSV_SYNC_FORCE=0 (test): nothing forced, for the controls of tests/zdxsv/syncset.sh
		bool ForceOff()
		{
			static const char* e = TestEnv("ZDXSV_SYNC_FORCE");
			return e && std::strcmp(e, "0") == 0;
		}

		// The forced values into c; names of the changed groups go to `changed` (nullptr: not wanted).
		void Force(Pcsx2Config& c, bool battle, std::string* changed)
		{
			const auto note = [changed](bool differs, const char* name) {
				if (differs && changed)
					*changed += (changed->empty() ? "" : ", ") + std::string(name);
			};
			const Pcsx2Config::CpuOptions cpu;
			note(c.EnableCheats, "cheats");
			c.EnableCheats = false;
			note(c.EnablePINE, "PINE");
			c.EnablePINE = false;
			const Pcsx2Config::RecompilerOptions& rec = cpu.Recompiler;
			const bool recs = c.Cpu.Recompiler.EnableEE != rec.EnableEE || c.Cpu.Recompiler.EnableIOP != rec.EnableIOP ||
			                  c.Cpu.Recompiler.EnableVU0 != rec.EnableVU0 || c.Cpu.Recompiler.EnableVU1 != rec.EnableVU1 ||
			                  c.Cpu.Recompiler.EnableEECache != rec.EnableEECache;
			note(recs, "recompilers");
			const u32 clamp_ee = c.Cpu.Recompiler.GetEEClampMode(), clamp_vu = c.Cpu.Recompiler.GetVUClampMode();
			note(clamp_ee != rec.GetEEClampMode() || clamp_vu != rec.GetVUClampMode(), "clamping");
			c.Cpu.Recompiler = rec;
			const bool round = c.Cpu.FPUFPCR.bitmask != cpu.FPUFPCR.bitmask || c.Cpu.FPUDivFPCR.bitmask != cpu.FPUDivFPCR.bitmask ||
			                   c.Cpu.VU0FPCR.bitmask != cpu.VU0FPCR.bitmask || c.Cpu.VU1FPCR.bitmask != cpu.VU1FPCR.bitmask;
			note(round, "rounding");
			c.Cpu.FPUFPCR = cpu.FPUFPCR;
			c.Cpu.FPUDivFPCR = cpu.FPUDivFPCR;
			c.Cpu.VU0FPCR = cpu.VU0FPCR;
			c.Cpu.VU1FPCR = cpu.VU1FPCR;
			note(c.Cpu.ExtraMemory, "extra memory");
			c.Cpu.ExtraMemory = false;
			// game fixes: the GameDB's only
			const Pcsx2Config::GamefixOptions fixes;
			note(!c.EnableGameFixes || c.Gamefixes != fixes, "game fixes");
			c.EnableGameFixes = true;
			c.Gamefixes = fixes;
			if (const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(VMManager::GetDiscSerial()))
				game->applyGameFixes(c, true);
			note(c.GS.HWDownloadMode != GSHardwareDownloadMode::Enabled, "GS download mode");
			c.GS.HWDownloadMode = GSHardwareDownloadMode::Enabled;
			if (!battle)
				return;
			Pcsx2Config::SpeedhackOptions hacks;
			hacks.vuThread = false; // g_mtvu_off
			note(c.Speedhacks != hacks, "speedhacks");
			c.Speedhacks = hacks;
			note(c.EmulationSpeed.NominalScalar != 1.0f, "speed");
			c.EmulationSpeed.NominalScalar = 1.0f;
			note(c.GS.FramerateNTSC != Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_NTSC, "frame rate");
			c.GS.FramerateNTSC = Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_NTSC;
			c.GS.FrameratePAL = Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_PAL;
		}

		// FNV-1a 64 of the build and the forced settings (round modes, not FPCR bits: x86 and ARM builds agree)
		std::string Fingerprint(const Pcsx2Config& c)
		{
			u64 h = 14695981039346656037ull;
			const auto add = [&h](const void* p, size_t n) {
				for (size_t i = 0; i < n; i++)
					h = (h ^ static_cast<const u8*>(p)[i]) * 1099511628211ull;
			};
			const auto add32 = [&add](u32 v) { add(&v, sizeof(v)); };
			add(BuildVersion::GitRev, std::strlen(BuildVersion::GitRev));
			// ZDXSV_SYNC_SALT=s (test): mixed in, as another build would be
			if (static const char* salt = TestEnv("ZDXSV_SYNC_SALT"); salt)
				add(salt, std::strlen(salt));
			add32(c.Cpu.Recompiler.bitset);
			for (const FPControlRegister& r : {c.Cpu.FPUFPCR, c.Cpu.FPUDivFPCR, c.Cpu.VU0FPCR, c.Cpu.VU1FPCR})
				add32(static_cast<u32>(r.GetRoundMode()) | (r.GetDenormalsAreZero() ? 0x10 : 0) | (r.GetFlushToZero() ? 0x20 : 0));
			add32(c.Cpu.ExtraMemory);
			add32(c.Gamefixes.bitset);
			add32(c.Speedhacks.bitset);
			add32(static_cast<u32>(c.Speedhacks.EECycleRate) & 0xff);
			add32(c.Speedhacks.EECycleSkip);
			add32(static_cast<u32>(c.GS.HWDownloadMode));
			add32(c.EnableCheats);
			char hex[17];
			std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
			return hex;
		}
	} // namespace

	void SyncSettingsEnforce()
	{
		if (!g_z_game || VMManager::GetState() == VMState::Shutdown)
			return;
		Pcsx2Config copy = EmuConfig;
		Force(copy, true, nullptr);
		{
			std::lock_guard lock(s_sync.mtx);
			s_sync.fingerprint = Fingerprint(copy);
		}
		if (ForceOff())
			return;
		std::string changed;
		Force(EmuConfig, s_sync.battle, &changed);
		if (changed.empty())
			return;
		Console.WriteLn("ZdxsvSync: forced to defaults%s: %s", s_sync.battle ? " (battle)" : "", changed.c_str());
		Host::AddIconOSDMessage("ZdxsvSyncForced", ICON_FA_TRIANGLE_EXCLAMATION,
			fmt::format(TRANSLATE_FS("Zdxsv", "Online play: {} set to the defaults."), changed), Host::OSD_WARNING_DURATION);
	}

	void SyncSettingsOnBattle(bool battle)
	{
		if (battle == s_sync.battle)
			return;
		s_sync.battle = battle;
		if (!g_z_game || ForceOff() || VMManager::GetState() == VMState::Shutdown)
			return;
		Console.WriteLn("ZdxsvSync: battle %s: settings reloaded", battle ? "start" : "end");
		VMManager::ApplySettings();
	}

	void SyncSettingsReset()
	{
		s_sync.battle = false;
	}

	bool SyncSettingsForced(bool battle_item)
	{
		return g_z_game && !ForceOff() && VMManager::GetState() != VMState::Shutdown && (!battle_item || s_sync.battle);
	}

	std::string SyncFingerprint()
	{
		std::lock_guard lock(s_sync.mtx);
		return s_sync.fingerprint;
	}
} // namespace Zdxsv
