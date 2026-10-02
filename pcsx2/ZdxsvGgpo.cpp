// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ZDXSV_GGPO="key=value,...": a GGPO session in a running game. Synctest by default:
// every frame is saved, and every `check` frames GGPO loads the frame `check` back, reruns the
// frames with the same inputs and compares the state checksums (EE RAM + delta state).
// With p2p= a 2-player P2P session instead: this instance plays one pad (random input, seeded per
// player), the peer the other. Both must start from the same state (-statefile). At the end the
// final checksum of every frame goes to logs/zdxsv_sync.txt: diff the two peers' files.
//   p2p=1        local player 1 or 2
//   port=7001    local UDP port
//   peer=7002    peer UDP port, on host= (default 127.0.0.1)
//   delay=0      GGPO frame delay of the local input
//   sync=0       no state hashes: checksum 0, no zdxsv_sync.txt, no dump= (play, not test)
//   dump=F       write the final delta state of frame F (scratch masked) to logs/zdxsv_dump.bin
//   start=1500   vsync (counted from boot) the session starts at
//   frames=3000  frames the session runs, then it is closed and reported
//   check=6      synctest check distance (1..6)
//   seed=1       random pad input (both pads; a new input every 5 frames)
//   input=host   pad 1 from the host pad instead of random (pad 2 stays random)
//   input=none   no buttons, sticks centered
//   mask=fcff    random buttons limited to these bits (hex, PadDualshock2::Inputs; fcff = no Select/Start)
//   control=input  control run: reruns get other inputs (must report mismatches)
//   iopflush=1   probe: reset the IOP recompiler at every save and load (empty IOP code cache per frame)
//   trace=F      probe: log SPU2 register access and IOP interrupts (with psxRegs.cycle) of frames F-7..F
//                to logs/zdxsv_trace.txt, for the first run and every rerun
// Results go to the log, lines start with "ZdxsvGgpo".

#include "ZdxsvGgpo.h"
#include "ZdxsvDeltaState.h"
#include "Config.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "SaveState.h"

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Console.h"
#include "common/StringUtil.h"
#include "common/Threading.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include "ggpo_log.h"
#include "ggponet.h"

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include "xxhash.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

extern u64 g_zdxsv_rec_counts[6]; // x86/ix86-32/iR5900.cpp

namespace ZdxsvGgpo
{
	bool g_enabled = std::getenv("ZDXSV_GGPO") != nullptr;
	bool g_active = false;
	bool g_in_rollback = false;
	std::FILE* g_trace = nullptr;

	namespace
	{
		constexpr int PLAYERS = 2;
		constexpr u32 BUTTONS = 16; // PadDualshock2::Inputs PAD_UP .. PAD_R3

		struct Input
		{
			u16 buttons; // bit i = PadDualshock2::Inputs i held
			u8 lx, ly, rx, ry;
			u8 unused[2];
		};
		static_assert(sizeof(Input) == 8);

		struct Stat
		{
			double sum = 0, max = 0;
			int n = 0;
			void Add(double v)
			{
				sum += v;
				max = std::max(max, v);
				n++;
			}
			double Mean() const { return n ? sum / n : 0; }
		};

		int s_start = 1500, s_frames = 3000, s_check = 6;
		u32 s_seed = 1;
		bool s_host_input = false, s_no_input = false, s_control_input = false;
		u16 s_mask = 0xffff;
		bool s_iop_flush = false;
		bool s_sync = true; // sync=0: no state hashes (checksum 0, no zdxsv_sync.txt)
		int s_trace_frame = 0; // trace= probe
		std::FILE* s_trace_file = nullptr;
		int s_p2p = 0; // local player 1/2, 0 = synctest
		int s_probe = 0; // probe= (no session)
		int s_port = 7001, s_peer_port = 7002, s_delay = 0, s_dump_frame = -1;
		std::string s_ref; // p2p: the peer's zdxsv_dump.bin, diffed against ours at the end
		std::string s_peer_host = "127.0.0.1";
		bool s_running = false; // p2p: GGPO_EVENTCODE_RUNNING seen
		bool s_disconnected = false;
		int s_frames_ahead = 0; // p2p: last GGPO_EVENTCODE_TIMESYNC
		int s_waits = 0; // p2p: frames that waited for the peer
		std::map<int, std::pair<u64, u64>> s_sums; // p2p: frame -> (EE RAM hash, delta state hash), last save wins

