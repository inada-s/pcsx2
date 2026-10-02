// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ZDXSV_INPUT_LATENCY="key=value,..." (any value, even empty, turns it on):
//   btn=down     button pressed on even presses (up/down/left/right/circle/cross/triangle/square/start/select)
//   back=up      button pressed on odd presses, so a cursor returns; none = always btn
//   addr=0x...   EE address of the byte the press changes; omitted = search mode
//   count=20     presses
//   start=600    vsync (counted from boot) of the first press
//   go=path      instead of start: first press 60 vsyncs after this file appears (e.g. in battle)
//   held=1       search for bytes that differ only while the button is held (a pad buffer),
//                not bytes that stay changed after the press (a cursor)
//   hold=4       frames each press is held
//   gap=60       frames from one press to the next (>= hold + 20)
//   seed=1       seed of the press time inside the frame before its poll
//   out=path     also write one CSV line per press there
// Results go to the log, lines start with "ZdxsvLatency".

#include "ZdxsvInputLatency.h"

#include "Config.h"
#include "Memory.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"

#include "common/Console.h"
#include "common/Timer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace ZdxsvInputLatency
{
	bool g_enabled = std::getenv("ZDXSV_INPUT_LATENCY") != nullptr;

	namespace
	{
		struct Button
		{
			const char* name;
			u32 bind; // PadDualshock2::Inputs
			u32 bit; // bit of the SIO2 button word, 0 = pressed
		};

		constexpr Button BUTTONS[] = {
			{"up", PadDualshock2::Inputs::PAD_UP, 12},
			{"right", PadDualshock2::Inputs::PAD_RIGHT, 13},
			{"down", PadDualshock2::Inputs::PAD_DOWN, 14},
			{"left", PadDualshock2::Inputs::PAD_LEFT, 15},
			{"triangle", PadDualshock2::Inputs::PAD_TRIANGLE, 4},
			{"circle", PadDualshock2::Inputs::PAD_CIRCLE, 5},
			{"cross", PadDualshock2::Inputs::PAD_CROSS, 6},
			{"square", PadDualshock2::Inputs::PAD_SQUARE, 7},
			{"select", PadDualshock2::Inputs::PAD_SELECT, 8},
			{"start", PadDualshock2::Inputs::PAD_START, 11},
		};

		struct Press
		{
			const Button* btn = nullptr;
			double t_event = 0; // virtual host press, uniform in the frame before the poll
			u64 v_poll = 0;
			double t_poll = 0;
			bool sio = false;
			u64 v_sio = 0;
			double t_sio = 0;
			bool ram = false;
			u64 v_ram = 0;
			double t_ram = 0;
			u64 frame = 0; // frame pushed at v_ram
			bool present = false;
			double t_present = 0;
			u8 pre = 0, post = 0;
		};

		struct Config
		{
			const Button* btn = &BUTTONS[2];
			const Button* back = &BUTTONS[0];
			bool has_addr = false;
			u32 addr = 0;
			u32 count = 20;
			u64 start = 600;
			u32 hold = 4;
			u32 gap = 60;
			u32 seed = 1;
			bool held = false;
			std::string go;
			std::string out;
		};

		Config s_cfg;
		bool s_parsed = false;
		bool s_done = false;
		u64 s_vsync = 0;
		Common::Timer::Value s_t0 = 0;
		double s_t_prev_poll = 0;
		std::mt19937 s_rng;
		std::vector<Press> s_presses;
		Press* s_cur = nullptr; // press waiting for its SIO2 read
		u64 s_frames_pushed = 0;

		// Present times by frame number, written by the GS thread.
		constexpr u32 RING = 256;
		std::array<std::atomic<u64>, RING> s_present_t{};
		std::atomic<u64> s_presented{0};

		// Present -> present intervals while presses run (frame pacing), GS thread.
		std::atomic<bool> s_pace_on{false};
		std::mutex s_pace_mutex;
		std::vector<double> s_pace_ms;
		Common::Timer::Value s_pace_prev = 0;

		// Search mode: candidate bytes and snapshots of EE main RAM.
		std::vector<u8> s_cand, s_idle, s_pre, s_prev_pre, s_post, s_rel;
		std::vector<u8> s_post0, s_post1, s_pre0; // values seen at the first two presses

		// Search/watch index: EE main RAM, then the scratchpad (0x70000000).
		constexpr u32 SEARCH_SIZE = Ps2MemSize::MainRam + Ps2MemSize::Scratch;

		u8* ByteAt(u32 idx)
		{
			return (idx < Ps2MemSize::MainRam) ? &eeMem->Main[idx] : &eeMem->Scratch[idx - Ps2MemSize::MainRam];
		}

		u32 EeAddr(u32 idx)
		{
			return (idx < Ps2MemSize::MainRam) ? idx : 0x70000000 + (idx - Ps2MemSize::MainRam);
		}

		double Now()
		{
			return Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - s_t0);
		}

		const Button* FindButton(const std::string& name)
		{
			for (const Button& b : BUTTONS)
			{
				if (name == b.name)
					return &b;
			}
			return nullptr;
		}

		void Parse()
		{
			s_parsed = true;
			s_t0 = Common::Timer::GetCurrentValue();
			std::string spec = std::getenv("ZDXSV_INPUT_LATENCY");
			size_t pos = 0;
			while (pos < spec.size())
			{
				size_t end = spec.find(',', pos);
				if (end == std::string::npos)
					end = spec.size();
				const std::string item = spec.substr(pos, end - pos);
				pos = end + 1;
				const size_t eq = item.find('=');
				if (eq == std::string::npos)
					continue;
				const std::string key = item.substr(0, eq), val = item.substr(eq + 1);
				if (key == "btn" && FindButton(val))
					s_cfg.btn = FindButton(val);
				else if (key == "back")
					s_cfg.back = (val == "none") ? nullptr : FindButton(val);
				else if (key == "addr")
				{
					s_cfg.has_addr = true;
					const u32 v = static_cast<u32>(std::strtoul(val.c_str(), nullptr, 0));
					s_cfg.addr = ((v >> 28) == 7) ? Ps2MemSize::MainRam + (v & (Ps2MemSize::Scratch - 1)) : (v & (Ps2MemSize::MainRam - 1));
				}
				else if (key == "count")
					s_cfg.count = std::max(1ul, std::strtoul(val.c_str(), nullptr, 0));
				else if (key == "start")
					s_cfg.start = std::max(16ull, std::strtoull(val.c_str(), nullptr, 0));
				else if (key == "hold")
					s_cfg.hold = std::max(1ul, std::strtoul(val.c_str(), nullptr, 0));
				else if (key == "gap")
					s_cfg.gap = std::strtoul(val.c_str(), nullptr, 0);
				else if (key == "seed")
					s_cfg.seed = std::strtoul(val.c_str(), nullptr, 0);
				else if (key == "held")
					s_cfg.held = (val != "0");
				else if (key == "go")
					s_cfg.go = val;
				else if (key == "out")
					s_cfg.out = val;
				else
					Console.Warning("ZdxsvLatency: unknown key '%s'", item.c_str());
			}
			s_cfg.gap = std::max(s_cfg.gap, s_cfg.hold + 20);
			s_rng.seed(s_cfg.seed);
			s_presses.reserve(s_cfg.count);
			Console.WriteLn("ZdxsvLatency: btn=%s back=%s %s count=%u start=%llu hold=%u gap=%u seed=%u",
				s_cfg.btn->name, s_cfg.back ? s_cfg.back->name : "none",
				s_cfg.has_addr ? "addr" : (s_cfg.held ? "search-held" : "search"), s_cfg.count,
				static_cast<unsigned long long>(s_cfg.start), s_cfg.hold, s_cfg.gap, s_cfg.seed);
			if (s_cfg.has_addr)
				Console.WriteLn("ZdxsvLatency: addr=%08x", EeAddr(s_cfg.addr));
			Console.WriteLn("ZdxsvLatency: LowLatencyVsync=%d", EmuConfig.EmulationSpeed.LowLatencyVsync ? 1 : 0);
		}

		void Snapshot(std::vector<u8>& dst)
		{
			dst.resize(SEARCH_SIZE);
			std::memcpy(dst.data(), eeMem->Main, Ps2MemSize::MainRam);
			std::memcpy(dst.data() + Ps2MemSize::MainRam, eeMem->Scratch, Ps2MemSize::Scratch);
		}

		void SearchCycle(u32 k)
		{
			if (s_cand.empty())
				s_cand.assign(SEARCH_SIZE, 1);
			const bool check_return = !s_cfg.held && s_cfg.back && (k & 1) && !s_prev_pre.empty();
			for (u32 a = 0; a < SEARCH_SIZE; a++)
			{
				if (!s_cand[a])
					continue;
				if (s_idle[a] != s_pre[a] || s_post[a] == s_pre[a] || (check_return && s_post[a] != s_prev_pre[a]) ||
					(s_cfg.held && s_rel[a] != s_pre[a]))
					s_cand[a] = 0;
			}
			if (k == 0)
			{
				s_pre0 = s_pre;
				s_post0 = s_post;
			}
			else if (k == 1)
				s_post1 = s_post;
			s_prev_pre.swap(s_pre);
			u32 n = 0;
			for (u8 c : s_cand)
				n += c;
			Console.WriteLn("ZdxsvLatency: search press %u: %u candidate bytes", k, n);
			for (u32 a = 0; a < SEARCH_SIZE && n <= 16; a++)
			{
				if (s_cand[a])
					Console.WriteLn("ZdxsvLatency:   %08x %02x -> %02x", EeAddr(a), s_prev_pre[a], s_post[a]);
			}
		}

		void SearchReport()
		{
			// Bytes the first press moved by exactly 1 (a cursor index) first.
			u32 shown = 0;
			for (int pass = 0; pass < 2; pass++)
			{
				for (u32 a = 0; a < SEARCH_SIZE && shown < 32; a++)
				{
					const int step = s_post0[a] - s_pre0[a];
					if (!s_cand[a] || ((step == 1 || step == -1) != (pass == 0)))
						continue;
					shown++;
					Console.WriteLn("ZdxsvLatency: candidate %08x: %02x -> %02x -> %02x", EeAddr(a), s_pre0[a], s_post0[a],
						s_post1.empty() ? 0 : s_post1[a]);
				}
			}
			Console.WriteLn("ZdxsvLatency: search done");
		}

		struct Stat
		{
			const char* name;
			std::vector<double> ms, frames;
		};

		double mean(const std::vector<double>& v)
		{
			double sum = 0;
			for (double x : v)
				sum += x;
			return sum / v.size();
		}

		void Summarize(Stat& s)
		{
			if (s.ms.empty())
			{
				Console.WriteLn("ZdxsvLatency: %-14s n=0", s.name);
				return;
			}
			Console.WriteLn("ZdxsvLatency: %-14s n=%zu ms min %.2f mean %.2f max %.2f | frames min %.0f mean %.2f max %.0f",
				s.name, s.ms.size(), *std::min_element(s.ms.begin(), s.ms.end()), mean(s.ms),
				*std::max_element(s.ms.begin(), s.ms.end()), *std::min_element(s.frames.begin(), s.frames.end()),
				mean(s.frames), *std::max_element(s.frames.begin(), s.frames.end()));
		}

		void Report()
		{
			FILE* f = s_cfg.out.empty() ? nullptr : std::fopen(s_cfg.out.c_str(), "w");
			if (f)
				std::fprintf(f, "press,btn,pre,post,event_to_poll_ms,poll_to_sio_ms,poll_to_sio_frames,sio_to_ram_ms,"
								"poll_to_ram_frames,ram_to_present_ms,event_to_present_ms\n");
			Stat wait{"event->poll"}, sio{"poll->sio"}, ram{"sio->ram"}, pram{"poll->ram"}, pres{"ram->present"}, total{"event->present"};
			for (size_t i = 0; i < s_presses.size(); i++)
			{
				const Press& p = s_presses[i];
				wait.ms.push_back(p.t_poll - p.t_event);
				wait.frames.push_back(0);
				if (p.sio)
				{
					sio.ms.push_back(p.t_sio - p.t_poll);
					sio.frames.push_back(static_cast<double>(p.v_sio - p.v_poll));
				}
				if (p.sio && p.ram)
				{
					ram.ms.push_back(p.t_ram - p.t_sio);
					ram.frames.push_back(static_cast<double>(p.v_ram - p.v_sio));
				}
				if (p.ram)
				{
					pram.ms.push_back(p.t_ram - p.t_poll);
					pram.frames.push_back(static_cast<double>(p.v_ram - p.v_poll));
				}
				if (p.ram && p.present)
				{
					pres.ms.push_back(p.t_present - p.t_ram);
					pres.frames.push_back(0);
					total.ms.push_back(p.t_present - p.t_event);
					total.frames.push_back(static_cast<double>(p.v_ram - p.v_poll));
				}
				Console.WriteLn("ZdxsvLatency: press %zu %s %02x->%02x event->poll %.2f ms | poll->sio %s | poll->ram %s | ram->present %s",
					i, p.btn->name, p.pre, p.post, p.t_poll - p.t_event,
					p.sio ? (std::to_string(p.v_sio - p.v_poll) + " f " + std::to_string(p.t_sio - p.t_poll) + " ms").c_str() : "none",
					p.ram ? (std::to_string(p.v_ram - p.v_poll) + " f " + std::to_string(p.t_ram - p.t_poll) + " ms").c_str() : "none",
					p.present ? (std::to_string(p.t_present - p.t_ram) + " ms").c_str() : "none");
				if (f)
				{
					std::fprintf(f, "%zu,%s,%u,%u,%.3f,", i, p.btn->name, p.pre, p.post, p.t_poll - p.t_event);
					if (p.sio)
						std::fprintf(f, "%.3f,%llu,", p.t_sio - p.t_poll, static_cast<unsigned long long>(p.v_sio - p.v_poll));
					else
						std::fprintf(f, ",,");
					if (p.sio && p.ram)
						std::fprintf(f, "%.3f,", p.t_ram - p.t_sio);
					else
						std::fprintf(f, ",");
					if (p.ram)
						std::fprintf(f, "%llu,", static_cast<unsigned long long>(p.v_ram - p.v_poll));
					else
						std::fprintf(f, ",");
					if (p.ram && p.present)
						std::fprintf(f, "%.3f,%.3f\n", p.t_present - p.t_ram, p.t_present - p.t_event);
					else
						std::fprintf(f, ",\n");
				}
			}
			if (f)
				std::fclose(f);
			Summarize(wait);
			Summarize(sio);
			Summarize(ram);
			Summarize(pram);
			Summarize(pres);
			Summarize(total);
			std::lock_guard lock(s_pace_mutex);
			if (!s_pace_ms.empty())
			{
				const double m = mean(s_pace_ms);
				double var = 0;
				size_t late = 0;
				for (double v : s_pace_ms)
				{
					var += (v - m) * (v - m);
					late += v > 1.5 * m;
				}
				Console.WriteLn("ZdxsvLatency: present->present n=%zu ms min %.2f mean %.2f max %.2f sd %.2f >1.5x %zu",
					s_pace_ms.size(), *std::min_element(s_pace_ms.begin(), s_pace_ms.end()), m,
					*std::max_element(s_pace_ms.begin(), s_pace_ms.end()), std::sqrt(var / s_pace_ms.size()), late);
			}
			Console.WriteLn("ZdxsvLatency: done");
		}

		// Fill present times of earlier presses once the GS thread got there.
		void CollectPresents()
		{
			const u64 presented = s_presented.load(std::memory_order_acquire);
			for (Press& p : s_presses)
			{
				if (p.ram && !p.present && p.frame <= presented && p.frame + RING > presented)
				{
					p.present = true;
					p.t_present = Common::Timer::ConvertValueToMilliseconds(s_present_t[p.frame % RING].load() - s_t0);
				}
			}
		}
	} // namespace

	void OnVsync()
	{
		if (!s_parsed)
			Parse();
		if (s_done)
			return;
		s_frames_pushed++;
		s_vsync++;
		const double now = Now();
		const double prev_poll = s_t_prev_poll;
		s_t_prev_poll = now;
		if (!s_cfg.go.empty())
		{
			std::error_code ec;
			if (s_vsync % 30 != 0 || !std::filesystem::exists(s_cfg.go, ec))
				return;
			s_cfg.go.clear();
			s_cfg.start = s_vsync + 60;
			Console.WriteLn("ZdxsvLatency: go at vsync %llu", static_cast<unsigned long long>(s_vsync));
		}
		if (s_vsync + 8 < s_cfg.start)
			return;

		const u64 rel = s_vsync + 8 - s_cfg.start; // 8 frames of idle before the first press
		const u32 k = static_cast<u32>(rel / s_cfg.gap);
		const u32 phase = static_cast<u32>(rel % s_cfg.gap); // 8 = the press
		if (k >= s_cfg.count)
		{
			CollectPresents();
			if (phase >= 8) // last press' frames are presented by now
			{
				s_done = true;
				s_pace_on.store(false, std::memory_order_release);
				if (s_cfg.has_addr)
					Report();
				else
					SearchReport();
				s_cand.clear();
				s_idle.clear();
				s_pre.clear();
				s_prev_pre.clear();
				s_post.clear();
				s_rel.clear();
			}
			return;
		}
		const Button* btn = (s_cfg.back && (k & 1)) ? s_cfg.back : s_cfg.btn;

		if (phase == 0)
		{
			if (!s_cfg.has_addr)
				Snapshot(s_idle);
		}
		else if (phase == 8)
		{
			Press np;
			np.btn = btn;
			np.t_event = prev_poll + std::uniform_real_distribution<double>(0.0, 1.0)(s_rng) * (now - prev_poll);
			np.v_poll = s_vsync;
			np.t_poll = now;
			if (s_cfg.has_addr)
				np.pre = *ByteAt(s_cfg.addr);
			else
				Snapshot(s_pre);
			s_presses.push_back(np);
			s_cur = &s_presses.back();
			s_pace_on.store(true, std::memory_order_release);
			Pad::SetControllerState(0, btn->bind, 1.0f);
		}
		else if (phase == 8 + s_cfg.hold)
		{
			Pad::SetControllerState(0, btn->bind, 0.0f);
		}

		if (phase == 8 + s_cfg.hold && !s_cfg.has_addr && s_cfg.held)
			Snapshot(s_post); // frames emulated while held
		if (phase == s_cfg.gap - 1 && !s_cfg.has_addr)
		{
			Snapshot(s_cfg.held ? s_rel : s_post);
			SearchCycle(k);
		}
		CollectPresents();
	}

	void OnFrameEnd()
	{
		if (!s_parsed || s_done || !s_cfg.has_addr || s_presses.empty())
			return;
		// The frame emulated since the last poll ends here, before it is pushed; with the
		// default order the limiter sleep follows, with LowLatencyVsync the push does.
		Press& p = s_presses.back();
		if (!p.ram && *ByteAt(s_cfg.addr) != p.pre)
		{
			p.ram = true;
			p.v_ram = s_vsync + 1; // frames counted like polls: written after poll v_poll -> 1
			p.t_ram = Now();
			p.post = *ByteAt(s_cfg.addr);
			p.frame = s_frames_pushed + 1;
		}
	}

	void OnPadPoll(u8 unifiedSlot, u32 buttons)
	{
		if (!s_cur || unifiedSlot != 0 || (buttons & (1u << s_cur->btn->bit)))
			return;
		s_cur->sio = true;
		s_cur->v_sio = s_vsync;
		s_cur->t_sio = Now();
		s_cur = nullptr;
	}

	void OnPresent()
	{
		const u64 n = s_presented.load(std::memory_order_relaxed) + 1;
		s_present_t[n % RING].store(Common::Timer::GetCurrentValue(), std::memory_order_relaxed);
		s_presented.store(n, std::memory_order_release);
		const Common::Timer::Value t = s_present_t[n % RING].load(std::memory_order_relaxed);
		if (s_pace_on.load(std::memory_order_acquire))
		{
			if (s_pace_prev)
			{
				std::lock_guard lock(s_pace_mutex);
				s_pace_ms.push_back(Common::Timer::ConvertValueToMilliseconds(t - s_pace_prev));
			}
			s_pace_prev = t;
		}
	}
} // namespace ZdxsvInputLatency
