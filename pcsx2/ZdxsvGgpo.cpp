// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ZDXSV_GGPO="key=value,...": a GGPO session in a running game. Synctest by default:
// every frame is saved, and every `check` frames GGPO loads the frame `check` back, reruns the
// frames with the same inputs and compares the state checksums (EE RAM + delta state).
// With net=1 a battle of players= peers instead (see NetInput): the session starts when the game
// arms its battle sock, every peer runs its own position, no state hashes.
//   net=1        GGPO battle session (zdxsv/rbk.sh, m4relay.sh)
//   players=4    peers (2..4); GGPO player = battle position + 1
//   port=7001    UDP port of position 0; position p listens on port + p, peers on host= (default 127.0.0.1)
//   relay=R      remote p is at R + 8 * me + p (zdxsv/udprelay.py per pair)
//   delay=0      GGPO frame delay of the local input
//   sync=0       no state hashes: checksum 0 (net: always)
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
#include "Host.h"
#include "VMManager.h"
#include "GS/GS.h"

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

		// net=1 (ai-automation#31 step 4): the Z battle runs over GGPO, game-side input delay 0.
		// Armed by the game's first key msg send on the battle sock: from then on that sock's send /
		// recv / poll RPCs are answered on the EE side (OnNetCall) and never reach the IOP.
		// Input of frame f = (A, B) of the host pad (ZdPadAB) + the kind-3 msgs the game sent before f.
		// The own pad goes to the game undelayed; the lockstep step reads every position's synced (A, B)
		// of the running GGPO frame (OnStepCopy). Every other own battle msg goes back to the game's recv
		// once per remote position (sender nibble rewritten): k, X, B bit 0 and the kind 7/9/f msgs are
		// game-wide. Kind 3 (round handshake) is the barrier: its n-th of each remote goes to recv once
		// all peers' n-th is in the synced stream. A rerun's sends are re-echoed, never transmitted
		// (compared: senddiff). GGPO player = battle position + 1.
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
		// ZDXSV_RBK=i/N (net=1; flycast rbk_test): started from a post-entry state (zdxsv/rbkprep.sh)
		// as battle position i (0-based) of N. Until GGPO arms, every lobby / battle connect RPC is
		// answered here (RbkCall: built-in battle start, recorded connect results, own battle msgs
		// echoed per remote position) and the limiter runs turbo; the process exits at the session end.
		// ZDXSV_RBK_TIME=s: rule time limit (recorded 210). ZDXSV_RBK_COUNT=n: battles (recorded 0 =
		// rematch by input). ZDXSV_RBK_GAUGE=v: 戦力ゲージ (recorded 600). ZDXSV_RAND_INPUT=seed: pad input.
		int s_rbk_me = -1, s_rbk_n = 0;
		const bool s_rbk = [] {
			const char* e = std::getenv("ZDXSV_RBK");
			return e && std::sscanf(e, "%d/%d", &s_rbk_me, &s_rbk_n) == 2 && s_rbk_me >= 0 && s_rbk_me < s_rbk_n && s_rbk_n <= 4;
		}();
		const char* s_rand_env = std::getenv("ZDXSV_RAND_INPUT");
		bool s_net = false; // net=1 parsed
		int s_players = 4; // players= (net)
		int s_relay = 0; // relay=R (net): remote p is at port R + 8 * me + p (zdxsv/udprelay.py per pair), not port + p
		u32 s_zds_echo = 0, s_zds_skip = 0;
		// ZDXSV_PW_HASH=1: per GGPO frame (last save wins) XXH3 of each player work 0x8395d8 + 0x2200*p,
		// written as `H frame h0 h1 h2 h3` to NET_TRACE at the report: the 4 machines simulate every
		// player, so in sync these agree across peers (own-position RAM elsewhere does not, sync=0).
		const bool s_pw_hash = [] {
			const char* e = std::getenv("ZDXSV_PW_HASH");
			return e && e[0] == '1';
		}();
		constexpr u32 PW_BASE = 0x8395d8, PW_SIZE = 0x2200;
		// Left out of the hash: machine-local 1-frame scratch (s623 pwdiff, in-sync 60 ms run):
		// +0x274/+0x2b4 go 1.0 -> a different float per machine -> 0 at one frame on all machines;
		// +0x1e64..+0x1e94 (stride 0x10) set on one machine for one frame. Player work agrees after.
		// +0x214c = player struct (0x839330 + 0x2200*q) +0x1f4 of q = p + 1: HUD gauge display value, moved 1/frame
		// toward +0x1f2 by 0x14d860 for the own position only (s625 ZDXSV_EE_WATCH: writer 0x14d910).
		constexpr u32 PW_MASK[] = {0x274, 0x2b4, 0x1e64, 0x1e74, 0x1e84, 0x1e94, 0x214c};
		// Viewer-team bits (s628 rbk N=4, pwdiff --own): equal on the machines of one side, set for the
		// other side's players: +0x68 0x300 (119 frames mid-battle), and from time-up on +0x58 0x100,
		// +0x9c 0x10000, +0x2068 bit 0, +0x2004 (pointer); +0x2074 0x100 on the time-up frame (s630). No other field follows them.
		// +0x2088 byte (struct +0x130): effect flag, set each frame by 0xe0a1b4, cleared by the MS-kind handler
		// (0x2a7560 cases 4/6) of the model update 0x1e4340, run for the own player + players in view only (s631).
		// Own player only (s634 rbk N=2 pwdiff --own): u16 +0xcc set on the own machine, 0 on others;
		// u16 +0x92 follows u16 +0x90 (gauge 4000, equal on all) on the own machine, stays 4000 on others.
		constexpr std::pair<u32, u32> PW_MASK_BITS[] = {{0x58, 0x100}, {0x68, 0x300}, {0x9c, 0x10000}, {0x2004, ~0u}, {0x2068, 1}, {0x2074, 0x100}, {0x2088, 0xff},
			{0xcc, 0xffff}, {0x90, 0xffff0000}};
		std::map<int, std::array<u64, 4>> s_pw;
		// ZDXSV_PW_DUMP=file: every save appends (s32 frame, 4 * PW_SIZE bytes of player work); rollback
		// re-saves a frame, the last record wins (`zdxsv/pwdiff.py` finds the fields behind H mismatches).
		std::FILE* s_pw_dump = [] {
			const char* p = std::getenv("ZDXSV_PW_DUMP");
			return p ? std::fopen(p, "wb") : nullptr;
		}();
		// zds: kind 3 (round handshake) is the one barrier: each machine reaches it at its own frame
		// (scene/load timing, s621 run1: side 2 one frame later), so it goes through the GGPO input
		// and the n-th kind 3 of every remote goes to recv once all peers' n-th is in the synced stream.
		std::vector<std::vector<u8>> s_zds_k3[GGPO_MAX_PLAYERS]; // per sender, by index
		int s_zds_seen[GGPO_MAX_PLAYERS] = {}, s_zds_rel = 0; // rollback state (NetFrame)
		u32 s_zds_k3rel = 0;
		// ZDXSV_ZDS_PS=1: play start. The battle load step 0x2b1d60 (scene step: waits for the load-busy
		// flag via 0x214260, then inits the per-battle work and sets tick state 8) passes 0x2b1d80 when this
		// machine's load is done: local timing (s630: player work initialized 1 frame apart). The rec hook
		// there counts the wish, returns 0 (step retried next frame) until every peer's synced count in
		// Input::unused[1] reaches n, then lets the n-th pass. Rollback state (NetFrame).
		const bool s_zds_ps = std::getenv("ZDXSV_ZDS_PS") != nullptr;
		struct PS
		{
			u8 n, rel;
			bool hold, go;
		} s_ps = {};
		Input s_zd_pad[128] = {}; // own host pad per frame & 127 (reruns reapply it)
		u16 s_zd_hist[128][GGPO_MAX_PLAYERS][2] = {}; // synced (A, B) per frame & 127
		u32 s_zd_steps = 0, s_zd_changed = 0;
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
			int k3seen[GGPO_MAX_PLAYERS], k3rel;
			PS ps;
		};
		NetFrame s_net_at[128]; // per frame & 127, at its save
		int s_net_end = -1; // frames since the end msg (kind f) was sent or received, -1 = not yet
		// ZDXSV_NET_TAIL=n: frames run after the end msg before the session stops (default 300).
		const int s_net_tail = [] {
			const char* e = std::getenv("ZDXSV_NET_TAIL");
			return e ? std::atoi(e) : 300;
		}();
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
		bool s_sync = true; // sync=0: no state hashes (checksum 0)
		int s_trace_frame = 0; // trace= probe
		std::FILE* s_trace_file = nullptr;
		int s_port = 7001, s_delay = 0;
		std::string s_peer_host = "127.0.0.1";
		bool s_running = false; // net: GGPO_EVENTCODE_RUNNING seen
		bool s_disconnected = false;
		int s_frames_ahead = 0; // net: last GGPO_EVENTCODE_TIMESYNC
		int s_waits = 0; // net: frames that waited for a peer

		GGPOSession* s_session = nullptr;
		// ZDXSV_SAVE_ALL=1: delta-save every GGPO frame. Default (net, sync=0): skip the save of a frame at or
		// below GGPO's last confirmed frame (all inputs received: never a rollback target).
		const bool s_save_all = [] {
			const char* e = std::getenv("ZDXSV_SAVE_ALL");
			return e && e[0] == '1';
		}();
		int s_save_skipped = 0;
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
		Stat s_rerun_ms, s_wait_ms; // between frames: rollback rerun emulation, net wait for a peer
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
				else if (key == "port")
					s_port = n;
				else if (key == "host")
					s_peer_host = std::string(value);
				else if (key == "delay")
					s_delay = n;
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
			Console.WriteLn("ZdxsvGgpo: %s frames %d rollback frames %d loads %d mismatches %d errors %d | save ms mean %.3f max %.3f skipped %d | hash ms mean %.3f | load ms mean %.3f max %.3f",
				what, s_session_frames, s_rollback_frames, s_loads, s_mismatches, s_errors, s_save_ms.Mean(), s_save_ms.max, s_save_skipped,
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
				Input in = (p == 0 && s_host_input) ? HostInput() : s_random[p];
				const GGPOErrorCode rc = ggpo_add_local_input(s_session, s_handles[p], &in, sizeof(in));
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
					Console.WriteLn("ZdxsvGgpo: running");
					break;
				case GGPO_EVENTCODE_TIMESYNC:
					s_frames_ahead = ev->u.timesync.frames_ahead;
					break;
				case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
					s_disconnected = true;
					Console.Warning("ZdxsvGgpo: peer disconnected");
					break;
				case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
					Console.Warning("ZdxsvGgpo: connection interrupted");
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
			// probelint: per-instruction trace, flushed at the next frame's call (unbuffered is too slow)
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
			int confirmed = -1;
			if (!s_save_all && !s_sync && ggpo_get_last_confirmed_frame(s_session, &confirmed) == GGPO_OK && frame <= confirmed)
				s_save_skipped++;
			else if (!ZdxsvDeltaState::Save(frame))
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

		// sync=1 part of SaveGameState: checksum, synctest diff samples.
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
			ggpo_set_log_function(nullptr);
			g_active = false;
			ZdxsvDeltaState::Clear();
			Report(what);
			if (s_net)
			{
				s_net_over = true; // the battle sock goes back to the IOP
				NetReport();
			}
			if (s_rbk)
			{
				Console.WriteLn("ZdxsvGgpo: rbk exit (%s) at vsync %u", what, g_FrameCount);
				Host::RunOnCPUThread([] { Host::RequestVMShutdown(false, false, false); });
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
		if (s_rbk && s_net_env && !s_net_armed)
			VMManager::SetLimiterMode(LimiterModeType::Turbo);
		// Once: the rbk knobs as received; zdxsv/rbk.sh compares this line to what it meant to pass
		static bool rbk_env_logged = false;
		if (s_rbk && !rbk_env_logged)
		{
			rbk_env_logged = true;
			const auto env = [](const char* k) { const char* v = std::getenv(k); return v && *v ? v : "-"; };
			Console.WriteLn("ZdxsvGgpo: rbk env pos=%d/%d rand=%s turbo=%s ps=%s clamp=%s ggpo=%s", s_rbk_me, s_rbk_n,
				env("ZDXSV_RAND_INPUT"), env("ZDXSV_RBK_TURBO"), env("ZDXSV_ZDS_PS"), env("ZDXSV_EE_CLAMP"), env("ZDXSV_GGPO"));
		}
		TracePad();
		TraceInputs();
		// ZDXSV_SNAP=dir,n: GS screenshot dir/v<vsync>.png every n vsyncs (needs a real renderer, not -Headless)
		static const char* snap = std::getenv("ZDXSV_SNAP");
		static const int snap_n = snap && std::strchr(snap, ',') ? std::atoi(std::strchr(snap, ',') + 1) : 0;
		if (snap_n > 0 && !g_in_rollback && g_FrameCount % snap_n == 0)
			GSQueueSnapshot(fmt::format("{}\\v{}.png", std::string(snap, std::strchr(snap, ',')), g_FrameCount));
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
		// net: the battle ended (end msg sent or received); s_net_tail frames for the peers to get it too.
		if (s_net && s_net_end >= 0 && ++s_net_end > s_net_tail)
		{
			Stop("net battle end");
			return;
		}
		if (s_disconnected)
		{
			Stop("disconnected");
			return;
		}
		// net: ahead of the peer, give it time to catch up (GGPO's suggested frames, at 60 fps).
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
			std::FILE* f = p ? std::fopen(p, "w") : nullptr;
			if (f) // unbuffered: the rig kills pcsx2, buffered lines would be lost (zdxsv/probelint.py)
				std::setvbuf(f, nullptr, _IONBF, 0);
			return f;
		}();
	} // namespace

	bool g_net_hook = s_net_trace != nullptr || s_net_env;
	bool g_zd_hook = s_net_env;
	bool g_ps_hook = s_zds_ps;

	// zdp: record (A, B) of a host pad, s613 bind table (OR-linear; B bit 0 = game state, left 0).
	static void ZdPadAB(const Input& in, u16& a, u16& b)
	{
		using I = PadDualshock2::Inputs;
		static constexpr struct { I i; u16 a, b; } bind[] = {
			{I::PAD_L3, 0, 0x0002}, {I::PAD_R2, 0x0180, 0x0004}, {I::PAD_L2, 0x0280, 0x0008},
			{I::PAD_R1, 0x0300, 0x0010}, {I::PAD_CIRCLE, 0x0040, 0x0020}, {I::PAD_CROSS, 0x0080, 0x0040},
			{I::PAD_L1, 0x0020, 0x0080}, {I::PAD_TRIANGLE, 0x0100, 0x0100}, {I::PAD_SQUARE, 0x0200, 0x0200},
			{I::PAD_RIGHT, 0, 0x0400}, {I::PAD_LEFT, 0, 0x0800}, {I::PAD_DOWN, 0, 0x1000},
			{I::PAD_UP, 0, 0x2000}, {I::PAD_SELECT, 0, 0x4000}};
		a = b = 0;
		for (const auto& e : bind)
			if ((in.buttons >> e.i) & 1)
				a |= e.a, b |= e.b;
	}

	// ps: rec hook at LOAD_STEP_PC (0x2b1d80, battle load step past its load-busy check). true = held:
	// v0 = 0 (step not done, the scene retries it next frame), pc = the epilogue 0x2b1fa8. Trace `P frame n`.
	bool OnLoadStep()
	{
		if (!g_active || !s_net_armed || s_net_over)
			return false;
		if (!s_ps.hold)
		{
			s_ps.hold = true;
			s_ps.n++;
			if (s_net_trace)
				std::fprintf(s_net_trace, "%u PH%s %d %d\n", g_FrameCount, g_in_rollback ? "r" : "", s_net_frame, s_ps.n);
		}
		if (s_ps.go)
		{
			s_ps.hold = s_ps.go = false;
			return false;
		}
		cpuRegs.GPR.n.v0.UD[0] = 0;
		cpuRegs.pc = 0x2b1fa8;
		return true;
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
		const int k = s_net_frame;
		const u16* ab = s_zd_hist[k & 127][p];
		const u16 na = ab[0];
		const u16 nb = static_cast<u16>((ab[1] & ~1u) | (b & 1u));
		s_zd_steps++;
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
			// `L vsync frame lead maxlead slots state`: lockstep lead 0xc62be0 (queued - executed
			// counters), its cap 0xc62be1, slots per key msg 0xc62bdf, tick state 0xc627b4, when changed
			static u8 last_l[4];
			const u8* l = eeMem->Main + 0xc62bdf;
			const u8 cl[4] = {l[1], l[2], l[0], eeMem->Main[0xc627b4]};
			if (std::memcmp(cl, last_l, 4) != 0)
			{
				std::memcpy(last_l, cl, 4);
				std::fprintf(s_net_trace, "%u L %d %d %d %d %d\n", g_FrameCount, s_net_frame, cl[0], cl[1], cl[2], cl[3]);
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
		// ZDXSV_EE_CLAMP=addr,max[,lo,hi][;addr,...]: u16 at addr set to max whenever above it (and in lo..hi);
		// a function of state, so rollback-safe. 0x117f566 = 出撃準備 frames left (3599 at
		// entry; 1800 and 0xffff in earlier phases, s630 ramcount.py).
		static const auto clamps = [] {
			std::vector<std::tuple<u32, u32, u32, u32>> v;
			for (const char* e = std::getenv("ZDXSV_EE_CLAMP"); e && *e;)
			{
				u32 a, m, lo = 0, hi = 0xffff;
				if (std::sscanf(e, "%x,%u,%u,%u", &a, &m, &lo, &hi) >= 2 && a + 2 <= Ps2MemSize::MainRam)
					v.emplace_back(a, m, lo, hi);
				e = std::strchr(e, ';');
				e = e ? e + 1 : nullptr;
			}
			return v;
		}();
		for (const auto& [caddr, cmax, clo, chi] : clamps)
		{
			u16& v = *reinterpret_cast<u16*>(eeMem->Main + (caddr & ~1u));
			if (v > cmax && v >= clo && v <= chi)
			{
				static int logged = 0;
				if (!g_in_rollback && logged++ < 6)
					Console.WriteLn("ZdxsvGgpo: EE clamp %x %u -> %u at vsync %u", caddr, v, cmax, g_FrameCount);
				v = static_cast<u16>(cmax);
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
			u32 msgs, keymsgs, slots, recs, unknown, kbad, xbad, rebuilt_bad;
		} s_rs;

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

	// EE rec hook at NET_RECV_RET_PC (fno 0x14 recv, after the wait RPC returned): s4 = sock,
	// result length at 0xc22c98, data at 0xc22ca0. Logs R lines and checks that every remote key
	// msg = BuildKeyMsg(its input fields + the local game's own k for that counter).
	void OnNetRecv()
	{
		u8* ram = eeMem->Main;
		const s32 sock = cpuRegs.GPR.n.s4.SL[0];
		const s32 len = *reinterpret_cast<const s32*>(ram + NET_RES_LEN);
		if (sock == NET_BATTLE_SOCK && s_net_trace && (cpuRegs.GPR.n.v0.SL[0] < 0 || len < 0 || len > 0x3ca))
			std::fprintf(s_net_trace, "%u E recv v0 %d len %d\n", g_FrameCount, cpuRegs.GPR.n.v0.SL[0], len);
		if (sock != NET_BATTLE_SOCK || cpuRegs.GPR.n.v0.SL[0] < 0 || len < 0 || len > 0x3ca || !s_net_trace)
			return;
		u8* d = ram + NET_REQ_DATA;
		if (len == 0)
			return;
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
		if (s_rs.keymsgs % 2000 == 1)
			std::fprintf(s_net_trace, "STATS msgs=%u keymsgs=%u slots=%u recs=%u unknown=%u kbad=%u xbad=%u rebuilt_bad=%u\n",
				s_rs.msgs, s_rs.keymsgs, s_rs.slots, s_rs.recs, s_rs.unknown, s_rs.kbad, s_rs.xbad, s_rs.rebuilt_bad);
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
			const int kind = m.size() >= 2 && m[0] == m.size() ? m[1] >> 4 : -1;
			if (kind == 3)
				; // GGPO input (below), released by NetSyncAndApply
			else if (kind == 2 || kind == 7 || kind == 9 || kind == 0xf)
			{
				for (int q = 0; q < s_players; q++)
					if (q != s_net_me)
					{
						s_net_rx.push_back(m[0]);
						s_net_rx.push_back(static_cast<u8>((m[1] & 0xf0) | q));
						s_net_rx.insert(s_net_rx.end(), m.begin() + 2, m.end());
					}
				if (!g_in_rollback)
					s_zds_echo++;
			}
			else if (!g_in_rollback && s_zds_skip++ < 20)
				Console.Warning("ZdxsvGgpo: zds msg kind %d (%zu bytes) not echoed", kind, m.size());
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
				if (m.size() >= 2 && (m[1] >> 4) == 3)
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
			std::memcpy(at.k3seen, s_zds_seen, sizeof(at.k3seen));
			at.k3rel = s_zds_rel;
			at.ps = s_ps;
			if (s_pw_hash)
			{
				std::array<u64, 4>& h = s_pw[frame];
				for (u32 p = 0; p < 4; p++)
				{
					std::array<u8, PW_SIZE> w;
					std::memcpy(w.data(), &eeMem->Main[PW_BASE + PW_SIZE * p], PW_SIZE);
					for (u32 o : PW_MASK)
						std::memset(&w[o], 0, 4);
					for (const auto& [o, bits] : PW_MASK_BITS)
					{
						u32 v;
						std::memcpy(&v, &w[o], 4);
						v &= ~bits;
						std::memcpy(&w[o], &v, 4);
					}
					h[p] =XXH3_64bits(w.data(), PW_SIZE);
				}
			}
			if (s_pw_dump)
			{
				const s32 f = frame;
				std::fwrite(&f, sizeof(f), 1, s_pw_dump);
				std::fwrite(&eeMem->Main[PW_BASE], PW_SIZE * 4, 1, s_pw_dump);
			}
		}

		void NetLoaded(int frame)
		{
			s_net_frame = frame;
			const NetFrame& at = s_net_at[frame & 127];
			s_net_pos = at.pos;
			s_net_rx = at.rx;
			std::memcpy(s_zds_seen, at.k3seen, sizeof(s_zds_seen));
			s_zds_rel = at.k3rel;
			s_ps = at.ps;
		}

		bool NetStart(GGPOSessionCallbacks& cb)
		{
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
			// ZDXSV_NET_DISCONNECT_MS=ms (default 5000): longer lets a peer stall (ZDXSV_RAM_DUMP) without a disconnect.
			const char* dms = std::getenv("ZDXSV_NET_DISCONNECT_MS");
			ggpo_set_disconnect_timeout(s_session, dms ? std::atoi(dms) : 5000);
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

		// ZDXSV_RAND_INPUT=seed: every 5 frames new buttons (each 1/4, never START / SELECT) and left
		// stick, from a generator seeded by (seed, position).
		Input RandInput()
		{
			static std::mt19937 rng;
			static Input in = {};
			static int calls = 0;
			if (calls == 0)
			{
				std::seed_seq seq{static_cast<u32>(std::atoi(s_rand_env)), static_cast<u32>(s_net_me)};
				rng.seed(seq);
			}
			if (calls++ % 5 == 0)
			{
				using I = PadDualshock2::Inputs;
				in.buttons = static_cast<u16>(rng() & rng() & ~((1u << I::PAD_START) | (1u << I::PAD_SELECT)));
				in.lx = static_cast<u8>(rng());
				in.ly = static_cast<u8>(rng());
				in.rx = in.ry = Pad::ANALOG_NEUTRAL_POSITION;
			}
			return in;
		}

		bool NetNextInputs()
		{
			NetInput in = s_net_local;
			in.pad = s_rand_env ? RandInput() : HostInput();
			s_zd_pad[s_net_frame & 127] = in.pad;
			u16 ab[2];
			ZdPadAB(in.pad, ab[0], ab[1]);
			if (s_net_trace)
				std::fprintf(s_net_trace, "%u Q %d %04x %04x\n", g_FrameCount, s_net_frame, ab[0], ab[1]);
			in.pad = {};
			std::memcpy(&in.pad, ab, sizeof(ab));
			in.pad.unused[1] = s_ps.n;
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

		// Inputs of frame s_net_frame: own host pad to pad 0, every position's (A, B) to s_zd_hist, kind-3
		// msgs with a new seq to the barrier (the n-th of each remote to recv once all peers' n-th arrived).
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
			ApplyPad(0, s_zd_pad[f & 127]);
			for (int p = 0; p < s_players; p++)
				std::memcpy(s_zd_hist[f & 127][p], &in[p].pad, sizeof(s_zd_hist[f & 127][p]));
			// a predicted input repeats its seq, so entries come only from real inputs: no rollback state
			for (int p = 0; p < s_players; p++)
			{
				const bool fresh = in[p].seq != s_net_seq_at[(f - 1) & 63][p];
				const u8* d = in[p].data;
				const u32 len = std::min<u32>(in[p].len, sizeof(in[p].data));
				if (fresh)
					for (u32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
						if ((d[i + 1] >> 4) == 3)
						{
							std::vector<std::vector<u8>>& v = s_zds_k3[p];
							if (v.size() <= static_cast<size_t>(s_zds_seen[p]))
								v.resize(s_zds_seen[p] + 1);
							v[s_zds_seen[p]++].assign(d + i, d + i + d[i]);
						}
				s_net_seq_at[f & 63][p] = in[p].seq;
			}
			for (;;)
			{
				bool all = true;
				for (int p = 0; p < s_players; p++)
					all = all && s_zds_seen[p] > s_zds_rel;
				if (!all)
					break;
				for (int p = 0; p < s_players; p++)
					if (p != s_net_me)
						s_net_rx.insert(s_net_rx.end(), s_zds_k3[p][s_zds_rel].begin(), s_zds_k3[p][s_zds_rel].end());
				if (s_net_trace)
					std::fprintf(s_net_trace, "%u K3%s %d %d\n", g_FrameCount, g_in_rollback ? "r" : "", f, s_zds_rel);
				if (!g_in_rollback)
					s_zds_k3rel++;
				s_zds_rel++;
			}
			if (s_zds_ps && s_ps.hold && !s_ps.go)
			{
				bool all = true;
				for (int p = 0; p < s_players; p++)
					all = all && static_cast<u8>(in[p].pad.unused[1] - s_ps.rel) >= 1 && static_cast<u8>(in[p].pad.unused[1] - s_ps.rel) < 128;
				if (all)
				{
					s_ps.go = true;
					s_ps.rel++;
					if (s_net_trace)
						std::fprintf(s_net_trace, "%u PS%s %d %d\n", g_FrameCount, g_in_rollback ? "r" : "", f, s_ps.rel);
				}
			}
			return true;
		}

		void NetReport()
		{
			Console.WriteLn("ZdxsvGgpo: net sends %u msgs %u (sent %zu, unsent %zu) recvs %u rxmsgs %u rxbytes %u polls %u other %u nowait %u senddiff %u toolong %u maxq %u waits %d",
				s_ns.sends, s_ns.msgs, s_net_sent.size(), s_net_out.size(), s_ns.recvs, s_ns.rxmsgs, s_ns.rxbytes, s_ns.polls,
				s_ns.other, s_ns.nowait, s_ns.senddiff, s_ns.toolong, s_ns.maxq, s_waits);
			Console.WriteLn("ZdxsvGgpo: zd steps %u changed %u echo %u skip %u k3 %d/%d/%d/%d rel %d (fwd %u)", s_zd_steps, s_zd_changed, s_zds_echo, s_zds_skip,
				s_zds_seen[0], s_zds_seen[1], s_zds_seen[2], s_zds_seen[3], s_zds_rel, s_zds_k3rel);
			if (s_pw_hash && s_net_trace)
			{
				for (const auto& [f, h] : s_pw)
					std::fprintf(s_net_trace, "0 H %d %016llx %016llx %016llx %016llx\n", f, static_cast<unsigned long long>(h[0]),
						static_cast<unsigned long long>(h[1]), static_cast<unsigned long long>(h[2]), static_cast<unsigned long long>(h[3]));
				std::fflush(s_net_trace);
				Console.WriteLn("ZdxsvGgpo: pw hashes %zu frames", s_pw.size());
				s_pw.clear();
			}
			if (s_pw_dump)
				std::fflush(s_pw_dump);
		}
	} // namespace

	namespace
	{
		// ZDXSV_RBK: answers of a real start (s626 trace, 4 players, fake_lobby.py): lobby frames are
		// `18 cat cmd size seq 00ffffff body` (BE, 12-byte header), the game's `81 01 ..`.
		std::vector<u8> s_rbk_rx; // what recv 0x13 / 0x14 and the poll's readable count see
		bool s_rbk_started = false; // 0x6910 (battle start) queued
		u32 s_rbk_calls[0x50] = {};
		constexpr u32 RBK_FNO_RECV_LOBBY = 0x13;
		const char* const RBK_USERS[4] = {
			"010100064a3953584e4d000682a082a082a000000064834a837e815b838681458372835f839300000000000097b989f081490000000000000000000000000000000082bb82bf82e782cc94ed8a518ff38bb582cd8148000093478b408c82946a814900000000000000000000000089b482c9944382b982eb814901",
			"02010006554a4239414d000682a282a282a200000064834a837e815b838681458372835f839300000000000097b989f081490000000000000000000000000000000082bb82bf82e782cc94ed8a518ff38bb582cd8148000093478b408c82946a814900000000000000000000000089b482c9944382b982eb814902",
			"030200063856514b5043000682a482a482a400000064834a837e815b838681458372835f839300000000000097b989f081490000000000000000000000000000000082bb82bf82e782cc94ed8a518ff38bb582cd8148000093478b408c82946a814900000000000000000000000089b482c9944382b982eb814903",
			"040200065a3537434e32000682a682a682a600000064834a837e815b838681458372835f839300000000000097b989f081490000000000000000000000000000000082bb82bf82e782cc94ed8a518ff38bb582cd8148000093478b408c82946a814900000000000000000000000089b482c9944382b982eb814904",
		};
		const char* const RBK_6917[4] = {
			"0100000000000000000010000000020000000d00000000",
			"0200000000000000000010000000020000000d00000000",
			"0300000000000000000010000000000000000f00000000",
			"0400000000000000000010000000000000000f00000000",
		};
		const char* const RBK_RULE = "006400000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0000025802580064006400d20000000002000000000000000000000000000001";
		constexpr size_t RBK_RULE_TIME = 80; // u16 BE seconds (0x00d2 = 210)
		constexpr size_t RBK_RULE_GAUGE = 72; // u16 BE x2 (72, 74), 戦力ゲージ (0x0258 = 600)
		constexpr size_t RBK_RULE_COUNT = 87; // u8 連続対戦数, 0 = 任意 (rematch picked by input)

		std::vector<u8> Unhex(std::string_view s)
		{
			std::vector<u8> b;
			for (size_t i = 0; i + 1 < s.size(); i += 2)
				b.push_back(static_cast<u8>(std::stoi(std::string(s.substr(i, 2)), nullptr, 16)));
			return b;
		}

		void RbkQueue(u8 cat, u16 cmd, u16 seq, const std::vector<u8>& body)
		{
			const u8 h[12] = {0x18, cat, static_cast<u8>(cmd >> 8), static_cast<u8>(cmd), static_cast<u8>(body.size() >> 8),
				static_cast<u8>(body.size()), static_cast<u8>(seq >> 8), static_cast<u8>(seq), 0, 0xff, 0xff, 0xff};
			s_rbk_rx.insert(s_rbk_rx.end(), h, h + 12);
			s_rbk_rx.insert(s_rbk_rx.end(), body.begin(), body.end());
		}

		// Answer body of lobby Q cmd (body q). Position p (1-based) of N plays the recorded 4-player
		// position of its side: p <= ceil(N/2) side 1 (recorded 1, 2), else side 2 (recorded 3, 4).
		std::vector<u8> RbkBody(u16 cmd, const u8* q, u32 qn)
		{
			const int p = qn ? q[0] : 0;
			const int half = (s_rbk_n + 1) / 2;
			switch (cmd)
			{
				case 0x6911: return {static_cast<u8>(s_rbk_n)};
				case 0x6912: return {static_cast<u8>(s_rbk_me + 1)};
				case 0x6913:
				case 0x6917:
				{
					if (p < 1 || p > s_rbk_n)
						break;
					std::vector<u8> b = Unhex((cmd == 0x6913 ? RBK_USERS : RBK_6917)[p <= half ? p - 1 : 2 + p - 1 - half]);
					b.front() = static_cast<u8>(p);
					if (cmd == 0x6913)
						b.back() = static_cast<u8>(p);
					return b;
				}
				case 0x6915: return Unhex("000d31373931303831323634393138");
				case 0x6914:
				{
					std::vector<u8> b = Unhex(RBK_RULE);
					if (const char* t = std::getenv("ZDXSV_RBK_TIME"))
					{
						const int s = std::atoi(t);
						b[RBK_RULE_TIME] = static_cast<u8>(s >> 8);
						b[RBK_RULE_TIME + 1] = static_cast<u8>(s);
					}
					if (const char* g = std::getenv("ZDXSV_RBK_GAUGE"))
					{
						const int v = std::atoi(g);
						for (size_t o : {RBK_RULE_GAUGE, RBK_RULE_GAUGE + 2})
						{
							b[o] = static_cast<u8>(v >> 8);
							b[o + 1] = static_cast<u8>(v);
						}
					}
					if (const char* c = std::getenv("ZDXSV_RBK_COUNT"))
						b[RBK_RULE_COUNT] = static_cast<u8>(std::atoi(c));
					return b;
				}
				case 0x6916: return Unhex("0004c0a8010800022012");
			}
			return {};
		}

		// Sock-0 RPC fno before GGPO arms. Results as recorded (s626 pr1): send 0, poll as the battle
		// poll, getopt 4 (0x2008 -> 0, 0x2001 -> 1 at data+2), 0x38 3, close / socket / connect /
		// setopt 0. Other fnos go to the IOP.
		bool RbkCall(u32 fno, s16 len, u8* d)
		{
			u8* ram = eeMem->Main;
			if (fno < std::size(s_rbk_calls) && s_rbk_calls[fno]++ < 3)
				Console.WriteLn("ZdxsvGgpo: rbk fno %x len %d at vsync %u", fno, len, g_FrameCount);
			s32 result = 0;
			switch (fno)
			{
				case NET_FNO_POLL:
					if (!std::exchange(s_rbk_started, true))
					{
						RbkQueue(0x10, 0x6910, 0x1000, {});
						Console.WriteLn("ZdxsvGgpo: rbk position %d of %d, battle start at vsync %u", s_rbk_me + 1, s_rbk_n, g_FrameCount);
					}
					*reinterpret_cast<u16*>(ram + NET_REQ_LEN) = 4;
					*reinterpret_cast<u16*>(ram + NET_REQ_DATA + 2) = 0x2000;
					*reinterpret_cast<u16*>(ram + NET_REQ_DATA + 4) = static_cast<u16>(std::min<size_t>(s_rbk_rx.size(), NET_RX_MAX));
					break;
				case RBK_FNO_RECV_LOBBY: // header, then body
				case NET_FNO_RECV:
				{
					const u32 max = static_cast<u32>(std::clamp<s32>(len, 0, NET_RX_MAX));
					u32 n = 0;
					if (fno == NET_FNO_RECV) // whole McsMessages
						while (n + 1 < s_rbk_rx.size() && s_rbk_rx[n] >= 2 && n + s_rbk_rx[n] <= std::min<size_t>(s_rbk_rx.size(), max))
							n += s_rbk_rx[n];
					else
						n = std::min<u32>(max, static_cast<u32>(s_rbk_rx.size()));
					std::memcpy(d, s_rbk_rx.data(), n);
					s_rbk_rx.erase(s_rbk_rx.begin(), s_rbk_rx.begin() + n);
					result = static_cast<s32>(n);
					break;
				}
				case NET_FNO_SEND:
				{
					const s32 n = std::clamp<s32>(len, 0, 0x3ca);
					if (n >= 12 && d[0] == 0x81) // lobby Qs
					{
						for (s32 o = 0; o + 12 <= n;)
						{
							const u16 cmd = static_cast<u16>(d[o + 2] << 8 | d[o + 3]);
							const u32 size = std::min<u32>(d[o + 4] << 8 | d[o + 5], n - o - 12);
							const u16 seq = static_cast<u16>(d[o + 6] << 8 | d[o + 7]);
							const std::vector<u8> body = RbkBody(cmd, d + o + 12, size);
							RbkQueue(0x02, cmd, seq, body);
							Console.WriteLn("ZdxsvGgpo: rbk Q %04x -> %zu B", cmd, body.size());
							o += 12 + size;
						}
					}
					else if (n >= 12 && d[0] == 0x82) // battle conn msg: the greet needs no answer
						;
					else // battle McsMessages: back once per remote position, sender nibble rewritten
					{
						for (s32 i = 0; i + 1 < n && d[i] >= 2 && i + d[i] <= n; i += d[i])
							for (int q = 0; q < s_rbk_n; q++)
								if (q != s_rbk_me)
								{
									const size_t at = s_rbk_rx.size();
									s_rbk_rx.insert(s_rbk_rx.end(), d + i, d + i + d[i]);
									s_rbk_rx[at + 1] = static_cast<u8>((d[i + 1] & 0xf0) | q);
								}
					}
					break;
				}
				case 7: // battle server connect: its greet
				{
					static constexpr u8 greet[12] = {0x28, 0x01, 0x10, 0x31, 0, 0, 0, 1, 0, 0xff, 0xff, 0xff};
					s_rbk_rx.insert(s_rbk_rx.end(), greet, greet + 12);
					break;
				}
				case 4:
				{
					const u32 v = *reinterpret_cast<const u16*>(ram + NET_REQ_LEN) == 0x2001;
					std::memcpy(ram + NET_REQ_DATA + 2, &v, 4);
					result = 4;
					break;
				}
				case 0x38:
					result = 3;
					break;
				case 0xd: // lobby close
					s_rbk_rx.clear();
					break;
				case 3:
				case 0x16:
					break;
				default:
					return false;
			}
			*reinterpret_cast<s32*>(ram + NET_RES_LEN) = result;
			cpuRegs.GPR.n.v0.SD[0] = result;
			cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
			return true;
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
		const bool key = fno == NET_FNO_SEND && sock == NET_BATTLE_SOCK && len > 0 && len <= 0x3ca && HasKeyMsg(d, len);
		if (s_rbk && !s_net_armed && !key) // connect (fno 7) has the address in the sock field
			return RbkCall(fno, len, d);
		if (sock != NET_BATTLE_SOCK)
			return false;
		if (!s_net_armed)
		{
			// sock 0 is the lobby TCP too: arm at the battle's first key msg
			if (!key)
				return false;
			s_net_armed = true;
			if (s_rbk)
			{
				s_net_rx.insert(s_net_rx.end(), s_rbk_rx.begin(), s_rbk_rx.end());
				s_rbk_rx.clear();
				// ZDXSV_RBK_TURBO=1: the battle runs turbo too (GGPO paces the peers by frame)
				if (!std::getenv("ZDXSV_RBK_TURBO"))
					VMManager::SetLimiterMode(LimiterModeType::Nominal);
			}
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

namespace ZdxsvGgpo
{
	// EE probe (iR5900.cpp): GGPO frame being run, the frame numbers of NET_TRACE H lines and PW dumps.
	int ProbeFrame() { return s_net_frame; }
} // namespace ZdxsvGgpo