		GGPOSession* s_session = nullptr;
		GGPOPlayerHandle s_handles[PLAYERS] = {};
		bool s_started = false; // the session was opened once (it is not reopened)
		bool s_frame_ended = false; // the CPU left Execute() at a vsync
		int s_vsyncs = 0;
		int s_session_frames = 0;
		std::mt19937 s_rng;
		Input s_random[PLAYERS] = {};
		std::array<float, PadDualshock2::Inputs::LENGTH> s_host = {};

		int s_rollback_frames = 0, s_loads = 0, s_mismatches = 0, s_errors = 0;
		Stat s_save_ms, s_hash_ms, s_load_ms;
		Stat s_rerun_ms, s_wait_ms; // between frames: rollback rerun emulation, p2p wait for the peer
		Stat s_emu_ms, s_exit_ms, s_ours_ms; // wall: last OnExecuteReturned end -> OnVsync -> OnExecuteReturned start -> its end
		Common::Timer::Value s_t_vsync = 0, s_t_returned = 0;

		void Parse()
		{
			for (const std::string_view item : StringUtil::SplitString(std::getenv("ZDXSV_GGPO"), ','))
			{
				const size_t eq = item.find('=');
				if (eq == std::string_view::npos)
					continue;
				const std::string_view key = item.substr(0, eq);
				const std::string_view value = item.substr(eq + 1);
				const int n = StringUtil::FromChars<int>(value).value_or(0);
				if (key == "start")
					s_start = n;
				else if (key == "frames")
					s_frames = n;
				else if (key == "check")
					s_check = std::clamp(n, 1, 6); // GGPO keeps MAX_PREDICTION_FRAMES + 2 = 8 states: frames 0..check
				else if (key == "seed")
					s_seed = static_cast<u32>(n);
				else if (key == "input")
				{
					s_host_input = (value == "host");
					s_no_input = (value == "none");
				}
				else if (key == "mask")
					s_mask = StringUtil::FromChars<u16>(value, 16).value_or(0xffff);
				else if (key == "control")
					s_control_input = (value == "input");
				else if (key == "iopflush")
					s_iop_flush = (n != 0);
				else if (key == "trace")
					s_trace_frame = n;
				else if (key == "p2p")
					s_p2p = std::clamp(n, 0, PLAYERS);
				else if (key == "port")
					s_port = n;
				else if (key == "peer")
					s_peer_port = n;
				else if (key == "host")
					s_peer_host = std::string(value);
				else if (key == "delay")
					s_delay = n;
				else if (key == "probe")
					s_probe = n;
				else if (key == "ref")
					s_ref = std::string(value);
				else if (key == "dump")
					s_dump_frame = n;
				else if (key == "sync")
					s_sync = (n != 0);
				else
					Console.Warning("ZdxsvGgpo: unknown key '%.*s'", static_cast<int>(key.size()), key.data());
			}
		}

