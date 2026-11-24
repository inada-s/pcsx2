// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// ZDXSV_HOST_LATENCY (docs/zdxsv/options.md), macOS: the host side of the pad-to-picture path, the part
// ZDXSV_INPUT_LATENCY does not see. Log lines start with "ZdxsvHostLatency".
//   input:  a face button's HID report reaches the OS -> the CPU thread gets its SDL event
//   window: GSDeviceMTL waits in nextDrawable; present submitted -> on the display; both: frame -> on the display
// Samples start skip= seconds (default 20) after the first present: boot is heavy and paces frames unevenly.
// Needs proper testing against a camera measurement.

#include "Zdxsv/InputHooks.h"
#include "Zdxsv/MediaHooks.h"
#include "Zdxsv/TestOptions.h"

#include "common/Console.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <mach/mach_time.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#endif

namespace Zdxsv
{
#ifdef __APPLE__
	const bool g_host_latency_enabled = Zdxsv::TestEnv("ZDXSV_HOST_LATENCY") != nullptr;

	namespace
	{
		// Face buttons: HID button usages 1-4 and SDL_GAMEPAD_BUTTON_SOUTH..NORTH (0-3), on a DualSense and
		// most pads. Other buttons are not paired: L2/R2 are HID buttons but SDL axes.
		constexpr u32 FACE_BUTTONS = 4;
		constexpr u64 PAIR_WINDOW_NS = 200'000'000;
		constexpr size_t INPUT_REPORT_EVERY = 10;

		struct Samples
		{
			std::vector<double> ms;

			void Report(const char* name)
			{
				if (ms.empty())
					return;
				std::sort(ms.begin(), ms.end());
				const auto at = [this](double q) { return ms[std::min(ms.size() - 1, static_cast<size_t>(q * ms.size()))]; };
				Console.WriteLn("ZdxsvHostLatency: %s n=%zu min=%.1f median=%.1f p90=%.1f max=%.1f ms", name, ms.size(), ms.front(),
					at(0.5), at(0.9), ms.back());
				ms.clear();
			}
		};

		struct HidWatch
		{
			std::mutex mutex; // all below; the HID thread and the CPU thread
			std::thread thread;
			std::atomic<bool> stop{false};
			std::deque<u64> presses; // HID face button presses not yet seen by SDL, ns
			Samples input;
			u64 unpaired = 0;

			// A process with no VM shutdown (the replay parent) still has the thread at exit.
			~HidWatch()
			{
				stop.store(true, std::memory_order_release);
				if (thread.joinable())
					thread.join();
			}
		};
		HidWatch s_hid;

		struct WindowStats
		{
			std::mutex mutex; // GS thread and Metal completion threads
			Samples acquire, present, total;
			double frame_start_s = 0; // GS thread: BeginPresent of the frame being presented
			u64 dropped = 0;
			size_t frames = 0;
		};
		WindowStats s_window;

		// ns of the first present + skip; 0 = no present yet. Reset by HostLatencyReset (a new VM boots again).
		std::atomic<u64> s_measure_from{0};

		double Key(const char* key, double def, double lo, double hi)
		{
			const char* v = Zdxsv::TestEnv("ZDXSV_HOST_LATENCY");
			const char* k = v ? std::strstr(v, key) : nullptr;
			return std::clamp(k ? std::strtod(k + std::strlen(key), nullptr) : def, lo, hi);
		}

		u64 SkipNs()
		{
			return static_cast<u64>(Key("skip=", 20.0, 0.0, 3600.0) * 1e9);
		}

		size_t WindowReportEvery()
		{
			static const size_t n = static_cast<size_t>(Key("every=", 600.0, 10.0, 100000.0));
			return n;
		}

		// mach_absolute_time in ns: the clock of IOHIDValueGetTimeStamp and CACurrentMediaTime.
		u64 NowNs()
		{
			return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
		}

		bool Measuring(u64 now)
		{
			const u64 from = s_measure_from.load(std::memory_order_acquire);
			return from != 0 && now >= from;
		}

		u64 MachToNs(u64 t)
		{
			static const mach_timebase_info_data_t tb = [] {
				mach_timebase_info_data_t i;
				mach_timebase_info(&i);
				return i;
			}();
			return static_cast<u64>(static_cast<unsigned __int128>(t) * tb.numer / tb.denom);
		}

		void OnHidValue(void*, IOReturn, void*, IOHIDValueRef value)
		{
			IOHIDElementRef el = IOHIDValueGetElement(value);
			const u32 usage = IOHIDElementGetUsage(el);
			if (IOHIDElementGetUsagePage(el) != kHIDPage_Button || usage < 1 || usage > FACE_BUTTONS ||
				IOHIDValueGetIntegerValue(value) == 0)
				return;
			const u64 t = MachToNs(IOHIDValueGetTimeStamp(value));
			std::lock_guard lock(s_hid.mutex);
			if (s_hid.presses.size() >= 16)
				s_hid.presses.pop_front();
			s_hid.presses.push_back(t);
		}

