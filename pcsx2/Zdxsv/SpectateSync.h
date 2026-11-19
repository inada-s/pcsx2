// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Four-screen replay: one replay played from each position
// with a file, one PCSX2 process per position, kept on the same frame through a file-backed shared memory group.
// The host (the process the replay was started in) also publishes its pause, speed and seeks; the guests follow.
namespace Zdxsv
{
	// Frames a member may be ahead of the slowest one before it waits.
	constexpr int SPECTATE_SYNC_SLACK = 2;

	struct SpectateSyncControl
	{
		bool paused = false;
		int limiter = -1; // LimiterModeType, -1 = not published yet
		uint32_t seek_gen = 0; // bumped per host seek
		int seek_target = 0; // frame the host seeked to
	};

	// Joins `group` (claims a slot); `host`: also the writer of SpectateSyncControl.
	bool SpectateSyncJoin(const std::string& group, bool host);
	void SpectateSyncLeave();
	bool SpectateSyncActive();
	bool SpectateSyncIsHost();

	// The frame this member has reached. `catching_up`: seeking or running unlimited, no target for the others to seek to.
	void SpectateSyncPublish(int frame, bool catching_up);
	// Live members (this one included); slowest frame of them, newest of those not catching up (-1 if none).
	int SpectateSyncMembers(int& slowest, int& newest);
	// The newest frame of any live member that is not catching up, -1 alone.
	int SpectateSyncLeader();
	// Holds this member at `frame` while a live peer (catching up too) is more than SPECTATE_SYNC_SLACK frames behind.
	// False: alone, or gave up after max_wait_ms (playback goes on, a spectator never hangs).
	bool SpectateSyncWaitForPeers(int frame, int max_wait_ms);

	void SpectateSyncHostPaused(bool paused);
	void SpectateSyncHostLimiter(int limiter);
	void SpectateSyncHostSeek(int frame);
	bool SpectateSyncReadControl(SpectateSyncControl& out);
	// Guest: the host left the group or its process is gone.
	bool SpectateSyncHostGone();

	constexpr int SPECTATE_SYNC_GRID = 4;
	int SpectateSyncSelfPid();
	// Host: starts this process's command line again, with `env` added to its environment and -batch + `args`
	// before its `--`. Returns the guest's pid, 0 on failure (or not on Windows).
	int SpectateSyncSpawnGuest(const std::vector<std::pair<std::string, std::string>>& env, const std::vector<std::string>& args);
	// Host: places the main window of each pid (index = cell, 0 = none) in a 2x2 grid on this process's monitor's
	// work area. Returns the windows placed.
	int SpectateSyncTileWindows(const int pids[SPECTATE_SYNC_GRID]);
} // namespace Zdxsv