		void Report(const char* what)
		{
			Console.WriteLn("ZdxsvGgpo: %s frames %d rollback frames %d loads %d mismatches %d errors %d | save ms mean %.3f max %.3f | hash ms mean %.3f | load ms mean %.3f max %.3f",
				what, s_session_frames, s_rollback_frames, s_loads, s_mismatches, s_errors, s_save_ms.Mean(), s_save_ms.max,
				s_hash_ms.Mean(), s_load_ms.Mean(), s_load_ms.max);
			Console.WriteLn("ZdxsvGgpo: %s wall ms per frame: emulate mean %.2f max %.1f | exit %.2f | between frames (save, ggpo, rollbacks) mean %.2f max %.1f",
				what, s_emu_ms.Mean(), s_emu_ms.max, s_exit_ms.Mean(), s_ours_ms.Mean(), s_ours_ms.max);
			const double n = std::max(s_session_frames, 1);
			const double ours = s_ours_ms.sum / n, split = (s_save_ms.sum + s_hash_ms.sum + s_load_ms.sum + s_rerun_ms.sum + s_wait_ms.sum) / n;
			Console.WriteLn("ZdxsvGgpo: %s between frames ms per frame %.2f: save %.2f hash %.2f load %.2f rerun %.2f wait %.2f rest (ggpo) %.2f | sync=%d",
				what, ours, s_save_ms.sum / n, s_hash_ms.sum / n, s_load_ms.sum / n, s_rerun_ms.sum / n, s_wait_ms.sum / n, ours - split, s_sync);
			Console.WriteLn("ZdxsvGgpo: %s delta %s | %s", what, ZdxsvDeltaState::Times().c_str(), SaveState_DeltaTimes().c_str());
			Console.WriteLn("ZdxsvGgpo: %s EE rec per frame: recompiles %.1f manual discards %.1f recClear %.1f page resets %.1f overlap clears %.1f vtlb protect clears %.1f backpatch clears %.1f",
				what, g_zdxsv_rec_counts[0] / n, g_zdxsv_rec_counts[1] / n, g_zdxsv_rec_counts[2] / n,
				g_zdxsv_rec_counts[3] / n, g_zdxsv_rec_counts[4] / n, (g_zdxsv_rec_counts[5] % 1000000) / n, (g_zdxsv_rec_counts[5] / 1000000) / n);
		}

		Input HostInput()
		{
			Input in = {};
			for (u32 i = 0; i < BUTTONS; i++)
				if (s_host[i] >= 0.5f)
					in.buttons |= static_cast<u16>(1u << i);
			const auto axis = [](float neg, float pos) {
				return static_cast<u8>(std::clamp(127.5f + (pos - neg) * 127.5f, 0.0f, 255.0f));
			};
			using I = PadDualshock2::Inputs;
			in.lx = axis(s_host[I::PAD_L_LEFT], s_host[I::PAD_L_RIGHT]);
			in.ly = axis(s_host[I::PAD_L_UP], s_host[I::PAD_L_DOWN]);
			in.rx = axis(s_host[I::PAD_R_LEFT], s_host[I::PAD_R_RIGHT]);
			in.ry = axis(s_host[I::PAD_R_UP], s_host[I::PAD_R_DOWN]);
			return in;
		}

		void ApplyInputs(const Input* inputs)
		{
			for (int p = 0; p < PLAYERS; p++)
			{
				PadBase* pad = Pad::GetPad(static_cast<u8>(p));
				if (!pad)
					continue;
				const Input& in = inputs[p];
				for (u32 i = 0; i < BUTTONS; i++)
				{
					const bool held = (in.buttons >> i) & 1;
					pad->SetRawPressureButton(i, std::make_tuple(held, static_cast<u8>(held ? 255 : 0)));
				}
				pad->SetRawAnalogs({in.lx, in.ly}, {in.rx, in.ry});
			}
		}

		bool SyncAndApply(bool rerun)
		{
			Input inputs[PLAYERS] = {};
			int disconnect_flags = 0;
			const GGPOErrorCode rc = ggpo_synchronize_input(s_session, inputs, sizeof(inputs), &disconnect_flags);
			if (rc != GGPO_OK)
			{
				Console.Error("ZdxsvGgpo: synchronize_input %d", rc);
				return false;
			}
			if (rerun && s_control_input)
				inputs[0].buttons ^= static_cast<u16>(s_rng() | 1);
			ApplyInputs(inputs);
			return true;
		}