		CFDictionaryRef Matching(u32 usage)
		{
			const int page = kHIDPage_GenericDesktop;
			const int u = static_cast<int>(usage);
			CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
			CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &u);
			const void* keys[] = {CFSTR(kIOHIDDeviceUsagePageKey), CFSTR(kIOHIDDeviceUsageKey)};
			const void* values[] = {p, n};
			CFDictionaryRef d = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
				&kCFTypeDictionaryValueCallBacks);
			CFRelease(p);
			CFRelease(n);
			return d;
		}

		// Its own manager and run loop: SDL's hidapi opens the pad non-exclusively, so both get every report.
		void HidThread()
		{
			IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
			const void* match[] = {Matching(kHIDUsage_GD_GamePad), Matching(kHIDUsage_GD_Joystick)};
			CFArrayRef arr = CFArrayCreate(kCFAllocatorDefault, match, 2, &kCFTypeArrayCallBacks);
			CFRelease(match[0]);
			CFRelease(match[1]);
			IOHIDManagerSetDeviceMatchingMultiple(mgr, arr);
			CFRelease(arr);
			IOHIDManagerRegisterInputValueCallback(mgr, OnHidValue, nullptr);
			IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
			const IOReturn r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
			Console.WriteLn("ZdxsvHostLatency: HID watch %s (0x%x); press the face buttons", r == kIOReturnSuccess ? "on" : "failed", r);
			while (!s_hid.stop.load(std::memory_order_acquire))
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
			IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
			IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
			CFRelease(mgr);
		}
	} // namespace

	void HostLatencyStart()
	{
		std::lock_guard lock(s_hid.mutex);
		if (s_hid.thread.joinable())
			return;
		s_hid.stop.store(false, std::memory_order_release);
		s_hid.thread = std::thread(HidThread);
	}

	void HostLatencyReset()
	{
		if (!g_host_latency_enabled)
			return;
		std::thread t;
		{
			std::lock_guard lock(s_hid.mutex);
			s_hid.stop.store(true, std::memory_order_release);
			t = std::move(s_hid.thread);
		}
		if (t.joinable())
			t.join();
		std::lock_guard lock(s_hid.mutex);
		s_hid.input.Report("input HID -> SDL event");
		s_hid.presses.clear();
		s_hid.unpaired = 0;
		s_measure_from.store(0, std::memory_order_release);
	}

	void HostLatencyOnSdlButton(u32 button, bool down)
	{
		if (!down || button >= FACE_BUTTONS)
			return;
		HostLatencyStart();
		const u64 now = NowNs();
		if (!Measuring(now))
			return;
		std::lock_guard lock(s_hid.mutex);
		while (!s_hid.presses.empty() && now - s_hid.presses.front() > PAIR_WINDOW_NS)
			s_hid.presses.pop_front();
		if (s_hid.presses.empty())
		{
			s_hid.unpaired++;
			Console.WriteLn("ZdxsvHostLatency: press with no HID report within 200 ms (%llu)", s_hid.unpaired);
			return;
		}
		const double ms = static_cast<double>(now - s_hid.presses.front()) / 1e6;
		s_hid.presses.pop_front();
		Console.WriteLn("ZdxsvHostLatency: press HID -> SDL event %.1f ms", ms);
		s_hid.input.ms.push_back(ms);
		if (s_hid.input.ms.size() >= INPUT_REPORT_EVERY)
			s_hid.input.Report("input HID -> SDL event");
	}

	void HostLatencyOnDrawableWait(double start_s, double end_s)
	{
		HostLatencyStart();
		const u64 now = NowNs();
		u64 expected = 0;
		if (s_measure_from.compare_exchange_strong(expected, now + SkipNs(), std::memory_order_acq_rel))
			Console.WriteLn("ZdxsvHostLatency: samples start in %.0f s", static_cast<double>(SkipNs()) / 1e9);
		if (!Measuring(now))
			return;
		std::lock_guard lock(s_window.mutex);
		s_window.frame_start_s = start_s;
		s_window.acquire.ms.push_back((end_s - start_s) * 1000.0);
	}

	double HostLatencyFrameStart()
	{
		std::lock_guard lock(s_window.mutex);
		return s_window.frame_start_s;
	}

	void HostLatencyOnPresented(double start_s, double submit_s, double presented_s)
	{
		if (start_s <= 0 || !Measuring(static_cast<u64>(start_s * 1e9)))
			return;
		std::lock_guard lock(s_window.mutex);
		if (presented_s <= 0)
			s_window.dropped++;
		else
		{
			s_window.present.ms.push_back((presented_s - submit_s) * 1000.0);
			s_window.total.ms.push_back((presented_s - start_s) * 1000.0);
		}
		if (++s_window.frames < WindowReportEvery())
			return;
		s_window.acquire.Report("window nextDrawable wait");
		s_window.present.Report("window present -> on display");
		s_window.total.Report("window frame -> on display");
		Console.WriteLn("ZdxsvHostLatency: window %zu frames, %llu not shown", s_window.frames, s_window.dropped);
		s_window.frames = 0;
		s_window.dropped = 0;
	}
#else
	const bool g_host_latency_enabled = false;

	void HostLatencyReset() {}
	void HostLatencyOnSdlButton(u32, bool) {}
	void HostLatencyOnDrawableWait(double, double) {}
	double HostLatencyFrameStart() { return 0; }
	void HostLatencyOnPresented(double, double, double) {}
#endif
} // namespace Zdxsv
