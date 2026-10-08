// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/SpectateSync.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <cwchar>
#include <thread>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Zdxsv::SpectateSync
{
	namespace
	{
		constexpr uint32_t MAGIC = 0x5a445353; // "ZDSS"
		constexpr int SLOTS = 8;
		// A slot without a heartbeat for this long is dead (a crashed or paused member does not hold the group).
		constexpr int64_t STALE_US = 1'000'000;

		struct Slot
		{
			std::atomic<int64_t> heartbeat_us;
			std::atomic<int32_t> frame;
			std::atomic<int32_t> pid;
			std::atomic<uint32_t> catching_up;
		};

		struct Header
		{
			std::atomic<uint32_t> magic;
			Slot slots[SLOTS];
			std::atomic<int32_t> host_pid;
			std::atomic<uint32_t> host_left;
			std::atomic<uint32_t> paused;
			std::atomic<int32_t> limiter;
			std::atomic<uint32_t> seek_gen;
			std::atomic<int32_t> seek_target;
		};

		Header* s_map = nullptr;
		Slot* s_slot = nullptr;
		bool s_host = false;
		std::string s_group;
#ifdef _WIN32
		HANDLE s_file = nullptr;
#endif

		int64_t NowUs()
		{
			return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		int32_t Pid()
		{
#ifdef _WIN32
			return static_cast<int32_t>(GetCurrentProcessId());
#else
			return static_cast<int32_t>(getpid());
#endif
		}

		bool PidAlive(int32_t pid)
		{
#ifdef _WIN32
			HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
			if (!h)
				return false;
			const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
			CloseHandle(h);
			return alive;
#else
			return kill(pid, 0) == 0;
#endif
		}

		// A real file, not a named section: processes spawned in another session would get their own "Local\" one.
		// Windows: every member holds it open delete-on-close, so it goes with the last member, killed or not.
		std::string SharedPath(const std::string& group)
		{
#ifdef _WIN32
			char dir[MAX_PATH];
			if (GetTempPathA(MAX_PATH, dir) == 0)
				return {};
			return std::string(dir) + "zdxsv_spec_sync_" + group;
#else
			return "/tmp/zdxsv_spec_sync_" + group;
#endif
		}

		Header* Map(const std::string& group)
		{
			const std::string path = SharedPath(group);
			if (path.empty())
				return nullptr;
#ifdef _WIN32
			HANDLE f = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
			if (f == INVALID_HANDLE_VALUE)
				return nullptr;
			HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READWRITE, 0, sizeof(Header), nullptr);
			void* v = m ? MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Header)) : nullptr;
			if (m)
				CloseHandle(m); // the view keeps the section
			if (!v)
			{
				CloseHandle(f);
				return nullptr;
			}
			s_file = f;
			return static_cast<Header*>(v);
#else
			const int fd = open(path.c_str(), O_RDWR | O_CREAT, 0666);
			if (fd < 0)
				return nullptr;
			if (ftruncate(fd, sizeof(Header)) != 0)
			{
				close(fd);
				return nullptr;
			}
			void* v = mmap(nullptr, sizeof(Header), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			close(fd);
			return v == MAP_FAILED ? nullptr : static_cast<Header*>(v);
#endif
		}

		bool Live(const Slot& s, int64_t now)
		{
			const int64_t hb = s.heartbeat_us.load(std::memory_order_acquire);
			return hb != 0 && now - hb <= STALE_US;
		}

		// Slowest frame of the live members (this one included), newest of those not catching up: a member catching up
		// holds the others (else in turbo its seek never reaches them), but is no target to seek to; false when it is alone.
		bool Range(int& slowest, int& newest)
		{
			const int64_t now = NowUs();
			int peers = 0;
			slowest = INT_MAX;
			newest = -1;
			for (const Slot& s : s_map->slots)
			{
				if (!Live(s, now))
					continue;
				peers++;
				const int f = s.frame.load(std::memory_order_acquire);
				slowest = std::min(slowest, f);
				if (!s.catching_up.load(std::memory_order_acquire))
					newest = std::max(newest, f);
			}
			return peers > 1;
		}
	} // namespace

	bool Join(const std::string& group, bool host)
	{
		Leave();
		Header* h = Map(group);
		if (!h)
		{
			Console.Error("ZdxsvSync: cannot map group %s", group.c_str());
			return false;
		}
		s_map = h;
		h->magic.store(MAGIC);
		const int64_t now = NowUs();
		for (int i = 0; i < SLOTS; i++)
		{
			Slot& s = h->slots[i];
			if (s.pid.load() == Pid() || !Live(s, now))
			{
				s.pid.store(Pid());
				s.frame.store(0);
				s.catching_up.store(1);
				s.heartbeat_us.store(now);
				s_slot = &s;
				s_host = host;
				s_group = group;
				if (host)
				{
					h->host_left.store(0);
					h->host_pid.store(Pid());
				}
				Console.WriteLn("ZdxsvSync: joined group %s in slot %d as %s", group.c_str(), i, host ? "host" : "guest");
				return true;
			}
		}
		Console.Error("ZdxsvSync: no free slot in group %s", group.c_str());
		Leave();
		return false;
	}

	void Leave()
	{
		if (s_slot)
			s_slot->heartbeat_us.store(0);
		if (s_map && s_host)
			s_map->host_left.store(1);
		s_slot = nullptr;
		if (s_map)
		{
#ifdef _WIN32
			UnmapViewOfFile(s_map);
			CloseHandle(s_file); // the last member's close deletes the file
			s_file = nullptr;
#else
			bool last = true;
			const int64_t now = NowUs();
			for (const Slot& s : s_map->slots)
				last &= !Live(s, now);
			munmap(s_map, sizeof(Header));
			if (last)
				unlink(SharedPath(s_group).c_str());
#endif
		}
		s_map = nullptr;
		s_host = false;
		s_group.clear();
	}

	bool Active() { return s_slot != nullptr; }
	bool IsHost() { return s_slot && s_host; }

	void Publish(int frame, bool catching_up)
	{
		if (!s_slot)
			return;
		s_slot->frame.store(frame, std::memory_order_release);
		s_slot->catching_up.store(catching_up ? 1 : 0, std::memory_order_release);
		s_slot->heartbeat_us.store(NowUs(), std::memory_order_release);
	}

	int Members(int& slowest, int& newest)
	{
		slowest = INT_MAX;
		newest = -1;
		if (!s_slot)
			return 0;
		int n = 0;
		const int64_t now = NowUs();
		for (const Slot& s : s_map->slots)
			n += Live(s, now);
		Range(slowest, newest);
		return n;
	}

	int Leader()
	{
		int slowest, newest;
		if (!s_slot || !Range(slowest, newest))
			return -1;
		return newest;
	}

	bool WaitForPeers(int frame, int max_wait_ms)
	{
		if (!s_slot)
			return false;
		Publish(frame, false);
		const int64_t deadline = NowUs() + static_cast<int64_t>(max_wait_ms) * 1000;
		for (;;)
		{
			int slowest, newest;
			if (!Range(slowest, newest) || slowest == INT_MAX)
				return false;
			// Only while someone is more than SLACK behind: whoever is behind is never held, so the group converges
			// (exact equality deadlocks members arriving out of step).
			if (frame <= slowest + SLACK)
				return true;
			if (NowUs() > deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
	}

	void HostPaused(bool paused)
	{
		if (IsHost())
			s_map->paused.store(paused ? 1 : 0);
	}

	void HostLimiter(int limiter)
	{
		if (IsHost())
			s_map->limiter.store(limiter + 1);
	}

	void HostSeek(int frame)
	{
		if (!IsHost())
			return;
		s_map->seek_target.store(frame);
		s_map->seek_gen.fetch_add(1, std::memory_order_release);
	}

	bool ReadControl(Control& out)
	{
		if (!s_map)
			return false;
		out.paused = s_map->paused.load() != 0;
		out.limiter = static_cast<int>(s_map->limiter.load()) - 1;
		out.seek_gen = s_map->seek_gen.load(std::memory_order_acquire);
		out.seek_target = s_map->seek_target.load();
		return true;
	}

	bool HostGone()
	{
		if (!s_map || s_host)
			return false;
		if (s_map->host_left.load())
			return true;
		const int32_t pid = s_map->host_pid.load();
		return pid != 0 && !PidAlive(pid);
	}

	int SelfPid() { return Pid(); }

#ifdef _WIN32
	namespace
	{
		// CommandLineToArgvW's rules: backslashes double only before a quote.
		std::wstring QuoteArg(const std::wstring& a)
		{
			if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
				return a;
			std::wstring q = L"\"";
			size_t bs = 0;
			for (const wchar_t c : a)
			{
				if (c == L'\\')
				{
					bs++;
					continue;
				}
				q.append(c == L'"' ? bs * 2 + 1 : bs, L'\\');
				bs = 0;
				q += c;
			}
			q.append(bs * 2, L'\\');
			return q + L"\"";
		}

		std::wstring Wide(const std::string& s)
		{
			std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
			return w;
		}

		struct WinSearch
		{
			DWORD pid;
			HWND best = nullptr;
			LONG area = 0;
		};

		BOOL CALLBACK FindLargest(HWND w, LPARAM lp)
		{
			WinSearch& f = *reinterpret_cast<WinSearch*>(lp);
			DWORD pid = 0;
			GetWindowThreadProcessId(w, &pid);
			RECT r;
			if (pid != f.pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER) || !GetWindowRect(w, &r))
				return TRUE;
			if (const LONG a = (r.right - r.left) * (r.bottom - r.top); a > f.area)
			{
				f.best = w;
				f.area = a;
			}
			return TRUE;
		}
	} // namespace

	int SpawnGuest(const std::vector<std::pair<std::string, std::string>>& env, const std::vector<std::string>& args)
	{
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argv)
			return 0;
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring cmd = QuoteArg(exe);
		int i = 1;
		for (; i < argc && std::wcscmp(argv[i], L"--") != 0; i++)
			cmd += L" " + QuoteArg(argv[i]);
		cmd += L" -batch"; // the guest quits with its VM
		for (const std::string& a : args)
			cmd += L" " + QuoteArg(Wide(a));
		for (; i < argc; i++)
			cmd += L" " + QuoteArg(argv[i]);
		LocalFree(argv);
		// the child inherits this process's environment block: set, spawn, restore
		std::vector<std::pair<std::wstring, std::wstring>> old;
		for (const auto& [k, v] : env)
		{
			const std::wstring wk = Wide(k);
			wchar_t buf[4096];
			const DWORD n = GetEnvironmentVariableW(wk.c_str(), buf, 4096);
			old.emplace_back(wk, n > 0 && n < 4096 ? std::wstring(buf, n) : std::wstring());
			SetEnvironmentVariableW(wk.c_str(), Wide(v).c_str());
		}
		STARTUPINFOW si = {};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi = {};
		const BOOL ok = CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
		for (const auto& [k, v] : old)
			SetEnvironmentVariableW(k.c_str(), v.empty() ? nullptr : v.c_str());
		if (!ok)
		{
			Console.Error("ZdxsvSync: CreateProcess failed (%lu)", GetLastError());
			return 0;
		}
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return static_cast<int>(pi.dwProcessId);
	}

	int TileWindows(const int pids[GRID])
	{
		WinSearch own{GetCurrentProcessId()};
		EnumWindows(FindLargest, reinterpret_cast<LPARAM>(&own));
		MONITORINFO mi = {sizeof(mi)};
		if (!GetMonitorInfoW(MonitorFromWindow(own.best, MONITOR_DEFAULTTOPRIMARY), &mi))
			return 0;
		const RECT& wa = mi.rcWork;
		const LONG w = wa.right - wa.left, h = wa.bottom - wa.top;
		int placed = 0;
		for (int c = 0; c < GRID; c++)
		{
			if (!pids[c])
				continue;
			WinSearch f{static_cast<DWORD>(pids[c])};
			EnumWindows(FindLargest, reinterpret_cast<LPARAM>(&f));
			if (!f.best)
				continue;
			// 1P top-left, 2P top-right, 3P bottom-left, 4P bottom-right; odd pixels to the right and bottom cells
			const LONG x0 = wa.left + (c % 2) * (w / 2), y0 = wa.top + (c / 2) * (h / 2);
			const LONG cw = c % 2 ? w - w / 2 : w / 2, ch = c / 2 ? h - h / 2 : h / 2;
			ShowWindow(f.best, SW_RESTORE);
			SetWindowPos(f.best, nullptr, x0, y0, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
			placed++;
		}
		return placed;
	}
#else
	int SpawnGuest(const std::vector<std::pair<std::string, std::string>>&, const std::vector<std::string>&)
	{
		Console.Error("ZdxsvSync: four-screen spawns guests on Windows only");
		return 0;
	}

	int TileWindows(const int[GRID]) { return 0; }
#endif
} // namespace Zdxsv::SpectateSync