		// Local inputs of the next frame, then the synced inputs go to the pads.
		bool NextInputs()
		{
			if (s_session_frames % 5 == 0)
			{
				for (Input& in : s_random)
				{
					in.buttons = static_cast<u16>(s_rng() & s_rng() & s_mask);
					if (s_no_input)
						in.buttons = 0;
					in.lx = static_cast<u8>(s_rng());
					in.ly = static_cast<u8>(s_rng());
					in.rx = in.ry = Pad::ANALOG_NEUTRAL_POSITION;
					if (s_no_input)
						in.lx = in.ly = Pad::ANALOG_NEUTRAL_POSITION;
				}
			}
			for (int p = 0; p < PLAYERS; p++)
			{
				if (s_p2p && p != s_p2p - 1)
					continue;
				Input in = (p == 0 && s_host_input) ? HostInput() : s_random[p];
				GGPOErrorCode rc = ggpo_add_local_input(s_session, s_handles[p], &in, sizeof(in));
				// p2p: too far ahead of the peer's confirmed input, wait for it.
				if (rc == GGPO_ERRORCODE_PREDICTION_THRESHOLD)
				{
					s_waits++;
					Common::Timer wait;
					while (rc == GGPO_ERRORCODE_PREDICTION_THRESHOLD && !s_disconnected && wait.GetTimeSeconds() < 10)
					{
						ggpo_idle(s_session, 0);
						Threading::Sleep(1);
						rc = ggpo_add_local_input(s_session, s_handles[p], &in, sizeof(in));
					}
				}
				if (rc != GGPO_OK)
				{
					Console.Error("ZdxsvGgpo: add_local_input %d", rc);
					return false;
				}
			}
			return SyncAndApply(false);
		}

		// Runs the CPU to the next vsync.
		bool RunFrame()
		{
			s_frame_ended = false;
			Cpu->Execute();
			if (!std::exchange(s_frame_ended, false))
			{
				Console.Error("ZdxsvGgpo: the CPU stopped before the end of a rerun frame");
				return false;
			}
			return true;
		}

		// First-run save of a frame, to name what a rerun changed.
		struct Sample
		{
			std::vector<u64> pages; // hash per EE RAM page
			std::vector<u8> state; // the rest
		};
		constexpr u32 PAGE_SIZE = 4096;
		std::map<int, Sample> s_first;
		Sample s_dump; // p2p: final save of frame dump=
		bool s_rerun = false; // the save is of a rerun frame
		int s_diff_logged = 0;

		void Diff(int frame, const Sample& first, const std::vector<u64>& pages, const std::vector<u8>& state)
		{
			if (s_diff_logged >= 60)
				return;
			for (size_t i = 0; i < pages.size() && s_diff_logged < 60; i++)
			{
				if (first.pages[i] != pages[i])
				{
					Console.WriteLn("ZdxsvGgpo: DIFF frame %d EE page 0x%08zx", frame, i * PAGE_SIZE);
					s_diff_logged++;
				}
			}
			size_t last = 0;
			bool any = false;
			for (size_t i = 0; i < std::min(state.size(), first.state.size()) && s_diff_logged < 60; i++)
			{
				if (first.state[i] == state[i])
					continue;
				if (!any || i > last + 8)
				{
					Console.WriteLn("ZdxsvGgpo: DIFF frame %d state offset %zu = %s: %02x -> %02x", frame, i,
						SaveState_DeltaDescribe(first.state, i).c_str(), first.state[i], state[i]);
					// Words around it, first run / rerun (pointers name the code that wrote them).
					const size_t w0 = (i & ~size_t{3}) >= 16 ? (i & ~size_t{3}) - 16 : 0;
					std::string a, b;
					for (size_t w = w0; w + 4 <= std::min(state.size(), first.state.size()) && w < w0 + 36; w += 4)
					{
						u32 x, y;
						std::memcpy(&x, &first.state[w], 4);
						std::memcpy(&y, &state[w], 4);
						a += fmt::format(" {:08x}", x);
						b += fmt::format(" {:08x}", y);
					}
					Console.WriteLn("ZdxsvGgpo: DIFF words from offset %zu first:%s", w0, a.c_str());
					Console.WriteLn("ZdxsvGgpo: DIFF words from offset %zu rerun:%s", w0, b.c_str());
					s_diff_logged++;
				}
				any = true;
				last = i;
			}
			if (state.size() != first.state.size())
				Console.WriteLn("ZdxsvGgpo: DIFF frame %d state size %zu -> %zu", frame, first.state.size(), state.size());
		}

