// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Four-screen replay (as flycast's gdxsv_spectate_sync / gdxsv_multi_pov): one replay played from each position
// with a file, one PCSX2 process per position, kept on the same frame through a file-backed shared memory group.
// The host (the process the replay was started in) also publishes its pause, speed and seeks; the guests follow.
namespace Zdxsv::SpectateSync
{
	// Frames a member may be ahead of the slowest one before it waits.
	constexpr int SLACK = 2;

	struct Control
	{
		bool paused = false;
		int limiter = -1; // LimiterModeType, -1 = not published yet
		uint32_t seek_gen = 0; // bumped per host seek
		int seek_target = 0; // frame the host seeked to
	};

	// Joins `group` (claims a slot); `host`: also the writer of Control.
	bool Join(const std::string& group, bool host);
	void Leave();
	bool Active();
	bool IsHost();

	// The frame this member has reached. `catching_up`: seeking or running unlimited, no target for the others to seek to.
	void Publish(int frame, bool catching_up);
	// Live members (this one included); slowest frame of them, newest of those not catching up (-1 if none).
	int Members(int& slowest, int& newest);
	// The newest frame of any live member that is not catching up, -1 alone.
	int Leader();
	// Holds this member at `frame` while a live peer (catching up too) is more than SLACK frames behind.
	// False: alone, or gave up after max_wait_ms (playback goes on, a spectator never hangs).
	bool WaitForPeers(int frame, int max_wait_ms);

	void HostPaused(bool paused);
	void HostLimiter(int limiter);
	void HostSeek(int frame);
	bool ReadControl(Control& out);
	// Guest: the host left the group or its process is gone.
	bool HostGone();

	constexpr int GRID = 4;
	int SelfPid();
	// Host: starts this process's command line again, with `env` added to its environment and -batch + `args`
	// before its `--`. Returns the guest's pid, 0 on failure (or not on Windows).
	int SpawnGuest(const std::vector<std::pair<std::string, std::string>>& env, const std::vector<std::string>& args);
	// Host: places the main window of each pid (index = cell, 0 = none) in a 2x2 grid on this process's monitor's
	// work area. Returns the windows placed.
	int TileWindows(const int pids[GRID]);
} // namespace Zdxsv::SpectateSync
