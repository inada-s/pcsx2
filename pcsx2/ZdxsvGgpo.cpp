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
#include "Counters.h"
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

#include <climits>

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include "xxhash.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <limits>

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

		// net=1 (ai-automation#31 step 4): the Z battle msgs travel in GGPO inputs instead of DEV9.
		// Armed by the game's first key msg send on the battle sock: from then on that sock's send /
		// recv / poll RPCs are answered on the EE side (OnNetCall) and never reach the IOP.
		// Input of frame f = the pad + whole msgs the game sent before f (the rest waits for the next
		// frame); a player's msgs go to the game's recv in the first frame its seq changed.
		// The game's sends are a stream: a rollback rerun re-sends its prefix (compared: senddiff),
		// only msgs past the committed end are new. GGPO player = battle position + 1.
		struct NetInput
		{
			Input pad; // that player's pad 0
			u8 seq; // +1 per input with new msgs; unchanged input = nothing new (GGPO predicts this)
			u8 len;
			u8 data[22];
		};
		static_assert(sizeof(NetInput) == 32);
		const bool s_net_env = [] {
			const char* e = std::getenv("ZDXSV_GGPO");
			return e && std::strstr(e, "net=1");
		}();
		bool s_net = false; // net=1 parsed
		int s_players = 4; // players= (net)
		int s_relay = 0; // relay=R (net): remote p is at port R + 8 * me + p (zdxsv/udprelay.py per pair), not port + p
		// zd=1 (net, @inada-s: game-side input delay 0, all delay from GGPO): the own pad goes to the
		// game undelayed; NetInput.pad carries the game's own last record (A, B) instead, and the
		// lockstep step reads every position's (A, B) of the running GGPO frame (OnStepCopy), not
		// its ring entry that the msgs filled 3 counters ahead.
		const bool s_zd_env = [] {
			const char* e = std::getenv("ZDXSV_GGPO");
			return e && s_net_env && std::strstr(e, "zd=1");
		}();
		u16 s_zd_a = 0, s_zd_b = 0; // own last record
		Input s_zd_pad[128] = {}; // own host pad per frame & 127 (reruns reapply it)
		u16 s_zd_ab[GGPO_MAX_PLAYERS][2] = {}; // synced (A, B) of frame s_net_frame
		u16 s_zd_hist[128][GGPO_MAX_PLAYERS][2] = {}; // synced (A, B) per frame & 127
		// GGPO frame where sender p's key slot c entered the synced input stream. Peers step slot c at
		// different frames (own msg is local, s616 run1), so the step applies (A, B) of a frame all
		// peers agree on: K(c) = 2nd latest entry frame, the earliest frame any peer can step c.
		int s_zd_fin[64][GGPO_MAX_PLAYERS];
		u32 s_zd_steps = 0, s_zd_changed = 0, s_zd_nok = 0;
		bool s_net_armed = false, s_net_over = false;
		int s_net_me = -1; // local battle position
		std::vector<std::vector<u8>> s_net_sent; // every msg the game sent since armed, in order
		size_t s_net_pos = 0; // msgs sent so far in the current timeline
		std::deque<std::vector<u8>> s_net_out; // committed, not yet in a local input
		NetInput s_net_local = {};
		u8 s_net_seq_at[64][GGPO_MAX_PLAYERS] = {}; // per frame & 63: each player's synced seq
		std::vector<u8> s_net_rx; // remote msgs not yet given to the game's recv
		int s_net_frame = 0; // GGPO frame being run (last save or load)
		struct NetFrame
		{
			size_t pos;
			std::vector<u8> rx;
		};
		NetFrame s_net_at[128]; // per frame & 127, at its save
		int s_net_end = -1; // frames since the end msg (kind f) was sent or received, -1 = not yet
		struct NetStats
		{
			u32 sends, msgs, recvs, rxmsgs, rxbytes, polls, other, nowait, senddiff, toolong, maxq;
		} s_ns = {};
		bool NetStart(GGPOSessionCallbacks& cb);
		bool NetNextInputs();
		bool NetSyncAndApply();
		void NetSaved(int frame);
		void NetLoaded(int frame);
		void NetReport();

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
		GGPOPlayerHandle s_handles[GGPO_MAX_PLAYERS] = {};
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
				else if (key == "net")
					s_net = (n != 0);
				else if (key == "players")
					s_players = std::clamp(n, 2, GGPO_MAX_PLAYERS);
				else if (key == "relay")
					s_relay = n;
				else
					Console.Warning("ZdxsvGgpo: unknown key '%.*s'", static_cast<int>(key.size()), key.data());
			}
			if (s_net)
			{
				s_sync = false; // the peers run different games (own position): no common hash
				s_frames = std::numeric_limits<int>::max(); // ends with the battle (end msg)
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

		void ApplyPad(int p, const Input& in)
		{
			PadBase* pad = Pad::GetPad(static_cast<u8>(p));
			if (!pad)
				return;
			for (u32 i = 0; i < BUTTONS; i++)
			{
				const bool held = (in.buttons >> i) & 1;
				pad->SetRawPressureButton(i, std::make_tuple(held, static_cast<u8>(held ? 255 : 0)));
			}
			pad->SetRawAnalogs({in.lx, in.ly}, {in.rx, in.ry});
		}

		void ApplyInputs(const Input* inputs)
		{
			for (int p = 0; p < PLAYERS; p++)
				ApplyPad(p, inputs[p]);
		}

		bool SyncAndApply(bool rerun)
		{
			if (s_net)
				return NetSyncAndApply();
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
			if (s_net)
				return NetNextInputs();
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
			if (s_net)
				NetSaved(frame);
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
			if (s_net)
				NetLoaded(*reinterpret_cast<int*>(buffer));
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
			if (s_net)
			{
				s_net_over = true; // the battle sock goes back to the IOP
				NetReport();
			}
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
			if (s_net)
				return NetStart(cb);
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

	void TracePad();
	void TraceInputs();

	void OnVsync()
	{
		TracePad();
		TraceInputs();
		if (!g_enabled)
			return;
		if (!g_active)
		{
			if (!g_enabled || s_started)
				return;
			if (s_vsyncs++ == 0)
				Parse();
			if (s_net ? !s_net_armed : s_vsyncs < s_start)
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
		// net: the battle ended (end msg sent or received); 300 frames for the peers to get it too.
		if (s_net && s_net_end >= 0 && ++s_net_end > 300)
		{
			Stop("net battle end");
			return;
		}
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

	namespace
	{
		// Z net RPC request header (EE RAM): sock s16, len s16, then data. Result length s16 at 0xc22c98.
		constexpr u32 NET_REQ_SOCK = 0xc22c9c;
		constexpr u32 NET_REQ_LEN = 0xc22c9e;
		constexpr u32 NET_REQ_DATA = 0xc22ca0;
		constexpr u32 NET_FNO_SEND = 0x10;
		constexpr u32 NET_BATTLE_SOCK = 0;
		u32 s_game_gp = 0; // game gp, latched in OnNetRpc
		std::FILE* s_net_trace = [] {
			const char* p = std::getenv("ZDXSV_NET_TRACE");
			return p ? std::fopen(p, "w") : nullptr;
		}();
	} // namespace

	bool g_net_hook = s_net_trace != nullptr || s_net_env;
	bool g_zd_hook = s_zd_env;

	// K(c) (s_zd_fin) or -1 if unusable. An entry > 32 frames older than the newest is the previous
	// use of the slot (64 counters ago) = not yet in the stream (own msg, sent < delay frames ago).
	static int ZdKeyFrame(int c)
	{
		int newest = INT_MIN;
		for (int p = 0; p < s_players; p++)
			newest = std::max(newest, s_zd_fin[c][p]);
		int first = INT_MIN, second = INT_MIN;
		for (int p = 0; p < s_players; p++)
		{
			const int v = s_zd_fin[c][p] < newest - 32 ? INT_MAX : s_zd_fin[c][p];
			if (v > first)
				second = first, first = v;
			else if (v > second)
				second = v;
		}
		if (second == INT_MAX || second > s_net_frame || second < s_net_frame - 100)
			return -1;
		return second;
	}

	// zd: rec hook at the lockstep step's ring read (0x312bf4, per active position: s0 = position,
	// a1 = ring entry: +2 A, +6 B as u16; bit 0 of B is game-wide state, kept). Trace `Z frame p slot A B`.
	void OnStepCopy()
	{
		if (!g_active || !s_net_armed)
			return;
		const int p = static_cast<s8>(cpuRegs.GPR.n.s0.UL[0]);
		const u32 e = cpuRegs.GPR.n.a1.UL[0] & 0x1ffffff;
		if (p < 0 || p >= s_players || e + 8 > Ps2MemSize::MainRam)
			return;
		u8* ram = eeMem->Main;
		u16 a, b;
		std::memcpy(&a, ram + e + 2, 2);
		std::memcpy(&b, ram + e + 6, 2);
		const int c = ram[e] & 63;
		const int k = ZdKeyFrame(c);
		const u16* ab = k >= 0 ? s_zd_hist[k & 127][p] : s_zd_ab[p];
		const u16 na = ab[0];
		const u16 nb = static_cast<u16>((ab[1] & ~1u) | (b & 1u));
		s_zd_steps++;
		if (k < 0)
			s_zd_nok++;
		if (a != na || b != nb)
			s_zd_changed++;
		std::memcpy(ram + e + 2, &na, 2);
		std::memcpy(ram + e + 6, &nb, 2);
		if (s_net_trace)
			std::fprintf(s_net_trace, "%u Z%s %d %d %02x %04x %04x %04x %04x %02x %d\n", g_FrameCount, g_in_rollback ? "r" : "",
				s_net_frame, p, ((e - 0xc61f40) / 8) & 63, na, nb, a, b, c, k);
	}

	// One key slot per frame: counter c (6 bits), game-wide k and X (X only in records), and for an
	// input record its A/B words. Key msg (kind 2) = 2 slots: `(0x80|c, k)` or `00 c A0 A1 X k B0 B1`.
	struct KeySlot
	{
		u8 c, x, k;
		bool rec;
		u16 a, b;
	};

	static bool ParseKeySlots(const u8* m, u32 n, std::vector<KeySlot>& out)
	{
		for (u32 i = 2; i < n;)
		{
			if (m[i] & 0x80)
			{
				if (i + 2 > n)
					return false;
				out.push_back({static_cast<u8>(m[i] & 0x3f), 0, m[i + 1], false, 0, 0});
				i += 2;
			}
			else
			{
				if (i + 8 > n)
					return false;
				out.push_back({static_cast<u8>(m[i + 1] & 0x3f), m[i + 4], m[i + 5], true,
					static_cast<u16>(m[i + 2] << 8 | m[i + 3]), static_cast<u16>(m[i + 6] << 8 | m[i + 7])});
				i += 8;
			}
		}
		return true;
	}

	// zd: the game's own last record A/B (= its input, lag 0 from the pad edge).
	static void NoteOwnRecords(const u8* d, s32 len)
	{
		for (s32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
		{
			std::vector<KeySlot> slots;
			if ((d[i + 1] >> 4) == 2 && ParseKeySlots(d + i, d[i], slots))
				for (const KeySlot& s : slots)
					if (s.rec)
						s_zd_a = s.a, s_zd_b = s.b;
		}
	}

	void NoteOwnSend(const KeySlot& s);

	// NET_TRACE: per frame, the pad in EE RAM when it changed: `P` raw SIO buffer (8 B, buttons
	// active-low at +2) and the game's copy byte.
	void TracePad()
	{
		constexpr u32 PAD_RAW = 0x6f2460;
		constexpr u32 PAD_GAME = 0x117f4d9;
		// `A vsync frame A0..A3`: pad module A cur per position (0x6f2500 + 16p, the applied input, s614)
		if (s_net_trace && !g_in_rollback)
		{
			static u16 last_a[4];
			u16 a[4];
			for (int p = 0; p < 4; p++)
				std::memcpy(&a[p], eeMem->Main + 0x6f2500 + 16 * p, 2);
			if (std::memcmp(a, last_a, sizeof(a)) != 0)
			{
				std::memcpy(last_a, a, sizeof(a));
				std::fprintf(s_net_trace, "%u A %d %04x %04x %04x %04x\n", g_FrameCount, s_net_frame, a[0], a[1], a[2], a[3]);
			}
		}
		static u8 last[9];
		u8 cur[9];
		std::memcpy(cur, eeMem->Main + PAD_RAW, 8);
		cur[8] = eeMem->Main[PAD_GAME];
		if (!s_net_trace || g_in_rollback || std::memcmp(cur, last, 9) == 0)
			return;
		std::memcpy(last, cur, 9);
		std::fprintf(s_net_trace, "%u P %02x%02x%02x%02x%02x%02x%02x%02x %02x\n", g_FrameCount,
			cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7], cur[8]);
	}

	// NET_TRACE `I`: the game's per-position input array (16 B entries at *(gp-0x5b28) + 0x4d8,
	// reader 0x2ba63c, s614), bytes 0-7 of each + the base, when changed.
	void TraceInputs()
	{
		// ZDXSV_RAM_DUMP=dir,start,step,count: EE RAM (32 MB) to dir/<frame>.bin.
		static const auto dump = [] {
			std::tuple<std::string, u32, u32, u32> d{"", 0, 1, 0};
			if (const char* e = std::getenv("ZDXSV_RAM_DUMP"))
			{
				char dir[512];
				u32 a, b, c;
				if (std::sscanf(e, "%511[^,],%u,%u,%u", dir, &a, &b, &c) == 4)
					d = {dir, a, b ? b : 1, c};
			}
			return d;
		}();
		const auto& [ddir, dstart, dstep, dcount] = dump;
		if (!g_in_rollback && dcount && g_FrameCount >= dstart && g_FrameCount < dstart + dstep * dcount &&
			(g_FrameCount - dstart) % dstep == 0)
		{
			if (std::FILE* fp = std::fopen(fmt::format("{}/{}.bin", ddir, g_FrameCount).c_str(), "wb"))
			{
				std::fwrite(eeMem->Main, 1, Ps2MemSize::MainRam, fp);
				std::fclose(fp);
			}
		}
		if (!s_net_trace || g_in_rollback || !s_game_gp)
			return;
		const u32 base = *reinterpret_cast<const u32*>(eeMem->Main + ((s_game_gp - 0x5b28) & 0x1fffffc)) & 0x1ffffff;
		if (base == 0 || base + 0x4d8 + 64 > Ps2MemSize::MainRam)
			return;
		static u8 last[36];
		u8 cur[36];
		for (int p = 0; p < 4; p++)
			std::memcpy(cur + 8 * p, eeMem->Main + base + 0x4d8 + 16 * p, 8);
		std::memcpy(cur + 32, &base, 4);
		if (std::memcmp(cur, last, 36) == 0)
			return;
		std::memcpy(last, cur, 36);
		std::string hex;
		for (int i = 0; i < 32; i++)
			hex += (i % 8 == 0 ? " " : "") + fmt::format("{:02x}", cur[i]);
		std::fprintf(s_net_trace, "%u I%s %x\n", g_FrameCount, hex.c_str(), base);
	}

	void OnNetRpc()
	{
		s_game_gp = cpuRegs.GPR.n.gp.UL[0];
		const u32 fno = cpuRegs.GPR.n.a0.UL[0];
		const u8* ram = eeMem->Main;
		const s16 sock = *reinterpret_cast<const s16*>(ram + NET_REQ_SOCK);
		const s16 len = *reinterpret_cast<const s16*>(ram + NET_REQ_LEN);
		if (fno != NET_FNO_SEND || sock != NET_BATTLE_SOCK || len <= 0 || len > 0x3ca)
			return;
		const u8* d = ram + NET_REQ_DATA;
		NoteOwnRecords(d, len);
		if (!s_net_trace)
			return;
		std::string hex;
		for (int i = 0; i < len; i++)
			hex += fmt::format("{:02x}", d[i]);
		std::fprintf(s_net_trace, "%u S %s", g_FrameCount, hex.c_str());
		// McsMessage framing: byte 0 = length, byte 1 = kind << 4 | sender.
		for (int i = 0; i + 1 < len && d[i] >= 2; i += d[i])
		{
			if ((d[i + 1] >> 4) != 2 || i + d[i] > len)
				continue;
			std::vector<KeySlot> slots;
			if (!ParseKeySlots(d + i, d[i], slots))
			{
				std::fputs(" bad", s_net_trace);
				continue;
			}
			for (const KeySlot& s : slots)
				NoteOwnSend(s), std::fprintf(s_net_trace, s.rec ? " %02x:%02x:%02x:%04x/%04x" : " %02x:%02x", s.c, s.k, s.x, s.a, s.b);
		}
		std::fputc('\n', s_net_trace);
		std::fflush(s_net_trace);
	}

	namespace
	{
		constexpr u32 NET_RES_LEN = 0xc22c98;
		// Own key table (from the game's sends): k per counter, frame it was sent, last record X.
		u8 s_own_k[64];
		u32 s_own_frame[64];
		int s_own_x = -1;
		struct RecvStats
		{
			u32 msgs, keymsgs, slots, recs, unknown, kbad, xbad, rebuilt_bad, held, forced, maxhold, dropped;
		} s_rs;

		// ZDXSV_NET_SYNTH=1: the game gets rebuilt key msgs instead of the received ones: input
		// fields (record flag, A, X, B) from the remote msg, k from the local game's own send of that
		// counter; a msg is held until the local game sent all its counters (zero-latency lockstep).
		// Value = frames a msg may be held before it goes out with its received k (0 = never).
		const char* s_synth_env = std::getenv("ZDXSV_NET_SYNTH");
		const bool s_synth = s_synth_env != nullptr;
		const u32 s_synth_force = s_synth ? static_cast<u32>(std::atoi(s_synth_env)) : 0;
		struct HeldMsg
		{
			u32 frame, sender;
			std::vector<KeySlot> slots;
		};
		std::deque<HeldMsg> s_held;

		// Key msg bytes from its slots (inverse of ParseKeySlots), sender p.
		std::vector<u8> BuildKeyMsg(u32 p, const std::vector<KeySlot>& slots)
		{
			std::vector<u8> m{0, static_cast<u8>(0x20 | p)};
			for (const KeySlot& s : slots)
			{
				if (s.rec)
					m.insert(m.end(), {0, s.c, static_cast<u8>(s.a >> 8), static_cast<u8>(s.a), s.x, s.k,
										  static_cast<u8>(s.b >> 8), static_cast<u8>(s.b)});
				else
					m.insert(m.end(), {static_cast<u8>(0x80 | s.c), s.k});
			}
			m[0] = static_cast<u8>(m.size());
			return m;
		}
	} // namespace

	void NoteOwnSend(const KeySlot& s)
	{
		s_own_k[s.c] = s.k;
		s_own_frame[s.c] = g_FrameCount;
		if (s.rec)
			s_own_x = s.x;
	}

	static bool OwnKnown(u8 c)
	{
		return s_own_frame[c] != 0 && g_FrameCount - s_own_frame[c] < 32;
	}

	// Replaces the recv result (d, len) by: non-key msgs as received, then every held key msg whose
	// counters the local game has sent (per sender in order), rebuilt with the local k. A msg held
	// > ZDXSV_NET_SYNTH frames (if nonzero) goes out with its received k (forced).
	static void SynthRecv(u8* d, s32 len)
	{
		std::vector<u8> out;
		for (s32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
		{
			std::vector<KeySlot> slots;
			const u32 sender = d[i + 1] & 0xfu;
			if ((d[i + 1] >> 4) == 2 && ParseKeySlots(d + i, d[i], slots))
			{
				s_held.push_back({g_FrameCount, sender, std::move(slots)}), s_rs.held++;
				continue;
			}
			// kind 3 (`04 3P 00 00`) = the sender's counter restarts at 0 (s610 run3): its held key
			// msgs are for counters this game never reaches; holding them blocked its new ones.
			if ((d[i + 1] >> 4) == 3)
			{
				const size_t before = s_held.size();
				s_held.erase(std::remove_if(s_held.begin(), s_held.end(), [sender](const HeldMsg& h) { return h.sender == sender; }), s_held.end());
				s_rs.dropped += static_cast<u32>(before - s_held.size());
			}
			out.insert(out.end(), d + i, d + i + d[i]);
		}
		const u32 max = std::min<u32>(cpuRegs.GPR.n.s2.UL[0], 0x3ca);
		bool blocked[16] = {};
		for (auto it = s_held.begin(); it != s_held.end();)
		{
			bool ready = !blocked[it->sender];
			for (const KeySlot& s : it->slots)
				ready = ready && OwnKnown(s.c);
			const u32 age = g_FrameCount - it->frame;
			const bool force = !blocked[it->sender] && !ready && s_synth_force && age > s_synth_force;
			if (!(ready || force))
			{
				blocked[it->sender] = true;
				++it;
				continue;
			}
			std::vector<KeySlot> slots = it->slots;
			if (ready)
				for (KeySlot& s : slots)
					s.k = s_own_k[s.c];
			const std::vector<u8> m = BuildKeyMsg(it->sender, slots);
			if (out.size() + m.size() > max)
				break;
			out.insert(out.end(), m.begin(), m.end());
			s_rs.forced += force;
			s_rs.maxhold = std::max(s_rs.maxhold, age);
			it = s_held.erase(it);
		}
		if (len == 0 && out.empty())
			return;
		std::memcpy(d, out.data(), out.size());
		*reinterpret_cast<s32*>(eeMem->Main + NET_RES_LEN) = static_cast<s32>(out.size());
		std::string hex;
		for (u8 b : out)
			hex += fmt::format("{:02x}", b);
		std::fprintf(s_net_trace, "%u Y %s\n", g_FrameCount, hex.c_str());
	}

	// EE rec hook at NET_RECV_RET_PC (fno 0x14 recv, after the wait RPC returned): s4 = sock,
	// result length at 0xc22c98, data at 0xc22ca0. Logs R lines and checks that every remote key
	// msg = BuildKeyMsg(its input fields + the local game's own k for that counter).
	void OnNetRecv()
	{
		u8* ram = eeMem->Main;
		const s32 sock = cpuRegs.GPR.n.s4.SL[0];
		const s32 len = *reinterpret_cast<const s32*>(ram + NET_RES_LEN);
		if (sock != NET_BATTLE_SOCK || cpuRegs.GPR.n.v0.SL[0] < 0 || len < 0 || len > 0x3ca || !s_net_trace)
			return;
		u8* d = ram + NET_REQ_DATA;
		if (len == 0)
		{
			// nothing received: still release held msgs the local game has caught up with
			if (s_synth && !s_held.empty())
				SynthRecv(d, 0);
			return;
		}
		std::string hex;
		for (int i = 0; i < len; i++)
			hex += fmt::format("{:02x}", d[i]);
		std::fprintf(s_net_trace, "%u R %s", g_FrameCount, hex.c_str());
		for (int i = 0; i + 1 < len && d[i] >= 2; i += d[i])
		{
			s_rs.msgs++;
			if ((d[i + 1] >> 4) != 2 || i + d[i] > len)
				continue;
			s_rs.keymsgs++;
			std::vector<KeySlot> slots;
			if (!ParseKeySlots(d + i, d[i], slots))
			{
				std::fputs(" bad", s_net_trace);
				continue;
			}
			for (KeySlot& s : slots)
			{
				s_rs.slots++;
				s_rs.recs += s.rec;
				// local k for c must be from a send within the last 32 frames (the counter wraps at 64)
				const bool known = s_own_frame[s.c] != 0 && g_FrameCount - s_own_frame[s.c] < 32;
				char flag = ' ';
				if (!known)
					s_rs.unknown++, flag = '?';
				else if (s_own_k[s.c] != s.k)
					s_rs.kbad++, flag = '!';
				if (s.rec && s.x != s_own_x)
					s_rs.xbad++, flag = flag == ' ' ? 'x' : flag;
				if (s.rec)
					std::fprintf(s_net_trace, " %02x:%02x:%02x:%04x/%04x%c", s.c, s.k, s.x, s.a, s.b, flag);
				else
					std::fprintf(s_net_trace, " %02x:%02x%c", s.c, s.k, flag);
				if (known)
					s.k = s_own_k[s.c];
				if (s.rec && s_own_x >= 0)
					s.x = static_cast<u8>(s_own_x);
			}
			const std::vector<u8> m = BuildKeyMsg(d[i + 1] & 0xf, slots);
			if (m.size() != d[i] || std::memcmp(m.data(), d + i, m.size()) != 0)
				s_rs.rebuilt_bad++, std::fputs(" REBUILT_DIFF", s_net_trace);
		}
		std::fputc('\n', s_net_trace);
		if (s_synth)
			SynthRecv(d, len);
		if (s_rs.keymsgs % 2000 == 1)
			std::fprintf(s_net_trace, "STATS msgs=%u keymsgs=%u slots=%u recs=%u unknown=%u kbad=%u xbad=%u rebuilt_bad=%u held=%u forced=%u maxhold=%u dropped=%u\n",
				s_rs.msgs, s_rs.keymsgs, s_rs.slots, s_rs.recs, s_rs.unknown, s_rs.kbad, s_rs.xbad, s_rs.rebuilt_bad,
				s_rs.held, s_rs.forced, s_rs.maxhold, s_rs.dropped);
		std::fflush(s_net_trace);
	}

	namespace
	{
		constexpr u32 NET_FNO_POLL = 0xf;
		constexpr u32 NET_FNO_RECV = 0x14;
		constexpr u32 NET_NOWAIT = 0xc22c10; // s16: the wrapper takes the nowait RPC path if nonzero
		constexpr u32 NET_RX_MAX = 0x384; // the battle recv's max

		void NetSend(std::vector<u8> m)
		{
			s_ns.msgs++;
			if (s_net_pos < s_net_sent.size())
			{
				if (s_net_sent[s_net_pos] != m && s_ns.senddiff++ < 40)
				{
					std::string a, b;
					char h[4];
					for (u8 v : s_net_sent[s_net_pos])
						std::snprintf(h, sizeof(h), "%02x", v), a += h;
					for (u8 v : m)
						std::snprintf(h, sizeof(h), "%02x", v), b += h;
					Console.Warning("ZdxsvGgpo: net rerun send %zu differs (frame %d) sent %s rerun %s", s_net_pos, s_net_frame, a.c_str(), b.c_str());
				}
			}
			else
			{
				if (m.size() >= 2 && (m[1] >> 4) == 0xf && s_net_end < 0)
					s_net_end = 0;
				s_net_sent.push_back(m);
				s_net_out.push_back(std::move(m));
				s_ns.maxq = std::max<u32>(s_ns.maxq, static_cast<u32>(s_net_out.size()));
			}
			s_net_pos++;
		}

		// McsMessage framing: byte 0 = length (>= 2), byte 1 = kind << 4 | sender.
		bool HasKeyMsg(const u8* d, s32 len)
		{
			for (s32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
				if ((d[i + 1] >> 4) == 2)
					return true;
			return false;
		}

		void NetSaved(int frame)
		{
			s_net_frame = frame;
			NetFrame& at = s_net_at[frame & 127];
			at.pos = s_net_pos;
			at.rx = s_net_rx;
		}

		void NetLoaded(int frame)
		{
			s_net_frame = frame;
			const NetFrame& at = s_net_at[frame & 127];
			s_net_pos = at.pos;
			s_net_rx = at.rx;
		}

		bool NetStart(GGPOSessionCallbacks& cb)
		{
			std::fill(&s_zd_fin[0][0], &s_zd_fin[0][0] + 64 * GGPO_MAX_PLAYERS, -100000);
			if (s_net_me < 0 || s_net_me >= s_players)
			{
				Console.Error("ZdxsvGgpo: net position %d not below players=%d", s_net_me, s_players);
				return false;
			}
			if (ggpo_start_session(&s_session, &cb, "zdxsv", s_players, sizeof(NetInput), static_cast<unsigned short>(s_port + s_net_me), nullptr, 0) != GGPO_OK)
			{
				s_session = nullptr;
				return false;
			}
			ggpo_set_disconnect_timeout(s_session, 5000);
			ggpo_set_disconnect_notify_start(s_session, 1000);
			for (int p = 0; p < s_players; p++)
			{
				GGPOPlayer player{};
				player.size = sizeof(GGPOPlayer);
				player.player_num = p + 1;
				player.type = (p == s_net_me) ? GGPO_PLAYERTYPE_LOCAL : GGPO_PLAYERTYPE_REMOTE;
				if (player.type == GGPO_PLAYERTYPE_REMOTE)
				{
					StringUtil::Strlcpy(player.u.remote.ip_address, s_peer_host.c_str(), sizeof(player.u.remote.ip_address));
					player.u.remote.port = static_cast<unsigned short>(s_relay ? s_relay + 8 * s_net_me + p : s_port + p);
				}
				if (ggpo_add_player(s_session, &player, &s_handles[p]) != GGPO_OK)
					return false;
				if (player.type == GGPO_PLAYERTYPE_LOCAL)
					ggpo_set_frame_delay(s_session, s_handles[p], s_delay);
			}
			// Every peer arms at its own first key msg: block until all are synchronized.
			Common::Timer wait;
			while (!s_running && wait.GetTimeSeconds() < 60)
			{
				ggpo_idle(s_session, 0);
				Threading::Sleep(1);
			}
			if (!s_running)
			{
				Console.Error("ZdxsvGgpo: net peers not synchronized in 60 s");
				return false;
			}
			Console.WriteLn("ZdxsvGgpo: net player %d of %d port %d delay %d, %zu msgs sent before the start, waited %.1f s",
				s_net_me + 1, s_players, s_port + s_net_me, s_delay, s_net_sent.size(), wait.GetTimeSeconds());
			return true;
		}

		bool NetNextInputs()
		{
			NetInput in = s_net_local;
			in.pad = HostInput();
			if (s_zd_env)
			{
				s_zd_pad[s_net_frame & 127] = in.pad;
				const u16 ab[2] = {s_zd_a, s_zd_b};
				in.pad = {};
				std::memcpy(&in.pad, ab, sizeof(ab));
			}
			std::vector<u8> data;
			while (!s_net_out.empty())
			{
				const std::vector<u8>& m = s_net_out.front();
				if (m.size() > sizeof(in.data))
				{
					if (s_ns.toolong++ < 20)
						Console.Error("ZdxsvGgpo: net msg of %zu bytes dropped", m.size());
				}
				else if (data.size() + m.size() > sizeof(in.data))
					break;
				else
					data.insert(data.end(), m.begin(), m.end());
				s_net_out.pop_front();
			}
			if (!data.empty())
			{
				in.seq++;
				in.len = static_cast<u8>(data.size());
				std::memset(in.data, 0, sizeof(in.data));
				std::memcpy(in.data, data.data(), data.size());
			}
			s_net_local = in;
			GGPOErrorCode rc = ggpo_add_local_input(s_session, s_handles[s_net_me], &in, sizeof(in));
			if (rc == GGPO_ERRORCODE_PREDICTION_THRESHOLD)
			{
				s_waits++;
				Common::Timer wait;
				while (rc == GGPO_ERRORCODE_PREDICTION_THRESHOLD && !s_disconnected && wait.GetTimeSeconds() < 10)
				{
					ggpo_idle(s_session, 0);
					Threading::Sleep(1);
					rc = ggpo_add_local_input(s_session, s_handles[s_net_me], &in, sizeof(in));
				}
				s_wait_ms.Add(wait.GetTimeMilliseconds());
			}
			if (rc != GGPO_OK)
			{
				Console.Error("ZdxsvGgpo: net add_local_input %d", rc);
				return false;
			}
			return NetSyncAndApply();
		}

		// Inputs of frame s_net_frame: own pad to pad 0, remote msgs with a new seq to the recv queue.
		bool NetSyncAndApply()
		{
			NetInput in[GGPO_MAX_PLAYERS] = {};
			int disconnect_flags = 0;
			const GGPOErrorCode rc = ggpo_synchronize_input(s_session, in, sizeof(NetInput) * s_players, &disconnect_flags);
			if (rc != GGPO_OK)
			{
				Console.Error("ZdxsvGgpo: net synchronize_input %d", rc);
				return false;
			}
			const int f = s_net_frame;
			ApplyPad(0, s_zd_env ? s_zd_pad[f & 127] : in[s_net_me].pad);
			if (s_zd_env)
				for (int p = 0; p < s_players; p++)
				{
					std::memcpy(s_zd_ab[p], &in[p].pad, sizeof(s_zd_ab[p]));
					std::memcpy(s_zd_hist[f & 127][p], &in[p].pad, sizeof(s_zd_ab[p]));
				}
			for (int p = 0; p < s_players; p++)
			{
				const bool fresh = in[p].seq != s_net_seq_at[(f - 1) & 63][p];
				const u8* d = in[p].data;
				const u32 len = std::min<u32>(in[p].len, sizeof(in[p].data));
				// a predicted input repeats its seq, so entries come only from real inputs: no rollback state
				if (s_zd_env && fresh)
					for (u32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
					{
						std::vector<KeySlot> slots;
						if ((d[i + 1] >> 4) == 2 && ParseKeySlots(d + i, d[i], slots))
							for (const KeySlot& s : slots)
								s_zd_fin[s.c][p] = f;
					}
				if (p != s_net_me && fresh)
				{
					for (u32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
						if ((d[i + 1] >> 4) == 0xf && s_net_end < 0)
							s_net_end = 0;
					s_net_rx.insert(s_net_rx.end(), d, d + len);
				}
				s_net_seq_at[f & 63][p] = in[p].seq;
			}
			return true;
		}

		void NetReport()
		{
			Console.WriteLn("ZdxsvGgpo: net sends %u msgs %u (sent %zu, unsent %zu) recvs %u rxmsgs %u rxbytes %u polls %u other %u nowait %u senddiff %u toolong %u maxq %u waits %d",
				s_ns.sends, s_ns.msgs, s_net_sent.size(), s_net_out.size(), s_ns.recvs, s_ns.rxmsgs, s_ns.rxbytes, s_ns.polls,
				s_ns.other, s_ns.nowait, s_ns.senddiff, s_ns.toolong, s_ns.maxq, s_waits);
			if (s_zd_env)
				Console.WriteLn("ZdxsvGgpo: zd steps %u changed %u nok %u", s_zd_steps, s_zd_changed, s_zd_nok);
		}
	} // namespace

	// EE rec hook at NET_RPC_PC (the net RPC wrapper's entry): trace, and in net mode answer the
	// battle sock's RPCs here. Returns true when answered (v0 = result, pc = ra).
	bool OnNetCall()
	{
		OnNetRpc();
		if (!s_net_env || s_net_over)
			return false;
		const u32 fno = cpuRegs.GPR.n.a0.UL[0];
		u8* ram = eeMem->Main;
		const s16 sock = *reinterpret_cast<const s16*>(ram + NET_REQ_SOCK);
		const s16 len = *reinterpret_cast<const s16*>(ram + NET_REQ_LEN);
		u8* d = ram + NET_REQ_DATA;
		if (sock != NET_BATTLE_SOCK)
			return false;
		if (!s_net_armed)
		{
			// sock 0 is the lobby TCP too: arm at the battle's first key msg
			if (fno != NET_FNO_SEND || len <= 0 || len > 0x3ca || !HasKeyMsg(d, len))
				return false;
			s_net_armed = true;
			for (s32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
				if ((d[i + 1] >> 4) == 2)
					s_net_me = d[i + 1] & 0xf;
			Console.WriteLn("ZdxsvGgpo: net armed at vsync %u, position %d", g_FrameCount, s_net_me);
		}
		if (*reinterpret_cast<const s16*>(ram + NET_NOWAIT) != 0)
			s_ns.nowait++;
		s32 result = 0;
		if (fno == NET_FNO_SEND)
		{
			s_ns.sends++;
			const s32 n = std::clamp<s32>(len, 0, 0x3ca);
			s32 i = 0;
			for (; i + 1 < n && d[i] >= 2 && i + d[i] <= n; i += d[i])
				NetSend(std::vector<u8>(d + i, d + i + d[i]));
			if (i < n) // not McsMessage framed: one msg
				NetSend(std::vector<u8>(d + i, d + n));
			result = n;
		}
		else if (fno == NET_FNO_RECV)
		{
			s_ns.recvs++;
			u32 n = 0;
			while (n + 1 < s_net_rx.size() && s_net_rx[n] >= 2 && n + s_net_rx[n] <= s_net_rx.size() && n + s_net_rx[n] <= NET_RX_MAX)
				n += s_net_rx[n], s_ns.rxmsgs++;
			if (n == 0 && !s_net_rx.empty() && s_net_rx.size() <= NET_RX_MAX) // unframed rest
				n = static_cast<u32>(s_net_rx.size());
			std::memcpy(d, s_net_rx.data(), n);
			s_net_rx.erase(s_net_rx.begin(), s_net_rx.begin() + n);
			s_ns.rxbytes += n;
			result = static_cast<s32>(n);
		}
		else if (fno == NET_FNO_POLL)
		{
			// s609 battle polls: state 4, 0x2000 send space, readable bytes
			s_ns.polls++;
			*reinterpret_cast<u16*>(ram + NET_REQ_LEN) = 4;
			*reinterpret_cast<u16*>(ram + NET_REQ_DATA + 2) = 0x2000;
			*reinterpret_cast<u16*>(ram + NET_REQ_DATA + 4) = static_cast<u16>(std::min<size_t>(s_net_rx.size(), NET_RX_MAX));
		}
		else
		{
			s_ns.other++;
			return false;
		}
		*reinterpret_cast<s32*>(ram + NET_RES_LEN) = result;
		cpuRegs.GPR.n.v0.SD[0] = result;
		cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
		return true;
	}
} // namespace ZdxsvGgpo