		bool __cdecl BeginGame(const char*) { return true; }
		bool __cdecl OnEvent(GGPOEvent* ev)
		{
			switch (ev->code)
			{
				case GGPO_EVENTCODE_RUNNING:
					s_running = true;
					Console.WriteLn("ZdxsvGgpo: p2p running");
					break;
				case GGPO_EVENTCODE_TIMESYNC:
					s_frames_ahead = ev->u.timesync.frames_ahead;
					break;
				case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
					s_disconnected = true;
					Console.Warning("ZdxsvGgpo: p2p peer disconnected");
					break;
				case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
					Console.Warning("ZdxsvGgpo: p2p connection interrupted");
					break;
				default:
					break;
			}
			return true;
		}

		// trace= probe: the next frame to run is `frame`.
		void TraceFrame(const char* what, int frame)
		{
			if (s_trace_frame <= 0)
				return;
			g_trace = nullptr;
			if (s_trace_file)
				std::fflush(s_trace_file);
			if (frame < s_trace_frame - 7 || frame >= s_trace_frame)
				return;
			if (!s_trace_file)
				s_trace_file = FileSystem::OpenCFile(Path::Combine(EmuFolders::Logs, "zdxsv_trace.txt").c_str(), "w");
			if (!s_trace_file)
				return;
			std::fprintf(s_trace_file, "== %s %d cycle %08x\n", what, frame, psxRegs.cycle);
			g_trace = s_trace_file;
		}

		void HashSave(int frame, int* checksum);

		bool __cdecl SaveGameState(unsigned char** buffer, int* len, int* checksum, int frame)
		{
			TraceFrame("save", frame);
			Common::Timer timer;
			if (s_iop_flush)
				psxCpu->Reset();
			if (!ZdxsvDeltaState::Save(frame))
				return false;
			s_save_ms.Add(timer.GetTimeMilliseconds());
			timer.Reset();
			*checksum = 0;
			if (s_sync)
				HashSave(frame, checksum);
			s_hash_ms.Add(timer.GetTimeMilliseconds());
			int* saved = new int(frame);
			*buffer = reinterpret_cast<unsigned char*>(saved);
			*len = sizeof(int);
			// GGPO never goes back further than its check distance / prediction window.
			ZdxsvDeltaState::DiscardBefore(frame - std::max(s_check, 8) - 4);
			return true;
		}

		// sync=1 part of SaveGameState: checksum, p2p per-frame sums and dump, synctest diff samples.
		void HashSave(int frame, int* checksum)
		{
			const std::vector<u8>* state = ZdxsvDeltaState::GetState(frame);
			Sample sample;
			sample.pages.resize(Ps2MemSize::ExposedRam / PAGE_SIZE);
			for (size_t i = 0; i < sample.pages.size(); i++)
				sample.pages[i] = XXH3_64bits(&eeMem->Main[i * PAGE_SIZE], PAGE_SIZE);
			const u64 ram_hash = XXH3_64bits(sample.pages.data(), sample.pages.size() * sizeof(u64));
			const u64 state_hash = ZdxsvDeltaState::HashState(*state);
			const u64 hash = ram_hash ^ state_hash;
			*checksum = static_cast<int>(hash ^ (hash >> 32));
			if (s_p2p)
			{
				// A rerun after a misprediction legitimately differs: keep the last save of each frame.
				s_sums[frame] = {ram_hash, state_hash};
				if (frame == s_dump_frame)
				{
					std::vector<u8> masked = *state;
					ZdxsvDeltaState::MaskScratch(masked);
					if (std::FILE* f = FileSystem::OpenCFile(Path::Combine(EmuFolders::Logs, "zdxsv_dump.bin").c_str(), "wb"))
					{
						std::fwrite(sample.pages.data(), sizeof(u64), sample.pages.size(), f);
						std::fwrite(masked.data(), 1, masked.size(), f);
						std::fclose(f);
					}
					s_dump.pages = sample.pages;
					s_dump.state = masked;
				}
				return;
			}
			std::vector<u8> masked = *state;
			ZdxsvDeltaState::MaskScratch(masked);
			if (s_rerun)
			{
				const auto first = s_first.find(frame);
				if (first != s_first.end())
					Diff(frame, first->second, sample.pages, masked);
			}
			else
			{
				sample.state = std::move(masked);
				s_first[frame] = std::move(sample);
				while (!s_first.empty() && s_first.begin()->first < frame - 16)
					s_first.erase(s_first.begin());
			}
		}

		bool __cdecl LoadGameState(unsigned char* buffer, int len)
		{
			if (len != sizeof(int))
				return false;
			Common::Timer timer;
			const bool ok = ZdxsvDeltaState::Load(*reinterpret_cast<int*>(buffer));
			if (s_iop_flush)
				psxCpu->Reset();
			TraceFrame("load", *reinterpret_cast<int*>(buffer));
			s_load_ms.Add(timer.GetTimeMilliseconds());
			s_loads++;
			return ok;
		}

		bool __cdecl LogGameState(char* filename, unsigned char* buffer, int len)
		{
			// Called by the synctest on a checksum mismatch, for the original and the rerun.
			if (std::strstr(filename, "original"))
			{
				if (s_mismatches++ < 20)
					Console.WriteLn("ZdxsvGgpo: MISMATCH %s (session frame %d)", filename, s_session_frames);
			}
			return true;
		}

		void __cdecl FreeBuffer(void* buffer)
		{
			delete static_cast<int*>(buffer);
		}

		bool __cdecl AdvanceFrame(int)
		{
			if (!SyncAndApply(true))
				return false;
			g_in_rollback = true;
			Common::Timer timer;
			const bool ok = RunFrame();
			s_rerun_ms.Add(timer.GetTimeMilliseconds());
			g_in_rollback = false;
			s_rerun = true;
			ggpo_advance_frame(s_session);
			s_rerun = false;
			s_rollback_frames++;
			return ok;
		}

		void OnLog(int level, const char* msg)
		{
			if (level > GGPO_LOG_WARNING)
				return;
			if (s_errors++ < 20)
				Console.Warning("ZdxsvGgpo: ggpo: %s", msg);
		}

		void Stop(const char* what)
		{
			if (s_session)
				ggpo_close_session(s_session);
			s_session = nullptr;
			if (s_p2p && s_sync)
			{
				// Frames near the end may still be unconfirmed: leave out the last prediction window.
				int written = 0;
				if (std::FILE* f = FileSystem::OpenCFile(Path::Combine(EmuFolders::Logs, "zdxsv_sync.txt").c_str(), "w"))
				{
					for (const auto& [frame, sums] : s_sums)
					{
						if (frame >= s_session_frames - GGPO_MAX_PREDICTION_FRAMES - 2)
							break;
						std::fprintf(f, "%d %016llx %016llx\n", frame, static_cast<unsigned long long>(sums.first),
							static_cast<unsigned long long>(sums.second));
						written++;
					}
					std::fclose(f);
				}
				Console.WriteLn("ZdxsvGgpo: p2p sync file %d frames, waits %d", written, s_waits);
				// DIFF lines: "first" = the peer, "rerun" = this instance.
				if (!s_ref.empty() && !s_dump.pages.empty())
				{
					if (std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(s_ref.c_str()))
					{
						const size_t page_bytes = s_dump.pages.size() * sizeof(u64);
						if (data->size() >= page_bytes)
						{
							Sample peer;
							peer.pages.resize(s_dump.pages.size());
							std::memcpy(peer.pages.data(), data->data(), page_bytes);
							peer.state.assign(data->data() + page_bytes, data->data() + data->size());
							s_diff_logged = 0;
							Diff(s_dump_frame, peer, s_dump.pages, s_dump.state);
							Console.WriteLn("ZdxsvGgpo: p2p dump diff vs %s done", s_ref.c_str());
						}
					}
					else
						Console.Warning("ZdxsvGgpo: p2p no peer dump %s", s_ref.c_str());
				}
			}
			ggpo_set_log_function(nullptr);
			g_active = false;
			ZdxsvDeltaState::Clear();
			Report(what);
		}

		bool Start()
		{
			ggpo_set_log_function(OnLog);
			GGPOSessionCallbacks cb{};
			cb.begin_game = BeginGame;
			cb.save_game_state = SaveGameState;
			cb.load_game_state = LoadGameState;
			cb.log_game_state = LogGameState;
			cb.free_buffer = FreeBuffer;
			cb.advance_frame = AdvanceFrame;
			cb.on_event = OnEvent;
			if (s_p2p)
			{
				if (ggpo_start_session(&s_session, &cb, "zdxsv", PLAYERS, sizeof(Input), static_cast<unsigned short>(s_port), nullptr, 0) != GGPO_OK)
				{
					s_session = nullptr;
					return false;
				}
				ggpo_set_disconnect_timeout(s_session, 5000);
				ggpo_set_disconnect_notify_start(s_session, 1000);
				for (int p = 0; p < PLAYERS; p++)
				{
					GGPOPlayer player{};
					player.size = sizeof(GGPOPlayer);
					player.player_num = p + 1;
					player.type = (p == s_p2p - 1) ? GGPO_PLAYERTYPE_LOCAL : GGPO_PLAYERTYPE_REMOTE;
					if (player.type == GGPO_PLAYERTYPE_REMOTE)
					{
						StringUtil::Strlcpy(player.u.remote.ip_address, s_peer_host.c_str(), sizeof(player.u.remote.ip_address));
						player.u.remote.port = static_cast<unsigned short>(s_peer_port);
					}
					if (ggpo_add_player(s_session, &player, &s_handles[p]) != GGPO_OK)
						return false;
					if (player.type == GGPO_PLAYERTYPE_LOCAL)
						ggpo_set_frame_delay(s_session, s_handles[p], s_delay);
				}
				// Block until the peer is synchronized (both instances wait here at the same vsync).
				Common::Timer wait;
				while (!s_running && wait.GetTimeSeconds() < 60)
				{
					ggpo_idle(s_session, 0);
					Threading::Sleep(1);
				}
				if (!s_running)
				{
					Console.Error("ZdxsvGgpo: p2p peer not synchronized in 60 s");
					return false;
				}
				s_rng.seed(s_seed);
				Console.WriteLn("ZdxsvGgpo: p2p player %d port %d peer %s:%d delay %d start=%d frames=%d seed=%u mask=%04x",
					s_p2p, s_port, s_peer_host.c_str(), s_peer_port, s_delay, s_start, s_frames, s_seed, s_mask);
				return true;
			}
			if (ggpo_start_synctest(&s_session, &cb, "zdxsv", PLAYERS, sizeof(Input), s_check) != GGPO_OK)
			{
				s_session = nullptr;
				return false;
			}
			ggpo_idle(s_session, 0);
			for (int p = 0; p < PLAYERS; p++)
			{
				GGPOPlayer player{sizeof(GGPOPlayer), GGPO_PLAYERTYPE_LOCAL, p + 1};
				if (ggpo_add_player(s_session, &player, &s_handles[p]) != GGPO_OK)
					return false;
			}
			s_rng.seed(s_seed);
			Console.WriteLn("ZdxsvGgpo: synctest start=%d frames=%d check=%d seed=%u input=%s mask=%04x control=%d iopflush=%d",
				s_start, s_frames, s_check, s_seed, s_host_input ? "host" : s_no_input ? "none" : "random", s_mask, s_control_input, s_iop_flush);
			return true;
		}
	} // namespace

	void OnVsync()
	{
		if (!g_active)
		{
			if (!g_enabled || s_started)
				return;
			if (s_vsyncs++ == 0)
				Parse();
			if (s_vsyncs < s_start)
				return;
			s_started = true;
			g_active = true;
			std::fill_n(g_zdxsv_rec_counts, 6, 0);
		}
		s_frame_ended = true;
		if (!g_in_rollback)
		{
			s_t_vsync = Common::Timer::GetCurrentValue();
			if (s_t_returned)
				s_emu_ms.Add(Common::Timer::ConvertValueToMilliseconds(s_t_vsync - s_t_returned));
		}
		Cpu->ExitExecution();
	}

	static void Returned();

	void OnExecuteReturned()
	{
		if (!g_active || !std::exchange(s_frame_ended, false))
			return;
		const Common::Timer::Value t0 = Common::Timer::GetCurrentValue();
		if (s_t_vsync)
			s_exit_ms.Add(Common::Timer::ConvertValueToMilliseconds(t0 - s_t_vsync));
		Returned();
		s_t_returned = Common::Timer::GetCurrentValue();
		s_ours_ms.Add(Common::Timer::ConvertValueToMilliseconds(s_t_returned - t0));
	}

	static void Returned()
	{
		// probe=: no session, frame cost of 1 exit per vsync, 2 + delta save, 3 + hashes as in SaveGameState.
		if (s_probe)
		{
			if (s_probe == 2)
			{
				Common::Timer timer;
				ZdxsvDeltaState::Save(s_session_frames);
				s_save_ms.Add(timer.GetTimeMilliseconds());
				ZdxsvDeltaState::DiscardBefore(s_session_frames - 12);
			}
			else if (s_probe >= 3)
			{
				unsigned char* buffer;
				int len, checksum;
				SaveGameState(&buffer, &len, &checksum, s_session_frames);
				FreeBuffer(buffer);
			}
			if (++s_session_frames >= s_frames)
				Stop("probe done");
			return;
		}
		if (!s_session)
		{
			if (!Start())
			{
				Stop("start failed");
				return;
			}
			// The synctest saves frame 0 at the first synchronize_input.
			if (!NextInputs())
				Stop("failed");
			return;
		}
		if (ggpo_advance_frame(s_session) != GGPO_OK || ggpo_idle(s_session, 0) != GGPO_OK)
		{
			Stop("failed");
			return;
		}
		s_session_frames++;
		if (s_disconnected)
		{
			Stop("disconnected");
			return;
		}
		// p2p: ahead of the peer, give it time to catch up (GGPO's suggested frames, at 60 fps).
		if (const int ahead = std::exchange(s_frames_ahead, 0); ahead > 0)
		{
			Common::Timer wait;
			while (wait.GetTimeMilliseconds() < ahead * 1000.0 / 60.0)
			{
				ggpo_idle(s_session, 0);
				Threading::Sleep(1);
			}
			s_wait_ms.Add(wait.GetTimeMilliseconds());
		}
		if (s_session_frames >= s_frames)
		{
			Stop("done");
			return;
		}
		if (s_session_frames % 600 == 0)
			Report("progress");
		if (!NextInputs())
			Stop("failed");
	}

	bool CaptureHostInput(u32 controller, u32 bind, float value)
	{
		if (!g_active)
			return false;
		if (controller == 0 && bind < s_host.size())
			s_host[bind] = value;
		return controller < PLAYERS;
	}
} // namespace ZdxsvGgpo
